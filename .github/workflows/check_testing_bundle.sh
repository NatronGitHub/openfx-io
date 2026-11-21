#!/bin/bash
# Check that a Linux testing bundle loads on a clean system that has only the documented runtime requirements
# ("Linux runtime requirements" in README.md), so that a library that is neither bundled nor documented fails CI
# (#270: libOpenCL.so.1, NEEDED by Ubuntu's libavutil, was not bundled, and the runners have it installed).
#
#   check_testing_bundle.sh <IO.ofx.bundle> <docker image, e.g. ubuntu:24.04> <verify_plugin_loads>
#
# 1. Every NEEDED library of the bundle's ELF files that is not in the bundle must be in RUNTIME_SONAMES.
# 2. Even if 1. failed: in a clean container of the image (Ubuntu or Debian), with RUNTIME_PACKAGES installed and
#    nothing else, verify_plugin_loads (built on the runner, with RTLD_NOW) loads IO.ofx and lists its plug-ins.
# Requires readelf (binutils) and docker.

set -eo pipefail

if [ $# -ne 3 ]; then
  echo "usage: $0 <IO.ofx.bundle> <docker image> <verify_plugin_loads>" >&2
  exit 2
fi
bundle=$(realpath "$1")
image=$2
verify=$(realpath "$3")
bindir="$bundle/Contents/Linux-x86-64"
test -f "$bindir/IO.ofx" || { echo "$bindir/IO.ofx not found" >&2; exit 2; }

# The system libraries that the bundle may need but doesn't carry (keep in sync with find_and_copy_deps.py and
# "Linux runtime requirements" in README.md): glibc, the GCC runtime, GLVND and libdrm, libselinux and libudev, the
# X11 client libraries.
RUNTIME_SONAMES="ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libmvec.so.1 libdl.so.2 libpthread.so.0 librt.so.1
  libresolv.so.2 libutil.so.1
  libstdc++.so.6 libgcc_s.so.1 libgomp.so.1 libatomic.so.1
  libGL.so.1 libGLX.so.0 libGLdispatch.so.0 libdrm.so.2
  libselinux.so.1 libudev.so.1
  libX11.so.6 libX11-xcb.so.1 libXau.so.6 libXdmcp.so.6 libXext.so.6 libXfixes.so.3 libXrender.so.1
  libxcb.so.1 libxcb-dri3.so.0 libxcb-render.so.0 libxcb-shm.so.0"
# The Ubuntu (and Debian) packages that provide them. libgl1 is GLVND's libGL (with libglx0 and libglvnd0),
# without a GL driver (--no-install-recommends): loading IO.ofx doesn't need one.
RUNTIME_PACKAGES="libc6 libstdc++6 libgcc-s1 libgomp1 libatomic1 libgl1 libdrm2 libselinux1 libudev1
  libx11-6 libx11-xcb1 libxau6 libxdmcp6 libxext6 libxfixes3 libxrender1 libxcb1 libxcb-dri3-0 libxcb-render0 libxcb-shm0"

echo "== NEEDED libraries that are not in the bundle"
bundled=$(cd "$bindir" && ls)
fail=0
needed=$(for f in "$bindir"/*; do
  if [ -f "$f" ] && [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ]; then
    readelf -d "$f" | sed -n "s/.*(NEEDED).*\[\(.*\)\]/\1 $(basename "$f")/p"
  fi
done | sort)
for lib in $(echo "$needed" | awk '{print $1}' | sort -u); do
  if echo "$bundled" | grep -qxF "$lib"; then
    continue
  fi
  users=$(echo "$needed" | awk -v l="$lib" '$1 == l {print $2}' | head -5 | tr '\n' ' ')
  if [[ " ${RUNTIME_SONAMES//$'\n'/ } " == *" $lib "* ]]; then
    echo "  $lib (system): $users"
  else
    echo "  $lib is neither bundled nor a documented runtime requirement (needed by $users)"
    fail=1
  fi
done
if [ "$fail" != 0 ]; then
  echo "error: bundle it (find_and_copy_deps.py), or document it (README.md, RUNTIME_SONAMES and RUNTIME_PACKAGES)"
  # still run the load check below, which shows what a user's system would report
fi

# shellcheck disable=SC2086,SC2116
echo "== Loading IO.ofx in a clean $image container (only: $(echo $RUNTIME_PACKAGES))"
# shellcheck disable=SC2016
docker run --rm -e "RUNTIME_PACKAGES=$RUNTIME_PACKAGES" -v "$bundle:/bundle/IO.ofx.bundle:ro" \
  -v "$verify:/usr/local/bin/verify_plugin_loads:ro" "$image" bash -eo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends $RUNTIME_PACKAGES > /dev/null
    if ldconfig -p | grep -E "libOpenCL\.so"; then
      echo "error: the container has an OpenCL ICD loader: it is not a clean system"
      exit 1
    fi
    plugin=/bundle/IO.ofx.bundle/Contents/Linux-x86-64/IO.ofx
    if ldd "$plugin" | grep "not found"; then
      echo "error: these libraries are missing on a clean system"
    fi
    verify_plugin_loads "$plugin"
  ' || { echo "error: IO.ofx doesn't load in a clean $image container"; exit 1; }
if [ "$fail" != 0 ]; then
  exit 1
fi
echo "== OK: IO.ofx loads in $image"
