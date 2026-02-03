
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "DeckLinkAPI.h"

// Include Min-API after DeckLinkAPI to avoid conflicts with system headers/macros included by Max SDK
#include "c74_min.h"

#include <vector>
#include <string>
#include <cstdlib>

using namespace c74::min;

class jit_decklink : public object<jit_decklink> {
public:
    MIN_DESCRIPTION {"Send Jitter Matrix to Blackmagic DeckLink device"};
    MIN_TAGS {"video, output, decklink"};
    MIN_AUTHOR {"Jules"};
    MIN_RELATED {"jit.gl.videoplane, jit.record"};

    inlet<> m_inlet { this, "(matrix) Input video" };
    outlet<> m_outlet_menu { this, "(list) Menu commands", "umenu" };

    // Optional: Dump outlet for other messages
    outlet<> m_outlet_dump { this, "(anything) Dump outlet" };

    jit_decklink(const atoms& args = {}) {
        if (!args.empty()) {
             // Handle arguments if any
        }
    }

    ~jit_decklink() {
        cleanup_decklink();
    }

    // Attributes
    attribute<int> device_index { this, "device", 0,
        description {"Index of the DeckLink device to use."},
        setter { MIN_FUNCTION {
            if (args.size() > 0) {
                int index = args[0];
                if (index != m_current_device_index) {
                    m_current_device_index = index;
                    initialize_device(m_current_device_index);
                }
            }
            return args;
        }}
    };

    // Messages
    message<> scan { this, "scan", "Scan for available DeckLink devices",
        MIN_FUNCTION {
            m_outlet_menu.send("clear");

            IDeckLinkIterator* deckLinkIterator = CreateDeckLinkIteratorInstance();
            if (!deckLinkIterator) {
                cerr << "jit.decklink: This application requires a DeckLink driver installed." << endl;
                return {};
            }

            IDeckLink* deckLink = nullptr;
            int index = 0;

            while (deckLinkIterator->Next(&deckLink) == S_OK) {
                #if defined(__APPLE__)
                    CFStringRef modelNameCF = NULL;
                    if (deckLink->GetModelName(&modelNameCF) == S_OK) {
                        char buffer[64];
                        // Convert CFString to C string (UTF8)
                        if (CFStringGetCString(modelNameCF, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
                             m_outlet_menu.send("append", std::string(buffer));
                        }
                        CFRelease(modelNameCF);
                    }
                #elif defined(_WIN32)
                    BSTR modelNameBSTR = nullptr;
                    if (deckLink->GetModelName(&modelNameBSTR) == S_OK) {
                        // TODO: Implement BSTR to std::string conversion for Windows
                        // For now we just free it to prevent leaks if it succeeded
                        // SysFreeString(modelNameBSTR); // Requires <oleauto.h>
                        m_outlet_menu.send("append", "DeckLink Device (Windows)");
                    }
                #else
                    // Linux
                    const char* modelNameString = nullptr;
                    if (deckLink->GetModelName(&modelNameString) == S_OK) {
                        std::string name(modelNameString);
                        m_outlet_menu.send("append", name);
                        free((void*)modelNameString);
                    }
                #endif

                deckLink->Release();
                index++;
            }

            deckLinkIterator->Release();
            return {};
        }
    };

    // Respond to 'int' message to select device from umenu (if umenu sends index)
    message<> m_int { this, "int", "Select device by index",
        MIN_FUNCTION {
            device_index = args[0];
            return {};
        }
    };

    message<> jit_matrix { this, "jit_matrix", "Input matrix",
        MIN_FUNCTION {
            symbol mname = args[0];
            void* matrix = c74::max::jit_object_findregistered(mname);

            if (matrix && c74::max::jit_object_method(matrix, c74::max::_jit_sym_class_jit_matrix)) {
                 // Lock the matrix
                 long savelock = (long)c74::max::jit_object_method(matrix, c74::max::_jit_sym_lock, 1);

                 c74::max::t_jit_matrix_info info;
                 char* bp = nullptr;
                 c74::max::jit_object_method(matrix, c74::max::_jit_sym_getinfo, &info);
                 c74::max::jit_object_method(matrix, c74::max::_jit_sym_getdata, &bp);

                 // Type check
                 if (info.type != c74::max::_jit_sym_char) {
                     cerr << "jit.decklink: Only char data type is supported." << endl;
                 } else if (bp && m_deck_link_output) {
                     copy_matrix_to_decklink(info, (unsigned char*)bp);
                 }

                 c74::max::jit_object_method(matrix, c74::max::_jit_sym_lock, (void*)savelock);
            }
            return {};
        }
    };

private:
    IDeckLink* m_deck_link = nullptr;
    IDeckLinkOutput* m_deck_link_output = nullptr;
    int m_current_device_index = -1;
    bool m_playback_started = false;

    void cleanup_decklink() {
        if (m_playback_started && m_deck_link_output) {
            m_deck_link_output->DisableVideoOutput();
            m_playback_started = false;
        }
        if (m_deck_link_output) {
            m_deck_link_output->Release();
            m_deck_link_output = nullptr;
        }
        if (m_deck_link) {
            m_deck_link->Release();
            m_deck_link = nullptr;
        }
    }

    void initialize_device(int index) {
        cleanup_decklink();

        IDeckLinkIterator* deckLinkIterator = CreateDeckLinkIteratorInstance();
        if (!deckLinkIterator) return;

        IDeckLink* deckLink = nullptr;
        int i = 0;
        while (deckLinkIterator->Next(&deckLink) == S_OK) {
            if (i == index) {
                m_deck_link = deckLink;
                // Query Output Interface
                // Mac uses CFPlugIn/CFUUIDBytes for IID, passed by value/ref
                // Windows/Linux use standard COM IID*
                #if defined(__APPLE__)
                    if (m_deck_link->QueryInterface(IID_IDeckLinkOutput, (void**)&m_deck_link_output) != S_OK) {
                #else
                    if (m_deck_link->QueryInterface(&IID_IDeckLinkOutput, (void**)&m_deck_link_output) != S_OK) {
                #endif
                    cerr << "jit.decklink: Could not obtain IDeckLinkOutput interface." << endl;
                    m_deck_link->Release();
                    m_deck_link = nullptr;
                }
                break;
            }
            deckLink->Release();
            i++;
        }
        deckLinkIterator->Release();

        if (m_deck_link_output) {
            // Enable Video Output - Hardcoded to 1080p25 for this example.
            if (m_deck_link_output->EnableVideoOutput(bmdModeHD1080p25, bmdVideoOutputFlagDefault) == S_OK) {
                m_playback_started = true;
            } else {
                 cerr << "jit.decklink: Could not enable video output." << endl;
            }
        }
    }

    void copy_matrix_to_decklink(const c74::max::t_jit_matrix_info& info, unsigned char* data) {
        if (!m_deck_link_output) return;

        long width = info.dim[0];
        long height = info.dim[1];
        long count = width * height;

        // We will create a frame.
        IDeckLinkMutableVideoFrame* frame = nullptr;

        // 1080p25 is 1920x1080.
        int rowBytes = 1920 * 4; // BGRA

        if (m_deck_link_output->CreateVideoFrame(1920, 1080, rowBytes, bmdFormat8BitBGRA, bmdVideoOutputFlagDefault, &frame) == S_OK) {
            void* buffer = nullptr;
            // Explicitly cast to base interface to ensure GetBytes is found
            // (fixes potential ambiguity or lookup issues in some SDK versions/platforms)
            // On some systems (macOS) we might need to query the interface or use standard access.
            // If GetBytes is hidden, we can try to rely on inheritance.
            // NOTE: If static_cast fails (compiler error), it means inheritance is ambiguous or virtual.
            // On macOS, it's often an interface inheriting IUnknown.
            // Let's try direct call first after include re-order.

            frame->GetBytes(&buffer);

            auto* src = data;
            auto* dst = (uint8_t*)buffer;

            // Safety check for dimensions and planes
            // Only support 4 plane char for now
            if (width == 1920 && height == 1080 && info.planecount == 4) {
                 for (long i = 0; i < count; ++i) {
                     // Convert ARGB/RGBA to BGRA
                     // We'll assume RGBA (std jitter)
                     // 0=R, 1=G, 2=B, 3=A

                     // BGRA target
                     dst[0] = src[2]; // B
                     dst[1] = src[1]; // G
                     dst[2] = src[0]; // R
                     dst[3] = src[3]; // A

                     src += 4;
                     dst += 4;
                 }
            }

            m_deck_link_output->DisplayVideoFrameSync(frame);
            frame->Release();
        }
    }
};

MIN_EXTERNAL(jit_decklink);
