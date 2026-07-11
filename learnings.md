# Learnings

Notes on non-obvious decisions in this repo and what led to them. Written so the
reasoning survives even after the code around it changes.

## PlatformIO project structure

**One shared `src_dir`, not `PLATFORMIO_SRC_DIR` per example.** The repo originally
picked which example to build by setting the `PLATFORMIO_SRC_DIR` environment variable
before calling `pio run`. That only worked when something remembered to set it - CI's
workflow script did, but a plain `pio run -e webH264` from a terminal or from VSCode's
PlatformIO sidebar didn't, silently falling back to the ini's default `src_dir` and
building the wrong example's code under the `webH264` environment. The fix was a single
`src_dir = examples` shared by both environments, with `build_src_filter` scoping
`webJPEG` and an explicit `examples/CMakeLists.txt` scoping `webH264` (see next point).
Lesson: anything that depends on an environment variable being set externally will
eventually run in a context that doesn't set it.

**`build_src_filter` does nothing under the ESP-IDF/CMake builder.** `webH264` builds
Arduino as an ESP-IDF component (`framework = arduino, espidf`) because `esp_h264`
ships as a prebuilt IDF component the plain Arduino/SCons builder can't consume. That
builder uses CMake's own source discovery, which needs every file listed explicitly in
`examples/CMakeLists.txt` - `build_src_filter` is a SCons concept and is silently
ignored. This is also why `webJPEG.ino` had to become `webJPEG.cpp`: PlatformIO's
`.ino`-to-`.cpp` conversion only scans the top level of `src_dir`, not subdirectories,
so once both examples shared one `src_dir`, the `.ino` file nested a level down was
invisible to it - not a CMake issue at all, just a second consequence of the same
restructuring.

**`board_build.partitions` in `platformio.ini`, not just `sdkconfig.defaults`.**
`webH264` needs a large app partition (esp_h264 + WiFi + display libs don't fit in the
board's default). `sdkconfig.defaults` sets `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`
correctly, but PlatformIO's own espressif32 platform independently derives a partition
table from `board_build.partitions` for its own flashing-offset bookkeeping - and that
one wins if the two disagree. Without also setting it in `platformio.ini`, the build
silently used a 1MB app partition instead of the intended 6MB one; flash usage looked
like it was at 99% instead of the true 16%. Caught by chance while checking flash usage
after adding code, not by any error - a wrong partition table doesn't fail the build,
it just quietly caps how much room you have.

**Two example projects sharing `src_dir` means a library can silently duplicate.**
`webH264`'s `platformio.ini` used to list `xinyuan-lilygo/LilyGo-AMOLED-Series` (the
unmodified upstream package) in `lib_deps`, alongside this repo's own modified fork at
`src/`. Under the ESP-IDF builder, each `lib_deps` entry becomes its own component, so
both the upstream package and the local fork got compiled - and the linker picked
whichever one it resolved first, which turned out to be the *upstream* one. A local fix
to `src/LilyGo_AMOLED.cpp` (see below) silently never took effect for `webH264` because
of this. Fixed by compiling `src/` explicitly via `examples/CMakeLists.txt` and
removing the registry package from `lib_deps` entirely. Lesson: if the same class can
be sourced two ways, prefer breaking silently in "used the wrong one" rather than
"duplicate symbol error" - the former is much harder to notice.

## Firmware behavior

**`beginAMOLED_241()`'s default panics without an SD card.** The T4-S3 board's
auto-detecting `begin()` used to call `beginAMOLED_241()` with its default
`disable_sd=false`, which tries to mount an SD card unconditionally; with none
inserted, the failure path hits an uninitialized FreeRTOS queue and panics (boot loop).
`webH264` originally worked around this by skipping auto-detection entirely and calling
`beginAMOLED_241(true)` directly - which meant it only ever worked on the T4-S3, not
the other boards. The actual fix was in the shared library: `begin()`'s T4-S3 branch
now passes `disable_sd=true`. This also fixed the same latent bug in `webJPEG`, which
had been calling plain `begin()` all along and would have hit it too on a T4-S3 with no
SD card - nobody had filed that issue yet.

**One `/boardinfo` endpoint, one shared setup flow, one shared streaming page.**
`webJPEG` and `webH264` used to have separate streaming pages
(`webrtc_stream.html`/`h264_stream.html`) and different on-device WiFi connect screens.
Consolidating them into `examples/stream.html` (which auto-detects which firmware it's
talking to via a `variant` field in `/boardinfo`) and porting `webJPEG`'s WiFi
spinner/QR-code-on-failure flow onto `webH264` meant both examples now behave
identically up to the point where streaming starts - one thing to get right instead of
two, and one thing to test.

## Debugging the flasher

The browser flasher stopped letting users set SSID/password after `webH264` was added.
Root cause: `flasher-manifest.yml`'s `examples:` key still said `T-Display-AMOLED`,
which had been the PlatformIO environment name before an earlier rename to `webJPEG`.
The packaging script matches manifest entries to built firmware by exact environment
name; the mismatch meant the lookup silently returned nothing, so the firmware got
packaged with no `variables` (the SSID/password/mDNS fields) at all - no error, just a
flasher card with nothing to fill in. Renaming things has a way of leaving stale string
references in config files that don't get type-checked against anything.

## H.264 decode performance

**Resolution dominates, not the decoder.** A YouTube demo of the same chip and decoder
running much faster turned out to be decoding at 160x128 versus this example's
600x450 - about 13x fewer pixels, which lines up almost exactly with the measured
decode time difference. Software H.264 decode cost scales with pixel/macroblock count;
there's no hardware decode block on ESP32-S3 (only ESP32-P4 has one), so at a given
resolution the decode time is close to fixed regardless of encoder tuning.

**Bitrate and keyframes matter too, just less than resolution.** Decode cost also
scales with how much residual data there is per frame - this example's own per-frame
timing log showed a 1038-byte frame decode in 188ms versus 280ms for a 6392-byte frame
at the same resolution. Keyframes are the expensive case within that: every macroblock
is intra-predicted from scratch, with no equivalent of an inter-frame's cheap "skip
macroblock" (copy unchanged regions straight from the reference frame) available. For
screen content specifically - large static areas, small changing regions - lowering
bitrate and switching to variable bitrate mode both let the encoder actually take
advantage of that structure instead of spending a steady bit (and decode) budget
regardless of content. See `examples/webH264/README.md`'s "Tuning bitrate for screen
content" for the practical guidance this produced.

**Limited-range vs. full-range YUV is a classic, easy-to-miss bug.** Streamed video had
washed-out grays instead of true blacks, while on-device status text (drawn directly in
RGB, no YUV involved) looked fine - a strong hint the bug was in the YUV->RGB
conversion, not the display path. H.264 conventionally encodes luma/chroma in
limited/studio range (Y: 16-235) rather than full range (0-255) - unlike JPEG, which
genuinely is full-range by specification, so the same conversion-matrix code that's
correct for the JPEG decoder was wrong here. Confirmed by adding a debug print of the
decoded top-left pixel's color: it read back as `(16,16,16)` for content that should
have been pure black - exactly what you'd get from feeding limited-range `Y=16` through
a conversion that assumes full range. The fix was a small rescale before the existing
(already-correct) conversion matrix, not a different matrix.

## General

**Comments should describe the code as it is, not the story of how it got there.**
Comments accumulated during heavy debugging tend to narrate the journey ("this used to
break because...", "confirmed via the debug log added earlier...") - useful in the
moment, stale the moment the next change lands. Worth a periodic pass to convert
"why we changed this" into "why this is the way it is," or delete it if the reason no
longer applies.

**A stale comment is worse than no comment.** Several bugs above were adjacent to
comments that were still confidently describing behavior that had already changed
(a function name that was renamed, a dependency that was removed, a workaround for a
bug fixed elsewhere). None of these caused the bugs directly, but they made the code
harder to trust while debugging something else nearby.
