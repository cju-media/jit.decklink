// jit.decklink.send~
//
// Sends Jitter video matrices and multichannel (MC) MSP audio to a Blackmagic
// Design DeckLink device (e.g. UltraStudio 3G).
//
// Named with a ".send" segment because it's the output half of a pair: a
// companion jit.decklink.receive~ (input, for capture devices) is planned
// but not yet built -- see the project README.
//
// -- Header ordering --------------------------------------------------------
// DeckLinkAPI.h must be included BEFORE c74_min.h. Both headers define/pull in
// a `GetBytes` symbol on macOS (DeckLink as a virtual method, Carbon/QuickTime
// as a legacy macro via Min-API's system includes) and the two collide if the
// macro is still defined when DeckLinkAPI.h's method declarations are parsed.
// We `#undef` it on both sides of the c74_min.h include as a defensive belt +
// braces measure.
// -----------------------------------------------------------------------------

#ifdef __APPLE__
    #include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef GetBytes
    #undef GetBytes
#endif

#include "DeckLinkAPI.h"

#include "c74_min.h"

#ifdef GetBytes
    #undef GetBytes
#endif

#ifdef _WIN32
    #include <windows.h>
    #include <oleauto.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace c74::min;

namespace {

// -----------------------------------------------------------------------------
// A small single-producer (MSP perform)/single-consumer (scheduler timer) ring
// buffer of interleaved float audio frames.
//
// The DeckLink SDK's synchronous audio call (WriteAudioSamplesSync) can block
// on hardware I/O, so it must never be called from the real-time audio thread.
// Instead, the perform routine below converts and pushes whole DSP vectors
// into this buffer (one mutex acquisition per vector, not per sample), and a
// low-priority `c74::min::timer` drains it periodically and hands blocks to
// the DeckLink SDK. The mutex's critical sections are just memcpy-sized, so
// contention is brief; this trades a small amount of real-time purity for an
// implementation that is far easier to get correct without DeckLink hardware
// on hand to profile.
// -----------------------------------------------------------------------------
class interleaved_ring_buffer {
  public:
    void reset(size_t channels, size_t capacity_frames) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_channels = channels;
        m_capacity_frames = capacity_frames;
        m_data.assign(channels * capacity_frames, 0.0f);
        m_write = 0;
        m_read = 0;
        m_overflows = 0;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_channels = 0;
        m_capacity_frames = 0;
        m_data.clear();
        m_write = 0;
        m_read = 0;
    }

    size_t channels() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_channels;
    }

    // Push `frame_count` frames of `src_channels`-wide interleaved audio.
    // Frames are padded with silence or truncated to match the buffer's
    // configured channel count. Returns false if any frames had to be
    // dropped because the buffer was full (see take_overflow_count()).
    bool push_block(const float* interleaved, size_t frame_count, size_t src_channels) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_channels == 0 || m_capacity_frames == 0)
            return false;

        bool dropped_any = false;
        size_t copy_channels = std::min(src_channels, m_channels);

        for (size_t i = 0; i < frame_count; ++i) {
            if ((m_write - m_read) >= m_capacity_frames) {
                ++m_overflows;
                dropped_any = true;
                continue;
            }
            float* dst = &m_data[(m_write % m_capacity_frames) * m_channels];
            const float* src = interleaved + i * src_channels;
            for (size_t c = 0; c < copy_channels; ++c)
                dst[c] = src[c];
            for (size_t c = copy_channels; c < m_channels; ++c)
                dst[c] = 0.0f;
            ++m_write;
        }
        return !dropped_any;
    }

    // Pop up to `max_frames` frames (each `channels()` samples wide) into
    // `dest`. Returns the number of frames actually copied.
    size_t pop_frames(float* dest, size_t max_frames) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_channels == 0)
            return 0;
        size_t available = m_write - m_read;
        size_t n = std::min(available, max_frames);
        for (size_t i = 0; i < n; ++i) {
            const float* src = &m_data[((m_read + i) % m_capacity_frames) * m_channels];
            std::memcpy(dest + i * m_channels, src, sizeof(float) * m_channels);
        }
        m_read += n;
        return n;
    }

    long take_overflow_count() {
        std::lock_guard<std::mutex> lock(m_mutex);
        long v = m_overflows;
        m_overflows = 0;
        return v;
    }

  private:
    std::mutex m_mutex;
    std::vector<float> m_data;
    size_t m_channels{ 0 };
    size_t m_capacity_frames{ 0 };
    size_t m_write{ 0 };
    size_t m_read{ 0 };
    long m_overflows{ 0 };
};

// Render an HRESULT the way the DeckLink SDK docs name it (these are the
// standard CFPlugInCOM error codes on macOS -- same numeric values as
// Windows COM), so a failure is self-explanatory in the console instead of
// requiring a lookup.
std::string describe_hresult(HRESULT hr) {
    switch ((uint32_t)hr) {
        case 0x80000004: return "E_NOINTERFACE";
        case 0x80000003: return "E_INVALIDARG";
        case 0x80000002: return "E_OUTOFMEMORY";
        case 0x80000001: return "E_NOTIMPL";
        case 0x80000005: return "E_POINTER";
        case 0x80000006: return "E_HANDLE";
        case 0x80000007: return "E_ABORT";
        case 0x80000008: return "E_FAIL";
        case 0x80000009: return "E_ACCESSDENIED";
        case 0x8000FFFF: return "E_UNEXPECTED";
        default: {
            char buf[16];
            snprintf(buf, sizeof(buf), "0x%08X", (uint32_t)hr);
            return buf;
        }
    }
}

// Reduce a display-mode / device name to lowercase alphanumerics only, so
// "1080p29.97", "1080p2997", and "1080P 29.97" all compare equal.
std::string normalize_name(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (std::isalnum(c))
            out += (char)std::tolower(c);
    }
    return out;
}

// Returns a name suitable for showing the user (scan's umenu list, console
// messages). Deliberately uses IDeckLink::GetDisplayName(), not
// GetModelName(): on multi-port cards (DeckLink Duo/Quad/8K Pro etc.),
// each physical connector is its own IDeckLink instance from the
// iterator, and GetModelName() returns the *same* string for all of them
// (e.g. "DeckLink Quad 2" x4) -- indistinguishable in the scan list.
// GetDisplayName() is Blackmagic's own per-connector-differentiated name
// (e.g. "DeckLink Quad 2 (3)"), which is what actually lets a user pick
// the right port. Falls back to GetModelName() if GetDisplayName() ever
// comes back empty, so we still show something rather than nothing.
std::string get_device_name(IDeckLink* deck_link) {
#if defined(__APPLE__)
    auto cf_to_std = [](CFStringRef cf) -> std::string {
        if (!cf)
            return {};
        char buffer[256];
        if (CFStringGetCString(cf, buffer, sizeof(buffer), kCFStringEncodingUTF8))
            return buffer;
        return {};
    };
    std::string result;
    CFStringRef cf_display_name = nullptr;
    if (deck_link->GetDisplayName(&cf_display_name) == S_OK && cf_display_name) {
        result = cf_to_std(cf_display_name);
        CFRelease(cf_display_name);
    }
    if (result.empty()) {
        CFStringRef cf_model_name = nullptr;
        if (deck_link->GetModelName(&cf_model_name) == S_OK && cf_model_name) {
            result = cf_to_std(cf_model_name);
            CFRelease(cf_model_name);
        }
    }
    return result.empty() ? "Unknown Device" : result;
#elif defined(_WIN32)
    auto bstr_to_std = [](BSTR bstr) -> std::string {
        if (!bstr)
            return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, bstr, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0)
            return {};
        std::string tmp(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, bstr, -1, &tmp[0], len, nullptr, nullptr);
        return tmp;
    };
    std::string result;
    BSTR bstr_display_name = nullptr;
    if (deck_link->GetDisplayName(&bstr_display_name) == S_OK && bstr_display_name) {
        result = bstr_to_std(bstr_display_name);
        SysFreeString(bstr_display_name);
    }
    if (result.empty()) {
        BSTR bstr_model_name = nullptr;
        if (deck_link->GetModelName(&bstr_model_name) == S_OK && bstr_model_name) {
            result = bstr_to_std(bstr_model_name);
            SysFreeString(bstr_model_name);
        }
    }
    return result.empty() ? "Unknown Device" : result;
#else
    std::string result;
    const char* display_name_str = nullptr;
    if (deck_link->GetDisplayName(&display_name_str) == S_OK && display_name_str) {
        result = display_name_str;
        free((void*)display_name_str);
    }
    if (result.empty()) {
        const char* model_name_str = nullptr;
        if (deck_link->GetModelName(&model_name_str) == S_OK && model_name_str) {
            result = model_name_str;
            free((void*)model_name_str);
        }
    }
    return result.empty() ? "Unknown Device" : result;
#endif
}

std::string get_display_mode_name(IDeckLinkDisplayMode* mode) {
#if defined(__APPLE__)
    std::string result = "Unknown";
    CFStringRef cf_name = nullptr;
    if (mode->GetName(&cf_name) == S_OK && cf_name) {
        char buffer[256];
        if (CFStringGetCString(cf_name, buffer, sizeof(buffer), kCFStringEncodingUTF8))
            result = buffer;
        CFRelease(cf_name);
    }
    return result;
#elif defined(_WIN32)
    std::string result = "Unknown";
    BSTR bstr_name = nullptr;
    if (mode->GetName(&bstr_name) == S_OK && bstr_name) {
        int len = WideCharToMultiByte(CP_UTF8, 0, bstr_name, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string tmp(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, bstr_name, -1, &tmp[0], len, nullptr, nullptr);
            result = tmp;
        }
        SysFreeString(bstr_name);
    }
    return result;
#else
    std::string result = "Unknown";
    const char* name_str = nullptr;
    if (mode->GetName(&name_str) == S_OK && name_str) {
        result = name_str;
        free((void*)name_str);
    }
    return result;
#endif
}

// Counts events (calls, successful frames, ...) and, roughly every
// `report_interval_sec`, hands back a one-line rate summary to post via
// cout. Used to make throughput problems (like the GL-capture path
// dropping to a fraction of the expected frame rate) directly measurable
// from the Max console instead of guessed at.
class rate_counter {
  public:
    explicit rate_counter(double report_interval_sec = 3.0)
        : m_interval{ report_interval_sec }
        , m_last_report{ std::chrono::steady_clock::now() }
    {
    }

    void tick(bool success) {
        ++m_calls;
        if (success)
            ++m_successes;
    }

    // Returns a summary string (and resets the counters) if the report
    // interval has elapsed, otherwise an empty string.
    std::string maybe_report(const std::string& label) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_last_report).count();
        if (elapsed < m_interval)
            return {};

        std::string out = label + ": " + std::to_string(m_calls / elapsed) + " calls/sec, "
            + std::to_string(m_successes / elapsed) + " successful/sec";
        m_calls = 0;
        m_successes = 0;
        m_last_report = now;
        return out;
    }

  private:
    double m_interval;
    std::chrono::steady_clock::time_point m_last_report;
    long m_calls = 0;
    long m_successes = 0;
};

} // namespace

class jit_decklink_send_tilde : public object<jit_decklink_send_tilde>, public vector_operator<>, public mc_operator_base {
  public:
    MIN_DESCRIPTION {"Send Jitter video matrices and MC audio to a Blackmagic DeckLink device"};
    MIN_TAGS {"video, audio, output, decklink, blackmagic"};
    MIN_AUTHOR {"Jules"};
    MIN_RELATED {"jit.gl.videoplane, jit.record~, mc.pack~"};

    inlet<> m_inlet_video { this, "(matrix) Jitter video input; also accepts device/mode messages", "" };
    inlet<> m_inlet_audio { this, "(multichannel signal) Audio to send to the DeckLink device", "signal" };

    // NOTE: the outlet "type" parameter (3rd arg) is used by Max to
    // type-check patch-cord connections -- it must name an actual Max
    // message/inlet type ("", "signal", "int", "float", "list", ...), not
    // an object class name. Using "umenu" here (as the original version of
    // this file did) made Max refuse to connect this outlet to anything,
    // since no inlet actually declares itself as type "umenu" -- a plain
    // umenu's inlet just accepts generic messages. Leaving it "" (any)
    // lets it connect normally to a umenu (or anywhere else).
    outlet<> m_outlet_menu { this, "(list) Device list, for a connected umenu", "" };
    outlet<> m_outlet_dump { this, "(anything) Status / diagnostic messages", "" };

    jit_decklink_send_tilde(const atoms& args = {}) {
        // Device selection at instantiation is done via the `device`
        // attribute's default value (below), which mirrors how patchers
        // typically set it afterwards (`device 2` message, or `@device 2`
        // as an object-box attribute argument). We intentionally don't hand
        // -parse `args` here to avoid double-initializing the device.
        (void)args;
        m_audio_flush_timer.delay(10.0);
    }

    ~jit_decklink_send_tilde() {
        m_audio_flush_timer.stop();
        m_gl_poll_timer.stop();
        teardown_gl_asyncread_helper();
        std::lock_guard<std::mutex> lock(m_device_mutex);
        cleanup_decklink_locked();
    }

  private:
    // -------------------------------------------------------------------
    // DeckLink device / video / audio state (guarded by m_device_mutex,
    // except where noted). All of this is touched from Max's main/message
    // thread and/or the scheduler thread (m_audio_flush_timer) -- never
    // from the real-time MSP audio thread, which only ever talks to m_ring.
    //
    // IMPORTANT: this block must stay declared *before* the attributes
    // below. Min-API attributes apply their default value (running our
    // setter lambda) during the attribute's own construction -- i.e. while
    // `jit_decklink_send_tilde` itself is still being built, in member
    // declaration order. The `device` attribute's setter calls
    // initialize_device(), which touches all of this state; if this block
    // were declared *after* the attributes (as it originally was), that
    // call would run against not-yet-constructed memory (an
    // uninitialized std::mutex, std::string, std::vector, ...), which is
    // undefined behavior and crashed Max on every instantiation.
    // -------------------------------------------------------------------
    std::mutex m_device_mutex;
    IDeckLink* m_deck_link = nullptr;
    IDeckLinkOutput* m_deck_link_output = nullptr;
    int m_current_device_index = -1;
    std::string m_device_name;

    bool m_video_enabled = false;
    long m_frame_width = 0;
    long m_frame_height = 0;
    int32_t m_row_bytes = 0;
    std::string m_active_mode_name;
    double m_frame_rate_fps = 30.0; // updated from the active display mode; see setup_video_output_locked()
    std::chrono::steady_clock::time_point m_gl_poll_next_fire; // see schedule_next_gl_poll()

    std::atomic<bool> m_audio_enabled { false };
    int m_audio_bits_active = 16;

    // Rate-limiting for repeated warnings so a fast-running jit_matrix
    // sender can't flood the Max console.
    bool m_warned_type = false;
    bool m_warned_dims2d = false;
    bool m_warned_planes = false;
    bool m_warned_no_device = false;
    std::string m_last_video_dims_warning;

    // Internal jit.gl.asyncread helper (GL texture readback), main/Jitter
    // thread only -- see handle_gl_texture()/ensure_gl_asyncread_helper().
    c74::max::t_object* m_gl_asyncread_box = nullptr;
    symbol m_gl_asyncread_texture { "" };
    symbol m_gl_asyncread_out_name { "" };

    // Throughput diagnostics -- see rate_counter, and note_video_frame_result()/
    // handle_gl_texture()/poll_gl_asyncread_frame() for how each is used.
    rate_counter m_gl_texture_msg_rate;
    rate_counter m_gl_poll_rate;
    rate_counter m_video_frame_rate;

    interleaved_ring_buffer m_ring;

    // Drains m_ring and hands blocks to the DeckLink SDK on the scheduler
    // thread, well away from the real-time audio thread. Re-arms itself.
    // (Its constructor only registers a Max clock; delay() -- which is what
    // actually arms it -- is called from our constructor's body below,
    // which runs after every member is constructed.)
    timer<> m_audio_flush_timer { this, MIN_FUNCTION {
        flush_audio_to_decklink();
        m_audio_flush_timer.delay(10.0); // ~10ms cadence
        return {};
    }};

    // Polls the internal jit.gl.asyncread helper (see
    // ensure_gl_asyncread_helper()) at the active display mode's frame
    // rate while it's alive, and stops re-arming itself once it's torn
    // down. Only ever running while m_gl_asyncread_box is non-null.
    //
    // Rearms itself via schedule_next_gl_poll() rather than a flat
    // `.delay(period)` here, which would make the *actual* period
    // `period + poll_gl_asyncread_frame()'s own running time` (each cycle
    // schedules the next one only after finishing its own work) --
    // compounding into a real, measured throughput drop as that per-cycle
    // work (pixel conversion, DisplayVideoFrameSync) takes an increasing
    // share of the intended period. schedule_next_gl_poll() instead targets
    // a fixed wall-clock schedule, so as long as our own work costs less
    // than one period, the average rate holds at m_frame_rate_fps.
    timer<> m_gl_poll_timer { this, MIN_FUNCTION {
        poll_gl_asyncread_frame();
        if (m_gl_asyncread_box)
            schedule_next_gl_poll();
        return {};
    }};

  public:
    // -------------------------------------------------------------------
    // Attributes
    // -------------------------------------------------------------------

    // NOTE ON DECLARATION ORDER: `device` is declared *last* among these
    // four attributes, and this matters, not just for style. Min-API
    // applies each attribute's default value (running its setter) during
    // that attribute's own construction, in member declaration order.
    // `device`'s setter is the one that actually opens a device
    // (initialize_device()), which reads displaymode/audiochannels/
    // audiobits; if `device` were constructed before them, that read would
    // hit not-yet-constructed attribute objects -- undefined behavior that
    // manifests as an intermittent crash (it can appear to work when the
    // reused memory happens to look like a valid default, and crash the
    // next time it doesn't). The other three attributes' setters only
    // touch device state (m_deck_link_output, and in
    // audiochannels/audiobits' case, each other) inside an `if
    // (m_deck_link_output)` guard, which is false for all of them at
    // construction as long as `device` hasn't run yet -- so their relative
    // order doesn't matter, only that `device` comes after all of them.
    attribute<symbol> displaymode { this, "displaymode", "1080p2997",
        description {"Output display mode/timing, e.g. 1080p2997, 1080p25, 1080i5994, 720p60, NTSC, PAL, "
                     "4K2160p2997. Matching is case/punctuation-insensitive. Send 'getdisplaymodes' to list "
                     "the exact modes the selected device supports."},
        setter { MIN_FUNCTION {
            symbol wanted = args.empty() ? symbol("1080p2997") : symbol(args[0]);
            if (m_deck_link_output) {
                std::lock_guard<std::mutex> lock(m_device_mutex);
                setup_video_output_locked(wanted);
            }
            atoms out;
            out.push_back(wanted);
            return out;
        }}
    };

    attribute<int> audiochannels { this, "audiochannels", 2,
        description {"Number of audio channels to send to the DeckLink device. Must be 2, 8, or 16."},
        setter { MIN_FUNCTION {
            int v = args.empty() ? 2 : (int)args[0];
            if (v != 2 && v != 8 && v != 16) {
                int clamped = (v <= 2) ? 2 : (v <= 8 ? 8 : 16);
                cwarn << "audiochannels must be 2, 8, or 16; using " << clamped << " instead of " << v << "." << endl;
                v = clamped;
            }
            if (m_deck_link_output) {
                std::lock_guard<std::mutex> lock(m_device_mutex);
                setup_audio_output_locked(v, audiobits);
            }
            atoms out;
            out.push_back(v);
            return out;
        }}
    };

    attribute<int> audiobits { this, "audiobits", 16,
        description {"Audio sample bit depth to send to the DeckLink device. Must be 16 or 32 (integer PCM)."},
        setter { MIN_FUNCTION {
            int v = args.empty() ? 16 : (int)args[0];
            if (v != 16 && v != 32) {
                cwarn << "audiobits must be 16 or 32; using 16 instead of " << v << "." << endl;
                v = 16;
            }
            if (m_deck_link_output) {
                std::lock_guard<std::mutex> lock(m_device_mutex);
                setup_audio_output_locked(audiochannels, v);
            }
            atoms out;
            out.push_back(v);
            return out;
        }}
    };

    attribute<bool> perfstats { this, "perfstats", false,
        description {"Print periodic (~every 3s) throughput measurements to the console -- messages/frames per "
                     "second at each stage of the video pipeline. Useful when diagnosing frame-rate problems; "
                     "off by default so normal use doesn't print anything once things are working."}
    };

    attribute<int> device { this, "device", 0,
        description {"Index of the DeckLink device to use, as listed by the 'scan' message (0-based)."},
        setter { MIN_FUNCTION {
            int index = args.empty() ? 0 : (int)args[0];
            m_current_device_index = index;
            initialize_device(index);
            return args;
        }}
    };

    // -------------------------------------------------------------------
    // Messages
    // -------------------------------------------------------------------

    message<> scan { this, "scan", "Scan for DeckLink devices and send their names out the right outlet (for a connected umenu).",
        MIN_FUNCTION {
            do_scan();
            return {};
        }
    };

    // Alias, in case patchers expect a more descriptive name for the same action.
    message<> getdevicelist { this, "getdevicelist", "Alias for 'scan'.",
        MIN_FUNCTION {
            do_scan();
            return {};
        }
    };

    message<> getdisplaymodes { this, "getdisplaymodes", "List the display modes supported by the selected device (sent out the dump outlet).",
        MIN_FUNCTION {
            std::lock_guard<std::mutex> lock(m_device_mutex);
            if (!m_deck_link_output) {
                cwarn << "select a device first (see 'scan')." << endl;
                return {};
            }
            IDeckLinkDisplayModeIterator* it = nullptr;
            if (m_deck_link_output->GetDisplayModeIterator(&it) != S_OK || !it) {
                cwarn << "could not enumerate display modes." << endl;
                return {};
            }
            IDeckLinkDisplayMode* mode = nullptr;
            while (it->Next(&mode) == S_OK) {
                atoms a;
                a.push_back(symbol("displaymode"));
                a.push_back(symbol(get_display_mode_name(mode)));
                a.push_back((int)mode->GetWidth());
                a.push_back((int)mode->GetHeight());
                m_outlet_dump.send(a);
                mode->Release();
            }
            it->Release();
            return {};
        }
    };

    // Lets a umenu wired straight back into the left inlet select a device by index.
    message<> m_int { this, "int", "Select the DeckLink device by index (e.g. from a connected umenu).",
        MIN_FUNCTION {
            if (!args.empty())
                device = (int)args[0];
            return {};
        }
    };

    message<> jit_matrix { this, "jit_matrix", "Video matrix input; converted and sent to the DeckLink output.",
        MIN_FUNCTION {
            if (args.empty())
                return {};

            // A real jit_matrix source taking over from a GL texture source:
            // tear down the internal jit.gl.asyncread helper rather than let
            // both fight over the same output frame. See jit.rtmpstream's
            // notes on this exact failure mode in handle_gl_texture().
            if (m_gl_asyncread_box)
                teardown_gl_asyncread_helper();

            symbol matrix_name = args[0];
            void* matrix = c74::max::jit_object_findregistered(matrix_name);
            if (!matrix)
                return {};

            long savelock = (long)c74::max::jit_object_method(matrix, c74::max::_jit_sym_lock, 1);

            c74::max::t_jit_matrix_info info{};
            char* bp = nullptr;
            c74::max::jit_object_method(matrix, c74::max::_jit_sym_getinfo, &info);
            c74::max::jit_object_method(matrix, c74::max::_jit_sym_getdata, &bp);

            if (info.type != c74::max::_jit_sym_char) {
                warn_once(m_warned_type, cerr, "only char (0..255 per-plane) matrices are supported; "
                                          "use [jit.matrix @type char] upstream.");
            }
            else if (info.dimcount != 2) {
                warn_once(m_warned_dims2d, cerr, "only 2D matrices are supported.");
            }
            else if (info.planecount != 3 && info.planecount != 4) {
                warn_once(m_warned_planes, cerr, "only 3-plane (RGB) or 4-plane (ARGB) matrices are supported.");
            }
            else if (bp) {
                copy_matrix_to_decklink(info, (unsigned char*)bp);
            }

            c74::max::jit_object_method(matrix, c74::max::_jit_sym_lock, (void*)savelock);
            return {};
        }
    };

    // A jit.gl.* object connected directly to the inlet sends this instead
    // of jit_matrix. See the comment on ensure_gl_asyncread_helper() for how
    // this is handled.
    message<> jit_gl_texture { this, "jit_gl_texture", "OpenGL texture input; read back and sent to the DeckLink output.",
        MIN_FUNCTION {
            handle_gl_texture(args);
            return {};
        }
    };

    // The internal jit.gl.asyncread helper, once created, keeps reading its
    // texture on every poll tick regardless of whether a patch cord is still
    // connected. If you're switching from a GL source back to jit_matrix,
    // that switch happens automatically (see the jit_matrix message above);
    // this is for explicitly stopping GL capture without sending a matrix.
    message<> gl_disconnect { this, "gl_disconnect", "Stop and tear down the internal jit.gl.asyncread helper, if one exists.",
        MIN_FUNCTION {
            teardown_gl_asyncread_helper();
            return {};
        }
    };

    // -------------------------------------------------------------------
    // Audio (MC signal input)
    // -------------------------------------------------------------------

    // No signal outlet is declared, so `output` is always zero-channel; we
    // only ever consume `input` here. Being both a vector_operator<> (which
    // supplies the actual perform callback) and an mc_operator_base (an
    // empty tag type Min-API checks for with is_base_of<> to turn on
    // Z_MC_INLETS) is what makes the signal inlet accept a dynamic number
    // of channels from an mc.* signal instead of being pinned to 1.
    void operator()(audio_bundle input, audio_bundle output) {
        (void)output;
        if (!m_audio_enabled.load(std::memory_order_relaxed))
            return;
        // DeckLink only outputs audio at 48kHz. setup_audio_output_locked()
        // already checks this before enabling audio, but the driver's
        // samplerate can change out from under a running DSP chain, so we
        // guard again here per-vector as cheap insurance.
        if (this->samplerate() != 48000.0)
            return;

        long in_channels = input.channel_count();
        long frames = input.frame_count();
        if (in_channels <= 0 || frames <= 0)
            return;

        thread_local std::vector<float> interleaved;
        interleaved.resize((size_t)in_channels * (size_t)frames);
        for (long i = 0; i < frames; ++i) {
            for (long ch = 0; ch < in_channels; ++ch) {
                interleaved[(size_t)i * in_channels + ch] = (float)input.samples(ch)[i];
            }
        }

        m_ring.push_block(interleaved.data(), (size_t)frames, (size_t)in_channels);
    }

  private:
    // -------------------------------------------------------------------
    // Helper methods. (The data members they use are declared earlier --
    // see the comment on that block, just after the destructor, for why.)
    // -------------------------------------------------------------------

    // `stream` is normally `cerr` (red -- something that breaks/prevents
    // output) or `cwarn` (yellow -- worth knowing, doesn't break anything).
    void warn_once(bool& flag, logger& stream, const char* msg) {
        if (!flag) {
            stream << msg << endl;
            flag = true;
        }
    }

    void warn_throttled(std::string& last_msg, logger& stream, const std::string& msg) {
        if (last_msg != msg) {
            stream << msg << endl;
            last_msg = msg;
        }
    }

    void do_scan() {
        auto names = enumerate_device_names();
        m_outlet_menu.send("clear");
        for (auto& n : names)
            m_outlet_menu.send("append", n);
        if (names.empty())
            cwarn << "no DeckLink devices found (is a Blackmagic device connected and Desktop Video installed?)." << endl;
        else
            cout << "found " << names.size() << " DeckLink device(s)." << endl;
    }

    std::vector<std::string> enumerate_device_names() {
        std::vector<std::string> names;
        IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
        if (!iterator) {
            cwarn << "no Blackmagic DeckLink driver found (install Desktop Video)." << endl;
            return names;
        }
        IDeckLink* deck_link = nullptr;
        while (iterator->Next(&deck_link) == S_OK) {
            names.push_back(get_device_name(deck_link));
            deck_link->Release();
        }
        iterator->Release();
        return names;
    }

    // -------------------------------------------------------------------
    // GL texture input: delegate the actual GPU->CPU readback to a hidden,
    // internally-instantiated [jit.gl.asyncread] rather than touching OpenGL
    // ourselves. Jitter's GPU texture struct layout (t_jit_gl_texture) isn't
    // part of the public Max SDK headers -- only opaque helpers like
    // jit_gl_texture_get() are exposed, without the struct needed to read
    // GLuint/target/dims back out of what they return -- so hand-rolling a
    // readback against undocumented internals would be fragile across Max
    // versions. Using jit.gl.asyncread instead means we inherit Cycling'74's
    // own correct GL-context/thread handling and PBO double-buffering for
    // free. This mirrors the overall approach used by rtmp.stream~'s
    // handle_gl_texture()/ensure_gl_asyncread_helper()/
    // poll_gl_asyncread_frame() (create hidden, read its out_name matrix
    // directly rather than relying on outlet wiring) -- see
    // ensure_gl_asyncread_helper() below for one place this project departs
    // from that reference, and why.
    // -------------------------------------------------------------------

    void handle_gl_texture(const atoms& args) {
        if (args.empty())
            return;

        symbol texname = args[0];

        if (!ensure_gl_asyncread_helper())
            return;

        // Diagnostic: "successful" here means the incoming texture name
        // actually differed from last time, i.e. we reset the helper's
        // `texture` attribute. If this is happening on every single
        // message (rate close to the incoming message rate), that's worth
        // knowing -- resetting that attribute every frame is a plausible
        // way to defeat jit.gl.asyncread's own double-buffering.
        bool changed = (texname != m_gl_asyncread_texture);
        if (changed) {
            c74::max::object_attr_setsym(m_gl_asyncread_box, c74::max::gensym("texture"), texname);
            m_gl_asyncread_texture = texname;
        }
        if (perfstats) {
            m_gl_texture_msg_rate.tick(changed);
            std::string report = m_gl_texture_msg_rate.maybe_report("jit_gl_texture messages (successful = texture name changed)");
            if (!report.empty())
                cout << report << endl;
        }
    }

    // Lazily create a hidden [jit.gl.asyncread] instance in our own patcher,
    // configured to output matrices. We don't rely on wiring its outlet to
    // us -- poll_gl_asyncread_frame() reads its internally-registered
    // readback matrix directly instead (see the comment there for why).
    bool ensure_gl_asyncread_helper() {
        if (m_gl_asyncread_box)
            return true;

        c74::max::t_object* patcher_obj = patcher();
        if (!patcher_obj) {
            cerr << "could not locate the owning patcher for the internal jit.gl.asyncread helper." << endl;
            return false;
        }

        // @automatic 1 (the default): let jit.gl.asyncread trigger its own
        // capture on every actual Jitter render pass, same as it would in a
        // normal patch. We only ever *read* its result on our poll timer
        // (see poll_gl_asyncread_frame()), never trigger capture ourselves.
        //
        // An earlier version of this used @automatic 0 plus an explicit
        // "bang" from our own scheduler-thread timer to drive capture,
        // reasoning that having two independent trigger sources hitting the
        // same async PBO double-buffer risked corrupting its state. That
        // part held up, but driving capture from a timer completely
        // decoupled from the actual GL render cadence -- rather than
        // letting it ride the real render pass -- turned out to tank
        // throughput by roughly 30x in practice (measured: ~30fps via a
        // normal @automatic patch cord, ~1fps through that path). Sticking
        // to @automatic's own trigger and only reading the result avoids
        // both problems: still one trigger source, but the *right* one.
        m_gl_asyncread_box = c74::max::newobject_sprintf(
            patcher_obj, "@maxclass jit.gl.asyncread @matrixoutput 1 @automatic 1");
        if (!m_gl_asyncread_box) {
            cerr << "could not instantiate the internal jit.gl.asyncread helper -- is Jitter installed?" << endl;
            return false;
        }
        c74::max::jbox_set_hidden(m_gl_asyncread_box, 1);

        // jit.gl.asyncread's "out_name" attribute (documented) gives us the
        // name of its internally-registered readback matrix, which we read
        // with the exact same jit_object_findregistered/lock/getinfo/
        // getdata sequence the jit_matrix message above already uses.
        c74::max::t_symbol* out_name_sym = c74::max::object_attr_getsym(m_gl_asyncread_box, c74::max::gensym("out_name"));
        m_gl_asyncread_out_name = out_name_sym ? symbol(out_name_sym) : symbol("");

        if (m_gl_asyncread_out_name == symbol("")) {
            cerr << "internal jit.gl.asyncread has no out_name -- matrixoutput may not have taken effect." << endl;
            teardown_gl_asyncread_helper();
            return false;
        }

        m_gl_poll_next_fire = std::chrono::steady_clock::now();
        schedule_next_gl_poll();
        return true;
    }

    // Schedules m_gl_poll_timer against a fixed wall-clock cadence (see the
    // comment on m_gl_poll_timer for why) rather than a flat delay from
    // "now". If we've fallen behind (our own work took longer than one
    // period), fire again as soon as possible instead of piling up an
    // ever-growing backlog of delay.
    void schedule_next_gl_poll() {
        auto period = std::chrono::duration<double, std::milli>(1000.0 / std::max(1.0, m_frame_rate_fps));
        m_gl_poll_next_fire += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);

        auto now = std::chrono::steady_clock::now();
        if (m_gl_poll_next_fire < now)
            m_gl_poll_next_fire = now;

        double delay_ms = std::chrono::duration<double, std::milli>(m_gl_poll_next_fire - now).count();
        m_gl_poll_timer.delay(delay_ms);
    }

    // Called periodically by m_gl_poll_timer (main/scheduler thread) while
    // the helper is alive. Read-only -- @automatic 1 (see
    // ensure_gl_asyncread_helper()) is what actually drives capture, tied
    // to the real Jitter render pass; we just pick up whatever its latest
    // result is, at whatever rate suits our own output (the DeckLink's
    // active display mode's frame rate), independent of the GL source's
    // own render rate.
    void poll_gl_asyncread_frame() {
        if (!m_gl_asyncread_box || m_gl_asyncread_out_name == symbol(""))
            return;

        bool used = false;
        void* matrix = c74::max::jit_object_findregistered(m_gl_asyncread_out_name);
        if (matrix && c74::max::jit_object_method(matrix, c74::max::_jit_sym_class_jit_matrix)) {
            long savelock = (long)c74::max::jit_object_method(matrix, c74::max::_jit_sym_lock, 1);

            c74::max::t_jit_matrix_info info{};
            char* bp = nullptr;
            c74::max::jit_object_method(matrix, c74::max::_jit_sym_getinfo, &info);
            c74::max::jit_object_method(matrix, c74::max::_jit_sym_getdata, &bp);

            if (bp && info.type == c74::max::_jit_sym_char && info.dimcount == 2
                && (info.planecount == 3 || info.planecount == 4)) {
                copy_matrix_to_decklink(info, (unsigned char*)bp);
                used = true;
            }

            c74::max::jit_object_method(matrix, c74::max::_jit_sym_lock, (void*)savelock);
        }

        // Diagnostic: "successful" means the poll found a usable matrix
        // registered under out_name and forwarded it. A tick rate much
        // lower than the expected display frame rate would mean the timer
        // itself isn't being rearmed/serviced promptly; a tick rate near
        // expected but a low success rate would point at out_name / the
        // helper's own capture instead.
        if (perfstats) {
            m_gl_poll_rate.tick(used);
            std::string report = m_gl_poll_rate.maybe_report("gl poll ticks (successful = usable frame forwarded)");
            if (!report.empty())
                cout << report << endl;
        }
    }

    void teardown_gl_asyncread_helper() {
        if (m_gl_asyncread_box) {
            c74::max::object_free(m_gl_asyncread_box);
            m_gl_asyncread_box = nullptr;
        }
        m_gl_asyncread_texture = symbol("");
        m_gl_asyncread_out_name = symbol("");
    }

    void cleanup_decklink_locked() {
        m_audio_enabled.store(false, std::memory_order_relaxed);
        if (m_deck_link_output) {
            if (m_video_enabled)
                m_deck_link_output->DisableVideoOutput();
            m_deck_link_output->DisableAudioOutput();
            m_deck_link_output->Release();
            m_deck_link_output = nullptr;
        }
        if (m_deck_link) {
            m_deck_link->Release();
            m_deck_link = nullptr;
        }
        m_video_enabled = false;
        m_ring.clear();
        m_last_video_dims_warning.clear();
        m_warned_no_device = false;
    }

    void initialize_device(int index) {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        cleanup_decklink_locked();

        IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
        if (!iterator) {
            cerr << "no Blackmagic DeckLink driver found (install Desktop Video)." << endl;
            return;
        }

        IDeckLink* found = nullptr;
        IDeckLink* deck_link = nullptr;
        int count = 0;
        while (iterator->Next(&deck_link) == S_OK) {
            if (count == index)
                found = deck_link;
            else
                deck_link->Release();
            ++count;
        }
        iterator->Release();

        if (!found) {
            cerr << "no DeckLink device at index " << index << " (found " << count
                 << " device(s); send 'scan' to refresh)." << endl;
            return;
        }
        m_deck_link = found;
        m_device_name = get_device_name(m_deck_link);

#if defined(__APPLE__)
        HRESULT qi = m_deck_link->QueryInterface(IID_IDeckLinkOutput, (void**)&m_deck_link_output);
#else
        HRESULT qi = m_deck_link->QueryInterface(&IID_IDeckLinkOutput, (void**)&m_deck_link_output);
#endif
        if (qi != S_OK || !m_deck_link_output) {
            cerr << "'" << m_device_name << "' has no output capability." << endl;
            m_deck_link->Release();
            m_deck_link = nullptr;
            return;
        }

        cout << "using '" << m_device_name << "' (index " << index << ")." << endl;

        setup_video_output_locked(displaymode);
        setup_audio_output_locked(audiochannels, audiobits);
    }

    // Caller must hold m_device_mutex. `wanted_mode` is passed explicitly
    // (rather than read from the `displaymode` attribute) so this can be
    // called from that attribute's own setter before the new value has
    // been committed to the attribute.
    void setup_video_output_locked(symbol wanted_mode) {
        m_video_enabled = false;
        if (!m_deck_link_output)
            return;

        IDeckLinkDisplayModeIterator* it = nullptr;
        if (m_deck_link_output->GetDisplayModeIterator(&it) != S_OK || !it) {
            cerr << "could not enumerate display modes." << endl;
            return;
        }

        std::string wanted = normalize_name(wanted_mode.c_str());
        IDeckLinkDisplayMode* chosen = nullptr;
        IDeckLinkDisplayMode* fallback = nullptr;
        IDeckLinkDisplayMode* mode = nullptr;

        while (it->Next(&mode) == S_OK) {
            if (!chosen && normalize_name(get_display_mode_name(mode)) == wanted) {
                chosen = mode;
                continue;
            }
            if (!fallback) {
                fallback = mode;
                continue;
            }
            mode->Release();
        }
        it->Release();

        IDeckLinkDisplayMode* use_mode = chosen ? chosen : fallback;
        if (!use_mode) {
            cerr << "'" << m_device_name << "' reports no display modes." << endl;
            return;
        }
        if (!chosen) {
            cwarn << "display mode '" << wanted_mode.c_str() << "' not found on '" << m_device_name
                 << "'; using '" << get_display_mode_name(use_mode)
                 << "' instead. Send 'getdisplaymodes' to see valid names." << endl;
        }

        BMDDisplayMode mode_val = use_mode->GetDisplayMode();
        long width = use_mode->GetWidth();
        long height = use_mode->GetHeight();
        std::string mode_name = get_display_mode_name(use_mode);

        int32_t row_bytes = 0;
        m_deck_link_output->RowBytesForPixelFormat(bmdFormat8BitBGRA, (int32_t)width, &row_bytes);

        // Not every display mode supports every pixel format on every
        // device. Check first so a mismatch produces a clear message
        // instead of just an opaque EnableVideoOutput failure.
        bool pixel_format_supported = true;
        BMDDisplayMode actual_mode = mode_val;
        HRESULT support_hr = m_deck_link_output->DoesSupportVideoMode(
            bmdVideoConnectionUnspecified, mode_val, bmdFormat8BitBGRA, bmdNoVideoOutputConversion,
            bmdSupportedVideoModeDefault, &actual_mode, &pixel_format_supported);

        if (support_hr == S_OK && !pixel_format_supported) {
            cerr << "'" << mode_name << "' doesn't support 8-bit BGRA output on '" << m_device_name << "'." << endl;
            return;
        }

        HRESULT enable_hr = m_deck_link_output->EnableVideoOutput(mode_val, bmdVideoOutputFlagDefault);
        if (enable_hr == S_OK) {
            m_frame_width = width;
            m_frame_height = height;
            m_row_bytes = row_bytes;
            m_active_mode_name = mode_name;
            m_video_enabled = true;
            m_last_video_dims_warning.clear();

            BMDTimeValue frame_duration = 0;
            BMDTimeScale time_scale = 0;
            if (use_mode->GetFrameRate(&frame_duration, &time_scale) == S_OK && frame_duration > 0)
                m_frame_rate_fps = (double)time_scale / (double)frame_duration;
            else
                m_frame_rate_fps = 30.0;

            cout << "video output enabled -- " << mode_name << " (" << width << "x" << height << ")." << endl;
        }
        else if (enable_hr == E_ACCESSDENIED) {
            cerr << "EnableVideoOutput failed for '" << mode_name << "' (" << describe_hresult(enable_hr)
                 << ") -- the device's video output is already claimed by someone. If no other app has it open, "
                 << "this is almost always a stale handle from an earlier crashed/relaunched Max process still "
                 << "holding the output enabled at the driver level (quitting a patch doesn't call our cleanup "
                 << "code if Max itself crashed first). Try, in order: (1) fully quit Max (Cmd+Q, not just close "
                 << "the window) and reopen it; (2) check the Blackmagic Desktop Video Setup / Status utility for "
                 << "a 'device busy' indicator; (3) restart the machine, which always clears it." << endl;
        }
        else {
            cerr << "EnableVideoOutput failed for '" << mode_name << "' (" << describe_hresult(enable_hr) << ")." << endl;
        }

        if (chosen)
            chosen->Release();
        if (fallback && fallback != chosen)
            fallback->Release();
    }

    // Caller must hold m_device_mutex. `channels`/`bits` are passed
    // explicitly for the same reason as setup_video_output_locked()'s
    // `wanted_mode` -- see the audiochannels/audiobits attribute setters.
    void setup_audio_output_locked(int channels, int bits) {
        m_audio_enabled.store(false, std::memory_order_relaxed);
        if (!m_deck_link_output)
            return;

        // DeckLink hardware only outputs audio at 48kHz. sys_getsr() can
        // report 0 before any audio driver has ever been selected; treat
        // that as "unknown" rather than blocking setup.
        double sr = c74::max::sys_getsr();
        if (sr != 0.0 && sr != 48000.0) {
            cerr << "DeckLink audio output requires Max's audio driver to run at 48kHz (currently "
                 << sr << " Hz); audio output is disabled. Video is unaffected." << endl;
            return;
        }

        BMDAudioSampleType sample_type = (bits == 32) ? bmdAudioSampleType32bitInteger : bmdAudioSampleType16bitInteger;
        if (m_deck_link_output->EnableAudioOutput(bmdAudioSampleRate48kHz, sample_type, (uint32_t)channels,
                                                   bmdAudioOutputStreamContinuous) != S_OK) {
            cerr << "could not enable " << channels << "-channel audio output on '" << m_device_name << "'." << endl;
            return;
        }

        m_ring.reset((size_t)channels, 48000); // ~1 second of buffering headroom
        m_audio_bits_active = bits;
        m_audio_enabled.store(true, std::memory_order_relaxed);
        cout << "audio output enabled (" << channels << " ch, " << bits << "-bit, 48kHz)." << endl;
    }

    // Diagnostic: reports how many frames per second are actually reaching
    // DisplayVideoFrameSync (vs. attempted, e.g. dropped for a dims
    // mismatch) -- regardless of source (jit_matrix or GL). Added to make
    // throughput problems directly measurable instead of guessed at.
    void note_video_frame_result(bool success) {
        if (!perfstats)
            return;
        m_video_frame_rate.tick(success);
        std::string report = m_video_frame_rate.maybe_report("video frames (successful = sent to DisplayVideoFrameSync)");
        if (!report.empty())
            cout << report << endl;
    }

    void copy_matrix_to_decklink(const c74::max::t_jit_matrix_info& info, unsigned char* data) {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (!m_deck_link_output || !m_video_enabled) {
            warn_once(m_warned_no_device, cerr, "no DeckLink device/display mode is active; send 'scan' then select a device.");
            note_video_frame_result(false);
            return;
        }

        long width = info.dim[0];
        long height = info.dim[1];

        if (width != m_frame_width || height != m_frame_height) {
            std::string msg = "incoming matrix is " + std::to_string(width) + "x" + std::to_string(height)
                + " but the active display mode (" + m_active_mode_name + ") is " + std::to_string(m_frame_width) + "x"
                + std::to_string(m_frame_height) + "; conform it upstream, e.g. [jit.matrix " + std::to_string(m_frame_width)
                + " " + std::to_string(m_frame_height) + "].";
            warn_throttled(m_last_video_dims_warning, cerr, msg);
            note_video_frame_result(false);
            return;
        }

        IDeckLinkMutableVideoFrame* frame = nullptr;
        if (m_deck_link_output->CreateVideoFrame((int32_t)m_frame_width, (int32_t)m_frame_height, m_row_bytes,
                                                   bmdFormat8BitBGRA, bmdFrameFlagDefault, &frame) != S_OK
            || !frame) {
            cerr << "CreateVideoFrame failed." << endl;
            note_video_frame_result(false);
            return;
        }

        // Newer DeckLink SDKs (12.7+) split frame pixel storage out into a
        // separate IDeckLinkVideoBuffer, obtained via QueryInterface on the
        // frame CreateVideoFrame() just returned -- IDeckLinkMutableVideoFrame
        // itself no longer exposes GetBytes() directly.
        IDeckLinkVideoBuffer* buffer = nullptr;
        if (frame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&buffer) == S_OK && buffer) {
            if (buffer->StartAccess(bmdBufferAccessWrite) == S_OK) {
                void* bytes = nullptr;
                if (buffer->GetBytes(&bytes) == S_OK && bytes) {
                    auto* dst_base = (uint8_t*)bytes;
                    long planecount = info.planecount;
                    long src_stride = info.dimstride[1];

                    for (long y = 0; y < height; ++y) {
                        const uint8_t* src_row = data + y * src_stride;
                        uint8_t* dst_row = dst_base + y * m_row_bytes;
                        for (long x = 0; x < width; ++x) {
                            const uint8_t* s = src_row + x * planecount;
                            uint8_t* d = dst_row + x * 4;
                            if (planecount == 4) {
                                // Jitter's 4-plane char matrices are ARGB
                                // (plane 0 = alpha). DeckLink's 8-bit BGRA
                                // wants bytes in B,G,R,A order.
                                d[0] = s[3]; // B
                                d[1] = s[2]; // G
                                d[2] = s[1]; // R
                                d[3] = s[0]; // A
                            }
                            else {
                                // 3-plane char matrices are RGB; output is opaque.
                                d[0] = s[2]; // B
                                d[1] = s[1]; // G
                                d[2] = s[0]; // R
                                d[3] = 255;  // A
                            }
                        }
                    }
                }
                buffer->EndAccess(bmdBufferAccessWrite);
            }
            buffer->Release();
        }

        m_deck_link_output->DisplayVideoFrameSync(frame);
        frame->Release();
        note_video_frame_result(true);
    }

    // Runs on Max's scheduler thread via m_audio_flush_timer, never on the
    // real-time audio thread.
    void flush_audio_to_decklink() {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (!m_deck_link_output || !m_audio_enabled.load(std::memory_order_relaxed))
            return;

        size_t channels = m_ring.channels();
        if (channels == 0)
            return;

        const size_t max_frames_per_flush = 4800; // ~100ms @ 48kHz
        thread_local std::vector<float> scratch_f;
        scratch_f.resize(max_frames_per_flush * channels);
        size_t got = m_ring.pop_frames(scratch_f.data(), max_frames_per_flush);

        if (got > 0) {
            uint32_t written = 0;
            if (m_audio_bits_active == 32) {
                thread_local std::vector<int32_t> scratch_i;
                scratch_i.resize(got * channels);
                for (size_t i = 0; i < got * channels; ++i) {
                    float s = std::max(-1.0f, std::min(1.0f, scratch_f[i]));
                    scratch_i[i] = (int32_t)(s * 2147483647.0f);
                }
                m_deck_link_output->WriteAudioSamplesSync(scratch_i.data(), (uint32_t)got, &written);
            }
            else {
                thread_local std::vector<int16_t> scratch_i;
                scratch_i.resize(got * channels);
                for (size_t i = 0; i < got * channels; ++i) {
                    float s = std::max(-1.0f, std::min(1.0f, scratch_f[i]));
                    scratch_i[i] = (int16_t)(s * 32767.0f);
                }
                m_deck_link_output->WriteAudioSamplesSync(scratch_i.data(), (uint32_t)got, &written);
            }
        }

        long overflows = m_ring.take_overflow_count();
        if (overflows > 0) {
            cwarn << "audio ring buffer overflowed (" << overflows
                 << " frames dropped); the DeckLink audio drain is falling behind." << endl;
        }
    }
};

MIN_EXTERNAL(jit_decklink_send_tilde);
