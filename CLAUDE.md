# wetplate

A collodion wet plate — blue-sensitive and seconds long — as an FFGL **effect** for
Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows
`.dll`. MIT.

Read `AGENTS.md` before changing the spectral weights, the bucket ring, the curve or
the coating.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/wttest --out /tmp/f.png --size 1920x1080`
  (90 frames of the moving card at a synthetic 60 fps, then the last one)
- Set anything by name: `--set "Plate=2" --set "Exposure=0.375" --set "Buckets=12"`
  (0..1 for sliders, the real integer for Buckets, the element index for options;
  `--set "Take=1"` is one press before the first frame)
- List parameters, kinds, defaults and ranges: `./build/wttest --list`
- Other sources: `--source flat --level 0.5`, `--source white`, `--source black`
- The exact GLSL the plugin compiles: `./build/wttest --dump-shaders DIR`
- Recompute the spectral weights: `python3 tools/spectral_weights.py` (`--print` to see
  them, `--check` to compare with the committed `source/Spectral.h`)
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps) and an optional
  `--script` of `frame Parameter Name value` cues. A slider ramps linearly between
  cues; an option, a boolean and an integer STEP (they hold the last cue at or before
  the frame); an event fires on its cue frame only. A cue naming no parameter is refused
  with exit 2, a partial frame at the end of stdin ends the stream cleanly, a failed
  render or a closed stdout exits 1:
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/wttest --pipe --size 1920x1080 --fps 30 [--script cues.txt] | ffmpeg …`
- Trace the clock and the ring per frame: `WETPLATE_TRACE=1 ./build/wttest …`
  (one line per frame on stderr: now, dt, cell, current slot)

## Verify
- Everything: `tools/verify.sh` (fresh universal build + the weights recompute + glslc
  + the reserved-word grep + every check at 320x180 AND 1280x720 + --pipe + the sweep
  + the bundle, ~4 min on this Mac)
- Pure R, G, B expose in the ratio of the computed weights, each light: `./build/wttest --spectral`
- A moving square's smear is (frames covered) / n: `./build/wttest --ghost`
- The bucketed window is the box mean within one bucket's edge: `./build/wttest --bucket`
- A grey wedge maps through the stated curve: `./build/wttest --curve`
- Density along the drain axis is a + b √s: `./build/wttest --drainage`
- The plate stops changing exactly Exposure after the Take: `./build/wttest --take`
- A resize mid-exposure carries the exposure across: `./build/wttest --resize`
- The checks can fail: `./build/wttest --negative`; one perturbation verbosely:
  `./build/wttest --perturb BITS --ghost` (bits in `Model.h`)
- Every check takes `--size WxH`; CI runs them at 320x180
- No name over 16 characters, none duplicated: `./build/wttest --names`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost and the state held: `./build/wttest --bench` (best of three; the GPU here is shared)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Wetplate.bundle`

## Notes
- **The plate is a process, not a grade.** Exposure through Smits' spectral rule with
  the plate's weights → a ring of bucket sums → the window's mean H → the
  characteristic curve on a coating of thickness h → silver over black (or a negative
  on light). `Model.h` and `Spectral.h` hold the numbers; the `kModel` GLSL library in
  `Shaders.cpp` holds the arithmetic. A wrong curve is a GLSL fix.
- **The weights are generated.** `source/Spectral.h` is written by
  `tools/spectral_weights.py` from a digitised sensitivity curve and Smits' table; edit
  the script, never the header. `verify.sh` refuses a header that does not recompute.
- **Time is reduced in double on the CPU.** Which bucket a frame goes in, what a frame
  is worth, whether a take is over: all decided here. Nothing absolute crosses into
  GLSL; the shaders see a per-frame `Dt` and per-bucket weights.
- **The buckets survive a resize by being resampled, not reallocated.** A reallocated
  `PassBuffer` is a cleared one. `--resize` checks it.
- **Exposure at a multiple of 1/8 is a power of two of seconds**, exactly. A window of
  0.50000002 s put the bucket grid a frame late by the sixth bucket; the harness found
  it and the law was changed to land exactly.
- **Take is edge-triggered** on the value crossing 0.5, so a host restating 1.0 does
  not fire it again.
- **Output alpha is 1.** A plate is opaque. Resolume's demo clips carry alpha.
- **`Perturb` bits and `Probe` are test hooks**, always 0 in the plugin; they exist so
  `--negative` can prove the checks fail and so the checks can read H raw.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen
  it, so every slider is 0..1 and `Controls.cpp` holds the units, with inverses.
  `FF_TYPE_INTEGER` is exempt: Buckets holds its real value. Options are mapped by
  index in `Controls.cpp`.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `wetplate_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- macOS build must be universal. Verify with `lipo`, never the build log.
- GLSL 4.10 reserved words are not identifiers: `patch sample input output filter
  common active half layout flat packed` and the rest of s3.6; `verify.sh` greps the
  dumped shaders for them. MSVC has no `M_PI` and `far`/`near` are macros there.
- FFGL id is `WT01`, display name `SW Wetplate`.

## Not done yet
- **Never loaded into Resolume.** Everything numeric is measured offline on macOS
  against the real plugin class in a headless CGL context, plus an `oxbow` load.
- Seen on the synthetic card and on eight of Resolume's bundled demo clips through
  `--pipe`; never on camera footage of people or places.
- No OpenFX port, no browser demo, no factory presets, no user guide (`guide=""`).
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies; the fleet's
  are generated by stoatworks-backend's `sync-about.py` and `sync-attributions.py`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/wetplate/wetplate.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\wetplate\logs\wetplate.YYYY-MM-DD.log   (Windows)
