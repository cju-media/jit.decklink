# jit.decklink.send~

A Max/MSP external that sends Jitter video matrices and multichannel (MC) MSP
audio to a Blackmagic Design DeckLink device (e.g. UltraStudio 3G, DeckLink
Duo, etc.).

Named with a `.send` segment because it's the output half of a planned pair:
a companion **`jit.decklink.receive~`** (input, for capture devices like the
UltraStudio 3G *Recorder*) is intended for later, not built yet.

## Features

* **Device discovery**: `scan` enumerates connected DeckLink devices and
  populates a connected `umenu` with their names.
* **Device selection**: pick a device by index, either by wiring a `umenu`
  straight back into the left inlet, or with the `device` attribute/message.
* **Video output**: send a `jit_matrix` (char, 3- or 4-plane) to the left
  inlet; it's converted to the DeckLink's active display mode and displayed.
* **Configurable display mode**: `displaymode` selects the output timing
  (e.g. `1080p2997`, `1080p25`, `720p60`, `NTSC`, `4K2160p2997`...);
  `getdisplaymodes` lists exactly what the selected device supports.
* **MC audio output**: the right inlet is a multichannel signal inlet --
  connect an `mc.*` signal (or any signal) and it's sent to the DeckLink's
  audio output.
* **GL texture input**: connect a `jit.gl.*` object directly to the left
  inlet and it works like any other GL video consumer -- no manual matrix
  conversion needed. See "About textures" below for how.

### About textures

Sending an OpenGL texture (`jit_gl_texture`) doesn't read GPU memory
directly -- Jitter's internal texture struct layout isn't part of the
public Max SDK headers, so there's no documented/stable way to do that from
a third-party external. Instead, on first use this object lazily creates
and hides its own internal `[jit.gl.asyncread]` (the same object you'd
patch in by hand: `@matrixoutput 1`), points it at the incoming texture,
and reads the matrix it produces back on a poll timer matched to the
active display mode's frame rate. This reuses Cycling '74's own tested
GPU->CPU readback rather than reimplementing it, at the cost of one frame
or so of extra latency versus a hand-rolled synchronous readback.

A few things worth knowing:

* If you switch from a GL texture source to sending `jit_matrix` directly,
  the internal helper is torn down automatically (both sources writing to
  the output at once causes visible flashing). To stop GL capture without
  sending a matrix, send `gl_disconnect`.
* The helper is created in, and only works from within, this object's own
  patcher (it needs an owning patcher to instantiate into). This is only a
  problem for the unusual case of a `jit.decklink.send~` with no patcher context
  at all.
* Only char/RGB-or-ARGB readback matrices are used, same as `jit_matrix`
  (see below) -- the dimensions must match the active display mode.

## Prerequisites

* **Cycling '74 Max SDK / Min-API**: vendored in `min-api/` (includes
  `max-sdk-base`).
* **Blackmagic DeckLink SDK headers**: vendored in `include/` (the minimal
  set actually needed: `DeckLinkAPI.h` and friends, plus the Mac/Linux
  `DeckLinkAPIDispatch.cpp` shim, which resolves the installed driver's
  entry points dynamically at runtime -- no DeckLink `.dylib`/`.framework`
  is required at build time, only Blackmagic's **Desktop Video** driver
  installed on the machine that actually runs the external).

## Build

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

The resulting external is written to `build/externals/jit.decklink.send~.mxo`.
Copy it to your Max package/externals folder to use it.

### Troubleshooting: codesign fails under iCloud Drive

If your checkout lives inside an iCloud-synced folder (e.g. under
`~/Documents` with "Desktop & Documents Folders" sync enabled), the build's
ad-hoc code-signing step can fail with an error like:

```
resource fork, Finder information, or similar detritus not allowed
```

This is iCloud tagging new build directories with Finder/file-provider
extended attributes that `codesign` refuses to sign over -- it's not a
project code issue. Work around it either by building outside the
iCloud-synced folder, or by skipping the sign step:

```bash
cmake -DMAX_SDK_CODESIGN_EXTERNS=OFF ..
```

### Architecture

The build produces a universal (`arm64` + `x86_64`) external by default, so
it loads in Max regardless of whether Max itself is running natively on
Apple Silicon or under Rosetta. (Min-API's own default, if left alone, is
`x86_64` only -- which fails to load with an "incorrect architecture" error
in a native-arm64 Max -- so this project overrides it in `CMakeLists.txt`.)
To build for a single architecture instead:

```bash
cmake -DCMAKE_OSX_ARCHITECTURES=arm64 ..    # or x86_64
```

## Usage

1. Create a `jit.decklink.send~` object in Max.
2. Connect a `umenu` to the right-most (dump) outlet region -- specifically
   the menu outlet -- and send `scan` to populate it with connected
   DeckLink device names.
3. Wire the `umenu`'s output back into the left inlet (it sends an `int`,
   which selects the device by index), or send `device <index>` /
   set `@device <index>`.
4. Optionally set `@displaymode 1080p2997` (or send `getdisplaymodes` first
   to see what the device actually supports).
5. Send a `jit_matrix` (e.g. from `jit.movie`, `jit.grab`) to the left
   inlet, or connect a `jit.gl.*` object directly for GL texture input
   (see "About textures" above).
6. Connect an MC (or regular) signal to the right inlet to send audio.
   DeckLink hardware only outputs audio at 48kHz, so **Max's audio driver
   must be running at 48kHz** -- the object logs a warning and skips audio
   (video still works) if it isn't.

### For best/most consistent frame timing: enable Overdrive

Turning on Max's **Overdrive** (Options menu) is recommended when using
this object, especially for GL texture input. It runs Max's scheduler on a
dedicated high-priority thread instead of sharing time with the main UI
thread, which noticeably reduces jitter in the internal poll timer that
drives GL texture readback (see `perfstats` below if you want to measure
this yourself).

## Attributes

* `device` (int, default 0): index of the DeckLink device to use, as
  listed by `scan`.
* `displaymode` (symbol, default `1080p2997`): output display mode/timing.
  Matching against the device's supported modes is case/punctuation
  -insensitive (`1080p2997` matches `"1080p29.97"`). Falls back to the
  device's first supported mode (with a console warning) if there's no
  match.
* `audiochannels` (int, default 2): number of audio channels to send.
  Must be 2, 8, or 16 (DeckLink hardware's supported channel counts).
* `audiobits` (int, default 16): audio bit depth to send, 16 or 32
  (integer PCM).
* `perfstats` (bool, default off): print periodic (~every 3s) throughput
  measurements to the console -- messages/frames per second at each stage
  of the video pipeline (incoming `jit_gl_texture` messages, GL poll ticks,
  frames actually sent to the DeckLink). Useful for diagnosing frame-rate
  problems; off by default so normal use is quiet.

## Messages

* `scan` / `getdevicelist`: refresh and output the list of connected
  DeckLink devices (out the menu outlet, for a `umenu`).
* `getdisplaymodes`: list the display modes the selected device supports
  (out the dump outlet, as `displaymode <name> <width> <height>`).
* `int`: select a device by index (what a `umenu` sends).
* `jit_matrix`: video frame to output. Must be a 2D char matrix with 3
  (RGB) or 4 (ARGB -- Jitter's standard order) planes, matching the active
  display mode's pixel dimensions (conform it upstream with
  `[jit.matrix <w> <h>]` if not).
* `jit_gl_texture`: GL texture frame to output, from a `jit.gl.*` object
  connected directly to the inlet. See "About textures" above.
* `gl_disconnect`: stop and tear down the internal `jit.gl.asyncread`
  helper (if one exists) without needing to send a `jit_matrix`.

## Known limitations / possible future work

* Video is always output as 8-bit BGRA (`bmdFormat8BitBGRA`); there's no
  YUV/10-bit path yet.
* Frames are pushed synchronously as they arrive (`DisplayVideoFrameSync`),
  matching Blackmagic's documented "synchronous playback" mode for
  interactively-generated content -- there's no `ScheduleVideoFrame`
  -based pacing/lookahead.
* GL texture readback goes through a polling timer against an internal
  `jit.gl.asyncread`, not a hand-rolled synchronous readback -- see "About
  textures" above for why, and the latency tradeoff that comes with it.
* No `jit.decklink.receive~` yet (input from a DeckLink capture device into
  Jitter/MSP) -- planned as a separate external, sharing this one's vendored
  `include/` and `min-api/`. Would need its own `add_library()` target in
  `CMakeLists.txt` alongside this one (see the comment there) rather than
  being folded into this object -- capture and playback are different
  `IDeckLink` interfaces (`IDeckLinkInput` vs `IDeckLinkOutput`) with
  callback-driven (not pull/poll) delivery, different enough to warrant a
  separate object rather than a mode switch on this one.
