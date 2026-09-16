"""PlatformIO pre-script: the app image's version field IS FIRMWARE_VERSION.

ESP-IDF writes a version string into every app image (esp_app_desc.version,
printed at boot as "App version" and shown by esptool). Unless told otherwise
it takes `git describe --dirty` at build time. A release is built BEFORE its
commit exists, so every image so far carried its PARENT commit plus "-dirty"
(2.7.1 says v2.7.0-17-g9755d0e-dirty): a field that names the wrong commit,
and one more thing that stops a rebuild from matching the published bytes
(memory/gotcha-log.md, 2026-09-16, reproducible builds).

The firmware's own version is FIRMWARE_VERSION in platformio.ini -- what
/api/status reports, what build_release.ps1 checks and what ROTA compares.
This script hands that same value to CMake as PROJECT_VER, which IDF uses
before any other source (tools/cmake/project.cmake). The version stays in ONE
place: each env's build_flags, as before.

How: board_build.* options reach IDF through the board configuration, and the
espidf builder passes `build.cmake_extra_args` to CMake. PlatformIO keeps one
platform instance per build, so updating that option here, in a PRE script, is
what the builder reads a moment later -- the same mechanism PlatformIO uses
for board_build.* in platformio.ini.

The build fails if the env has no FIRMWARE_VERSION, more than one distinct
value, a value IDF would truncate (32-byte field), or a PROJECT_VER already
set some other way: a silently wrong version field is the state this ends.
"""

import re
import sys

Import("env")  # noqa: F821 -- SCons injects Import() and exports env

# esp_app_desc.version is char[32]; IDF cuts PROJECT_VER to 31 characters.
MAX_LEN = 31


def fail(msg):
    sys.stderr.write("Error: project_version.py: %s\n" % msg)
    env.Exit(1)


flags = env.GetProjectOption("build_flags", [])
text = " ".join(flags) if isinstance(flags, (list, tuple)) else str(flags)
found = set(re.findall(r'-DFIRMWARE_VERSION=\\?"([^"\\]+)\\?"', text))
if len(found) != 1:
    fail("expected exactly one -DFIRMWARE_VERSION in build_flags of env %s, found %s"
         % (env["PIOENV"], sorted(found) or "none"))
version = found.pop()
if len(version) > MAX_LEN:
    fail("FIRMWARE_VERSION %r is longer than the %d characters the app image holds"
         % (version, MAX_LEN))

board = env.BoardConfig()
extra = board.get("build.cmake_extra_args", "")
if "PROJECT_VER" in extra:
    fail("board_build.cmake_extra_args already sets PROJECT_VER; the version comes "
         "from FIRMWARE_VERSION, set it there")
board.update("build.cmake_extra_args", (extra + " -DPROJECT_VER=" + version).strip())
print("App image version (esp_app_desc.version): %s" % version)
