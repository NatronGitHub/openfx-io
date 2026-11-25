openfx-io [![GPL2 License](http://img.shields.io/:license-gpl2-blue.svg?style=flat-square)](https://github.com/NatronGitHub/openfx-io/blob/master/LICENSE) [![Open Hub](https://www.openhub.net/p/openfx-io/widgets/project_thin_badge?format=gif&ref=Thin+badge)](https://www.openhub.net/p/openfx-io?ref=Thin+badge) [![CI](https://github.com/NatronGitHub/openfx-io/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/NatronGitHub/openfx-io/actions/workflows/ci.yml)
=========

A set of Readers/Writers plugins written using the [OpenFX](https://openfx.readthedocs.io/) standard.

License
-------

<!-- BEGIN LICENSE BLOCK -->
Copyright (C) 2013-2018 INRIA

openfx-io is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

openfx-io is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with openfx-io.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
<!-- END LICENSE BLOCK -->

## Getting the sources from github

To fetch the latest sources from github, execute the following commands:

    git clone https://github.com/NatronGitHub/openfx-io.git
    cd openfx-io
    git submodule update --init --recursive

GitHub's source archives (the "Source code" tarballs and zips of releases and tags, and `archive/<ref>.tar.gz`) don't
contain the submodules, and they have no `.git`, so `git submodule update` can't fetch them. Either clone the tag
with its submodules:

    git clone --recursive --branch <tag> https://github.com/NatronGitHub/openfx-io.git

or extract the archive of each submodule into its path, at the commit that GitHub shows next to its directory in the
parent repository's tree (e.g. "openfx @ <sha>" in openfx-io's tree at that tag; tinydir's parent is SequenceParsing):

- `openfx`: [NatronGitHub/openfx](https://github.com/NatronGitHub/openfx)
- `SupportExt`: [NatronGitHub/openfx-supportext](https://github.com/NatronGitHub/openfx-supportext)
- `IOSupport/SequenceParsing`: [NatronGitHub/SequenceParsing](https://github.com/NatronGitHub/SequenceParsing)
- `IOSupport/SequenceParsing/tinydir`: [NatronGitHub/tinydir](https://github.com/NatronGitHub/tinydir)

In order to get a specific tag, corresponding to a source release, do `git tag -l`
to get the list of tags, and then `git checkout tags/<tag_name>`
to checkout a given tag.

## Requirements

- A C++17 compiler. The plug-ins also compile as C++20.
- On Linux, GCC 9 or later (`std::filesystem` without `-lstdc++fs`;
  [OpenImageIO](https://github.com/AcademySoftwareFoundation/OpenImageIO/blob/main/INSTALL.md) 3.x needs 9.3 or later)
- [OpenColorIO](https://opencolorio.org) 2.x
- [OpenImageIO](https://github.com/AcademySoftwareFoundation/OpenImageIO) 2.x or 3.x, with
  [LibRaw](https://www.libraw.org) for the RAW formats
- [OpenEXR](https://openexr.com) and [Imath](https://github.com/AcademySoftwareFoundation/Imath)
- [FFmpeg](https://ffmpeg.org) 8.0.1 to 9 (the [n8.0 release](https://github.com/FFmpeg/FFmpeg/blob/n8.0/doc/APIchanges)'s
  libavcodec 62.11.100, libavformat 62.3.100, libavutil 60.8.100 and libswscale 9.1.100, or later), but not 8.0 itself,
  whose libswscale truncates in its range conversions ([fixed in
  8.0.1](https://github.com/FFmpeg/FFmpeg/commit/44bfe0da6167c19221810f4865ba578e3ddd2c39)). The build stops with an
  older FFmpeg, and the FFmpeg plug-ins don't load if the FFmpeg libraries found at run time are 8.0 (this only
  matters for builds that use the system's FFmpeg, such as unbundled Linux builds; a host's plug-in cache may hide the
  failed load until the cache is cleared).
  [`tests/ffmpeg_syntax_check.sh`](tests/ffmpeg_syntax_check.sh) checks that the FFmpeg plug-ins compile against a given
  FFmpeg version
- [libpng](http://www.libpng.org/pub/png/libpng.html)
- [SeExpr](https://github.com/wdas/SeExpr) [2.11](https://github.com/wdas/SeExpr/releases/tag/v2.11), which has the SeExpr 1 API (`SeExpression.h`); SeExpr 3.x
  (the SeExpr2 API) does not work

The [CI workflow](.github/workflows/ci.yml) builds and tests the plug-ins on Linux (Ubuntu packages),
on Windows with [MSYS2](https://www.msys2.org) [UCRT64](https://www.msys2.org/docs/environments/), and on macOS with
[Xcode](https://developer.apple.com/xcode/)'s clang and [MacPorts](https://www.macports.org). It is the reference for the
packages to install and the commands to run on each platform. It builds with [CMake](https://cmake.org) and tests with
[`ctest`](https://cmake.org/cmake/help/latest/manual/ctest.1.html): a Release C++17 build on every push and pull
request, and weekly (on Saturdays) also a Debug C++20 build, with the same tests. The Makefiles are only checked weekly on
Linux (a Release build of the plug-ins). The weekly run builds on Linux, Windows and macOS every week, since Ubuntu's
updates, MSYS2 and MacPorts move, and builds Natron's Linux testing bundle (`linux-natron`) only if master changed since
the last weekly run.
[Natron](https://github.com/NatronGitHub/Natron) 2.6 builds the plug-ins on [Rocky Linux](https://rockylinux.org) 9 (see [Linux runtime requirements](#linux-runtime-requirements)), on macOS 14 and later (arm64), and on
MSYS2 UCRT64.

### Linux runtime requirements

Linux release bundles carry their libraries (OpenImageIO, OpenColorIO, FFmpeg, …) but no system libraries, which
come from the system:

- [glibc](https://sourceware.org/glibc/) 2.34 or later;
- [libstdc++](https://gcc.gnu.org/onlinedocs/libstdc++/) and `libgcc_s` from [GCC](https://gcc.gnu.org) 11 or later;
- [libgomp](https://gcc.gnu.org/onlinedocs/libgomp/), GCC's [OpenMP](https://www.openmp.org) runtime, when LibRaw
  uses OpenMP, and `libatomic` (GCC), when a library needs it (e.g. Ubuntu 26.04's GLib);
- `libGL` and `libGLX` ([libglvnd](https://gitlab.freedesktop.org/glvnd/libglvnd)),
  [libdrm](https://gitlab.freedesktop.org/mesa/drm) (`libdrm.so.2`, `libdrm_*`), and
  [Mesa](https://www.mesa3d.org)'s `libgbm` and `libglapi`: they must match the system's graphics driver (Mesa's
  drivers load in the same process, and a bundled older copy breaks them);
- `libudev` (from [systemd](https://systemd.io): it reads the running udev's database, whose format follows the
  system's udev) and [libselinux](https://github.com/SELinuxProject/selinux);
- the GPU drivers: `libcuda`, NVIDIA's `libnvidia-*` and `libnvcuvid`, and the OpenCL drivers (the vendor ICDs:
  `libnvidia-opencl`, `libamdocl*`, `libigdrcl`, `libintelocl`, `libMesaOpenCL`, `libRusticlOpenCL`, `libpocl`), which
  the OpenCL ICD loader finds through `/etc/OpenCL/vendors/*.icd` and `dlopen()`s;
- the desktop libraries, which must match the system's display server and sound system: the X11 client libraries
  (`libX*`, e.g. `libX11`, `libX11-xcb`, `libXext`, `libXfixes`, `libXrender`, `libXau`, `libXdmcp`, and
  [libxcb](https://xcb.freedesktop.org) and its extensions, e.g. `libxcb-render`, `libxcb-shm`, `libxcb-dri3`: a
  bundled libxcb older than the system's breaks the system's libX11), `libSM`, `libICE`,
  [libxkbcommon](https://xkbcommon.org) and `libxkbcommon-x11`, and [ALSA](https://www.alsa-project.org)'s
  `libasound`.

These are the system libraries of openfx's
[`linuxCheckBundle.sh`](https://github.com/NatronGitHub/openfx/blob/master/Support/include/linuxCheckBundle.sh), which
must never be bundled, plus the desktop libraries, as in Natron's Linux installer (`SYSTEM_LIBS_RE` in
[`tools/jenkins/build-Linux-installer.sh`](https://github.com/NatronGitHub/Natron/blob/RB-2.6/tools/jenkins/build-Linux-installer.sh)).
Everything else is bundled, including the [OpenCL ICD loader](https://github.com/OCL-dev/ocl-icd) (`libOpenCL.so.1`,
needed by some FFmpeg builds such as Ubuntu's libavutil): it is vendor-neutral and not in a default install (Ubuntu's
[`ocl-icd-libopencl1`](https://packages.ubuntu.com/noble/ocl-icd-libopencl1), Rocky Linux's `ocl-icd`), and it finds
the system's OpenCL drivers by itself.

Release builds target [Rocky Linux](https://rockylinux.org) [9](https://docs.rockylinux.org/releases/release_notes/9_0/)
(glibc 2.34, GCC 11), with [GCC Toolset](https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/9/html/developing_c_and_cpp_applications_in_rhel_9/assembly_additional-toolsets-for-development-rhel-9_developing-applications)
(e.g. `gcc-toolset-14`) if a newer compiler is needed: GCC Toolset links the newer parts of the C++ runtime statically,
so the plug-ins still only need the system's libstdc++. Their symbol versions must not be newer than those of Rocky
Linux 9: `GLIBC_2.34` (Rocky Linux 9's glibc 2.34 also has a few backported `GLIBC_2.35` symbols, e.g. `__memcmpeq`),
`GLIBCXX_3.4.29` and `CXXABI_1.3.13` (libstdc++), `GCC_7.0.0` (libgcc_s, x86-64), `GOMP_5.0.1` and `OMP_5.0.1`
(libgomp). The Linux bundles built by CI are testing builds, not releases (see [Natron's testing
bundles](#natrons-testing-bundles)).

#### The Linux testing bundles

CI's Linux testing bundles (see [Natron's testing bundles](#natrons-testing-bundles)) follow the same rules, with
Ubuntu's libraries. Each bundled library comes with its licence, in `IO.ofx.bundle/Contents/Resources/Licenses` (see
[Natron's testing bundles](#natrons-testing-bundles)). The system libraries that they need today are provided by the
Ubuntu (and Debian) packages
`libc6 libstdc++6 libgcc-s1 libgomp1 libatomic1 libgl1 libdrm2 libselinux1 libudev1 libx11-6 libx11-xcb1 libxau6
libxdmcp6 libxext6 libxfixes3 libxrender1 libxcb1 libxcb-dri3-0 libxcb-render0 libxcb-shm0`
(`RUNTIME_PACKAGES` in [`check_testing_bundle.sh`](.github/workflows/check_testing_bundle.sh), also printed in the
bundles' `README.txt`). CI checks each testing bundle in two ways:

1. strictly: every library that a file of the bundle needs (`DT_NEEDED`) and that the bundle doesn't carry must be one
   of the sonames that these packages provide (`RUNTIME_SONAMES`);
2. it loads `IO.ofx` (with `RTLD_NOW`) in a clean `ubuntu:24.04` or `ubuntu:26.04` container where only these packages
   and their dependencies are installed (`libgl1`, for instance, brings Mesa's GLX driver).

The other system libraries above (e.g. `libgbm`, `libglapi`, `libdrm_*`, `libEGL`, `libOpenGL`, `libGLES*`,
`libcrypt`, the GPU drivers and OpenCL ICDs, `libSM`, `libxkbcommon`, `libasound`) are never bundled either: when a
bundled library first needs one, it must be added to `RUNTIME_SONAMES` and `RUNTIME_PACKAGES`, and here.

The testing bundles also carry Ubuntu's [libva](https://github.com/intel/libva) (`libva.so.2`, `libva-drm.so.2`,
`libva-x11.so.2`) and `libvdpau`, because Ubuntu's libavutil.so.58 needs them (and libavcodec.so.60 needs
`libva.so.2`). A bundled libva only finds the VA-API drivers of the distribution it was built on, but nothing uses
them: the plug-ins don't use FFmpeg's hardware decoding, and neither does OpenImageIO.

## Compiling with CMake

[CMake](https://cmake.org) (3.21 or later) builds the combined `IO.ofx` bundle, with the same plug-ins, sources and defines as
the Makefiles. It needs an out-of-tree build directory:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release [-DSEEXPR_HOME=...] [-DFFMPEG_PKG_CONFIG_PATH=...]
    cmake --build build
    cmake --install build --prefix <prefix>   # installs <prefix>/OFX/Plugins/IO.ofx.bundle

`PLUGIN_INSTALLDIR` (default `OFX/Plugins`) is relative to the install prefix. Use the prefix of the
standard OFX plug-in folder of each OS, as `make install` does:
- Linux, FreeBSD: `sudo cmake --install build --prefix /usr` (`/usr/OFX/Plugins`)
- macOS: `sudo cmake --install build --prefix /Library` (`/Library/OFX/Plugins`; `/usr` is read-only on
  macOS)
- Windows: `cmake --install build --prefix "C:/Program Files/Common Files"`
  (`C:/Program Files/Common Files/OFX/Plugins`)

The bundle is also assembled in `build/Plugins/IO.ofx.bundle`. A group of plug-ins is left out when its
libraries are not found, unless `OFX_IO_REQUIRE_ALL` is ON. Options:

| Option | Default | Description |
|---|---|---|
| `OFX_IO_WITH_<group>` | ON | Build the group if its libraries are found. Groups: `OCIO`, `EXR`, `FFMPEG`, `OIIO`, `LIBRAW` (RAW support in ReadOIIO), `PNG`, `SEEXPR`, `RUNSCRIPT`. |
| `OFX_IO_REQUIRE_ALL` | OFF | Fail if a group that is not disabled can't be built (for packagers). |
| `SEEXPR_HOME` | empty (CMake's search paths) | SeExpr 2.11 prefix (`include/SeExpression.h`, `lib/libSeExpr`). |
| `FFMPEG_PKG_CONFIG_PATH` | empty (pkg-config's search path) | pkg-config directory of the FFmpeg to use, e.g. `/opt/local/libexec/ffmpeg8/lib/pkgconfig` for MacPorts' [`ffmpeg8`](https://ports.macports.org/port/ffmpeg8/) port, which MacPorts' OpenImageIO links. |
| `CMAKE_OSX_DEPLOYMENT_TARGET` | `14.0` when building natively on macOS, unless the `MACOSX_DEPLOYMENT_TARGET` environment variable is set and non-empty; no default when cross-compiling to another system. An existing build directory keeps its cached value: use a new one, or pass `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0` | Minimum macOS version of the plug-ins ([`CMAKE_OSX_DEPLOYMENT_TARGET`](https://cmake.org/cmake/help/latest/variable/CMAKE_OSX_DEPLOYMENT_TARGET.html)). |
| `OFX_CXX_STANDARD` | `CMAKE_CXX_STANDARD`, or 17 | C++ standard of the plug-ins and of the OFX Support library. |
| `OFX_DEPLOY_BUNDLES` | OFF | Make the installed bundle self-contained (macOS: [`osxDeploy.sh`](https://github.com/NatronGitHub/openfx/blob/master/Support/include/osxDeploy.sh), like the Makefiles' release builds). |
| `PLUGIN_INSTALLDIR` | `OFX/Plugins` | Where the bundle is installed (relative to `CMAKE_INSTALL_PREFIX`). |
| `OFX_PLUGIN_RPATH` | empty | Extra install rpath directories of the plug-in (`;`-separated; ignored on Windows). |
| `OFX_USE_OPENMP` | ON | Link [OpenMP](https://www.openmp.org) when LibRaw's pkg-config file asks for it. |
| `BUILD_TESTING` | ON | Build the test programs of [`tests/`](tests) ([`tests/CMakeLists.txt`](tests/CMakeLists.txt), the same as [`tests/Makefile`](tests/Makefile)'s), with this build's compiler, C++ standard and FFmpeg, and run them with `ctest` (`test-*`, besides `io-load` and `io-icons`). See [tests/README.md](tests/README.md). |
| `IO_TESTS_WITH_LIBRAW` | ON | The tests that link ReadOIIO also build its RAW support, when the bundle has it (`tests/Makefile`'s `WITH_LIBRAW=1`). |
| `IO_TESTS_TIMEOUT` | 1800 | Time limit of each `test-*` test, in seconds (`test-ffmpeg`: twice as much), so that a hung test fails. |
| `IO_TESTS_FUZZ_OPS` | 2000 | Operations of each short fuzz run of `test-ocio-anim` (`tests/Makefile`'s `FUZZ_OPS`). |
| `IO_TESTS_SANITIZERS` | OFF | Also build and run the sanitizer variants that `make test` doesn't run: `test-ofx14-scan-asan`, `test-exr-tsan`, `test-exr-ubsan` and `test-exr-tsan-debug` (not with MinGW). |
| `IO_HOSTDEMO_TESTS` | OFF | Build openfx's [hostDemo](https://github.com/NatronGitHub/openfx/tree/master/HostSupport/examples); `ctest` renders every plug-in of the installed bundle with it (`io-hostdemo`, [`tests/hostdemo/run.sh`](tests/hostdemo/run.sh)), and again in an OFX 1.4-only host, `hostDemo --ofx14` (`io-hostdemo-ofx14`). Needs `BUILD_TESTING`. |

## Compiling with Makefiles (Linux, FreeBSD, macOS, MSYS2)

The plugins can be compiled by typing in a terminal:
- `make [options]` to compile as a single combined plugin (see below
  for valid options).
- `make nomulti [options]` to compile as separate plugins (useful if
only a few plugins are needed, for example). `make` can also be
executed in any plugin's directory.

The most common options are `CONFIG=release` to compile a release
version, `CONFIG=debug` to compile a debug version. Or
`CONFIG=relwithdebinfo` to compile an optimized version with debugging
symbols.

Another common option is `BITS=32` for compiling a 32-bits version,
`BITS=64` for a 64-bits version, and `BITS=Universal` for a universal
binary (macOS only).

The libraries are found with [`pkg-config`](https://www.freedesktop.org/wiki/Software/pkg-config/). Other useful variables are `SEEXPR_HOME` (the SeExpr 2.11
prefix, default `/usr`) and `FFMPEG_PKG_CONFIG_PATH` (as for CMake, above). On macOS, `MACOSX_DEPLOYMENT_TARGET` (default `14.0`; make variable or
environment variable) is the minimum macOS version of the plug-ins. The Makefiles compile as
C++17 by default (`-std=c++17`, so plain `make CONFIG=release` works even
with compilers that default to an older standard, such as Apple clang),
unless `CXX`, `CXXFLAGS_ADD` or `CXXFLAGS_EXTRA` already contains a `-std=`
option (or set `CXXSTD`). To compile as
C++20, use e.g. `make CONFIG=release CXXSTD=c++20` (or `CXXSTD=-std=c++20`) or
`make CONFIG=release CXX="clang++ -std=c++20"`.
See the file [`Makefile.master`](Makefile.master) in the toplevel directory for other useful
flags/variables.

The compiled plugins are placed in subdirectories named after the
configuration, for example `Linux-64-release` for a 64-bits Linux
compilation. In each of these directories, a `*.bundle` directory is
created, which has to be moved to the proper place
(`/usr/OFX/Plugins` on Linux, `/Library/OFX/Plugins` on macOS, or
`C:\Program Files\Common Files\OFX\Plugins` on Windows), using
a command like the following, with the *same* options used for
compiling:

	sudo make install [options]

## Platform notes

See the [CI workflow](.github/workflows/ci.yml) for the complete package lists and build commands.

- **Linux**: install the development packages of the libraries above from the distribution. On [Ubuntu
  26.04](https://packages.ubuntu.com/resolute/) (whose [FFmpeg](https://packages.ubuntu.com/resolute/libavcodec-dev) is 8.0.1): `cmake libgl-dev libegl-dev libopencolorio-dev libopenimageio-dev libopenexr-dev libimath-dev libraw-dev
  libpng-dev libwebp-dev libtiff-dev libopenjp2-7-dev libavcodec-dev libavformat-dev libavutil-dev
  libswscale-dev`, plus `libfmt-dev libexpat1-dev ffmpeg openimageio-tools` for the tests and hostDemo.
  Ubuntu doesn't package SeExpr 2.11: CI builds it from source (with [`bison`](https://www.gnu.org/software/bison/) and [`flex`](https://github.com/westes/flex)) and passes its
  prefix in `SEEXPR_HOME`.
- **macOS**: CI uses MacPorts and Xcode's clang (`/usr/bin/clang++`), with `/opt/local/bin` first in
  `PATH` so that MacPorts' `pkg-config` and `cmake` are used:
  `port install pkgconfig cmake openimageio opencolorio openexr libraw ffmpeg8 libpng libomp`.
  Link the FFmpeg plug-ins with the FFmpeg that OpenImageIO links
  (`FFMPEG_PKG_CONFIG_PATH=/opt/local/libexec/ffmpeg8/lib/pkgconfig`). If they would link two different
  FFmpeg builds with the same library names, the build stops, because the bundle could only contain one of
  them. For SeExpr 2.11, install the [`seexpr211`](https://github.com/NatronGitHub/Natron/tree/RB-2.6/tools/MacPorts/graphics/seexpr211) port of
  Natron's MacPorts tree ([`tools/MacPorts`](https://github.com/NatronGitHub/Natron/tree/RB-2.6/tools/MacPorts) in the
  Natron repository, RB-2.6 branch) and use `SEEXPR_HOME=/opt/local`; CI instead builds SeExpr 2.11 from source with that
  port's patches.
  The plug-ins target macOS 14.0 (`MACOSX_DEPLOYMENT_TARGET` for the Makefiles,
  `CMAKE_OSX_DEPLOYMENT_TARGET` for CMake; see Apple's
  [macOS Deployment Target](https://developer.apple.com/documentation/xcode/build-settings-reference#macOS-Deployment-Target)
  setting), as Natron 2.6 does. MacPorts builds its libraries for the build machine's macOS unless the
  `macosx_deployment_target` option (read by [`macports.tcl`](https://github.com/macports/macports-base/blob/master/src/macports1.0/macports.tcl),
  not documented) is set in [`macports.conf`](https://guide.macports.org/chunked/internals.configuration-files.html):
  check the `minos` of the bundled libraries with `otool -l` (`LC_BUILD_VERSION`); a library that requires a newer macOS
  prevents the plug-ins from loading on older versions. After setting `macosx_deployment_target`, rebuild the installed ports from source
  (`sudo port -s upgrade --force installed`). With `OFX_DEPLOY_BUNDLES`, CMake warns about such libraries, and fails with
  `-DOFX_DEPLOY_STRICT_MINOS=ON` (recommended for release packages); the Makefile bundles (`osxDeploy.sh`) are not checked.
- **MS Windows**: build in an [MSYS2](https://www.msys2.org) [UCRT64](https://www.msys2.org/docs/environments/) shell, with the `mingw-w64-ucrt-x86_64-` packages of the
  libraries above. MSYS2 packages SeExpr 2.11 ([`mingw-w64-ucrt-x86_64-seexpr`](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-seexpr)), so `SEEXPR_HOME` is
  `$MINGW_PREFIX`, as a Windows path for CMake (with [Ninja](https://ninja-build.org)):
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSEEXPR_HOME="$(cygpath -m "$MINGW_PREFIX")"`,
  or `make CONFIG=release SEEXPR_HOME="$MINGW_PREFIX"`.

## IDE projects

The repository has no IDE project files. Generate one from the CMake build instead:
`cmake -S . -B build-xcode -G Xcode` for an [Xcode](https://developer.apple.com/xcode/) project on macOS, with the
same `-D` options as above and `/opt/local/bin` first in `PATH` (the libraries are found through their CMake config files
or `pkg-config`). [Xcode](https://cmake.org/cmake/help/latest/generator/Xcode.html) is a multi-configuration generator,
so `CMAKE_BUILD_TYPE` is ignored: choose the configuration when building (`cmake --build build-xcode --config Release`,
`ctest --test-dir build-xcode -C Release`) or in the Xcode scheme. Any IDE that reads `CMakeLists.txt` directly also works. On MS Windows, the only supported toolchain is MSYS2 UCRT64 (above);
MSVC and Visual Studio projects are not supported.

## Testing

The tests in [`tests/`](tests) link the plug-in sources with a minimal in-process host and need no OFX host
application. The CMake build builds them (`BUILD_TESTING`, above), and `ctest` runs them, as CI does:
`ctest --test-dir build --output-on-failure`. Without CMake: `cd tests; make test`. See
[tests/README.md](tests/README.md).

### Natron's testing bundles

[Natron](https://github.com/NatronGitHub/Natron)'s CI ([`.github/workflows/ci.yml`](https://github.com/NatronGitHub/Natron/blob/RB-2.6/.github/workflows/ci.yml)
on RB-2.6) tests Natron with the plug-ins of the assets of openfx-io's
[`natron_testing`](https://github.com/NatronGitHub/openfx-io/releases/tag/natron_testing) release:

| Asset | Natron's job | Built by (CI job, artifact) |
|-------|--------------|-----------------------------|
| `openfx-io-build-ubuntu_24-testing.zip` | Linux, `ubuntu-24.04` | `linux-natron` (Ubuntu 24.04, FFmpeg 9.0.2 built from source and bundled, since Ubuntu 24.04's FFmpeg 6.1 is too old), `openfx-io-build-ubuntu_24-testing` |
| `openfx-io-build-windows_ucrt64-testing.zip` | Windows, MSYS2 | `windows` (MSYS2 UCRT64), `openfx-io-build-windows_ucrt64-testing` |

Natron unzips each asset into its `Plugins` directory: the zip holds `IO.ofx.bundle` (with its libraries) and a
`README.txt` at its root.

Each testing bundle carries the licences of its libraries in `IO.ofx.bundle/Contents/Resources/Licenses`, where
`LIBRARIES.txt` gives the package and version of each library (`copy_bundled_licenses.sh`, checked by
`check_bundled_licenses.sh`, in [`.github/workflows`](.github/workflows)):
- Ubuntu's libraries: the [Debian copyright file](https://www.debian.org/doc/debian-policy/ch-docs.html) of each
  package, and in `common-licenses` the licences of `/usr/share/common-licenses` that these files refer to (GPL-2,
  LGPL-2.1, Apache-2.0...);
- the [FFmpeg](https://ffmpeg.org/legal.html) and [SeExpr](https://github.com/wdas/SeExpr) that CI builds: their
  licence files;
- Windows: the licence files (`share/licenses`) and the [`pacman -Qi`](https://man.archlinux.org/man/pacman.8)
  information (`PACKAGE-INFO.txt`) of each [MSYS2](https://www.msys2.org/) package. Some MSYS2 packages (e.g. ffmpeg,
  giflib) install no licence file: for those, when pacman reports SPDX ids (or legacy names that map to them), `spdx`
  has the texts of these licences (from [SPDX's licence list](https://github.com/spdx/license-list-data)), but not the
  projects' copyright notices.

The other artifacts (`openfx-io-build-ubuntu_26-testing` of the `linux` job, which needs Ubuntu 26.04, and the macOS
bundle) are not used by Natron. CI uploads the bundles only for tags and manual runs, and the assets are updated by
hand, from a run of the commit to test (e.g. master, after a merge that Natron's tests need):

```sh
gh workflow run ci.yml -R NatronGitHub/openfx-io --ref master   # then wait for the run: gh run watch
run=$(gh run list -R NatronGitHub/openfx-io --workflow ci.yml --event workflow_dispatch --branch master --limit 1 --json databaseId --jq '.[0].databaseId')
for a in ubuntu_24 windows_ucrt64; do
  gh run download "$run" -R NatronGitHub/openfx-io -n "openfx-io-build-$a-testing" -D "$a"
  (cd "$a" && zip -qr "../openfx-io-build-$a-testing.zip" .)
  gh release upload natron_testing "openfx-io-build-$a-testing.zip" -R NatronGitHub/openfx-io --clobber
done
```

[`tools/compare_bundles.sh`](tools/compare_bundles.sh) `BUNDLE_A BUNDLE_B` compares two bundles on Linux or macOS, e.g. the `IO.ofx.bundle`
built by the Makefiles and the one installed by CMake: file list, `Info.plist`, resources, exported
symbols, linked libraries and rpaths, and the plug-ins each binary loads. It also checks that each bundle
is self-contained. Run it without arguments for its options. On macOS, configure CMake with
`-DOFX_DEPLOY_BUNDLES=ON` for this comparison: the Makefiles' release builds deploy the bundle
([`osxDeploy.sh`](https://github.com/NatronGitHub/openfx/blob/master/Support/include/osxDeploy.sh)), and without that option the bundle installed by CMake is not self-contained, so all
its libraries differ.
