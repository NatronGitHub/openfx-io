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
make test-ocio-anim # GenericOCIO's animated colorspaces, in a Natron-like host (menus, keys, Swap, undo)
make test-runscript # RunScript: stderr line splitting (all systems); on MS Windows, quoting, program/batch/shebang launch, abort
make test-oiio-threads # OIIO's thread pool is stopped by the last OIIO plug-in's unload, not the first
make test-oiio-edge # OIIOResize and OIIOText with degenerate inputs (empty source, huge RoDs and positions)
make test-exr      # ReadEXR: pixels, data/display windows, UINT, truncated file, concurrent renders
make test-exr-tsan # the same, built with ThreadSanitizer
make test-exr-ubsan # the same, built with UndefinedBehaviorSanitizer
make test-exr-tsan-debug # test-exr and test-reader-hooks, DEBUG builds with ThreadSanitizer
make bench-exr     # ReadEXR's render time, 2048x1556 RGBA half, ZIP/PIZ/DWAA (EXR_THREADS=n: OpenEXR's threads)
```

Requires: a C++17 compiler, `pkg-config`, and dev libs for FFmpeg, OpenColorIO,
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
* **ReadFFmpeg missing frames** (issue #134): extend the frame range to
  0..N+3 and render frames 0 and N+3, which the movie doesn't have, with each
  "On Missing Frame" option, into an image filled with NaN. Hold previous, Load
  next and Load nearest render the first or last frame of the movie, Error fails
  with a persistent message, Black renders transparent black (also with RGBA
  output: alpha 0, not the opaque alpha 1 of the movie); no NaN may remain on
  success. Then a movie written with Fast Start is truncated in the middle of its
  `mdat` atom: frame 1 is decoded, and frame N, which is in the movie but can't be
  decoded, fails with a persistent message whatever the option (Black only applies
  to frames outside of the movie).
* **Matroska at 24 fps**: WriteFFmpeg writes a 24 fps MKV (millisecond
  timestamps) and ReadFFmpeg must decode every frame with the right content.
* **WriteFFmpeg views** (issue #116): render views 1 and 0 of each frame (view
  1 is the negative of the ramp), view 1 first, then view 0 first. The movie
  holds a single view, view 0, whatever the order: ReadFFmpeg must decode the
  ramp.
* **WriteFFmpeg Codec Options / Format Options** (issue #71), in an MP4: encode
  MPEG-4 part 2 with Keyframe Interval = 5 (some P frames), then with the codec
  option `g=1`, which must override it (every frame decoded by libavcodec in
  the test is an I frame), and the format option `movflags=+faststart` (the
  `moov` atom must precede `mdat`). With Fast Start checked,
  `movflags=+use_metadata_tags` must be appended to the plug-in movflags
  (faststart kept, `mdta` keys written), and `movflags=use_metadata_tags` must
  replace them. Options the encoder or muxer does not apply must be reported
  in a warning (with the user's value). Spaces around `=` are allowed
  (`g = 1`, `bogus_sp= 'x=y:z'`), as in FFmpeg's `av_get_token`. Syntax errors
  (issue #109: a key without `=` as in `tune preset=slow`, an unquoted `:` or
  space in a key as in `g:=1` or `a :=1`, a `=` after a complete pair as in
  `g=1 =2`, an empty value, an unterminated single quote, double quotes) must
  make `beginSequenceRender` fail with an error that says what is wrong and at
  which character, and so must invalid values (`g=abc`, `write_tmcd=abc`).
* **WriteFFmpeg start timecode** (issue #78): write short `.mov` and `.mp4`
  files with the `startTimecode` param set (including a drop-frame timecode at
  29.97 fps), and read them back with libavformat: the timecode (tmcd) track
  and the video stream must have the expected `timecode` tag, as `ffprobe`
  shows it. Invalid timecodes (syntax, field ranges, drop-frame frame numbers
  and frame rates, more than 255 fps in mov) must make `beginSequenceRender`
  fail without writing a file, and must be reported as soon as the param is
  edited; an empty one must write no timecode track.
  Issue #110: 47.952, 95.904 and 119.88 fps (drop-frame) movies must be written
  at 48000/1001, 96000/1001 and 120000/1001 fps (video stream and tmcd track);
  the 255 fps limit must only apply when a tmcd track is written (not in 3gp,
  nor with `write_tmcd=0`, but in 3gp with `write_tmcd=1`); and the persistent
  message must show all the current errors (timecode, and codec not supported
  by the container) after editing any param. MPEG-4 part 2 at 119.88 fps must be
  written at a rate close to 120000/1001 (40999/342, as the ffmpeg CLI does), and
  with the Format "Default", the format must be guessed from the filename param
  before the first render (a .m4v file has no 255 fps limit).
* **WriteFFmpeg, output RoD changed during the render**: from the second frame
  on, the render window and the source image are smaller (64x32) or larger
  (192x160) than the movie (128x128, the RoD at `beginSequenceRender`), as when
  Natron sets the project format from the first Reader once the render has
  started. The movie must keep its size, with the part of the image that is in
  the frame, and black elsewhere (ProRes, 16-bit buffer, and MPEG-4, 8-bit).
  WriteFFmpeg read the image as if it had the size of the movie: out of its
  buffer when it is smaller (a crash in NatronRenderer).
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

### 1e. `test-ocio-anim` — GenericOCIO's animated colorspaces (#128)

`ocio_colorspace_anim_test.cpp` drives `OCIOColorSpace`, `WriteOIIO` and `ReadOIIO`
(linked with `PLUGIN_SRCS` and the OpenGL sources of `ocio_cdl_test`) with `ofx_test_host.h`,
whose parameter suite it replaces with one that animates string and choice params like
Natron, and declares `kNatronOfxHostIsNatron` (GenericOCIO then builds the menus of each
instance):

* held values and all the keyframe functions, with keys at their exact times; only the OFX key
  list reports the first of the keys less than 1e-6 apart, as Natron's does; a param with an
  expression reports keys at 0 and 1; string params are read at `floor(t + 0.5)`, and their keys set at any time
  (including fractional times);
* a plug-in's `paramSetValue` on a param with keys sets a key at the current time; the
  plug-in's changes re-enter `kOfxActionInstanceChanged` before they return, only if they
  change the param (`paramDeleteKey` and `paramDeleteAllKeys` as a user edit);
* the user's key edits (set, delete in the curve editor, move in the curve editor or the
  dope sheet, paste, remove animation, restore default) are reported at the times Natron
  uses;
* the plug-in's edit blocks are undone as Natron's `MultipleKnobEditsUndoCommand` does (added
  keys are removed, changed keys are set back, deleted keys are not restored);
* loading a project restores the params before `kOfxActionCreateInstance`, and the test
  checks that the plug-in sets no key outside of an instance changed action.

The config is `swap.ocio` (`OCIO=swap.ocio`), where xN is N times the reference, so that the
renders tell the colorspaces apart. The test checks the keys of the colorspace string params
(rendered and saved) and of their menus (which must have the same keys), the renders, and the
number of instance changed actions of each user action (no loop, no redundant changes):

* keying the input menu, edits at a key or between keys, deleting keys, moving and pasting
  keys, removing the animation and restoring the default value;
* the state after loading a project (the strings have keys, the menus have none), then edits
  of the menu (including Set Key, where the menu shows the colorspace at time 0) or of
  another param, and a string key deleted where its colorspace is not canonical;
* Swap Colorspaces with only one side keyed, twice, with both sides keyed, with neither side
  keyed, after loading a project, and their undo;
* a string param with an expression; hosts that animate choices but not strings (like
  hostDemo), or strings but not choices; an instance without a config;
* WriteOIIO and ReadOIIO: loading a project with keyed colorspaces (no key set in
  createInstance), a file name with a colorspace on a keyed colorspace (not guessed), a menu
  edit after the colorspace was guessed by createInstance, and a key deleted by the plug-in
  (which doesn't mark the colorspace as set by the user);
* real keys at times 0 and 1 (Natron reads string params at `floor(t + 0.5)`), and a menu with
  an expression (e.g. that of Link to).

`ocio_anim_fuzz.cpp` (which includes `ocio_colorspace_anim_test.cpp`) applies random user
actions (menu and string edits, key deletions and moves, paste, Remove animation, Restore
default, Swap, undo and redo of the plug-in's edit blocks, which Natron merges, project
save/load, file names, expressions, time changes) to OCIOColorSpace, WriteOIIO or ReadOIIO,
and checks after each one that the menus have the keys and values of the strings, that no key
is set outside of an instance changed action, that there is no loop, that the renders follow
the strings, that ops that add no key add none, and that undoing a swap of keyed sides
restores them. `make test-ocio-anim` runs 2000 operations on each plug-in with fixed seeds
(`FUZZ_OPS=n` for more), a run with `FUZZ_NEAR=1` (keys and times near frames), and the seeds
that found bugs in the review (`FUZZ_NEAR=1`: reader 32, and cs 77 and 102 with 50000 operations).
`FUZZ_STRICT_A=1` doesn't skip params taken for expressions because of a hidden key next to
keys at 0 and 1; run `ocio_anim_fuzz cs|writer|reader <ops> <seed>` directly for
others. By default, the host models Natron 2.6, with NatronGitHub/Natron#1291;
`OCIO_ANIM_NATRON_EXACT=1` models the string keys of Natron before 2.6, without it, which hold
the index of their string (NatronGitHub/Natron#1290: some invariants then fail, e.g. the undo
of a swap);
`OCIO_ANIM_EDIT_BLOCK_BUG=1` models NatronGitHub/Natron#1335 (Natron without its fix,
NatronGitHub/Natron#1341), which ends
an edit block when the instance changed action of its first change returns (so that undoing a
swap only undoes its first change); `ocio_anim_fuzz scenario 0 0` with `SCEN=main` or
`SCEN=exact` prints some repros. `OCIO_ANIM_NOT_NATRON=1` models another host than Natron (no
`kNatronOfxHostIsNatron`, string params read at the given time): `make test-ocio-anim` also
runs the test and a fuzz run so.

### 1f. `test-oiio-threads` — OpenImageIO's thread pool, shared by the OIIO plug-ins (#118)

`oiio_threadpool_test.cpp` loads ReadOIIO, WriteOIIO and OIIOResize in the in-process
host of `ofx_test_host.h`, which share OpenImageIO's default thread pool, as in IO.ofx.
Each OIIO plug-in stops the pool in its unload action (its threads must not be joined
while the binary is unloaded, a deadlock on MS Windows), but only if it is the last OIIO
plug-in loaded: after ReadOIIO's unload (and a reload and unload, as in a plug-in scan),
the pool must keep its `hardware_concurrency() - 1` threads and OIIOResize must render
(`ImageBufAlgo::resize()`, with the host's 4 CPUs); after the unload of the last one,
the pool must have 0 threads. It sets `OCIO=test.ocio`.

### 1f2. `test-oiio-edge` — OIIOResize and OIIOText with degenerate inputs

`oiio_edge_test.cpp` links the OIIO plug-ins with `OIIOResize.cpp` and `OIIOText.cpp`, in the Filter context:

* **OIIOResize, empty source**: an empty (0x0) source image resized to 32x32 with the default filter renders black
  (it segfaulted in `ImageBufAlgo::resize()` with OpenImageIO 2.4).
* **OIIOResize, Preserve PAR**: the coordinates of a source RoD of +-DBL_MAX or +-inf are clamped to +-5e289 before
  the sizes and ratios (inf / inf was NaN, converted to int): 100x100, and 100x0 for DBL_MAX x 10; sizes above INT_MAX
  give the same RoD as before (an infinite x and y 0..3e9: 100x69); 200x100 into 100x100 gives 100x50.
* **OIIOResize, infinite source RoD**: when the source clip changes, the Size param is the RoD size clamped to INT_MAX
  (the subtraction overflowed int).
* **OIIOText**: a text position of +-DBL_MAX or 1e12 renders the source (it was converted to int).

The conversions are only detected by UBSan: build it with
`make oiio_edge_test CXX="clang++ -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all"`.
It sets `OCIO=test.ocio`.

### 1g. `test-exr` — ReadEXR (#123)

`exr_read_test.cpp` writes EXR files with OpenEXR in a scratch directory (`.exr_read`),
and reads them with ReadEXR (linked with `GenericReader.cpp` and its dependencies) in
the in-process host of `ofx_test_host.h`, in the Reader context:

* **pixels**: FLOAT and HALF files, with ZIP, PIZ or no compression, and the data and
  display windows of `hostdemo/run.sh` (offset, crop, overscan): the RoD, and every
  pixel and channel of the image, must be exact (the display window moved to (0,0), y
  flipped, the 1-pixel border black); the channels that are not in the file are 0; UINT
  channels are normalized by 2^32-1, as in ReadOIIO; luminance/chroma files (Y, RY, BY)
  are read as before (not converted).
* **render windows**: narrower than the data window (decode()'s buffer, in blocks of
  256 lines): exact inside, nothing written outside; render scale 1/2.
* **a rewritten file**: with larger and smaller windows, the next RoD and image are the new
  file's (the next render updates the header cache; so does the purge caches action); and
  with the RoD the host got before the rewrite, at scale 1 (straight into the output image,
  and through GenericReader's buffer) and 1/2: the whole render window is written, black
  out of the new frame bounds.
* **a truncated file**: the render must fail, with an error message.
* **concurrent renders**: 6 instances render 2 files (3 instances per file) 8 times each
  from 6 threads at once: each render must give the same image as a single-threaded one.

`make test-exr-tsan` builds and runs it with ThreadSanitizer (`-fsanitize=thread`,
no suppressions). OpenEXR itself is not instrumented, so the race on a shared
`Imf::InputFile` is found by the image comparison, not by ThreadSanitizer.
`make test-exr-tsan-debug` builds and runs this test and `test-reader-hooks` (which renders
from two threads) with `-fsanitize=thread -DDEBUG`, also with `halt_on_error=1` and no
suppressions (and `abort_on_error=0`: a race exits with an error, without a crash report). `DEBUG` enables the OFX Support library's property validation and logging,
which run while other threads render: this catches races in the Support library itself, such as
those on `PropertySet::_gPropLogging` and `PropertyDescription::_exists` fixed by openfx #144.
`make bench-exr` (optimized build) times 5 renders of 2048x1556 RGBA half files with ZIP,
PIZ and DWAA compression (`EXR_THREADS=n` sets OpenEXR's global thread count).
It sets `OCIO=test.ocio`.

### 2. `test-ffmpeg` — FFmpeg reader core (direct coverage of the reader fixes)

`ffmpeg_read_test.cpp` links `FFmpeg/FFmpegFile.cpp` directly (plus `ofx_stubs.cpp`
for the one Support symbol it references, `OFX::MultiThread::getNumCPUs`). It
decodes sequentially for a ground-truth per-frame signature, then re-decodes in a
seek-forcing order and asserts frame-accuracy; `--monotonic` also checks the
sequential signatures strictly increase. `--ramp OFFSET,STEP` checks that every
decoded frame f, sequential or seeked, holds the content of that frame
(OFFSET + STEP * (f - 1)), which catches a wrong frame as well as a missing one,
`--frames N` checks the number of frames the reader reports, and `--positions
P0,P1,...` describes a variable frame rate clip with timestamp gaps: ramp frame i is
at frame Pi, and a frame in a gap must be identical to the last ramp frame at or
before it, as players display it (and, with `--ramp`, hold its content).
`--max-seeks N` checks that decoding every frame sequentially seeks at most N times
(`FFmpegFile::getSeekCount()`).

`run_ffmpeg_read_test.sh` generates 40-frame gray-ramp clips (frame N luma = 16 + 4*N,
H.264 with GOP 8 + B-frames unless noted): MP4 (`start_time 0`, edit list), MP4 with a
non-zero start time, MOV, MPEG-TS (`start_time ~1.48 s`), Matroska at 23.976, 24, 25,
30 and 60 fps, ProRes in Matroska, VP9 and VP8 in WebM, and a Matroska file with
jittered (VFR) timestamps. The MPEG-TS clip stresses the non-zero stream start PTS.
Frame counts (issue #145): an AVCHD-like M2TS (interlaced H.264 at 29.97 fps, with an
AC3 audio track that starts before the video) must count the frames from the start of
the video stream; Matroska with a non-zero start time and FLV with B-frames (with and
without a start time, starting after the audio, and without the duration metadata, as
written to a pipe) store a duration that is not counted from the start of the movie, while
a Matroska duration rewritten as a length (as mkvmerge writes split parts, which needs
`perl`; with GOP 8 and with the default GOP) must not cut the end, and AAC audio ending
40 ms after the video in an FLV without the duration metadata must not add a frame; MP4
and MOV with B-frames and no edit list (fragmented MP4 with H.264 and HEVC,
`-use_editlist 0`) start at the B-frame delay and must keep their last frames, as must MOV/MP4
stream copies from a non-keyframe and VFR MP4 with a one-frame gap (fewer samples than frame
times); opening a 60 s Matroska with a start time, and a 60 s FLV, also truncated by 7, 377 and
12345 bytes, must read at most 2 MB (`--max-open-bytes`, `FFmpegFile::getBytesReadAtOpen()`), and
the index of the FLV must not reach its end after the open (`--max-indexed-frame`: an index entry
near the end, added while reading the last packets, made the seeks decode from near the start); a high
bit rate FLV without the duration metadata (400 kB per frame), with and without a start time, must
count its frames from its last packets, as must an FLV at 0.5 fps with AAC audio (its last 64
tags are audio tags and the AVC end of sequence); an MPEG-TS with two programs (the second one at
100 s), an MPEG-PS whose last reference frame has no PTS, and VOBs with B-frames (with MP2, AC3 or
no audio; the last reference frame has no PTS and no B-frame follows it) must count their video
packets, with 1 and 8 decoder threads (`FFMPEG_READ_TEST_THREADS`, read by `ofx_stubs.cpp`); an
MPEG-PS cut at 10 offsets (in the middle of a GOP, or on an I-frame followed by the leading
B-frames of an open GOP) must count its frames from the start time of the stream (its first PTS)
to the last frame that ffprobe decodes, with 1 and 8 threads: the leading B-frames of an open GOP
are before the start time, and not counted, while the frames after it whose reference frames are
cut are counted (they fail); NUT (MPEG-4 with B-frames) has no start time; an MPEG-2
elementary stream with B-frames has no timestamps, and its last frame has no PTS;
MP4 edit lists that do not display the first 1 or 3 frames (negative timestamps) must
not count these frames; AVI (MPEG-4 with B-frames) has no PTS, and every frame must
still decode at its own frame.
VFR clips with timestamp gaps (24 fps Matroska, MP4 and MOV, as in screen recordings
or phone videos with dropped frames) have gaps after a B-frame (24 frames, 2 frames
apart after frame 12), after the first GOP (frames 8-12 missing), and at the end (a
longer audio track makes the movie last longer than its last video frame). VFR
Matroska clips have a corrupted frame (8 bytes of slice data overwritten, so that it
decodes with errors) just before a gap: the gap must show the corrupted frame, as
decoded at its own frame, rather than fail. With a 20-frame gap after it, and with a
35-frame gap at the end, the sequential decode must seek only twice (for frame 1, and
to decode from the start once), not once per frame in the gap. A MOV clip has broken
timestamps (two PTS swapped with the `setts` bitstream filter): every frame must
decode, identically after a seek except at the two swapped frame times
(`--no-seek-check`): there, FFmpeg 8 gives timestamps after a seek that depend on the
timestamps decoded before, since `avcodec_flush_buffers()` does not reset its
best_effort_timestamp heuristic.
Matroska and WebM store timestamps in milliseconds: at 24 fps the third frame is at
83 ms, which `Stream::ptsToFrame()` must round to frame 2 (it used to truncate it to
frame 1, so that every third frame could not be decoded), and the movie duration
(1667 ms for 40 frames) must not count a 41st frame. Clips whose encoder is missing
from the `ffmpeg` CLI are skipped. `FFMPEG_READ_TEST_KEEP=dir` keeps the clips.

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
which builds openfx's example host `hostDemo` (`openfx/HostSupport/examples`, with HostSupport, which needs the
system expat) from the `openfx` submodule. It runs after `cmake --install`, on the installed bundle (it is skipped if the
bundle is not installed):

```sh
cmake -S . -B build -DIO_HOSTDEMO_TESTS=ON -DCMAKE_INSTALL_PREFIX=$PWD -DPLUGIN_INSTALLDIR=Bundle ...
cmake --build build && cmake --install build
ctest --test-dir build -R io-hostdemo --output-on-failure
# or directly:
tests/hostdemo/run.sh build/openfx/HostSupport/examples/hostDemo Bundle/IO.ofx.bundle build/io_verify_plugin_loads /tmp/hostdemo
```

`hostdemo/run.sh` lists the plug-ins of the bundle with `io_verify_plugin_loads`, and renders each one with
`hostDemo` (frames 0 and 1, two views, default parameters, `OCIO=tests/test.ocio`, on hostDemo's PAL SD input: at
time t, the left view is red and the right view cyan, at grey level t, so both are black at t=0):

* the writers, in the general context, write `image.png` (WritePNG), `image.exr` and `half.exr` (WriteEXR, 32-bit
  float and 16-bit half), `image.pfm` (WritePFM), `image.tif` and `image.jpg` (WriteOIIO) and `movie.mov`
  (WriteFFmpeg), which must exist and be non-empty. A still image file holds the last render (t=1, right view: cyan),
  the movie one frame per time (the left view, red);
* the readers, in the general context, read them back: ReadPNG, ReadEXR, ReadPFM, ReadOIIO (TIFF, JPEG, PNG, EXR) and
  ReadFFmpeg (MOV, its frames 1 and 2). No test file is committed. Their images are compared with those the writer
  rendered (`hostdemo/compare.py`, on hostDemo's `--report` files): each render of the reader must have the writer's
  bounds and component means, within 0.001 for the lossless formats (PNG, TIFF, EXR, PFM) and 0.03 for JPEG and MOV,
  the lossless ones also the same CRC-32 of the image (which catches flipped, mirrored or shifted images), and no
  pixel may be left unwritten (NaN). Since the two views and the two times differ, this also catches swapped or
  missing channels, black images, and the wrong movie frame;
* the filters (filter context): the OCIO plug-ins, OIIOText, OIIOResize, SeExpr, SeExprSimple, SeGrain, SeNoise and
  RunScript, with default parameters, except where defaults can't render: OCIOFileTransform gets an identity 1D
  LUT (it has no default file), and SeExpr, SeExprSimple (in hosts other than Natron) and RunScript get
  `validate=1` (they don't render a script that was not validated; RunScript's default script does nothing).
  OIIOText needs one of OpenImageIO's default fonts (DroidSans, cour, Courier New or FreeMono; on Ubuntu:
  `fonts-freefont-ttf`).

A plug-in fails on any load, describe, instance or render error (hostDemo's exit status), on a crash, on an error
message from the plug-in (`OfxMessageError`, `OfxMessageFatal`), if an output frame is missing, if a writer's file is
missing or empty (and then the reader of that file fails too), if a reader's images differ from its writer's, or on a
timeout: `HOSTDEMO_TIMEOUT` (120 s) per hostDemo run, and `HOSTDEMO_TOTAL_TIMEOUT` (600 s, less than ctest's 900 s)
for all of them. A timeout tells whether the render had finished (hostDemo writes its `--report` file before it
unloads the plug-ins). Every plug-in of the bundle must have a test, or an explicit skip with its reason in the
script's `SKIPS` list (none at the moment): a new plug-in without either fails the test. Tests of plug-ins that are not
in the bundle (their libraries were not found at build time) are reported, and a bundle that is not installed skips
the test, unless `IO_HOSTDEMO_STRICT=1` (as in CI), which makes both fail. The script needs Python 3 (`PYTHON`). In
CI, the outputs (`build/hostdemo/out`) are uploaded as an artifact when the job fails.

With `-DIO_HOSTDEMO_TESTS=ON`, the CMake configuration fails if the test can't be added (no bash, no hostDemo, or
`BUILD_TESTING=OFF`).

Not tested: ReadOIIO's RAW support (LibRaw). No RAW file can be generated at test time (no writer makes camera RAW
files), and real camera RAW or DNG files are much larger than the few KB a test file may take.

### 5. FFmpeg version compile check (FFmpeg 4.x and 5.x)

The FFmpeg plug-ins support FFmpeg 4.0 and later, but CI and the other tests use a recent FFmpeg. Some API
changes only show when compiling against an old version, e.g. `avformat_alloc_output_context2()` takes a non-const
`AVOutputFormat*` before libavformat 59 (FFmpeg 5.0, #141). `ffmpeg_syntax_check.sh` compiles `WriteFFmpeg.cpp`,
`ReadFFmpeg.cpp`, `FFmpegFile.cpp` and `PixelFormat.cpp` with `-fsyntax-only` against the FFmpeg found by
`pkg-config`. Run it in a container after changing the FFmpeg plug-ins, from the repository root (with the
submodules), e.g. with podman (or docker):

```sh
# FFmpeg 4.4 (RPM Fusion) on Rocky Linux 8, with GCC 14 as for Natron
podman run --rm -v "$PWD":/src:ro -w /src docker.io/rockylinux/rockylinux:8 sh -c '
  dnf -y -q install epel-release dnf-plugins-core && dnf config-manager --set-enabled powertools &&
  dnf -y -q install https://mirrors.rpmfusion.org/free/el/rpmfusion-free-release-8.noarch.rpm &&
  dnf -y -q install ffmpeg-devel gcc-toolset-14-gcc-c++ pkgconf-pkg-config &&
  . /opt/rh/gcc-toolset-14/enable && sh tests/ffmpeg_syntax_check.sh g++'
# FFmpeg 5.1 on Debian 12
podman run --rm -v "$PWD":/src:ro -w /src docker.io/library/debian:12 sh -c '
  apt-get -qq update && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends g++ pkg-config \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev &&
  sh tests/ffmpeg_syntax_check.sh g++'
```

## Files

| File | Purpose |
|------|---------|
| `ofx_actions_test.cpp`      | In-process OFX host + action-dispatch integration test (all plugins). |
| `ofx_test_host.h`           | Minimal in-process OFX host (suites, param values, image clips) shared by the render and reader-hook tests. |
| `ofx_render_test.cpp`       | Full WriteFFmpeg encode -> ReadFFmpeg decode round-trip through the plugins. |
| `reader_hooks_test.cpp`     | GenericReader's `isOutputFrameVarying()` and `getRenderTime()` hooks, on test readers. |
| `ocio_cdl_test.cpp`         | OCIOCDLTransform's Export param: the .cc file it writes is read back by OCIO. |
| `exr_read_test.cpp`         | ReadEXR (`test-exr`, `test-exr-tsan`, `test-exr-tsan-debug`, `bench-exr`): pixels, windows, UINT, luminance/chroma, a truncated file, concurrent renders, and timings. |
| `oiio_edge_test.cpp`        | OIIOResize and OIIOText with degenerate inputs (`test-oiio-edge`). |
| `ocio_colorspace_anim_test.cpp` | GenericOCIO's animated colorspaces (OCIOColorSpace, WriteOIIO, ReadOIIO) and Swap Colorspaces, with a Natron-like animated param suite. |
| `ocio_anim_fuzz.cpp`        | Random user actions on the same host, checking invariants (`test-ocio-anim`). |
| `oiio_layers_test.cpp`      | ReadOIIO's EXR channel name -> view/layer mapping (OpenEXR and Nuke multi-view layouts), on in-memory `ImageSpec`s. |
| `test.ocio`                 | Minimal OpenColorIO config for the render test (GenericOCIO needs `$OCIO`). |
| `swap.ocio`                 | OpenColorIO config where xN is N times the reference (`test-ocio-anim`, `hostdemo/run.sh`). |
| `ofx_entrypoints_test.cpp`  | OFX C-ABI enumeration/identity unit test. |
| `ffmpeg_read_test.cpp`      | Standalone FFmpeg reader-core harness (links `FFmpegFile.cpp`). |
| `ofx_stubs.cpp`             | Minimal OFX Support stub (`getNumCPUs`) for the reader-core harness. |
| `run_ffmpeg_read_test.sh`   | Generates known clips and runs the reader harness. |
| `run_oiio_roundtrip_test.sh`| OpenImageIO format roundtrip sanity check. |
| `ffmpeg_syntax_check.sh`    | Compile check of the FFmpeg plug-ins against the FFmpeg found by pkg-config (for FFmpeg 4.x/5.x containers). |
| `hostdemo/run.sh`           | Renders every plug-in of the installed bundle with openfx's hostDemo (ctest `io-hostdemo`). |
| `check_icons.cmake`         | Every plug-in of the CMake build tree's bundle has its icon, `Contents/Resources/<identifier>.png` (ctest `io-icons`). |
| `hostdemo/compare.py`       | Compares a reader's images with its writer's, from hostDemo's `--report` files (for `run.sh`). |
| `Makefile`                  | `make test` / `test-ffmpeg` / `test-oiio` / `test-ofx` / `test-render` / `test-layers` / `test-reader-hooks` / `test-ocio-cdl` / `test-ocio-anim`. |

## Notes / limitations

* The OFX action tests link a curated subset of dependencies (no LibRaw/OpenMP,
  no OpenGL overlay path). This is enough for the entry points; a full bundle build
  (`make` at the repo root) additionally needs LibRaw, SeExpr, OpenEXR, OpenGL, etc.
* Building the loadable `IO.ofx` and running it in a real host (Natron/Nuke) remains
  the way to test end-to-end rendering with UI; the harnesses here cover the
  library/entry-point layers without a host.
