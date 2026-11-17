# openfx-io command-line tests

Runnable, dependency-light test harnesses for the IO reader/writer code.

```sh
cd tests
make test          # builds + runs everything
# individually:
make test-ffmpeg   # FFmpeg reader-core: frame-accuracy + seek validation
make test-oiio     # OpenImageIO format roundtrip sanity check
make test-ofx      # OFX entry-point + action tests (plugin code linked directly)
make test-render   # full WriteFFmpeg encode -> ReadFFmpeg decode round-trip
make test-layers   # ReadOIIO EXR channel names -> views/layers (multi-view EXRs)
make test-reader-hooks # GenericReader's isOutputFrameVarying()/getRenderTime() hooks
make test-ocio-cdl # OCIOCDLTransform's Export writes a .cc file that OCIO reads back
make test-runscript # RunScript on MS Windows: quoting, program/batch/shebang launch (no-op elsewhere)
```

Requires: a C++14/17 compiler, `pkg-config`, and dev libs for FFmpeg, OpenColorIO,
OpenImageIO (+ `fmt`), plus the `ffmpeg` and `oiiotool` CLIs for the media harnesses.

## Correction to an earlier assumption

An earlier iteration of this file claimed the OFX plugins "cannot be built here
because the vendored `openfx` submodule is version-mismatched." **That was wrong.**
The symbols that appeared missing (`setLayoutHint`, `ClipComponentsArguments`,
`FrameViewsNeeded*`, the `isIdentity` signature, …) are gated behind the
`OFX_EXTENSIONS_NUKE`/`OFX_EXTENSIONS_NATRON` (etc.) macros that `Makefile.master`
defines. With those `-D` flags the plugin code compiles cleanly against the pinned
`openfx` submodule. The tests below actually compile and link the plugin
translation units.

## The tests

### 1. `test-ofx` — OFX entry points + actions, plugin code linked directly

This is the "unit test the OFX entry points / integrate against the real plugin
code" suite. It compiles the plugin translation units (`ReadFFmpeg`, `WriteFFmpeg`,
`ReadOIIO`, `WriteOIIO` + their in-repo deps) and links them with:

* the OFX C++ **Support library** (`openfx/Support/Library/ofxs*.cpp`), which
  provides the C-ABI entry points `OfxGetNumberOfPlugins` / `OfxGetPlugin`, and
* a compact **in-process OFX host** (`ofx_actions_test.cpp`) implementing the seven
  mandatory OFX suites (Property, ImageEffect, Parameter, Memory, MultiThread,
  Message, Interact) over a generic property bag.

No `.ofx` bundle, no `dlopen`, no external host.

* `ofx_entrypoints_test.cpp` — verifies the C-ABI: `OfxGetNumberOfPlugins() > 0`,
  each `OfxGetPlugin(i)` has the expected `pluginApi`, identifier, version, and
  non-null `setHost`/`mainEntry`.
* `ofx_actions_test.cpp` — drives the action dispatch for every registered plugin:
  `setHost` -> `kOfxActionLoad` -> `kOfxActionDescribe` (asserts declared contexts)
  -> `kOfxImageEffectActionDescribeInContext` (per context) -> `kOfxActionUnload`,
  asserting each returns `kOfxStatOK`/`kOfxStatReplyDefault`.

Current result: all four plugins pass Load/Describe/DescribeInContext/Unload.
RAW support (LibRaw) is intentionally excluded from the test build to avoid an
OpenMP dependency — it is not needed to exercise the entry points.

`kOfxActionCreateInstance` + the render action are covered by `test-render`
(section 1b), which drives a full encode/decode round-trip through the plugins.

### 1b. `test-render` — full encode/decode round-trip through the plugins

`ofx_render_test.cpp` uses an extended in-process host (`ofx_test_host.h`) with param *values*, image
clips with pixel buffers, and the `FnOfxImageEffectPlaneSuiteV2` so it can drive
the real render pipeline, linked directly (no host binary):

* **WriteFFmpeg**: `CreateInstance` (Writer) -> set the `filename` param ->
  `beginSequenceRender` -> `render` each of 10 frames of a gray ramp
  (frame N gray = N/11) -> `endSequenceRender`. The plugin encodes a real
  ProRes `.mov` (confirmed by `ffprobe`: prores, 128x128, 10 frames).
* **ReadFFmpeg**: `CreateInstance` (Reader) -> filename `changedParam` ->
  `getClipPreferences` (which requests RGB output) -> `getRegionOfDefinition`
  (returns the encoded 128x128) -> `render` each frame into a host output image.
  The decoded gray levels ramp 0.091..0.909 (== N/11), i.e. the decode
  round-trips the encode.
* **WriteFFmpeg Codec Options / Format Options** (issue #71), in an MP4: encode
  MPEG-4 part 2 with Keyframe Interval = 5 (some P frames), then with the codec
  option `g=1`, which must override it (every frame decoded by libavcodec in
  the test is an I frame), and the format option `movflags=+faststart` (the
  `moov` atom must precede `mdat`). With Fast Start checked,
  `movflags=+use_metadata_tags` must be appended to the plug-in movflags
  (faststart kept, `mdta` keys written), and `movflags=use_metadata_tags` must
  replace them. Options the encoder or muxer does not apply must be reported
  in a warning (with the user's value), and syntax errors (an empty value,
  double quotes) and invalid values (`g=abc`, `write_tmcd=abc`) must make
  `beginSequenceRender` fail.
* **WriteFFmpeg start timecode** (issue #78): write short `.mov` and `.mp4`
  files with the `startTimecode` param set (including a drop-frame timecode at
  29.97 fps), and read them back with libavformat: the timecode (tmcd) track
  and the video stream must have the expected `timecode` tag, as `ffprobe`
  shows it. Invalid timecodes (syntax, field ranges, drop-frame frame numbers
  and frame rates, more than 255 fps in mov) must make `beginSequenceRender`
  fail without writing a file, and must be reported as soon as the param is
  edited; an empty one must write no timecode track.
* **WriteOIIO** (regression test for issue #79): render a JPEG while the
  host's `clipGetImage` fails with `kOfxStatFailed` (input RoD outside of the
  render window), with the R/G/B/A checkboxes visible (as in Natron) and hidden.
  The plugin must write a black image the size of the render window. Same
  with a valid RGB source image whose bounds do not intersect the render
  window (all four R/G/B/A checkboxes checked, so the channels are packed):
  the written PNG and the output image must be black (run under ASan to
  catch the out-of-bounds read this used to cause).

`test-render` sets `OCIO=test.ocio` (a minimal committed OpenColorIO config)
because the plugins' GenericOCIO layer requires a loadable config. The test
generates its own media (WriteFFmpeg), so it needs no external clip.

### 1c. `test-reader-hooks` — GenericReader's hooks for animated reader params

`reader_hooks_test.cpp` defines three minimal test readers (`GenericReaderPlugin`
subclasses reading fake files that only need to exist: still images, or "video"
files of 10 frames) and drives them through the OFX actions with the in-process
host of `ofx_test_host.h`, linked with `GenericReader.cpp` and its dependencies only:

* **`isOutputFrameVarying()`**: after `getClipPreferences`, the output
  `kOfxImageEffectFrameVarying` is 0 for a still image held before and after with
  the default implementation, and 1 when the reader overrides it to return true.
* **`getRenderTime()`**: `decode()` records it and its `time` argument, and fills
  the image with a value computed from it (like a reader evaluating an animated
  param at the render time). For renders at integer and non-integer times, of a
  held still image and of a video with a time offset and a restricted frame
  range, `getRenderTime()` is the render time while `time` is the sequence time,
  and the output image depends on the render time.
* The previous value is restored when `render()` returns or throws, including for
  a render nested in another one's `decode()` on the same thread.
* Two threads rendering different times concurrently (synchronized inside
  `decode()`) each see their own render time.
* The same through `decodePlane()`, with a multi-planar test reader (the host
  then declares `kFnOfxImageEffectPropMultiPlanar`).

Like `test-render`, it sets `OCIO=test.ocio`.

### 1d. `test-ocio-cdl` — OCIOCDLTransform's Export to a .cc file (#105)

`ocio_cdl_test.cpp` drives `OCIOCDLTransform` (linked with `OCIOPluginBase.cpp`,
`GenericOCIO.cpp` and its OpenGL sources, as in the bundle) with the in-process host
of `ofx_test_host.h`, in the Filter context:

* with non-default slope, offset, power and saturation, the user sets the Export
  param to a new file in a scratch directory of the temporary directory
  (`kOfxActionInstanceChanged`, reason user edited): the file must be a
  ColorCorrection XML that an OCIO `FileTransform` reads back as a CDL with the
  same values, no error must be reported, and the param must be reset to its default;
* exporting to an existing file must report an error, leave the file unchanged, and
  reset the param;
* exporting an inverse grade (which a .cc file can't express) must report an error and
  write no file;
* exporting to a file that can't be created must report an error, without a persistent
  message (which would stop rendering), and reset the param.

The test itself needs OCIO >= 2 (it reads the file back with `Processor::createGroupTransform()`).

With OCIO 2, Export used to fail (`GroupTransform::write()` with a nonexistent "CDL"
format, on an empty group), leaving an empty file. It sets `OCIO=test.ocio`.

### 2. `test-ffmpeg` — FFmpeg reader core (direct coverage of the reader fixes)

`ffmpeg_read_test.cpp` links `FFmpeg/FFmpegFile.cpp` directly (plus `ofx_stubs.cpp`
for the one Support symbol it references, `OFX::MultiThread::getNumCPUs`). It
decodes sequentially for a ground-truth per-frame signature, then re-decodes in a
seek-forcing order and asserts frame-accuracy; `--monotonic` also checks the
sequential signatures strictly increase.

`run_ffmpeg_read_test.sh` generates two gray-ramp clips (frame N luma = 16 + 2*N):
MP4 (H.264, `start_time 0`) and MPEG-TS (H.264, `start_time ~1.48 s`, GOP 8 +
B-frames). The MPEG-TS clip stresses the non-zero stream start PTS.

Note on the `frameToPts` fix: the harness shows the reader is frame-accurate
with and without that fix (a mis-targeted seek is recovered by the forward scan in
`demuxAndDecode`, which uses the correct `ptsToFrame`). So that bug does not cause
wrong frames; the fix stands on correctness-of-intent and repairing the
divide-by-zero guard. No performance claim is made.

### 3. `test-oiio` — OpenImageIO format roundtrip (dependency sanity)

`run_oiio_roundtrip_test.sh` uses `oiiotool` to roundtrip EXR/TIFF/PNG within
per-format tolerances. It exercises the OpenImageIO library the OIIO plugins wrap,
not the plugin code itself (the OFX-level coverage is `test-ofx`).

### 4. `io-hostdemo` — every plug-in of the installed bundle, rendered by openfx's hostDemo

Not a `tests/Makefile` target: a ctest test of the CMake build, with `-DIO_HOSTDEMO_TESTS=ON` (CI turns it on),
which builds openfx's example host `hostDemo` (`openfx/HostSupport/examples`, with HostSupport and its bundled
expat) from the `openfx` submodule. It runs after `cmake --install`, on the installed bundle (it is skipped if the
bundle is not installed):

```sh
cmake -S . -B build -DIO_HOSTDEMO_TESTS=ON -DCMAKE_INSTALL_PREFIX=$PWD -DPLUGIN_INSTALLDIR=Bundle ...
cmake --build build && cmake --install build
ctest --test-dir build -R io-hostdemo --output-on-failure
# or directly:
tests/hostdemo/run.sh build/openfx/HostSupport/examples/hostDemo Bundle/IO.ofx.bundle build/io_verify_plugin_loads /tmp/hostdemo
```

`hostdemo/run.sh` lists the plug-ins of the bundle with `io_verify_plugin_loads`, and renders each one with
`hostDemo` (frames 0 and 1, two views, default parameters, on hostDemo's PAL SD input: black at t=0, white at t=1;
`OCIO=tests/test.ocio`):

* the writers, in the general context, write `image.png` (WritePNG), `image.exr` (WriteEXR), `image.pfm` (WritePFM),
  `image.tif` and `image.jpg` (WriteOIIO) and `movie.mov` (WriteFFmpeg), which must exist and be non-empty;
* the readers, in the general context, read them back: ReadPNG, ReadEXR, ReadPFM, ReadOIIO (TIFF, JPEG, PNG, EXR) and
  ReadFFmpeg (MOV). No test file is committed;
* the filters (filter context): the OCIO plug-ins, OIIOText, OIIOResize, SeExpr, SeExprSimple, SeGrain, SeNoise and
  RunScript, with default parameters, except where defaults can't render: OCIOFileTransform gets an identity 1D
  LUT (it has no default file), and SeExpr, SeExprSimple (in hosts other than Natron) and RunScript get
  `validate=1` (they don't render a script that was not validated; RunScript's default script does nothing).
  OIIOText needs one of OpenImageIO's default fonts (DroidSans, cour, Courier New or FreeMono; on Ubuntu:
  `fonts-freefont-ttf`).

A plug-in fails on any load, describe, instance or render error (hostDemo's exit status), on a crash or a timeout
(`HOSTDEMO_TIMEOUT`, 120 s per render), on an error message from the plug-in, if an output frame is missing, or if
a writer's file is missing or empty (and then the reader of that file fails too). Every plug-in of the bundle must
have a test, or an explicit skip with its reason in the script's `SKIPS` list (none at the moment): a new plug-in
without either fails the test. Tests of plug-ins that are not in the bundle (their libraries were not found at build
time) are reported, not failed.

Not tested: ReadOIIO's RAW support (LibRaw). No RAW file can be generated at test time (no writer makes camera RAW
files), and real camera RAW or DNG files are much larger than the few KB a test file may take.

## Files

| File | Purpose |
|------|---------|
| `ofx_actions_test.cpp`      | In-process OFX host + action-dispatch integration test (all plugins). |
| `ofx_test_host.h`           | Minimal in-process OFX host (suites, param values, image clips) shared by the render and reader-hook tests. |
| `ofx_render_test.cpp`       | Full WriteFFmpeg encode -> ReadFFmpeg decode round-trip through the plugins. |
| `reader_hooks_test.cpp`     | GenericReader's `isOutputFrameVarying()` and `getRenderTime()` hooks, on test readers. |
| `ocio_cdl_test.cpp`         | OCIOCDLTransform's Export param: the .cc file it writes is read back by OCIO. |
| `oiio_layers_test.cpp`      | ReadOIIO's EXR channel name -> view/layer mapping (OpenEXR and Nuke multi-view layouts), on in-memory `ImageSpec`s. |
| `test.ocio`                 | Minimal OpenColorIO config for the render test (GenericOCIO needs `$OCIO`). |
| `ofx_entrypoints_test.cpp`  | OFX C-ABI enumeration/identity unit test. |
| `ffmpeg_read_test.cpp`      | Standalone FFmpeg reader-core harness (links `FFmpegFile.cpp`). |
| `ofx_stubs.cpp`             | Minimal OFX Support stub (`getNumCPUs`) for the reader-core harness. |
| `run_ffmpeg_read_test.sh`   | Generates known clips and runs the reader harness. |
| `run_oiio_roundtrip_test.sh`| OpenImageIO format roundtrip sanity check. |
| `hostdemo/run.sh`           | Renders every plug-in of the installed bundle with openfx's hostDemo (ctest `io-hostdemo`). |
| `Makefile`                  | `make test` / `test-ffmpeg` / `test-oiio` / `test-ofx` / `test-render` / `test-layers` / `test-reader-hooks` / `test-ocio-cdl`. |

## Notes / limitations

* The OFX action tests link a curated subset of dependencies (no LibRaw/OpenMP,
  no OpenGL overlay path). This is enough for the entry points; a full bundle build
  (`make` at the repo root) additionally needs LibRaw, SeExpr, OpenEXR, OpenGL, etc.
* Building the loadable `IO.ofx` and running it in a real host (Natron/Nuke) remains
  the way to test end-to-end rendering with UI; the harnesses here cover the
  library/entry-point layers without a host.
