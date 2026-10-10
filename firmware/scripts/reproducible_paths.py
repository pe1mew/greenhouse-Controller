"""PlatformIO pre-script: remap OUR OWN source paths out of the debug info.

CONFIG_APP_REPRODUCIBLE_BUILD (firmware/sdkconfig.defaults) makes IDF remap the
IDF tree, the project directory, the build directory, every component directory
and the toolchain (its tools/cmake/prefix_map.cmake). It does not remap this
project's drivers. They live at <repo>/drivers/<name>/src -- outside the project
directory -- and each proxy component's COMPONENT_DIR is
firmware/components/<name>, not the directory the sources are really in, so no
IDF mapping covers them.

Measured with the option on and without this script (2026-10-10): 79 absolute
paths of the checkout survived in firmware.elf, 43 distinct drivers/<name>/src
directories among them. The embedded ELF SHA-256 therefore still differed
between two checkouts of the same commit, and with it the image's 0xB0..0xCF
and its digest -- so the option alone bought nothing here. Rebuilding a release
somewhere else is the whole point of it, so our own tree has to be covered too.

-fdebug-prefix-map only, never -ffile-prefix-map: __FILE__ must keep expanding
exactly as it does today or .rodata moves. No path of ours reaches the image
(counted: zero occurrences of "drivers" or the checkout name in firmware.bin),
so this script cannot change a byte of the image -- only the ELF, which is the
thing that has to match.

The root is computed here because it is machine-specific; hard-coding it in
platformio.ini would reproduce only on this machine, which is the bug. The
build fails if the tree does not look like this repo, rather than quietly
mapping nothing.
"""

import os
import sys

Import("env")  # noqa: F821 -- SCons injects Import() and exports env


def fail(msg):
    sys.stderr.write("Error: reproducible_paths.py: %s\n" % msg)
    env.Exit(1)


# $PROJECT_DIR is <repo>/firmware; the drivers sit beside it.
project_dir = os.path.abspath(env.subst("$PROJECT_DIR"))
repo_root = os.path.dirname(project_dir)
if not os.path.isdir(os.path.join(repo_root, "drivers")):
    fail("no drivers/ directory beside %s -- expected <repo>/firmware, so the "
         "path mapping would cover nothing" % project_dir)

mapped = repo_root.replace("\\", "/")
flag = "-fdebug-prefix-map=%s=/PROJECT" % mapped
env.Append(CCFLAGS=[flag], ASFLAGS=[flag])
print("Debug paths: %s -> /PROJECT" % mapped)
