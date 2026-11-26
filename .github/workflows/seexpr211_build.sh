#!/bin/bash
# Build and install SeExpr 2.11 (the SeExpr 1 API, for the SeExpr plug-ins) with the patches of Natron's SDKs:
#
#   seexpr211_build.sh <install prefix> <MacPorts|LinuxSDK> [extra cmake arguments...]
#
# NATRON_SEEXPR_COMMIT (a NatronGitHub/Natron commit) and NATRON_SEEXPR_PATCHES (patch file names, in order) come from
# the environment (ci.yml's workflow env). MacPorts: the patches of the seexpr211 port
# (tools/MacPorts/graphics/seexpr211/files, applied in the Portfile's order); LinuxSDK: the same ones from the Linux SDK
# (tools/jenkins/include/patches/SeExpr, applied by tools/jenkins/include/scripts/pkg/seexpr211.sh). The CI SeExpr
# caches are keyed on this script's hash, the pin, the patch list and the extra arguments: anything else that changes
# the build belongs here.
#
# Run from any directory: SeExpr is cloned and built in a temporary directory. The caller's PATH picks bison, flex and
# sed: SeExpr's build (src/build/macros.cmake) finds them with find_program() and takes no CMake variable for them.

set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <install prefix> <MacPorts|LinuxSDK> [extra cmake arguments...]" >&2
    exit 2
fi
prefix=$1
case $2 in
    MacPorts) patch_dir=tools/MacPorts/graphics/seexpr211/files ;;
    LinuxSDK) patch_dir=tools/jenkins/include/patches/SeExpr ;;
    *)
        echo "$0: unknown patch directory '$2' (MacPorts or LinuxSDK)" >&2
        exit 2
        ;;
esac
shift 2
: "${NATRON_SEEXPR_COMMIT:?}" "${NATRON_SEEXPR_PATCHES:?}"
if ! command -v bison >/dev/null || ! command -v flex >/dev/null || ! command -v sed >/dev/null; then
    echo "$0: bison, flex and sed must be in PATH" >&2
    exit 1
fi

src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT
git clone -q --depth 1 -b v2.11 https://github.com/wdas/SeExpr.git "$src"
cd "$src"
# shellcheck disable=SC2086 # a space-separated list
for p in $NATRON_SEEXPR_PATCHES; do
    curl -fsSL --retry 3 --retry-all-errors \
        "https://raw.githubusercontent.com/NatronGitHub/Natron/${NATRON_SEEXPR_COMMIT}/${patch_dir}/${p}" | patch -p1
done
# Only libSeExpr: no SeExprEditor (Qt), demos, tests or doc. Not sed -i, which differs between GNU and BSD sed.
sed -e "/SeExprEditor/d" -e "/demos/d" -e "/tests/d" -e "/doc/d" CMakeLists.txt > CMakeLists.txt.new
mv CMakeLists.txt.new CMakeLists.txt
# CMAKE_INSTALL_NAME_DIR (macOS only): an absolute install name, like MacPorts' libraries, so that osxDeploy.sh finds
# and bundles libSeExpr.
cmake . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_NAME_DIR="$prefix/lib" -DCMAKE_CXX_STANDARD=11 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN)"
cmake --install build
# its licence, for the Linux testing bundle (copy_bundled_licenses.sh)
mkdir -p "$prefix/share/licenses/SeExpr"
cp LICENSE "$prefix/share/licenses/SeExpr/"
