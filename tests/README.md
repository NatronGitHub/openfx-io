# openfx-io command-line tests

Runnable, dependency-light test harnesses for the IO reader/writer code.

The CMake build builds them ([`CMakeLists.txt`](CMakeLists.txt), with `BUILD_TESTING`, the default) in `build/tests`,
with the compiler, C++ standard (`OFX_CXX_STANDARD`) and FFmpeg (`FFMPEG_PKG_CONFIG_PATH`) of the build, and
[`ctest`](https://cmake.org/cmake/help/latest/manual/ctest.1.html) runs them, as CI does:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ...   # from the repository root
cmake --build build -j4
ctest --test-dir build --output-on-failure          # everything (with io-load, io-icons, io-hostdemo: ../README.md)
ctest --test-dir build -R '^test-ofx14(-11|-gl|-full)?$'   # the runs of `make test-ofx14`
```

They are the programs of `tests/Makefile`, with the same sources, defines and flags (`-O0 -g` for those that link the
plug-ins, `-O2 -g -Wall` for the others, and `assert()` enabled, whatever the build type), and the same runs:
each ctest test is one run of a `make` target, named after it (`test-<target>`, plus a suffix when the target runs
the program more than once: `test-ofx-entrypoints` and `test-ofx-actions` are `make test-ofx`; `test-ocio-anim`,
`test-ocio-anim-not-natron` and the `test-ocio-anim-fuzz-*` runs are `make test-ocio-anim`; `test-ofx14`,
`test-ofx14-11`, `test-ofx14-gl` and `test-ofx14-full` are `make test-ofx14`; `test-ofx14-scan` and
`test-ofx14-scan-full` are `make test-ofx14-scan`; `test-runscript-win32` and `test-runscript-output` are
`make test-runscript`). A test is only added if the bundle has the plug-ins it links (e.g. `test-seexpr`, with the
SeExpr of the build). The programs run in `build/tests`, where they write their files. CMake options (see
[`CMakeLists.txt`](CMakeLists.txt)):

* `IO_TESTS_WITH_LIBRAW` (ON): the tests that link ReadOIIO also build its RAW support, when the bundle has it
  (`make WITH_LIBRAW=1`);
* `IO_TESTS_TIMEOUT` (1800): the time limit of each test, in seconds (`test-ffmpeg`: twice as much);
* `IO_TESTS_FUZZ_OPS` (2000): `make FUZZ_OPS=...`;
* `IO_TESTS_SANITIZERS` (OFF): also `test-ofx14-scan-asan` (and `-full`), `test-exr-tsan`, `test-exr-ubsan`,
  `test-exr-tsan-debug` and `test-reader-hooks-tsan-debug`, which `make test` doesn't run either.

`make bench-exr` is not a ctest test. Without CMake, `tests/Makefile` builds and runs the same tests in `tests/`,
with `pkg-config`:

```sh
cd tests
make test          # builds + runs everything
# individually:
make test-ffmpeg   # FFmpeg reader-core: frame-accuracy + seek validation
make test-oiio     # OpenImageIO format roundtrip sanity check
make test-ofx      # OFX entry-point + action tests (plugin code linked directly)
make test-render   # full WriteFFmpeg encode -> ReadFFmpeg decode round-trip
make test-dnxhr444-noise # WriteFFmpeg DNxHR 444 on binary RGB noise: FFmpeg's encoder must not abort (#267)
make test-writeffmpeg-begin-failure # WriteFFmpeg: a failed beginSequenceRender after a render, then endSequenceRender (#289)
make test-writeffmpeg-dnxhd-profiles # WriteFFmpeg: each DNxHD/DNxHR profile in 1080p and 720p uses the expected CID (#305)
make test-writeffmpeg-encode-failure # WriteFFmpeg: frame 1 is aborted or fails while other threads wait (#288)
make test-writeffmpeg-encode-failure-disk-full # the same, frame 1 fails to be written (#288; not on MS Windows)
make test-swscale-exact # ReadFFmpeg/WriteFFmpeg Y'CbCr <-> RGB conversions give exact values (#207, #220, #316)
make test-writeffmpeg-param-change # WriteFFmpeg: param and clip changes, sync private data, during a render (#304)
make test-writeffmpeg-codec-params # WriteFFmpeg: the settings and pixel formats of each encoder, and its movies (#343)
make test-readffmpeg-destroy-closes-file # ReadFFmpeg: DestroyInstance closes the movie, before Unload (#290)
make test-layers   # ReadOIIO EXR channel names -> views/layers (multi-view EXRs)
make test-reader-hooks # GenericReader's isOutputFrameVarying()/getRenderTime()/refreshFrameBounds()/clipToFile() hooks
make test-ocio-cdl # OCIOCDLTransform's Export writes a .cc file that OCIO reads back
make test-ocio-filetransform # OCIOFileTransform's CCC Id param, with AddressSanitizer
make test-ocio-anim # GenericOCIO's animated colorspaces, in a Natron-like host (menus, keys, Swap, undo)
make test-runscript # RunScript: stderr line splitting (all systems); on MS Windows, quoting, program/batch/shebang launch, abort
make test-oiio-threads # OIIO's thread pool is stopped by the last OIIO instance's destruction and the last OIIO plug-in's unload
make test-oiio-threads-tsan # the same with ThreadSanitizer (instances counted from several threads)
make test-exit-without-unload # a host renders ReadOIIO, destroys the instance and exits without kOfxActionUnload (10 times)
make test-raw-unload # a host renders a DNG file with ReadOIIO (LibRaw, OpenMP), unloads IO.ofx, keeps running, exits (5 times)
make test-oiio-edge # OIIOResize and OIIOText with degenerate inputs (empty source, huge RoDs and positions)
make test-exr      # ReadEXR: pixels, data/display windows, UINT, truncated file, concurrent renders
make test-reader-window # ReadOIIO, ReadPFM, ReadPNG: narrow render windows, render scale 1/2, rewritten files (ASan, guard pages)
make test-png-corrupt # ReadPNG and WritePNG on libpng errors: corrupt PNG files fail cleanly (ASan, no leak)
make test-reader-window-host # the same readers and ReadFFmpeg in a host built on openfx's HostSupport (needs expat, oiiotool, ffmpeg)
make test-ofx14    # every plug-in described, instantiated and rendered in an OFX 1.4-only host (and the full host)
make test-ofx14-scan # the same after a plug-in scan (Load, Describe, then Unload of every plug-in, openfx #174)
make test-ofx14-scan-asan # the same, built with AddressSanitizer
make test-exr-tsan # the same, built with ThreadSanitizer
make test-exr-ubsan # the same, built with UndefinedBehaviorSanitizer
make test-exr-tsan-debug # test-exr and test-reader-hooks, DEBUG builds with ThreadSanitizer
make test-seexpr   # SeExpr's frames needed and cpixel() domain, SeNoise/SeGrain degenerate params (SEEXPR_HOME=..., SEEXPR_LIBDIR=...)
make bench-exr     # ReadEXR's render time, 2048x1556 RGBA half, ZIP/PIZ/DWAA (EXR_THREADS=n: OpenEXR's threads)
```

Requires: a C++17 compiler, `pkg-config` (or CMake), and dev libs for FFmpeg, OpenColorIO,
OpenImageIO (+ `fmt`), plus the `ffmpeg` and `oiiotool` CLIs for the media harnesses (`test-ffmpeg`, `test-oiio`,
bash scripts; `FFMPEG_READ_TEST_BIN`: the `ffmpeg_read_test` to run, default the one next to the script).

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
RAW support (LibRaw) is only built with `make WITH_LIBRAW=1` (which may need OpenMP), or in the CMake build when the
bundle has it (`IO_TESTS_WITH_LIBRAW`); it is not needed to exercise the entry points.

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
* **AVI with B-frames** ([#339](https://github.com/NatronGitHub/openfx-io/issues/339)): WriteFFmpeg
  writes H.264 and MPEG-2 with B-frames in AVI, which has no PTS, and ReadFFmpeg must read them back.
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
* **WriteFFmpeg color tags and Y'CbCr matrix, ReadFFmpeg round trip**: encode a
  constant color (0.8, 0.3, 0.1) with H.264 (libx264, MP4/MOV/Matroska), ProRes 4444
  and 422 HQ, and QuickTime Animation (qtrle, RGB), with the OCIO output colorspaces
  rec709, Rec2020, sRGB and Gamma2.2 of `test.ocio`, in SD (128x96) and 720p. The stream
  must keep the primaries and transfer function of the OCIO colorspace (ProRes: only
  the transfer functions of SMPTE RDD 36, so sRGB is unspecified there, and the frame
  header must have the same codes as the stream), its Y'CbCr matrix must be BT.2020 NCL
  with the BT.2020 primaries, else BT.709 in HD and SMPTE 170M in SD, and `gbr` (RGB) for
  qtrle; the Y' that libavcodec decodes must be the one of that matrix
  (Y' = Kr R + (1 - Kr - Kb) G + Kb B, video range), and ReadFFmpeg (Auto matrix and
  range) must decode the color back within 0.01. In Matroska, the OCIO output
  colorspace must be in the `uk.co.thefoundry.Colorspace` metadata, and ReadFFmpeg
  must use it as its File Colorspace. DNxHR HQX and HQ (720p, Rec2020, and HQ in SD,
  rec709) must use the BT.709 matrix, the one of the DNxHD frame header that FFmpeg's
  encoder writes and FFmpeg's decoder uses, whatever the primaries and size; DNxHR 444
  is RGB (`gbr`).
* **WriteFFmpeg, output RoD changed during the render**: from the second frame
  on, the render window and the source image are smaller (64x32) or larger
  (192x160) than the movie (128x128, the RoD at `beginSequenceRender`), as when
  Natron sets the project format from the first Reader once the render has
  started. The movie must keep its size, with the part of the image that is in
  the frame, and black elsewhere (ProRes, 16-bit buffer, and MPEG-4, 8-bit).
  WriteFFmpeg read the image as if it had the size of the movie: out of its
  buffer when it is smaller (a crash in NatronRenderer).
* **WriteFFmpeg uncompressed codecs** (issue #170): a color pattern (with an
  alpha ramp for the formats with alpha) is written with v210 and with
  rawvideo in QuickTime (2vuy, raw RGB and ARGB, v308, v408 and v410), Matroska, NUT and AVI. libavformat must read back the expected
  codec, tag and pixel format (and the BT.601 matrix in QuickTime and Matroska,
  also for the v308 and v410 encoders of FFmpeg < 9, since #210 tags every
  Y'CbCr stream; video range in Matroska), and ReadFFmpeg must decode the pattern exactly for
  RGB, or within the Y'CbCr conversion error. The pattern is 64x48 with values
  on the 8-bit grid, and also has values on no grid (as float, 10-bit and
  16-bit images: 16-bit RGB must be exact within 1e-4, a lower depth fails)
  and an odd size (63x47). RGB with 10 or 16 bits in QuickTime must write
  8-bit RGB with a warning (b48r is not written: QuickTime reads it
  byte-swapped), Y'CbCr with 12 or 16 bits v410; RGB with an odd width in
  QuickTime must write ARGB (FFmpeg and QuickTime do not read the padded RGB24
  rows); v210 with an odd width must fail with an error before opening the
  encoder. A project saved with the v308 or v410 encoder must write rawvideo
  with the same tag when FFmpeg (9) does not have them, with a warning that is
  still shown after the render (and UYVY with a warning in Matroska). When a
  frame fails after a good one (another file in encode(), or an Output image
  that can not be fetched in render()) while a warning is set, the error must
  stay the persistent message after endSequenceRender. Every frame of the NUT
  files is checked, including the last one (#215).
* **WriteOIIO** (regression test for issue #79): render a JPEG while the
  host's `clipGetImage` fails with `kOfxStatFailed` (input RoD outside of the
  render window), with the R/G/B/A checkboxes visible (as in Natron) and hidden.
  The plugin must write a black image the size of the render window. Same
  with a valid RGB source image whose bounds do not intersect the render
  window (all four R/G/B/A checkboxes checked, so the channels are packed):
  the written PNG and the output image must be black (run under ASan to
  catch the out-of-bounds read this used to cause).

`test-dnxhr444-noise` (issue #267) runs `ofx_render_test OUT --dnxhr444-noise`: WriteFFmpeg encodes two
256x120 frames of binary RGB noise in DNxHR 444. FFmpeg's dnxhd encoder stores the qscale of each macroblock
in a `uint8_t` (`libavcodec/dnxhdenc.h:101`) while its default qmax is 1024, so on such images it used to
abort (`put_bits.h` assertion), which killed the host; WriteFFmpeg now limits qmax to 256. The frames must
encode, or fail with "Cannot encode frame". It is a test of its own, so that an abort only fails it.

`test-writeffmpeg-begin-failure` (issue #289) runs `ofx_render_test OUT --begin-failure`: WriteFFmpeg renders a
movie, then on the same instance a beginSequenceRender fails (an interlaced source clip; DNxHD 36 in 720p, which only
exists in 1080p; the Codec Options `nitris_compat=2`, which FFmpeg's encoder rejects in `avcodec_open2()`), then the
host calls endSequenceRender, as Natron does. It used to write the trailer of a file whose header was never written
(with a NULL `AVIOContext`), and crash. The next render of the instance must work.

`test-writeffmpeg-dnxhd-profiles` (issue #305) runs `ofx_render_test OUT --dnxhd-profiles`: WriteFFmpeg encodes one
frame with each DNxHD Codec Profile, in 1920x1080 and 1280x720, at 23.976, 24, 25, 29.97, 30, 50, 59.94 and 60 fps
(the DNxHR profiles, which do not depend on the frame rate, at 23.976 fps only).
The compression ID (CID) in the frame header (offset 0x28, [SMPTE ST 2019-1](https://pub.smpte.org/latest/st2019-1/)
Table 8) must be the one of the profile: DNxHR 444, HQX, HQ, SQ and LB are CIDs 1270 to 1274; DNxHD 440x and 220x are
1235 in 1080p (220x is 1250 in 720p), 220 is 1238 (1251), 145 is 1237 (1252) and 36 is 1253. DNxHD 440x and 36 do not
exist in 720p: beginSequenceRender must fail with a message that says so. FFmpeg's encoder only uses the bitrate that
WriteFFmpeg sets to choose the CID (`ff_dnxhd_find_cid()` in `libavcodec/dnxhddata.c`), so a wrong bitrate makes
`avcodec_open2()` fail.

`test-writeffmpeg-encode-failure` (issue #288) runs `ofx_render_test OUT --encode-failure`: frames 2 to 4 are
rendered on other threads, which wait in `encode()` for frame 1, then frame 1 is aborted (the host's abort stays
true, as when the user cancels a render in Natron: `GenericWriterPlugin::render()` returns before `encode()`), or is
rendered to another file (it fails a check of `encode()`). The waiting renders must fail instead of waiting forever (a
deadlock in Natron, which waits for its render threads before endSequenceRender): the test fails after 60 s.
`test-writeffmpeg-encode-failure-disk-full` (`--encode-failure-disk-full`) does the same, with frame 1 failing to be
written because the file can not grow (`RLIMIT_FSIZE`, as on a full disk: the real write error path of avio, the
muxer and `writeToFile()`, without a test hook in the plug-in). It is skipped (exit status 77) on MS Windows, which
has no `RLIMIT_FSIZE`.

`test-swscale-exact` (issues #207, #220, #316) runs `ofx_render_test OUT --swscale-exact`. ReadFFmpeg reads rawvideo
(8 bits) and FFV1 (10 and 12 bits) written with libavcodec, without swscale (4:2:0, 4:2:2 and 4:4:4, 8, 10 and 12 bits, video and full range, with alpha,
odd widths), and must give the exact R'G'B' of [ITU-T H.273](https://www.itu.int/rec/T-REC-H.273) §8.3 in every pixel:
the last column included (#220: swscale's unscaled 4:2:0/4:2:2 path never wrote it at odd widths), rounded to the
nearest at 8 bits (#207), and white is 1 at more than 8 bits (#316: swscale gave 65283/65535,
[FFmpeg #25681](https://code.ffmpeg.org/FFmpeg/FFmpeg/issues/25681)). An MPEG-4 movie at 49x48 checks the last column of
a real codec. WriteFFmpeg's files, decoded without swscale: the 4:2:0 and 4:2:2 chroma of red and blue lines and columns
must be the average of the pixels (#207: `SWS_POINT` took one of them), and the Y'CbCr at 10 and 16 bits the exact one
(#316: white was 943 at 10 bits).

`test-writeffmpeg-param-change` (issue #304) runs `ofx_render_test OUT --param-change`: the frames of a movie are
rendered on a render thread, and between them the main thread calls `kOfxActionInstanceChanged` on the params and
the clip whose change updates the pixel format info (Codec, Format, the Pref. params, Show Avail., DNxHD Codec
Profile, Hap Format, the Source clip), as Natron does when the user edits them during a render, and
`kOfxActionSyncPrivateData`, as when the project is saved. Each action must succeed, every frame must be encoded and
the file complete: these actions used to free the format context of the render (`freeFormat()`), which made the next
frame fail ("file is not open") and lost the trailer, or, if a frame was being encoded, freed it under the render
thread.

`test-writeffmpeg-codec-params` (issue #343) runs `ofx_render_test OUT --codec-params`: for each encoder of the Codec
menu, it prints the encoder settings that WriteFFmpeg shows (`-` hidden, `d` disabled, `E` enabled: Output Quality,
Encoding Speed, Global Quality, Bitrate, Bitrate Tolerance, Quality, Keyframe Interval, Max B-Frames) and the pixel
format it selects for each Pref. Pixel Coding, Bit Depth and Alpha. Then it encodes 3 frames of a color pattern that
changes with the frame, in the first of mov, mkv, nut and avi whose muxer takes the codec (`avformat_query_codec()` and
`avformat_init_output()`), with the default settings, with Keyframe Interval = 1, Max B-Frames = 2 and Output Quality =
Lossless if they are shown, and with Alpha checked. Every movie must be encoded: a setting that is shown must work (e.g.
Max B-Frames, which the mpegvideo encoders without `AV_CODEC_CAP_DELAY` refuse), except Output Quality = Lossless with an
encoder that has no lossless mode (libvpx), which may fail with an error that says it. The test prints the codec, pixel
format, color tags, number of packets and of key packets, and a hash of the packets of each movie. libx264rgb is a known
failure ([#348](https://github.com/NatronGitHub/openfx-io/issues/348): WriteFFmpeg opens libx264, which refuses the RGB
pixel formats of libx264rgb), reported with its reason, and an unexpected pass if it encodes. To check that a change of
WriteFFmpeg keeps the settings and the movies of every encoder, diff the output of two builds.

`test-readffmpeg-destroy-closes-file` (issue #290) runs `ofx_render_test OUT --destroy-closes-file`: a ReadFFmpeg
instance renders a frame of a movie, which must then be open, and is destroyed: the movie must be closed before
kOfxActionUnload (the open files of ReadFFmpeg belong to the plug-in, keyed by the address of the instance, and used
to stay open until Unload). Twice, with the movie rewritten at another size in between: the second instance, which
may get the address of the first one (the test prints whether it does), must have the region of definition of the new
movie. "Open" is a file descriptor of the process on the same device and inode (`/proc/self/fd` on Linux, `/dev/fd`
on macOS), or, on MS Windows, a sharing violation when the test opens the file with `DELETE` access.

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
* **`clipToFile()`** (#232), called directly: float RGBA and 8-bit RGB images, with padded and negative row bytes,
  and files in, larger than, across, out of the render window, or empty: it returns the part of the window in the
  file, clears the rest of the window, and leaves everything else (and the row padding) untouched.
* **An Opaque file rewritten smaller between `getFrameBounds()` and `decode()`** (#232, the known issue of #225): a
  test reader whose `getFrameBounds()` is larger than the image that `decode()` reads, at scale 1 (`decode()` writes in
  the output image) and 1/2 (in GenericReader's buffer): alpha is 1 only where `decode()` wrote the image (the window
  that `clipToFile()` returned, or the one it reported with `setDecodedWindow()`), and the rest of the render window
  is transparent black.

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

### 1d2. `test-ocio-filetransform` — OCIOFileTransform's CCC Id param, with AddressSanitizer

`ocio_filetransform_test.cpp`, built with `-fsanitize=address` (not with MinGW), creates an OCIOFileTransform
instance (whose file name is empty) and changes the file name: the CCC Id param must be shown for a `.cdl`, `.cc` or
`.ccc` file only (the extension after the last `.`, in any case), not for `cc` or `x.icc`. `updateCCCId()` compared the end of the file name with the extensions with `std::equal` on the reversed
strings, which read before the start of a file name shorter than the extension, as the default, empty, one (an ASan
stack-buffer-underflow). With libstdc++ (Linux), ASan doesn't see that read, which stays inside the
`std::string` object (its short-string buffer): only libc++ (macOS) catches the old bug. It sets `OCIO=test.ocio`.

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

### 1f. `test-oiio-threads` — OpenImageIO's thread pool, shared by the OIIO plug-ins (#118, #250)

`oiio_threadpool_test.cpp` loads ReadOIIO, WriteOIIO and OIIOResize in the in-process
host of `ofx_test_host.h`, which share OpenImageIO's default thread pool, as in IO.ofx.
The instances of the OIIO plug-ins are counted (`OIIOThreadsUser`, OIIOGlobal.h): the first one starts the pool, and
the destruction of the last one stops it, for a host that exits without kOfxActionUnload (OpenImageIO requires its
pools to be shut down before exit, #250). Each OIIO plug-in also stops the pool in its unload action, but only if it
is the last OIIO plug-in loaded:
* after ReadOIIO's unload (and a reload and unload, as in a plug-in scan) while an OIIOResize instance exists, the
  pool must keep its `hardware_concurrency() - 1` threads and OIIOResize must render (`ImageBufAlgo::resize()`, with
  the host's 4 CPUs);
* with two instances, the destruction of one keeps the threads; the destruction of the last one stops the pool (0
  threads), while the plug-ins are still loaded, and a new instance starts it again;
* the unload of the last OIIO plug-in leaves it stopped;
* 8 threads create and destroy `OIIOThreadsUser` objects concurrently: the pool must run while each one exists, and
  be stopped at the end. `make test-oiio-threads-tsan` builds and runs it with
  [ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html) (ctest `test-oiio-threads-tsan`, with
  `IO_TESTS_SANITIZERS`).

It sets `OCIO=test.ocio`.

`make test-exit-without-unload` (ctest `test-exit-without-unload`, when the build has HostSupport) builds
`hostdemo/exit_without_unload.cpp`, a host built on HostSupport and linked with OpenImageIO, and runs it 10 times
(`hostdemo/exit_without_unload.sh`, with a 120 s watchdog each) with the bundle of `test-reader-window-host`: it renders
a TIFF file (written by `oiiotool`) with ReadOIIO, destroys the instance, and returns from `main()` without
kOfxActionUnload. Each run must exit. When the plug-in uses the program's OpenImageIO (the bundle is not deployed with
its own copy), and OpenImageIO can stop its pool (2.5.3 or later; not Ubuntu 24.04's 2.4), the pool must have
`hardware_concurrency() - 1` threads with the instance, and 0 after it was destroyed.

`make test-raw-unload` (ctest `test-raw-unload`, when the build has HostSupport, #239) builds `hostdemo/raw_unload.cpp`,
a host built on HostSupport, not linked with OpenImageIO, LibRaw or OpenMP (so that nothing else keeps them loaded),
and runs it 5 times (`hostdemo/raw_unload.sh`, with `OMP_NUM_THREADS=4` and `OMP_WAIT_POLICY=active`, and a 120 s
watchdog each) with the bundle of `test-reader-window-host`. It renders a 640x480 DNG file with ReadOIIO, destroys the
instance, unloads the plug-ins (kOfxActionUnload, then `dlclose()` or `FreeLibrary()`), keeps running for 3 s, and
exits. LibRaw's demosaic uses OpenMP on Linux (libgomp) and MS Windows (MSYS2's libgomp-1.dll), whose idle workers
outlive the render: ReadOIIO keeps the OpenMP runtime loaded (`pinOpenMPRuntime()`, OIIOGlobal.h). Each run checks:
* the OpenMP runtime loaded after the render is still loaded after the unload (the main check: an unloaded runtime does
  not always crash within the run); without one, the run prints a note, and fails with `IO_HOSTDEMO_STRICT=1` on Linux
  and MS Windows, where LibRaw uses OpenMP (so that the test can't silently stop exercising anything);
* the render was demosaiced by LibRaw: RGB or RGBA output, with R, G and B not all equal (a TIFF reader reads the CFA
  as one channel, which ReadOIIO outputs as Alpha);
* no crash, within the 3 s after the unload or at exit.

It also prints the thread counts (Linux, MS Windows) and whether the plug-in binary is still mapped after the unload: if
it is (e.g. its GNU unique symbols make `dlclose()` a no-op on Linux), the runtime stays loaded whatever the pin does.
macOS does not exercise the fix: MacPorts' LibRaw has no OpenMP; with `WITH_LIBRAW=1` the test bundle links libomp
itself (`tests/Makefile`), so the pin runs, but libomp then stays loaded with or without it; and the plug-in binary stays
mapped after `dlclose()` there, as on Linux ([#264](https://github.com/NatronGitHub/openfx-io/issues/264)).
The DNG file is written by `raw_unload --write-dng`: an uncompressed 16-bit RGGB CFA DNG with the minimal DNG tags,
which LibRaw reads (OpenImageIO's `raw` reader), since no RAW file can be committed, and a download would make the
test depend on the network.

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

### 1f3. `test-ofx14` — every plug-in in an OFX 1.4-only host

Every plug-in must work in a host that supports only OFX 1.4 or older: the features of OFX 1.5 and later, and the
Natron, Nuke, Vegas and other extensions, are optional. `ofx14_host_test.cpp` links every plug-in of `IO.ofx` (the
sources of `../IO/Makefile`, with OpenGL render support as in the bundle; the SeExpr plug-ins if SeExpr 2.11 is in
`SEEXPR_HOME`, the PNG ones if pkg-config finds libpng) with the host of `ofx_test_host.h` in its OFX 1.4-only mode
(`gOfx14Only`):

* `fetchSuite` returns NULL for every suite but the mandatory ones of OFX 1.4 (Property, ImageEffect, Parameter,
  Memory, MultiThread V1, Message V1 and V2, Interact V1): no OpenGL render suite, DrawSuite, MultiThread V2,
  multi-plane suite, camera suite...;
* any property that is not defined in the OFX 1.4 headers (`ofx14_names.h`, generated from the headers of
  [OFX_Release_1_4_TAG](https://github.com/AcademySoftwareFoundation/openfx/tree/OFX_Release_1_4_TAG/include),
  without the optional `ofxOpenGLRender.h`) is unknown, on every property set: reading it, or setting it, fails with
  `kOfxStatErrUnknown`. So the host has no Natron or other extension property, no colour management and no OpenGL;
* the plug-ins get the host from `OfxPlugin::setHost`; `OfxSetHost` (OFX 1.5) is never called.

A known property that was never set has its default value (0, "" or NULL), as in a real host, but only on the
property sets it belongs to (e.g. `kOfxParamProp*` on a param): on the host, on the action arguments, or on another
property set, it is unknown (`kOfxStatErrUnknown`), and so is its dimension.

For each plug-in: Load, Describe (and the overlay interact's Describe and CreateInstance, which must be an
OverlayInteractV1), DescribeInContext in each of its contexts, with an instance and an InstanceChanged on each of
its params, then in the filter context (filters) or the general context (readers, writers): CreateInstance,
GetClipPreferences, GetTimeDomain, GetRegionOfDefinition, GetRegionsOfInterest, GetFramesNeeded, IsIdentity,
Begin/EndSequenceRender, the render of one frame, DestroyInstance, Unload. Each must succeed without an error message;
filters and readers must write their whole output; the writers write the files that the readers then read (PNG, EXR,
PFM, TIFF, MOV), whose images must be the writer's input; the writers' time domain must be their input's frame range
(1..3: the default frame range is the time line's, and the host has no TimeLine suite, which is optional), and their
RoD the input's, also with the input format (`formatType` 0: the host has no `kOfxImageClipPropFormat`); the OCIO
plug-ins that are an identity with `test.ocio` must output their input. A plug-in of the binary without a test fails.

`make test-ofx14` runs it in four hosts:

* OFX 1.4 only;
* OFX 1.1 (`OFX14_LEVEL=11`): also without the properties of OFX 1.2 to 1.4 (e.g.
  `kOfxImageEffectPropSequentialRenderStatus`, `kOfxPropAPIVersion`), the Message suite V2 and RGB images;
* OFX 1.4 with the optional OpenGL render suite (`OFX14_GL=1`, which renders on the CPU), and a sequential render
  status of 0 (`OFX14_SEQSTATUS=0`);
* the full host (`--full`), where an OverlayInteractV2 also requires an OverlayInteractV1.

Other variants, from environment variables (see `ofx_test_host.h` and `ofx14_host_test.cpp`): `OFX14_NOSUITE` (suites
to drop), `OFX14_COMPS` and `OFX14_DEPTHS` (the host's components and depths), `OFX14_NOTILES`, `OFX14_NOMULTIRES`,
`OFX14_NOOVERLAYS`, `OFX14_SCALE` (the render scale, if the plug-in supports multiple resolutions, or always with
`OFX14_FORCESCALE`), `OFX14_SETINT` (`param=value,...`), `OFX14_SKIPPARAMS`, `OFX14_NOCTX` (no instance in every
context), and `--only <plug-in identifier>`. It sets `OCIO=test.ocio`; `make test-ofx14 SEEXPR_HOME=/opt/local` (the
[`seexpr211`](https://github.com/NatronGitHub/Natron/tree/RB-2.6/tools/MacPorts/graphics/seexpr211) port of Natron's
MacPorts tree) includes the SeExpr plug-ins.

`make test-ofx14-scan` runs the OFX 1.4-only and the full hosts with `--scan`: before the tests, every plug-in gets
Load and Describe, then they all get Unload, as in the plug-in scan of a host that unloads the binary after it (the
default of openfx's HostSupport since [openfx #174](https://github.com/NatronGitHub/openfx/pull/174), whose hostDemo
keeps it loaded with `OFX_HOSTDEMO_KEEP_LOADED=1`). Each test then loads, describes and uses its plug-in again, in the
same process, as a host does when the binary stays in memory (on macOS, a binary with thread-local variables is never
unmapped): the plug-ins' global state must survive Unload and be initialised again by Load and Describe. After the
scan, the Support library must have forgotten the host description, and the plug-ins must not have stopped OpenEXR's
global thread pool, which they did not start. `make test-ofx14-scan-asan` runs the same with
[AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html). The real unload of the binary (`dlclose`) is
tested by `io-hostdemo` (CMake, `IO_HOSTDEMO_TESTS`), whose hostDemo unloads it after its scan.

### 1g. `test-exr` — ReadEXR (#123)

`exr_read_test.cpp` writes EXR files with OpenEXR in a scratch directory (`.exr_read`),
and reads them with ReadEXR (linked with `GenericReader.cpp` and its dependencies) in
the in-process host of `ofx_test_host.h`, in the Reader context:

* **pixels**: FLOAT and HALF files, with ZIP, PIZ or no compression, and the data and
  display windows of `hostdemo/run.sh` (offset, crop, overscan): the RoD, and every
  pixel and channel of the image, must be exact (the display window moved to (0,0), y
  flipped, the 1-pixel border black); the channels that are not in the file are 0; UINT
  channels are normalized by 2^32-1, as in ReadOIIO; luminance/chroma files (Y, RY, BY)
  are read as before (not converted); channels whose names are empty once the digits and dots are
  removed (`1`, `2.3`, `.`, `..`), or not ASCII, are ignored (ReadEXR read out of bounds on them, #293).
* **render windows**: narrower than the data window (decode()'s buffer, in blocks of
  256 lines): exact inside, nothing written outside; render scale 1/2.
* **alpha 1 for files without alpha** (#234): an RGB and a grey PFM, TIFF, and an RGB PNG, read as RGBA with each
  `filePremult` (Opaque, PreMultiplied, UnPreMultiplied), without and with an OCIO conversion (from `x2` to `x1` of
  `swap.ocio`, next to `$OCIO`, which multiplies the colours by 2, so that GenericReader unpremultiplies, converts and
  premultiplies a PreMultiplied file): the colours are the file's (times 2 with OCIO), and alpha is 1. ReadPFM used
  to write alpha = 0, so with UnPreMultiplied, or PreMultiplied and OCIO, its colours were 0.
* **a rewritten file**: with larger and smaller windows, the next RoD and image are the new
  file's (the next render updates the header cache; so does the purge caches action); and
  with the RoD the host got before the rewrite, at scale 1 (straight into the output image,
  and through GenericReader's buffer) and 1/2: the whole render window is written, black
  out of the new frame bounds.
* **a reverted file**: rewritten with a smaller data window and rendered (the header cache
  then has the new header), then reverted to the first header, whose RoD the host still has:
  the render is the first image, not cropped to the cached data window (#163).
* **a truncated file**: the render must fail, with an error message.
* **concurrent renders**: 6 instances render 2 files (3 instances per file) 8 times each
  from 6 threads at once: each render must give the same image as a single-threaded one.
* **OpenEXR's thread pool** (`IOSupport/ExrThreads.h`, #250, #132): it is counted by the instances of its users
  (ReadEXR, WriteEXR, and the OIIO plug-ins through their `OIIOThreadsUser`). With the pool at 0 threads, as in a
  process where no OIIO instance ever existed, a ReadEXR instance starts it with `hardware_concurrency()` threads; the
  destruction of the last user stops it on MS Windows and leaves it elsewhere; a new instance starts it again; an OIIO
  instance created and destroyed while a ReadEXR instance exists (as Natron's Read node does with a temporary ReadOIIO)
  leaves it running (it is linked with `OIIOGlobal.cpp` for this).
* **ReadEXR renders while OIIO instances come and go**: 4 instances render 20 times each from 4 threads while another
  thread creates and destroys 400 `OIIOThreadsUser`, under a 300 s watchdog: stopping OpenEXR's pool while a render
  adds tasks could leave a task that no thread runs, and the render waiting forever.

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

### 1g. `test-reader-window` — render windows and scales of ReadOIIO, ReadPFM and ReadPNG (#160, #163, #232, #234)

`reader_window_test.cpp` writes files in a scratch directory (`.reader_window`): float TIFF (grey, RGB, RGBA), scanline
and tiled EXR with a data window smaller than the display window, and an EXR whose data window has a negative origin
and extends past the display window, with OpenImageIO, for ReadOIIO; an RGB and a grey PFM for ReadPFM; 8-bit RGB and RGBA, 16-bit
grey and RGBA, and interlaced (Adam7) 8-bit RGB and 16-bit RGBA PNG, with libpng, for ReadPNG (only if `pkg-config`
finds libpng). It is built with [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html) (not on MS
Windows), and the output image is between two guard pages (`mprotect()`, `VirtualProtect()` on MS Windows), flush
against one of them. Each file is read with two `filePremult` values, for both paths of `GenericReader::render()` at
scale 1: `decode()` into the output image (Opaque RGB files, PreMultiplied RGBA files), and into GenericReader's
buffer, which it then premultiplies (UnPreMultiplied files):

* **render scale 1/2**: GenericReader decodes a full-resolution window into its buffer: decode() gets a render scale
  of 1 (ReadOIIO and ReadPFM assert it, #160).
* **the file's pixels** at scale 1, including alpha: the file's alpha for RGBA files, 1 for Opaque RGB files (also in
  the black border that ReadOIIO adds around a data window, which is in the RoD), 0 out of the data window otherwise.
* **narrow render windows**: a few columns, a few rows, a box, a pixel, the edges, at scale 1 and 1/2, in an output
  image of the RoD (and, for ReadOIIO, which supports tiles, of the window): only the window is written, with the
  pixels of the render of the whole RoD (#163). ReadPNG reads a non-interlaced file only down to the last row of the
  window: these pixels are bit-identical to those of the whole image.
* **windows partly or entirely out of the RoD** (across an edge or a corner, longer than the RoD, the RoD with a
  border, beside it), in an image that covers the RoD and the window (and, for ReadOIIO, of the window only): the
  part in the RoD is the render of the whole RoD, the rest transparent black (#232).
* **a rewritten file**: smaller or larger, rendered with the RoD the host got before: only the window is written; for
  an Opaque RGB file (also a tiled EXR with a smaller data window, and an interlaced PNG), alpha is 1 in the new image
  and the rest of the window is transparent black.
* **concurrent renders** of narrow windows by 4 instances while another thread replaces the file by smaller and
  larger ones: only the windows are written, and alpha is 1 in the image, 0 in the black (the green of these files is
  never 0).

* **a 2-part EXR** (`READER_WINDOW_MULTIPLANE=1`, a second run in a Natron-like host: dynamic choices and the plane
  suite): part 1, smaller than part 0, read into the color plane as Opaque: alpha is 1 in the whole RoD (the union of
  the parts), also out of part 1.
* **no dependence on `kOfxImageEffectPropSequentialRenderStatus`**: the render of the whole RoD is the same, bit for
  bit, with a sequential render status of 1. The spec says it is "set to indicate whether the effect is currently
  being rendered in frame order on a single effect instance", not playback, and the readers don't read it (#200).

`READER_WINDOW_ONLY=text` runs only the tests whose title contains `text`. `READER_WINDOW_DUMP=dir` writes the render
of the whole RoD of each test to `dir` (raw RGBA float), to compare two builds bit by bit. `test-render` also renders
narrow windows of ReadFFmpeg, at scale 1 and 1/2. It sets `OCIO=test.ocio`.

### 1g1. `test-png-corrupt` — ReadPNG and WritePNG on libpng errors (#235)

libpng reports an error by `longjmp()`ing to `png_jmpbuf(png)`, which must be set with `setjmp()` in the function that
makes the libpng call ([libpng-manual.txt](https://github.com/pnggroup/libpng/blob/libpng16/libpng-manual.txt),
"Setup"). `png_corrupt_test.cpp` (with AddressSanitizer) writes two valid 8-bit RGB PNG files with libpng (one IDAT
chunk, and IDAT chunks of 256 bytes) in `.png_corrupt`, and reads them with ReadPNG. It then makes corrupt files from
them: an invalid IHDR with a valid CRC (zero width, bit depth 7), a bad IHDR CRC, files truncated in the middle of IHDR
and just after it, garbage after the signature, a bad CRC in the only IDAT chunk and in the first of several, IDAT
data truncated, corrupt zlib data (with a valid CRC), a bad signature and an empty file. For each one, it creates a
ReadPNG instance, asks for the Image Info, the RoD and renders it: the RoD (corrupt header) or the render (corrupt
image data) must fail with a persistent error message that has libpng's message (`libpng error: ...`). It also
renders an image wider than libpng's limit (1000001 pixels) with WritePNG, which must fail with libpng's message. It
then reads all the corrupt files 8 more times, and checks that the memory allocated
(`__sanitizer_get_current_allocated_bytes()`) and the lowest free file descriptor are the same as before (without
AddressSanitizer, e.g. on MS Windows, only the file descriptors are checked). It sets `OCIO=test.ocio`.

### 1g2. `test-reader-window-host` — the readers in a host built on HostSupport (#232)

`hostdemo/reader_window.cpp` (adapted from openfx-arena's `tests/readerwindow/reader_window.cpp`) is a host built on
openfx's HostSupport and hostDemo's host classes (`openfx/HostSupport/examples`; hostDemo itself renders the whole RoD
of a plug-in without tiles, whatever its `--window`). `make test-reader-window-host` builds it (with expat, from
`pkg-config`) and a bundle of ReadOIIO, ReadPFM, ReadPNG and ReadFFmpeg (`.reader_window_host/plugins`), then
`hostdemo/reader_window.sh` writes files with `oiiotool` (8-bit RGB and 16-bit RGBA PNG, float RGB and RGBA TIFF, an
EXR with a data window, a tiled EXR), `reader_window --write-pfm` (an RGB PFM: OpenImageIO 2.4's oiiotool, in Ubuntu
24.04, has no PFM writer) and `ffmpeg` (a ProRes MOV); a file that can't be written is skipped, with the reason (with
`IO_HOSTDEMO_STRICT=1`, as in CI, it fails the test, as does a missing bundle or oiiotool, or no passed run). It
renders each reader with both `filePremult` paths, at scale 1 and 1/2. A reference render of the whole RoD, then
windows in the image (the middle, edges, a row, a column, a pixel) and partly or entirely out of the RoD, each in a
new image of the RoD and the window (and, for a reader that supports tiles, of the window only), filled with
hostDemo's NaN sentinel: the window must have the reference's bits in the RoD and transparent black out of it, and
nothing outside the window may be written. The OFX spec allows such windows: "A host can ask an effect to render an
arbitrary window of pixels, generally these should be clipped to an effect's region of definition, however, depending
on the host, they may not be. [...] If an effect is asked to render outside of its region of definition, it should
fill those pixels in with black transparent pixels"
([ofxRendering.rst](https://github.com/AcademySoftwareFoundation/openfx/blob/main/Documentation/sources/Reference/ofxRendering.rst)).
It is skipped without `oiiotool`, and ReadFFmpeg without `ffmpeg`. ctest runs it as `test-reader-window-host` when the
build has HostSupport (`-DIO_HOSTDEMO_TESTS=ON`, as in CI), and `io-reader-window` runs the same script on the
installed `IO.ofx` bundle. The 2-part EXR run of `test-reader-window` is `test-reader-window-multiplane`.

### 1g. `test-seexpr` — SeExpr, SeNoise and SeGrain

`seexpr_test.cpp` links `SeExpr.cpp`, `SeNoise.cpp` and `SeGrain.cpp` with SeExpr 2.11 (the SeExpr 1 API, found in
`SEEXPR_HOME`, as for the plug-ins; the test is skipped if it is not there; ctest runs it as `test-seexpr`
with the SeExpr of the CMake build), in the Filter context:

* **frames needed**: the frames that `cpixel()` fetches, at frame 10, are found by an analysis of the parse tree
  (no NaN, no sampling): the values of the local variables (and of `frame` once assigned) are followed through the
  assignments and if blocks, computed from `frame` and constants only, and every call is analyzed, whether or not it
  would be evaluated (`x > 1e6 ? cpixel(1, frame + 3, x, y) : ...` gives 13). Known: `frame - 2`,
  `f = frame - 2; ... f` (8), `o = 3`, `frame -= 2`, `y = 2; ... frame - y`, both branches of `x > 0 ? frame :
  frame + 1` or of an if, `(1 || x) * 5`, `(x > 0 || frame > 0)`, `printf()` (0), `pick(frame, 0, 3)` (the builtins
  that convert an argument to int, with valid arguments). Anything that depends on another variable gives the input
  frame range (-5..5) and frame 10: `x`, `round(clamp(u, 0, 1) * 10)` (slit-scan), `Cs[0] - Cs[1]`, `x = x + frame`,
  a pixel value; and the builtins with invalid arguments (`pick(1, 0, 1e9)`, whose alloca overflowed the stack). The
  564 cases of the review corpus where an earlier version gave fewer frames than master (`seexpr_test_master.inc`,
  generated with master) must have at least the frames of master, or be a documented NaN artifact (`kNaNArtifacts`:
  master evaluated the expression with NaN for the other variables, e.g. `frame + max(x, 0)` gave the frame); the
  cases that this build of SeExpr can not parse or prep are skipped (with MacPorts' SeExpr 2.11, 533 are checked, 1
  is a NaN artifact and 30 are skipped).
* **render**: `cpixel()` with coordinates of 1e9, 1e12 or -1e300 gives the edge pixel, as before (they are clamped to
  +-1e9 before their conversion to int); a finite frame (1e12) is used as is, an infinite one is clamped; a NaN
  coordinate or frame (`sqrt(x - 100)`) gives black.
* **SeNoise** with a noise size of 0, a denormal size (1e-310) or a render scale of 0, and **SeGrain** with a render
  scale of 0: no output value may be NaN or infinite. A positive noise size below 0.001 (0.0005) is used as is.

It also passes when built with `-O3 -ffast-math` and with `-fsanitize=undefined,float-cast-overflow`.

### 2. `test-ffmpeg` — FFmpeg reader core (direct coverage of the reader fixes)

`ffmpeg_read_test.cpp` links `FFmpeg/FFmpegFile.cpp` directly (plus `ofx_stubs.cpp`
for the one Support symbol it references, `OFX::MultiThread::getNumCPUs`). It
decodes sequentially for a ground-truth per-frame signature, then re-decodes out of
order (backward and long jumps seek; short forward jumps may decode forward, without a
seek) and asserts frame-accuracy; `--monotonic` also checks the
sequential signatures strictly increase. `--ramp OFFSET,STEP` checks that every
decoded frame f, sequential or out of order, holds the content of that frame
(OFFSET + STEP * (f - 1)), which catches a wrong frame as well as a missing one,
`--frames N` checks the number of frames the reader reports, and `--positions
P0,P1,...` describes a variable frame rate clip with timestamp gaps: ramp frame i is
at frame Pi, and a frame in a gap must be identical to the last ramp frame at or
before it, as players display it (and, with `--ramp`, hold its content).
`--max-seeks N` checks that decoding every frame sequentially seeks at most N times
(`FFmpegFile::getSeekCount()`), and `--order-seek F1,F2,...` and `--order-no-seek
F1,F2,...` that the requests of these `--order` frames seek, or do not;
`--order-no-start-seek` that no `--order` request seeks to the start of the file
(`FFmpegFile::getStartSeekCount()`), and `--order-max-seeks N` that none seeks more than N times. `--matrix N` and `--range N` set the Y'CbCr matrix
(0 = Auto, 1 = Rec. 601, 2 = Rec. 709, 3 = Rec. 2020) and range (0 = Auto, 1 = Video,
2 = Full) of the conversion to RGB (`FFmpegFile::setColorOverrides()`, the ReadFFmpeg
"YCbCr Matrix" and "YCbCr Range" params), and `--rgb R,G,B[,TOL]` checks the mean RGB
of the first and the last frame (0-255 scale, default tolerance 1.5). `--bit-depth N` checks
the bit depth the reader reports (`FFmpegFile::getBitDepth()`), and `--fine-ramp MAX_STEP,MIN_LEVELS`
that the first frame, a horizontal gray ramp with one code value per column, decodes to at
least MIN_LEVELS distinct values (more than 256 needs more than 8 bits) with steps of at most
MAX_STEP (0-255 scale) between the means of adjacent columns. `run_ffmpeg_read_test.sh` uses
them on 10-bit VP9 (4:2:0 and 4:2:2), AV1 (decoded by libdav1d), HEVC Main 10 (libx265, or
VideoToolbox on macOS) and H.264 High 10, 12-bit VP9, 16-bit Bayer FFV1, and an 8-bit VP9
control (issue #213: the VP9, HEVC and libdav1d decoders leave `bits_per_raw_sample` at 0, and
4:2:0 10-bit was decoded to 8 bits). With `--skip-without-decoder`, a stream that the linked
libavcodec cannot decode (e.g. AV1 without libdav1d) exits with status 77 (skipped).

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
cut are counted (they fail).
NUT stores the start of the last packet of any stream as its duration (#215), and without an index,
the PTS of the last syncpoint (#331). NUT clips must count all their frames, and the frames held by
a longer audio track:
- FFV1, also with a non-zero start time and with PCM audio of the same length or longer;
- MPEG-4, ProRes, and MPEG-4 and H.264 with B-frames (MPEG-4 with B-frames has no start time);
- without an index: FFV1, also with a non-zero start time, and MPEG-4 and H.264 with B-frames;
- larger than the 4 MB that the end probe reads (about 10 MB of FFV1), with and without an index,
  also with a non-zero start time and with 3 s of audio;
- a single GOP larger than the probe (about 10 MB of lossless H.264), with and without an index (only
  the frame count is checked: every seek in a single GOP decodes from its start).

An MPEG-TS with 1 s of video and 8 s of SMPTE 302M audio, whose end probe fails, must count 192
frames. An MPEG-2 elementary stream with B-frames has no timestamps, and its last frame has no PTS;
MP4 edit lists that do not display the first 1 or 3 frames, and a MOV edit list that does not
display the first 2 (negative timestamps), must not count these frames; a 150-frame MPEG-TS and
M2TS (H.264 with B-frames, AC3 audio) stream-copied with `-output_ts_offset 95440` and `95441`, so
that their 33-bit timestamps wrap during the clip (libavformat then gives the frames before the wrap
negative timestamps), must count all 150 frames and give exactly the images of the clean clip in
every order, with 1 and 8 threads (issue #189: only the MOV/MP4 demuxer has edit-list pre-rolls); AVI (MPEG-4 with B-frames) has no PTS, and every frame must
still decode at its own frame.
Any FFmpeg decoder is used ([#339](https://github.com/NatronGitHub/openfx-io/issues/339)): DV, UtVideo in AVI,
and H.264 (with and without B-frames), MPEG-2 with B-frames and MPEG-1 in AVI must be read frame-accurately (with
H.264 B-pyramids the decoded frames are 2 frames behind the DTS, and the frame count must not lose the last 2; with
MPEG-1, only the first packet buffered at open has a PTS, and the reader probes again after a seek). `ffmpeg_read_test --image-files NAME=0|1...` checks which files
`FFmpegFile::isImageFile()` takes for single images: the extensions of FFmpeg's image2 muxer, except those that a
demuxer declares (AVIF, animated JPEG XL).
Forward requests ([#311](https://github.com/NatronGitHub/openfx-io/issues/311)): a request
up to the largest keyframe interval seen (in packets, at least 1 s and at most 10 s) after the
held frame decodes forward, without a seek, unless the index shows a keyframe between. H.264
with a GOP of 50 frames in MP4, Matroska and MPEG-TS is decoded in the order 10, 12, 15, 60,
55: 12 and 15 must not seek, 60 (after the next keyframe) and 55 (backward) must; and in
the order 30, 40, 45, where 40 and 45 must not seek. An H.264
MPEG-TS with a 20 s timestamp jump and an MPEG-4 ASF whose index is removed (with `perl`)
must seek for requests far ahead, and an HEVC MP4 with open GOPs must decode forward for
requests close ahead, also leading pictures; every frame must be the sequential one, with 1
and 8 decoder threads.
Leading pictures of open GOPs ([#328](https://github.com/NatronGitHub/openfx-io/issues/328)):
HEVC (x265 `open-gop=1:bframes=3`) and H.264 (x264 `open-gop=1`, 3 B-frames) in MP4, with a GOP
of 24 frames, are decoded in the orders 10, 23, 26, 47, 50, 70 (leading pictures after a far
frame), 70, 47, 23, 95, 72, 46, 22, in reverse order and in a random order, with 1 and 8 decoder
threads. The MP4 index is on the DTS, so a seek for a leading picture finds the keyframe displayed
after it: every frame must be the sequential one, decoded from the keyframe before, with at most 2
seeks per request and none to the start of the file. Open-GOP H.264 in MPEG-TS, whose index is not
used for this, must not seek more than twice per request either. The script first checks with
`ffprobe` that each clip has leading pictures (a keyframe packet followed by a lower PTS).
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
Damaged packets (issue #151): the `noise` bitstream filter (with expressions) modifies every byte of chosen packets, which the decoder rejects with
AVERROR_INVALIDDATA: an H.264 P-frame, IDR frame or B-frame in Matroska, two B-frames in a VFR
Matroska clip, and two frames of ProRes (MOV) and MJPEG (AVI), each with 1 and 8 decoder
threads (`FFMPEG_READ_TEST_THREADS`, read by `ofx_stubs.cpp`). The reader must drop them and
go on decoding: the frame time of a damaged packet shows the previous frame, with its ramp
content (`--gaps`, or `--positions` for the VFR clip), sequentially and after a seek (for the
intra-only codecs, the reader decodes from the previous frames). The other frames of the GOP
of a damaged H.264 frame (and the last frames of the previous GOP, which the decoder outputs
after the next reference frame) show what the decoder conceals: they must decode, but their
content is not checked (`--concealed`). With a damaged first GOP, its frames must fail with
an error message, without making the file invalid (`FFmpegFile::isInvalid()`, which makes the
plug-in refuse every frame), and the other frames must decode (`--fail`). An AV1 clip (libdav1d,
which reports damage as AVERROR(ENOMEM) with threads; the second half of two packets is
scrambled with `perl`, since the muxers reject AV1 packets damaged by `noise`) must decode every
frame. Truncated ProRes and MJPEG in MOV (faststart, cut in frame 16, a packet that the demuxer
flags as corrupt) must fail for the missing frames, with an error, sequentially and after a
seek, as for issue #134. A run of 4 damaged ProRes and MJPEG frames must show the frame before
them, also when each one is decoded first in a newly opened file (`--fresh`, in reverse and in
another order). A truncated H.264 MP4 (faststart, with B-frames) must fail from the DTS of the
truncated packet, found in the index at open, in any render order. In a 300-frame ProRes MOV with packets 100-199 damaged, the frames after the run must
decode, sequentially and decoded first in a newly opened file, with 1 and 8 threads; in one
damaged from its 7th packet on, decoding every frame sequentially, and in reverse order in a newly
opened file, must read at most 12 times the file (`--max-seq-bytes`, `--max-reverse-bytes`: the
reader remembers the damaged ranges, instead of reading the damaged packets again for each frame).
In one with packets 100-179 and 181-183 damaged, decoding frames 101, 184, 181 and 182 in that
order in a newly opened file (`--order`) must fail for 101, and give frame 181 for the other
three: with frame threads, the error of a damaged packet comes with a later packet, and the good
frame 181 must not join a damaged range. In a long-GOP H.264 MP4 (GOP 300) with a damaged first keyframe, every
frame must fail, and decoding every frame sequentially and in reverse order must read at most 4
times the file (the frames that cannot be decoded from the start of the file are not decoded again
for each request). In a Matroska H.264 clip with a GOP of 100 and B-frames, with packets 50-150
damaged, frames 49 and 52 (decoded before the run, and still in the decoder when it gives up after
too many damaged packets) must decode in any order, and frames 53-200 must fail. In a 300-frame ProRes MOV with packets 10-74 and 76 damaged (a run of more than 64
packets), frames 11-75 must fail with 1 and with 8 decoder threads (with frame threads, the
packets after the run were already sent when the error of the 65th damaged packet came back).
In a 12 s H.264 Matroska clip with a GOP of 100 and B-frames, where 200 bytes at offset 8 of the
packets 50-99, 50-113, 80-119 or 120-190 are overwritten with random bytes (`perl`, seeded; the
damage also hits the Matroska block headers, which gives keyframe flags to damaged packets, and
damaged packets are parsed as H.264 parameter sets), the frames that the reader decoded before
damaged packets were skipped (1-48 and 101-300, 1-48 and 201-300, 1-78 and 201-300, 1-118 and
201-300) must give exactly the images of the clean file (`--reference`), sequentially, in reverse
order, in a random order, with frame 120 then frame 1 in a newly opened file, and each frame in
its own newly opened file, with 1 and 8 threads; the same without cues (a stream copy written to
a pipe: the Matroska index is then only built from the blocks read, including the damaged ones), for
the packets 50-113 and 80-119, and in MP4 (a stream copy, packets 80-119, including the IDR frame of
packet 100). Opening a Matroska file with a single keyframe, and one without cues (20 s of 160x120
noise, about 9 MB), must read at most 1 MB, and decoding frame 3 after the open at most 1.5 times
the file. With only the flags byte of the block headers of the packets 80-119 damaged (some values
turn on lacing: blocks are lost or merged), frames 1-80, 101 and 201-300 must give the clean images
in every order. In a 400-frame H.264 Matroska clip with a GOP of 5 and B-frames, with every
keyframe from packet 20 damaged (the decoder then outputs frames whose best_effort_timestamp is the
DTS of a later packet), frames 1-18 must give the clean images in every order (the others fail:
the stream ends in damage). In an AVI with MPEG-4 B-frames (the reference frames have no PTS) with
packets 60-62 damaged, frames 1-59 and 73-200 must give the clean images in every order. Clean
48-frame VP9 clips (issue #159; 256x160, GOP 12, `libvpx-vp9` in 2 passes, since libvpx only makes
alt-ref frames in 2 passes), in WebM and Matroska, in 8 and 10 bits, with alt-ref frames (superframes,
which the VP9 decoder splits into a hidden and a shown frame) and without (`-auto-alt-ref 0`), and an
AV1 clip (`libaom-av1`), must give exactly the images of their lossless copy (FFV1, or lossless VP9
for 10 bits, which the reader decodes to 8 bits), decoded by the ffmpeg CLI with 1 thread (`--reference`), in every order, with 1, 4 and 8 decoder threads: with frame
threads, the VP9 decoder refuses a packet with AVERROR(EAGAIN) after a superframe, and the reader lost
it when the drain gave the requested frame. In all-intra H.264
(`-g 1`, 120 frames) with every other packet from packet 20 damaged, every frame must decode
(the damaged ones show the previous frame), and decoding every frame sequentially, and in reverse
order, must read at most 6 times the file. Intra-only codecs that the reader finds from the codec descriptor
(`AV_CODEC_PROP_INTRA_ONLY`; before, only ProRes, DNxHD, MJPEG, PNG, DPX, TGA and TIFF were known):
v210 in MOV, rawvideo, HuffYUV and JPEG-LS in AVI, JPEG 2000 in Matroska and Hap in MOV, 48 frames
with packets 10-12 and 30 damaged, must give the clean images for the other frames in every order,
with 1 and 8 threads, and a truncated v210 MOV (faststart, cut in frame 16) must fail from frame 16, as
ProRes and MJPEG. An MPEG-TS with a continuity counter error (a packet flagged
as corrupt, but not at the end of the file) must decode every frame, with 1 and 8 threads.
Matroska and WebM store timestamps in milliseconds: at 24 fps the third frame is at
83 ms, which `Stream::ptsToFrame()` must round to frame 2 (it used to truncate it to
frame 1, so that every third frame could not be decoded), and the movie duration
(1667 ms for 40 frames) must not count a 41st frame. Clips whose encoder is missing
from the `ffmpeg` CLI are skipped. `FFMPEG_READ_TEST_KEEP=dir` keeps the clips.
Y'CbCr matrix and range (`check_color`): clips of a constant Y'CbCr color (Y' 120, Cb 80,
Cr 200, written as raw yuv444p bytes with `perl`, in lossless H.264 with the tags in the
VUI, and in 10-bit FFV1 in Matroska with the tags in the container only), tagged bt709,
bt470bg, smpte170m, bt2020nc, fcc and smpte240m, in video and full range, and untagged
(64x48 and 1280x720), must decode to the RGB computed by the script from the Kr and Kb of
the matrix (ITU-T H.273) and from the range, not with FFmpeg (each clip is first checked to
decode to the samples it was encoded from): Auto uses the tags, else Rec. 601 below 720
lines and Rec. 709 from 720 lines, in video range, and `--matrix`/`--range` override them.

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

`io-hostdemo-ofx14` runs the same tests in `hostDemo --ofx14` (`HOSTDEMO_ARGS=--ofx14`, work directory
`build/hostdemo-ofx14`), a host that implements only OFX 1.4: only the OFX 1.4 host properties and suites (no
DrawSuite, plane suite V2 or Vegas suites), no `OfxSetHost`, no StrChoice or Bytes params, and one view (its output
files are `<prefix>.<t>.ppm`, without the `l`/`r` suffix). Readers, writers and filters must work there as in the
default host.

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

### 5. FFmpeg version compile check

The FFmpeg plug-ins support [FFmpeg](https://ffmpeg.org) 8.0 and later (libavcodec 62.11.100, libavformat 62.3.100,
libavutil 60.8.100 and libswscale 9.1.100, the versions of the [n8.0 release](https://github.com/FFmpeg/FFmpeg/blob/n8.0/doc/APIchanges)):
`FFmpeg/FFmpegFile.h` stops with an `#error` with an older FFmpeg, and the CMake configuration and the Makefiles with
an error message. CI builds with FFmpeg 8.0 (Linux, [Ubuntu 26.04](https://packages.ubuntu.com/resolute/libavcodec-dev))
and 9 (macOS and Windows). `ffmpeg_syntax_check.sh` compiles `WriteFFmpeg.cpp`, `ReadFFmpeg.cpp`, `FFmpegFile.cpp` and
`PixelFormat.cpp` with `-fsyntax-only` against the FFmpeg found by `pkg-config`, e.g. to check a given FFmpeg version
after changing the FFmpeg plug-ins. Run it from the repository root (with the submodules), with `PKG_CONFIG_PATH` set
to the FFmpeg to check, or in a container, e.g. with [podman](https://podman.io) (or docker):

```sh
# a local FFmpeg build, installed in $prefix
PKG_CONFIG_PATH=$prefix/lib/pkgconfig sh tests/ffmpeg_syntax_check.sh c++
# FFmpeg 8.0 on Ubuntu 26.04
podman run --rm -v "$PWD":/src:ro -w /src docker.io/library/ubuntu:26.04 sh -c '
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
| `seexpr_test.cpp`           | SeExpr's frames needed and `cpixel()` domain, SeNoise and SeGrain with degenerate params (`test-seexpr`); `seexpr_test_master.inc`: the frames needed on master for the review corpus. |
| `reader_hooks_test.cpp`     | GenericReader's `isOutputFrameVarying()`, `getRenderTime()`, `refreshFrameBounds()` and `clipToFile()` hooks, on test readers. |
| `ocio_cdl_test.cpp`         | OCIOCDLTransform's Export param: the .cc file it writes is read back by OCIO. |
| `ocio_filetransform_test.cpp` | OCIOFileTransform's CCC Id param, shown only for .cdl/.cc/.ccc files, with AddressSanitizer (`test-ocio-filetransform`). |
| `exr_read_test.cpp`         | ReadEXR (`test-exr`, `test-exr-tsan`, `test-exr-tsan-debug`, `bench-exr`): pixels, windows, UINT, luminance/chroma, a truncated file, concurrent renders, and timings. |
| `reader_window_test.cpp`    | ReadOIIO, ReadPFM and ReadPNG (`test-reader-window`): narrow render windows, windows out of the RoD, render scale 1/2, rewritten files, with ASan and guard pages. |
| `png_corrupt_test.cpp`      | ReadPNG and WritePNG on libpng errors (`test-png-corrupt`): corrupt and truncated PNG files, an image too wide for libpng, with ASan and a leak check. |
| `hostdemo/reader_window.cpp`, `hostdemo/reader_window.sh` | The readers' render windows in a host built on HostSupport (`test-reader-window-host`, ctest `io-reader-window`). |
| `oiio_edge_test.cpp`        | OIIOResize and OIIOText with degenerate inputs (`test-oiio-edge`). |
| `ocio_colorspace_anim_test.cpp` | GenericOCIO's animated colorspaces (OCIOColorSpace, WriteOIIO, ReadOIIO) and Swap Colorspaces, with a Natron-like animated param suite. |
| `ocio_anim_fuzz.cpp`        | Random user actions on the same host, checking invariants (`test-ocio-anim`). |
| `ofx14_host_test.cpp`       | Every plug-in in an OFX 1.4-only host, and in the full host (`test-ofx14`). |
| `ofx14_names.h`             | The names defined by the OFX 1.4 headers, for the OFX 1.4-only mode of `ofx_test_host.h`. |
| `oiio_layers_test.cpp`      | ReadOIIO's EXR channel name -> view/layer mapping (OpenEXR and Nuke multi-view layouts), on in-memory `ImageSpec`s. |
| `test.ocio`                 | Minimal OpenColorIO config for the render test (GenericOCIO needs `$OCIO`). |
| `swap.ocio`                 | OpenColorIO config where xN is N times the reference (`test-ocio-anim`, `hostdemo/run.sh`). |
| `ofx_entrypoints_test.cpp`  | OFX C-ABI enumeration/identity unit test. |
| `ffmpeg_read_test.cpp`      | Standalone FFmpeg reader-core harness (links `FFmpegFile.cpp`). |
| `ofx_stubs.cpp`             | Minimal OFX Support stub (`getNumCPUs`) for the reader-core harness. |
| `run_ffmpeg_read_test.sh`   | Generates known clips and runs the reader harness. |
| `run_oiio_roundtrip_test.sh`| OpenImageIO format roundtrip sanity check. |
| `ffmpeg_syntax_check.sh`    | Compile check of the FFmpeg plug-ins against the FFmpeg found by pkg-config (e.g. a given FFmpeg version). |
| `hostdemo/run.sh`           | Renders every plug-in of the installed bundle with openfx's hostDemo (ctest `io-hostdemo`). |
| `verify_plugin_loads.cpp`   | Loads an `IO.ofx` with openfx's HostSupport and lists its plug-ins (ctest `io-load`, the CI load checks, `tools/compare_bundles.sh`); linked with a copy of openfx's `ofxhBinary.cpp` that uses `RTLD_NOW` instead of `RTLD_LAZY`, so that a missing symbol makes loading fail. |
| `check_icons.cmake`         | Every plug-in of the CMake build tree's bundle has its icon, `Contents/Resources/<identifier>.png` (ctest `io-icons`). |
| `hostdemo/compare.py`       | Compares a reader's images with its writer's, from hostDemo's `--report` files (for `run.sh`). |
| `CMakeLists.txt`            | The same test programs and runs, as ctest tests of the CMake build (`test-*`). |
| `run_test.cmake`            | ctest helper: empties a work directory, then runs a test program (`test-exr`, `test-ofx14`, ...). |
| `Makefile`                  | `make test` / `test-ffmpeg` / `test-oiio` / `test-ofx` / `test-render` / `test-dnxhr444-noise` / `test-writeffmpeg-begin-failure` / `test-writeffmpeg-dnxhd-profiles` / `test-writeffmpeg-encode-failure` / `test-writeffmpeg-encode-failure-disk-full` / `test-writeffmpeg-param-change` / `test-writeffmpeg-codec-params` / `test-readffmpeg-destroy-closes-file` / `test-layers` / `test-reader-hooks` / `test-ocio-cdl` / `test-ocio-anim` / ... (without CMake). |

## Notes / limitations

* The OFX action tests link a curated subset of dependencies (no LibRaw/OpenMP,
  no OpenGL overlay path). This is enough for the entry points; a full bundle build
  (`make` at the repo root) additionally needs LibRaw, SeExpr, OpenEXR, OpenGL, etc.
* Building the loadable `IO.ofx` and running it in a real host (Natron/Nuke) remains
  the way to test end-to-end rendering with UI; the harnesses here cover the
  library/entry-point layers without a host.
