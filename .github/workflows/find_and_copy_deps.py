"""Copy the libraries that a binary needs, found by ldd, into a directory (the CI testing bundles).

Usage: find_and_copy_deps.py [--json=FILE] [--manifest=FILE] <target_binary> <output_directory> [search_directories]

The libraries are looked up in the search directories (and in the directory of the library that needs them), not
where ldd found them; the system libraries (_LINUX_SYSTEM_LIB_PREFIXES, and Windows' own DLLs) are not copied.
--json writes each binary's dependencies and where each was copied from (for copy_bundled_licenses.sh); --manifest
writes a Windows side-by-side assembly manifest that lists the copied DLLs.
"""

import argparse
import json
import os.path
import re
import shutil
import subprocess
import sys
import tempfile

# System libraries that are never copied on Linux (see "Linux runtime requirements" in README.md): the system list of
# openfx's linuxCheckBundle.sh, which never bundles them, plus the desktop libraries, as Natron's Linux installer
# (SYSTEM_LIBS_RE in tools/jenkins/build-Linux-installer.sh) does. These are prefixes of the file names:
# - glibc and the dynamic loader, libcrypt, and the GCC runtime (libstdc++, libgcc_s, libgomp, libatomic);
# - GLVND (libGL, libGLX, libEGL, libGLdispatch, libOpenGL, libGLES*), libdrm and libdrm_* (libdrm_amdgpu...),
#   and Mesa's libgbm and libglapi: they must match the system's graphics driver, which a bundled older copy
#   breaks;
# - libudev (it reads the running udev's database, whose format follows the system's udev) and libselinux;
# - the GPU drivers: libcuda, NVIDIA's libnvidia-* and libnvcuvid, and the OpenCL vendor ICDs (libamdocl*,
#   libigdrcl, libintelocl, libMesaOpenCL, libRusticlOpenCL, libpocl; libnvidia-opencl is a libnvidia-*). The
#   OpenCL ICD loader only opens the ICDs listed in /etc/OpenCL/vendors/*.icd: they are never NEEDED, and a
#   bundled copy would never be used;
# - the desktop libraries, which must match the system's display server and sound system: the X11 client
#   libraries (libX*, e.g. libX11, libX11-xcb, libXext; libxcb and its extensions: a bundled libxcb breaks the
#   system's libX11), libSM, libICE, libxkbcommon, libxkbcommon-x11, and ALSA's libasound.
# The OpenCL ICD loader itself, libOpenCL.so.1 (ocl-icd, NEEDED by Ubuntu's libavutil), is bundled: it is
# vendor-neutral and not in a default install (ocl-icd-libopencl1 on Ubuntu, ocl-icd on Rocky Linux), and
# IO.ofx doesn't load without it (#270). It finds the system's vendor ICDs by itself.
_LINUX_SYSTEM_LIB_PREFIXES = (
    # glibc, libcrypt, the dynamic loader
    "ld-linux",
    "ld64.so",
    "libBrokenLocale.so",
    "libanl.so",
    "libc.so",
    "libc_malloc_debug.so",
    "libcrypt.so",
    "libdl.so",
    "libm.so",
    "libmvec.so",
    "libnsl.so",
    "libnss_",
    "libpthread.so",
    "libresolv.so",
    "librt.so",
    "libthread_db.so",
    "libutil.so",
    # the GCC runtime
    "libatomic.so",
    "libgcc_s.so",
    "libgomp.so",
    "libstdc++.so",
    # GLVND, libdrm, Mesa's libgbm and libglapi
    "libEGL.so",
    "libGL.so",
    "libGLESv1_CM.so",
    "libGLESv2.so",
    "libGLX.so",
    "libGLdispatch.so",
    "libOpenGL.so",
    "libdrm.so",
    "libdrm_",
    "libgbm.so",
    "libglapi.so",
    # libudev, libselinux
    "libselinux.so",
    "libudev.so",
    # the GPU drivers, including the OpenCL vendor ICDs (not libOpenCL.so: the ICD loader is bundled)
    "libMesaOpenCL.so",
    "libRusticlOpenCL.so",
    "libamdocl",
    "libcuda.so",
    "libigdrcl.so",
    "libintelocl.so",
    "libnvcuvid.so",
    "libnvidia-",
    "libpocl.so",
    # the desktop libraries, as Natron's SYSTEM_LIBS_RE: libX* (libX11, libX11-xcb, libXau, libXext...), libxcb
    # and its extensions, libSM, libICE, libxkbcommon(-x11), and ALSA's libasound
    "libX",
    "libxcb.so",
    "libxcb-",
    "libSM.so",
    "libICE.so",
    "libxkbcommon.so",
    "libxkbcommon-x11.so",
    "libasound.so",
)

# A line of ldd's output for a library that it resolved (or not): "<name> => <location> (<address>)"
_LDD_LINE_RE = re.compile("^([^ ]+) => ([^(]+)( (.*))?$")


def fix_location(location_path):
    """Returns location_path as a Windows path (cygpath -m) on MS Windows, unchanged elsewhere."""
    if sys.platform == "win32" and location_path[0] == '/':
        new_location_path = subprocess.run(
            ["cygpath", "-m", location_path],
            capture_output=True).stdout.decode("utf-8").strip()
        return new_location_path

    return location_path


def get_deps_for_binary(binary_path, search_directories):
    """Returns the set of (name, location) of the libraries that binary_path needs (ldd), except the system ones.

    Each library is looked up in search_directories; exits with status 1 if one is not found there.
    """
    my_name = os.path.basename(binary_path)
    proc = subprocess.Popen(['ldd', binary_path], stdout=subprocess.PIPE)
    system_lib_prefixes = _LINUX_SYSTEM_LIB_PREFIXES if sys.platform == "linux" else ()
    ret = set()
    while True:
        line = proc.stdout.readline()
        if not line:
            break

        line = line.decode("utf-8").strip()

        m = _LDD_LINE_RE.match(line)
        if m:
            dep_name = m.group(1)
            ldd_dep_location = m.group(2)

            # Skip self, Windows DLLs, and platform system libraries.
            if (dep_name == my_name or
                "/c/windows/system" in ldd_dep_location.lower() or
                "/c/windows/winsxs/" in ldd_dep_location.lower() or
                os.path.basename(ldd_dep_location).startswith(system_lib_prefixes)):
                continue

            search_dep_location = None
            for x in search_directories:
                alt_dep_location = os.path.join(x, dep_name)
                if os.path.exists(alt_dep_location):
                    if os.path.isfile(alt_dep_location):
                        search_dep_location = alt_dep_location
                        break

            if search_dep_location:
                ret.add((dep_name, fix_location(search_dep_location)))
            elif ldd_dep_location == "not found":
                print(f"{binary_path} depends on {dep_name} but ldd could not find its location.")
                sys.exit(1)
            else:
                print(f"Could not find {dep_name} in search path, but ldd found it at {ldd_dep_location}. Please update search paths and run again.\n")
                sys.exit(1)
        else:
            if ("linux-vdso.so.1" in line or
                re.search(r"ld-linux[^/ ]*\.so", line) or  # the dynamic loader (x86-64, aarch64, ...)
                "statically linked" in line):
                continue
            print(f"Unexpected line in ldd output: {line}")
            sys.exit(1)
    return ret


def get_all_deps_for_binary(binary_key_name, json_dict):
    """Returns the set of the direct and indirect dependencies of binary_key_name (None if it is not in json_dict)."""
    if binary_key_name not in json_dict["binary_deps"]:
        return None

    ret = set()
    deps_list = json_dict["binary_deps"][binary_key_name].copy()
    while len(deps_list) > 0:
        dep = deps_list.pop()
        if dep not in ret:
            ret.add(dep)
            deps_list += json_dict["binary_deps"][dep]
    return ret


def main():
    parser = argparse.ArgumentParser(
                    prog='find_and_copy_deps',
                    description='Finds all the library dependencies of a executable \
                    or shared library and copies them to a specified directory.')
    parser.add_argument('--json')
    parser.add_argument('--manifest')
    parser.add_argument('target_binary')
    parser.add_argument('output_directory')
    parser.add_argument('search_directories', nargs="*", default=[])

    cmd_line_args = parser.parse_args()

    target_binary = cmd_line_args.target_binary
    output_directory = cmd_line_args.output_directory
    search_directories = cmd_line_args.search_directories.copy()

    if not os.path.exists(target_binary):
        sys.stderr.write(f"{target_binary} does not exist.\n")
        sys.exit(1)

    if not os.path.isfile(target_binary):
        sys.stderr.write(f"{target_binary} is not a file.\n")
        sys.exit(1)

    if not os.path.isdir(output_directory):
        sys.stderr.write(f"{output_directory} is not a directory.\n")
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmpdirname:
        print('created temporary directory', tmpdirname)

        binary_name_to_location_map = {}

        ldd_filename_path = target_binary
        binary_key_name = os.path.basename(target_binary)
        if sys.platform == "win32" and target_binary.endswith(".ofx"):
            # Need to make a copy of the plugin and rename it to have a dll extension so ldd
            # works properly.
            new_filename = os.path.splitext(os.path.basename(target_binary))[0] + ".dll"
            ldd_filename_path = os.path.join(tmpdirname, new_filename)
            shutil.copyfile(target_binary, ldd_filename_path)

        binary_deps = {}
        deps_remaining = set()

        binary_name_to_location_map[binary_key_name] = ldd_filename_path
        deps_remaining.add(binary_key_name)

        deps_visited = set()
        while len(deps_remaining) > 0:
            x = deps_remaining.pop()

            if x in deps_visited:
                continue
            deps_visited.add(x)

            print(f"Finding dependencies for {x}")
            search_directories.append(os.path.dirname(binary_name_to_location_map[x]))
            dep_info_list = get_deps_for_binary(binary_name_to_location_map[x], search_directories)
            search_directories.pop()
            deps_for_this_library = set()
            for (dep_name, dep_location) in dep_info_list:
                deps_for_this_library.add(dep_name)

                if dep_name not in binary_name_to_location_map:
                    binary_name_to_location_map[dep_name] = dep_location
                elif not os.path.samefile(binary_name_to_location_map[dep_name], dep_location):
                    print(f"{dep_name} appears to have more than one location. \
                     {binary_name_to_location_map[dep_name]} and {dep_location}")
                    sys.exit(1)

                if dep_name not in deps_visited:
                    deps_remaining.add(dep_name)

            binary_deps[x] = sorted(deps_for_this_library)

        json_dict = {
            "binary_deps": binary_deps,
            "binary_locations": binary_name_to_location_map,
        }

        full_deps_list = sorted(get_all_deps_for_binary(binary_key_name, json_dict))

        for x in full_deps_list:
            src = binary_name_to_location_map[x]
            print(f"{x} : {src}")
            shutil.copy(src, output_directory)

        if cmd_line_args.json:
            with open(cmd_line_args.json, "w", encoding='UTF-8') as write_file:
                json.dump(json_dict, write_file, sort_keys=True, indent=2)

        if cmd_line_args.manifest:
            # Creates a manifest that can be used with a command like the one below to
            # specify the required DLLs.
            # mt -nologo -manifest IO.ofx.manifest -outputresource:"IO.ofx;2"
            manifest = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
            manifest += '<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">\n'
            manifest += f'  <assemblyIdentity name="{os.path.basename(target_binary)}" version="1.0.0.0" type="win32" processorArchitecture="amd64"/>\n'

            for x in full_deps_list:
                manifest += f'  <file name="{x}"></file>\n'
            manifest += '</assembly>'

            with open(cmd_line_args.manifest, "w", encoding='UTF-8') as write_file:
                write_file.write(manifest)


if __name__ == '__main__':
    main()
