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
- [FFmpeg](https://ffmpeg.org) 4.0 to 9 ([`tests/ffmpeg_syntax_check.sh`](tests/ffmpeg_syntax_check.sh) checks that the FFmpeg plug-ins
  compile against a given FFmpeg version)
- [libpng](http://www.libpng.org/pub/png/libpng.html)
- [SeExpr](https://github.com/wdas/SeExpr) [2.11](https://github.com/wdas/SeExpr/releases/tag/v2.11), which has the SeExpr 1 API (`SeExpression.h`); SeExpr 3.x
  (the SeExpr2 API) does not work

The [CI workflow](.github/workflows/ci.yml) builds and tests the plug-ins on Linux (Ubuntu packages),
on Windows with [MSYS2](https://www.msys2.org) [UCRT64](https://www.msys2.org/docs/environments/), and on macOS with
[Xcode](https://developer.apple.com/xcode/)'s clang and [MacPorts](https://www.macports.org). It is the reference for the
packages to install and the commands to run on each platform. It builds with [CMake](https://cmake.org) and tests with
[`ctest`](https://cmake.org/cmake/help/latest/manual/ctest.1.html): a Release C++17 build on every push and pull
request, and nightly also a Debug C++20 build, with the same tests. The Makefiles are only checked nightly on Linux
(a Release build of the plug-ins).
[Natron](https://github.com/NatronGitHub/Natron) 2.6 builds the plug-ins on [Rocky Linux](https://rockylinux.org) 9 (see [Linux runtime requirements](#linux-runtime-requirements)), on macOS 14 and later (arm64), and on
MSYS2 UCRT64.

### Linux runtime requirements

Linux release bundles carry their libraries (OpenImageIO, OpenColorIO, FFmpeg, …) but no system libraries, which
come from the system:

- [glibc](https://sourceware.org/glibc/) 2.34 or later;
- [libstdc++](https://gcc.gnu.org/onlinedocs/libstdc++/) and `libgcc_s` from [GCC](https://gcc.gnu.org) 11 or later;
- [libgomp](https://gcc.gnu.org/onlinedocs/libgomp/), GCC's [OpenMP](https://www.openmp.org) runtime, when LibRaw
  uses OpenMP;
- `libGL` (e.g. [libglvnd](https://gitlab.freedesktop.org/glvnd/libglvnd)).

Release builds target [Rocky Linux](https://rockylinux.org) [9](https://docs.rockylinux.org/releases/release_notes/9_0/)
(glibc 2.34, GCC 11), with [GCC Toolset](https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/9/html/developing_c_and_cpp_applications_in_rhel_9/assembly_additional-toolsets-for-development-rhel-9_developing-applications)
(e.g. `gcc-toolset-14`) if a newer compiler is needed: GCC Toolset links the newer parts of the C++ runtime statically,
so the plug-ins still only need the system's libstdc++. Their symbol versions must not be newer than those of Rocky
Linux 9: `GLIBC_2.34` (Rocky Linux 9's glibc 2.34 also has a few backported `GLIBC_2.35` symbols, e.g. `__memcmpeq`),
`GLIBCXX_3.4.29` and `CXXABI_1.3.13` (libstdc++), `GCC_7.0.0` (libgcc_s, x86-64), `GOMP_5.0.1` and `OMP_5.0.1`
(libgomp). The Linux bundle built by CI is an Ubuntu 24.04 testing build, not a release: it needs the system libraries
of Ubuntu 24.04 or later.

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
C++20, use e.g. `make CONFIG=release CXXSTD=-std=c++20` or
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
  24.04](https://packages.ubuntu.com/noble/): `cmake libgl-dev libegl-dev libopencolorio-dev libopenimageio-dev libopenexr-dev libimath-dev libraw-dev
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
  The plug-ins target macOS 14.0 (`MACOSX_DEPLOYMENT_TARGET` for the Makefiles and Xcode,
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

## Using Xcode on macOS

In order to use the provided Xcode project file ([`IO.xcodeproj`](IO.xcodeproj)), you should add the
following definitions in Xcode's settings
(Settings > Locations > Custom Paths):
- EXR_PATH: path to OpenEXR installation prefix (e.g. /usr/local or /opt/local)
- FFMPEG_PATH: path to FFmpeg installation prefix  (e.g. /usr/local or /opt/local)
- OCIO_PATH: path to OpenColorIO installation prefix (e.g. /usr/local or /opt/local)
- OIIO_PATH: path to OpenImageIO installation prefix (e.g. /usr/local or /opt/local)
- SEEXPR_PATH: path to SeExpr installation prefix (e.g. /usr/local or /opt/local)

## Testing

The tests in [`tests/`](tests) link the plug-in sources with a minimal in-process host and need no OFX host
application. The CMake build builds them (`BUILD_TESTING`, above), and `ctest` runs them, as CI does:
`ctest --test-dir build --output-on-failure`. Without CMake: `cd tests; make test`. See
[tests/README.md](tests/README.md).

[`tools/compare_bundles.sh`](tools/compare_bundles.sh) `BUNDLE_A BUNDLE_B` compares two bundles on Linux or macOS, e.g. the `IO.ofx.bundle`
built by the Makefiles and the one installed by CMake: file list, `Info.plist`, resources, exported
symbols, linked libraries and rpaths, and the plug-ins each binary loads. It also checks that each bundle
is self-contained. Run it without arguments for its options. On macOS, configure CMake with
`-DOFX_DEPLOY_BUNDLES=ON` for this comparison: the Makefiles' release builds deploy the bundle
([`osxDeploy.sh`](https://github.com/NatronGitHub/openfx/blob/master/Support/include/osxDeploy.sh)), and without that option the bundle installed by CMake is not self-contained, so all
its libraries differ.
