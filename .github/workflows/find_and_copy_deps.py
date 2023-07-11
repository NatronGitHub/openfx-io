
import argparse
import os.path
import re
import shutil
import subprocess
import sys
import tempfile

import json

def fix_location(location_path):
    if sys.platform == "win32" and location_path[0] == '/':
        new_location_path = subprocess.run(
            ["cygpath", "-m", location_path],
            capture_output=True).stdout.decode("utf-8").strip()
        return new_location_path

    return location_path

def get_deps_for_binary(binary_path, search_directories):
    my_name = os.path.basename(binary_path)
    proc = subprocess.Popen(['ldd', binary_path], stdout=subprocess.PIPE)
    #print("proc ", proc)

    system_libs_to_skip = []
    if sys.platform == "linux":
        # System libraries to skip
        system_libs_to_skip = [
            "libc.so",
            "libdl.so",
            "libdrm.so",
            "libm.so",
            "libpthread.so",
            "libresolv.so",
            "libselinux.so",
            "libudev.so",
            "libGL.so",
            "libGLX.so",
            "libGLdispatch.so",
            "libX11.so", 
            "libXau.so",
            "libXdmcp.so",
            "libXext.so",
            "libXfixes.so",
            "libXrender.so",
        ]
    ret = set()
    while True:
        line = proc.stdout.readline()
        if not line:
            break

        line = line.decode("utf-8").strip()

        p = re.compile("^([^ ]+) => ([^(]+)( (.*))?$")
        m = p.match(line)
        if m:
            dep_name = m.group(1)
            ldd_dep_location = m.group(2)

            # Skip self, Windows DLLs, and platform system libraries.
            if (dep_name == my_name or
                "/c/windows/system" in ldd_dep_location.lower() or
                "/c/windows/winsxs/" in ldd_dep_location.lower() or
                (len(system_libs_to_skip) > 0 and any(lib_name in ldd_dep_location for lib_name in system_libs_to_skip))):
                continue

            #print(f"{dep_name} {dep_location}")

            search_dep_location = None
            for x in search_directories:
                alt_dep_location = os.path.join(x, dep_name)
                if os.path.exists(alt_dep_location):
                    if os.path.isfile(alt_dep_location):
                        search_dep_location = alt_dep_location
                        break

            if search_dep_location:
                #print("\t", dep_name, " -> ", search_dep_location)
                ret.add((dep_name, fix_location(search_dep_location)))
            elif ldd_dep_location == "not found":
                print(f"{binary_path} depends on {dep_name} but ldd could not find its location.")
                sys.exit(1)
            else:
                print(f"Could not find {dep_name} in search path, but ldd found it at {ldd_dep_location}. Please update search paths and run again.\n")
                sys.exit(1)
        else:
            if ("linux-vdso.so.1" in line or 
                "ld-linux-x86-64.so" in line or
                "statically linked" in line):
                continue
            print(f"Unexpected line in ldd output: {line}")
            sys.exit(1)
    return ret

def get_all_deps_for_binary(binary_key_name, json_dict):
    if  binary_key_name not in json_dict["binary_deps"]:
        return None

    ret = set()
    deps_list =  json_dict["binary_deps"][binary_key_name].copy()
    while len(deps_list) > 0:
        dep = deps_list.pop()
        if dep not in ret:
            ret.add(dep)
            deps_list += json_dict["binary_deps"][dep]
    return ret

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <target_binary> <output_directory> [search_directories]")
        sys.exit(1)

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
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmpdirname:
        print('created temporary directory', tmpdirname)

        binary_name_to_location_map = {}
        binary_deps = {}

        ldd_filename_path = target_binary
        binary_key_name = os.path.basename(target_binary)
        if sys.platform == "win32" and target_binary.endswith(".ofx"):
            # Need to make a copy of the plugin and rename it to have a dll extension so ldd
            # works properly.
            new_filename = os.path.splitext(os.path.basename(target_binary))[0] + ".dll"
            ldd_filename_path = os.path.join(tmpdirname,new_filename)
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

            sorted_deps_list = sorted(deps_for_this_library)
            #for y in deps_list:
            #    print("\t{}".format(y))

            binary_deps[x] = sorted_deps_list

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
            manifest += f'  <assemblyIdentity name="{ os.path.basename(target_binary)}" version="1.0.0.0" type="win32" processorArchitecture="amd64"/>\n'

            for x in full_deps_list:
                manifest += f'  <file name="{x}"></file>\n'
            manifest += '</assembly>'

            with open(cmd_line_args.manifest, "w", encoding='UTF-8') as write_file:
                write_file.write(manifest)


if __name__ == '__main__':
    main()
