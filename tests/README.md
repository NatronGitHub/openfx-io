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

`ofx_render_test.cpp` extends the in-process host with param *values*, image
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

`test-render` sets `OCIO=test.ocio` (a minimal committed OpenColorIO config)
because the plugins' GenericOCIO layer requires a loadable config. The test
generates its own media (WriteFFmpeg), so it needs no external clip.

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

## Files

| File | Purpose |
|------|---------|
| `ofx_actions_test.cpp`      | In-process OFX host + action-dispatch integration test (all plugins). |
| `ofx_render_test.cpp`       | Full WriteFFmpeg encode -> ReadFFmpeg decode round-trip through the plugins. |
| `test.ocio`                 | Minimal OpenColorIO config for the render test (GenericOCIO needs `$OCIO`). |
| `ofx_entrypoints_test.cpp`  | OFX C-ABI enumeration/identity unit test. |
| `ffmpeg_read_test.cpp`      | Standalone FFmpeg reader-core harness (links `FFmpegFile.cpp`). |
| `ofx_stubs.cpp`             | Minimal OFX Support stub (`getNumCPUs`) for the reader-core harness. |
| `run_ffmpeg_read_test.sh`   | Generates known clips and runs the reader harness. |
| `run_oiio_roundtrip_test.sh`| OpenImageIO format roundtrip sanity check. |
| `Makefile`                  | `make test` / `test-ffmpeg` / `test-oiio` / `test-ofx` / `test-render`. |

## Notes / limitations

* The OFX action tests link a curated subset of dependencies (no LibRaw/OpenMP,
  no OpenGL overlay path). This is enough for the entry points; a full bundle build
  (`make` at the repo root) additionally needs LibRaw, SeExpr, OpenEXR, OpenGL, etc.
* Building the loadable `IO.ofx` and running it in a real host (Natron/Nuke) remains
  the way to test end-to-end rendering with UI; the harnesses here cover the
  library/entry-point layers without a host.
