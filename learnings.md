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

**Rotation for sideways-mounted displays happens entirely in the browser, never on the
device.** Both firmwares only ever receive and render a plain landscape image - a
sideways physical mount is handled by rotating the captured content in
`stream.html` before it's sent, via the standard canvas
`translate`+`rotate`+`drawImage` trick (draw into a virtual swapped-dimension area,
then rotate that into the real landscape output size). This keeps both device-side
render paths exactly as they were - no new firmware code, no new wire format, nothing
to break the render-timing work above. The one place this wasn't free: webH264 normally
hands a captured `VideoFrame` straight from `MediaStreamTrackProcessor` to
`VideoEncoder.encode()` with no canvas step at all, specifically because that path's
throughput is already tight (see the whole "H.264 decode performance" section above).
Rotation needs pixels to go through a canvas, and `VideoEncoder.encode()` only accepts
a `VideoFrame`, so a non-zero rotation reconstructs one from the rotated canvas
(`new VideoFrame(canvas, { timestamp: original.timestamp })`) before encoding - real
extra cost, but only paid when rotation is actually in use; the default (0°) path is
untouched.

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

Video comparing H.264 vs webJPEG



https://github.com/user-attachments/assets/b82c2d7a-fd30-4112-9a69-beb496bad723



**Resolution dominates, not the decoder.** A YouTube demo of the same chip and decoder
running much faster turned out to be decoding at 160x128 versus this example's
600x450 - about 13x fewer pixels, which lines up almost exactly with the measured
decode time difference. Software H.264 decode cost scales with pixel/macroblock count;
there's no hardware decode block on ESP32-S3 (only ESP32-P4 has one), so at a given
resolution the decode time is close to fixed regardless of encoder tuning.

Confirmed with real hardware, not just estimation: the T4-S3 (600x450) and a 1.91"
board (536x240, streaming a mostly-static YouTube animation) measured 215ms and 105ms
average decode time respectively - a 2.04x difference, against a 2.10x difference in
pixel count. The two panels' `pushColors()` display-transfer time scaled less cleanly
(3.19x for 2.10x the pixels), which is more likely differences in the two panels'
interface overhead (RM690B0 vs RM67162) than anything this app controls. See
`examples/webH264/README.md`'s "Measured performance" for the full numbers. (Also
surfaced in passing: `webH264.cpp` explicitly sets `setRotation(0)`, which reported the
1.91" panel as 536x240 landscape - the opposite orientation from the 240x536 portrait
listed as its native resolution. `webJPEG` doesn't set rotation, so the two examples
may report different orientations for the same physical board.)

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

**webJPEG's cheap decode doesn't mean a faster overall pipeline.** On the same 1.91"
board, webJPEG measured slower end-to-end than webH264 (~5.4fps vs ~8.2fps) despite JPEG
decode itself being roughly 65x cheaper than H.264 decode (~1.6ms vs ~105ms) - the
opposite of what "JPEG decode is cheap" would suggest on its own. The actual bottleneck
turned out to be how `webJPEG.cpp` pushes pixels to the display: when the incoming
frame matches the display size exactly, it pushes each decoded 16x16 MCU block to the
display individually as the JPEG decoder produces it (510 separate `pushColors()` calls
for a 536x240 frame, ~360us each) rather than assembling a full frame buffer and
pushing once, which is what webH264 always does. Two lessons here: first, decode time
alone doesn't tell you the whole story - measure the full pipeline, not just the part
you assumed was the bottleneck. Second, webJPEG also has no flow control equivalent to
webH264's ack-based pacing (see above); it just drops fully-received frames it doesn't
have time to render (measured: ~70% of received frames dropped in the same session),
wasting browser-side encode and network work for nothing. webH264's ack protocol exists
specifically to avoid that waste.

There's also a structural reason the gap should widen further in webH264's favor on
more realistic (non-flat, detailed) content than this repo has been measured against so
far: webJPEG decodes every frame independently, so a detailed frame costs the same
whether it's the first frame or the ten-thousandth - nothing carries over between
frames. H.264 can reference the previous frame, so once a complex scene has been sent
once (as a keyframe), unchanged regions in subsequent frames decode as cheap "skip"
macroblocks - cost tracks how much *changed*, not how detailed the picture is. This is
the same "skip macroblock" mechanism behind the bitrate-tuning findings above, just
framed as a comparison to webJPEG's lack of any equivalent.

**The "line by line" render effect and the render bottleneck were the same bug.**
webJPEG used to have two special-cased render paths in `drawJPEG()`/`drawMonoJPEG()`:
when the incoming JPEG matched the display resolution exactly, each decoded 16x16 MCU
block was pushed straight to the display as it came off the decoder; otherwise, frames
went through a `TFT_eSprite` buffer and were pushed once. The direct-render path looked
like an optimization (skip the buffer, skip a copy) but was actually the opposite: 510
separate `pushColors()` calls for a 536x240 frame, at ~360us each, versus one bulk push
for the whole frame - see "webJPEG's cheap decode doesn't mean a faster overall
pipeline" above. It also visibly rendered top-to-bottom as blocks landed on screen one
at a time, which is what prompted the question that led here. The fix removed the
special case entirely: every frame now renders into the sprite and reaches the display
in one push, matching webH264's approach, which never had this problem because
`esp_h264`'s decoder only hands back whole frames anyway - it never had the *option* to
push partial blocks. Lesson: a path that avoids an extra buffer copy isn't necessarily
faster if it trades one bulk transfer for many small ones; measure the actual
display-bus cost, not just the memory-copy cost you're trying to avoid.

**Re-measured on real hardware: the drop rate improved a lot, the frame rate barely
did.** Same 1.91" board, same content: render time went from 183ms to 156.6ms average
(~5.4fps to ~5.9fps), while the frame-drop rate roughly halved (~70% to ~32% of
received frames dropped). That gap between "small time improvement" and "big drop-rate
improvement" makes sense once you separate the two: dropped frames happen when a new
frame arrives *while* the device is still rendering the previous one, so shaving even
~15% off render time meaningfully shrinks that window. But the *total* render time
barely moved, which means the original 183ms was never really "510 small SPI/QSPI
bus transactions are slow" - swapping those for 510 `spr.pushImage()` calls into RAM
(same call count, cheaper destination) recovered almost none of it. The real cost is
in making 510 separate calls at all: each one repeats bounds-checking and byte-swap
logic that would only need to happen once per frame if the MCU blocks were assembled
into the sprite's memory directly instead of going through the sprite's public
per-block API 510 times. Lesson: when a "fix" doesn't produce the improvement you
expected, don't assume it failed - check whether you fixed a different problem than
the one you thought you were fixing (here: the visible line-by-line artifact and the
drop rate, not raw decode throughput).

**A sustained-streaming crash surfaced that the render-path fix didn't cause or fix.**
After a period of successful streaming, real hardware testing hit a burst of
`JPEG decode failed!` errors, then a `Malloc failed for 0 bytes!`, then a `task_wdt`
abort on the `async_tcp` FreeRTOS task and a reboot. The shape of it - failures that
start clean and cascade, ending in a zero-size allocation failure - pointed at
internal-heap fragmentation: `webJPEG.cpp`'s WebSocket handler used to `malloc()` a
freshly-sized `wsAssemblyBuffer` for every incoming frame and `free()` it right after,
and different-sized allocations at a sustained rate are a classic way to fragment a
heap even when nothing is technically leaked. `webH264.cpp` already avoided this
(`wsAssemblyBuf`, allocated once at boot from PSRAM, reused for every message - see
its comment for the same reasoning), which is also why it never showed this failure
mode in testing despite doing conceptually the same job. Fixed by porting the same
pattern to webJPEG: `wsAssemblyBuffer`, `wsMonoAssemblyBuffer` and `frameBuffer` are
now fixed-size (`MAX_FRAME_SIZE`, 512KB) PSRAM buffers allocated once in `setup()`,
never freed; oversized frames are dropped (checked against `info->len` before copying
anything) instead of risking an allocation. The one behavior change this requires:
`frameBuffer` used to change *which* allocation it pointed to every frame (ownership
transferred from the WebSocket handler); now it's a fixed address that gets `memcpy`'d
into under the same mutex, which costs a small fixed copy (tens of KB) in exchange for
removing the allocation entirely. Not yet re-verified against a real long streaming
session - the crash was intermittent under sustained load, so absence of a repeat in
a short test wouldn't be strong evidence either way.

**Per-frame logs need an absolute timestamp, not just relative deltas.** Every
`Timing:`/`Frame #N:` line already broke down its own frame into Decode/Render/Push/etc,
but nothing tied that line to wall-clock time - so a serial log full of clean-looking
170ms frames gives no way to tell whether they arrived back-to-back or with multi-second
gaps between them (a stall, a WiFi retry, the runup to the crash above). Reading a
one-shot lastFrameTime figure or a burst of decode failures in isolation looked like
"streaming is working fine, then instantly broke" - the missing dimension was how much
real time separated any two lines. Both examples now prefix every diagnostic line with
device uptime via a `LOGF`/`LOGLN` macro wrapping `Serial.printf`/`millis()` (see
`webJPEG.cpp` and `webH264.cpp`) - cheap (`millis()` is a single counter read) and
applied only to lines that already stood alone (frame timings, connect/disconnect,
drops, fatal errors), not the multi-`Serial.print()` WiFi-connect screen, where a
timestamp would land mid-sentence.

**Once timestamped, webJPEG's real-world frame rate turned out much lower than the
per-frame timings suggested - and the gap was invisible to the device's own
instrumentation.** Real logs at both 5fps and 30fps targets showed frequent 400ms-1.25s
silences between rendered frames, even though each frame's own Decode/Render/Push add
up to only ~150ms - well under budget. The device is idle, not slow, during those
gaps, which is exactly why `Timing:` never caught them: it only starts its clock once
`drawJPEG()`/`drawMonoJPEG()` is called with a complete frame already in hand. Whatever
happens before that (network transit, WebSocket reassembly, or the device simply not
noticing yet) was completely unmeasured.

Leading suspect: `drawJPEG()`'s MCU-copy loop ran ~500 `spr.pushImage()` calls back to
back with no yield, holding `frameMutex` (and the CPU) for the entire ~150ms. AsyncTCP's
own task has no fixed core affinity by default, so it can share a core with `loopTask` -
and a core held solid for 150ms can delay that task long enough to delay TCP ACK
generation. A sender not getting timely ACKs looks exactly like packet loss to a TCP
stack, triggering retransmission backoff - and TCP's minimum RTO (commonly ~200ms,
doubling per retry: 200/400/800/1600ms) lines up closely with the observed gap sizes.
Two changes test and mitigate this without evidence of a different root cause yet:
1. `Timing:`/`Mono Timing:` now include a `Recv=` figure - wall-clock time from the
   first WebSocket fragment of a frame (`info->index==0`, timestamped in the
   `WS_EVT_DATA` handler) to when `drawJPEG()` actually started decoding it. This
   number, not `Total=`, is what will confirm or rule out the CPU-starvation theory:
   if `Recv=` is what balloons during a stall, the delay is upstream of the device
   even noticing a new frame; if `Total=`'s other fields balloon instead, it's
   something in the render path itself.
2. Both MCU-copy loops now call `taskYIELD()` every 64 blocks - lets the scheduler run
   other ready tasks (like AsyncTCP's) without the fixed-delay cost of `delay()`/
   `vTaskDelay()`, which would add real latency 8 times a frame for no benefit if the
   scheduler had nothing else to do.

Not yet confirmed on real hardware - this is a plausible, testable hypothesis backed by
how the ESP32's task scheduling and TCP's RTO backoff both work, not a verified root
cause. The next real log will show whether `Recv=` (not `Total=`) is where the gap time
actually lives, and whether the periodic yield changed anything.

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
