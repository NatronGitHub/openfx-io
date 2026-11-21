#!/bin/bash
# Check that a Linux testing bundle loads on a clean system that has only the documented runtime requirements
# ("Linux runtime requirements" in README.md), so that a library that is neither bundled nor documented fails CI
# (#270: libOpenCL.so.1, NEEDED by Ubuntu's libavutil, was not bundled, and the runners have it installed).
#
#   check_testing_bundle.sh <IO.ofx.bundle> <docker image, e.g. ubuntu:24.04> <verify_plugin_loads>
#   check_testing_bundle.sh --packages    # prints RUNTIME_PACKAGES, on one line
#
# 1. The strict part: every NEEDED library of the bundle's ELF files that is not in the bundle must be in
#    RUNTIME_SONAMES.
# 2. Even if 1. failed: in a clean container of the image (Ubuntu or Debian), with RUNTIME_PACKAGES and their
#    dependencies installed (e.g. libgl1 brings Mesa's GLX driver), verify_plugin_loads (built on the runner, with
#    RTLD_NOW) loads IO.ofx and lists its plug-ins. This also catches what 1. can't see, e.g. a missing symbol.
# Requires readelf (binutils) and docker.

set -euo pipefail

# The system libraries that the testing bundles need today and don't carry (keep in sync with find_and_copy_deps.py
# and "Linux runtime requirements" in README.md): glibc, the GCC runtime, GLVND and libdrm, libselinux and libudev,
# the X11 client libraries. The other system libraries, which are never bundled either (libgbm, libglapi, libdrm_*,
# libEGL, libOpenGL, libGLES*, libcrypt, the GPU drivers and OpenCL ICDs, libSM, libICE, libxkbcommon, libasound...),
# must be added here, with their packages, when a bundled library first needs them.
RUNTIME_SONAMES="ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libmvec.so.1 libdl.so.2 libpthread.so.0 librt.so.1
  libresolv.so.2 libutil.so.1
  libstdc++.so.6 libgcc_s.so.1 libgomp.so.1 libatomic.so.1
  libGL.so.1 libGLX.so.0 libGLdispatch.so.0 libdrm.so.2
  libselinux.so.1 libudev.so.1
  libX11.so.6 libX11-xcb.so.1 libXau.so.6 libXdmcp.so.6 libXext.so.6 libXfixes.so.3 libXrender.so.1
  libxcb.so.1 libxcb-dri3.so.0 libxcb-render.so.0 libxcb-shm.so.0"
# The Ubuntu (and Debian) packages that provide them.
RUNTIME_PACKAGES="libc6 libstdc++6 libgcc-s1 libgomp1 libatomic1 libgl1 libdrm2 libselinux1 libudev1
  libx11-6 libx11-xcb1 libxau6 libxdmcp6 libxext6 libxfixes3 libxrender1 libxcb1 libxcb-dri3-0 libxcb-render0 libxcb-shm0"
RUNTIME_PACKAGES=$(echo "$RUNTIME_PACKAGES" | tr -s ' \n' '  ' | sed 's/^ //; s/ $//')

if [ $# -eq 1 ] && [ "$1" = --packages ]; then
  echo "$RUNTIME_PACKAGES"
  exit 0
fi
if [ $# -ne 3 ]; then
  echo "usage: $0 <IO.ofx.bundle> <docker image> <verify_plugin_loads> | --packages" >&2
  exit 2
fi
for tool in readelf docker od realpath; do
  command -v "$tool" > /dev/null || { echo "$0: $tool is needed" >&2; exit 2; }
done
bundle=$(realpath "$1")
image=$2
verify=$(realpath "$3")
bindir="$bundle/Contents/Linux-x86-64"
test -f "$bindir/IO.ofx" || { echo "$bindir/IO.ofx not found" >&2; exit 2; }
test -x "$verify" || { echo "$verify is not an executable" >&2; exit 2; }

echo "== NEEDED libraries that are not in the bundle"
bundled=$(cd "$bindir" && ls)
fail=0
needed=""
for f in "$bindir"/*; do
  if [ -f "$f" ] && [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ]; then
    needed+=$(readelf -d "$f" | sed -n "s/.*(NEEDED).*\[\(.*\)\]/\1 $(basename "$f")/p")$'\n'
  fi
done
if ! grep -q "^libc\.so\.6 IO\.ofx$" <<< "$needed"; then
  echo "error: no NEEDED library found (IO.ofx must need at least libc.so.6): nothing was checked" >&2
  exit 1
fi
# shellcheck disable=SC2013 # sonames have no spaces
for lib in $(awk 'NF {print $1}' <<< "$needed" | sort -u); do
  if grep -qxF "$lib" <<< "$bundled"; then
    continue
  fi
  users=$(awk -v l="$lib" '$1 == l {print $2}' <<< "$needed" | sort | tr '\n' ' ')
  if [[ " ${RUNTIME_SONAMES//$'\n'/ } " == *" $lib "* ]]; then
    echo "  $lib (system): $users"
  else
    echo "  $lib is neither bundled nor a documented runtime requirement (needed by $users)"
    fail=1
  fi
done
if [ "$fail" != 0 ]; then
  echo "error: bundle it (find_and_copy_deps.py), or, if it is a system library (never bundled, see"
  echo "  find_and_copy_deps.py), document it: RUNTIME_SONAMES and RUNTIME_PACKAGES in $0, and README.md"
  # still run the load check below, which shows what a user's system would report
fi

echo "== Loading IO.ofx in a clean $image container, with: $RUNTIME_PACKAGES (and their dependencies)"
# exit status of the container: 3 if the packages can't be installed, 1 if IO.ofx doesn't load
status=0
# shellcheck disable=SC2016
docker run --rm -e "RUNTIME_PACKAGES=$RUNTIME_PACKAGES" -v "$bundle:/bundle/IO.ofx.bundle:ro" \
  -v "$verify:/usr/local/bin/verify_plugin_loads:ro" "$image" bash -uo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    dpkg-query -W -f "\${Package}\n" | sort > /tmp/before
    { apt-get update -qq && apt-get install -y -qq --no-install-recommends $RUNTIME_PACKAGES > /dev/null; } || exit 3
    dpkg-query -W -f "\${Package}\n" | sort > /tmp/after
    echo "installed: $(comm -13 /tmp/before /tmp/after | tr "\n" " ")"
    if ldconfig -p | grep -E "libOpenCL\.so"; then
      echo "error: the container has an OpenCL ICD loader: it is not a clean system"
      exit 3
    fi
    plugin=/bundle/IO.ofx.bundle/Contents/Linux-x86-64/IO.ofx
    if ldd "$plugin" | grep "not found"; then
      echo "error: these libraries are missing on a clean system"
    fi
    verify_plugin_loads "$plugin" || exit 1
  ' || status=$?
case "$status" in
  0) ;;
  3) echo "error: the $image container couldn't be set up (apt-get or network failure, or not a clean system)"; exit 1 ;;
  1) echo "error: IO.ofx doesn't load in a clean $image container"; exit 1 ;;
  *) echo "error: docker run $image failed (exit status $status)"; exit 1 ;;
esac
if [ "$fail" != 0 ]; then
  exit 1
fi
echo "== OK: IO.ofx loads in $image"
