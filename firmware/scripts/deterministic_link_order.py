"""PlatformIO post-script: link the ESP-IDF component libraries in CMake's order.

Without it, rebuilding an unchanged tree could relink and produce a different
binary (memory/gotcha-log.md, 2026-09-16).

How that happened, in platform espressif32 6.12.0:

- builder/frameworks/espidf.py, find_lib_deps(), orders the component libraries
  by the "dependencies" array of the ELF target in CMake's File API reply.
  CMake fills that array from a set ordered by object address, so each CMake
  run can list the same libraries in another order. PYTHONHASHSEED plays no
  part (tested).
- PlatformIO re-runs CMake on EVERY build once sdkconfig.defaults,
  sdkconfig.<env> or a top-level CMakeLists.txt is newer than CMakeCache.txt,
  and CMake rewrites CMakeCache.txt only when its contents change. Deleting
  sdkconfig.<env> after a defaults edit -- the way to make such an edit apply --
  leaves the build directory in that state until the next clean.
- Every re-run reshuffled the libraries, SCons saw a new link command and
  relinked, and the image moved: other addresses, another hash, sometimes
  another size (Xtensa call relaxation depends on call distances).

This script sorts only those library nodes, into the order of the link line
CMake generated in the same reply (link.commandFragments). That order does not
change between CMake runs, and it is the order `idf.py build` links in. Strings
in LIBS (-lcore, -lc, ...) keep their places. If the reply cannot be read the
build fails: linking in an arbitrary order without saying so is the state this
script exists to end.
"""

import json
import os
import sys
from collections import UserList

Import("env")  # noqa: F821 -- SCons injects Import() and exports env

BUILD_DIR = env.subst("$BUILD_DIR")


def fail(msg):
    sys.stderr.write("Error: deterministic_link_order.py: %s\n" % msg)
    env.Exit(1)


def cmake_link_rank():
    """Map each library PlatformIO builds (path relative to BUILD_DIR) to its
    first position on the link line CMake generated for the ELF."""
    reply = os.path.join(BUILD_DIR, ".cmake", "api", "v1", "reply")
    try:
        # The current File API index is the lexicographically last one.
        index = sorted(f for f in os.listdir(reply)
                       if f.startswith("index-") and f.endswith(".json"))[-1]
        with open(os.path.join(reply, index), encoding="utf-8") as fh:
            codemodel = json.load(fh)["reply"]["codemodel-v2"]["jsonFile"]
        with open(os.path.join(reply, codemodel), encoding="utf-8") as fh:
            targets = json.load(fh)["configurations"][0]["targets"]
        elves = [t for t in targets if t.get("name", "").endswith(".elf")]
        if len(elves) != 1:
            fail("expected one .elf target in the CMake code model, found %d" % len(elves))
        with open(os.path.join(reply, elves[0]["jsonFile"]), encoding="utf-8") as fh:
            fragments = json.load(fh)["link"]["commandFragments"]
    except (OSError, IndexError, KeyError, ValueError) as exc:
        fail("cannot read the CMake File API reply in %s (%r)" % (reply, exc))

    rank = {}
    for frag in fragments:
        path = frag.get("fragment", "").strip().strip('"')
        # The selection espidf.py's extract_link_args() makes: relative .a paths
        # are the libraries PlatformIO builds itself.
        if (frag.get("role") == "libraries" and path.endswith(".a")
                and not os.path.isabs(path) and not path.startswith("..")):
            rank.setdefault(os.path.normcase(os.path.normpath(path)), len(rank))
    if not rank:
        fail("the CMake link line for %s names no libraries" % elves[0]["name"])
    return rank


def as_node(item):
    # espidf.py prepends one single-node NodeList per component library.
    if isinstance(item, (list, tuple, UserList)):
        if len(item) != 1:
            fail("unexpected LIBS entry holding %d nodes" % len(item))
        item = item[0]
    return item if hasattr(item, "get_abspath") else None


def main():
    libs = list(env.get("LIBS", []))
    slots = [i for i, item in enumerate(libs) if as_node(item) is not None]
    if not slots:
        return

    rank = cmake_link_rank()
    rel = {i: os.path.normcase(os.path.relpath(as_node(libs[i]).get_abspath(), BUILD_DIR))
           for i in slots}
    unranked = sum(rel[i] not in rank for i in slots)
    ordered = sorted(slots, key=lambda i: (rank.get(rel[i], len(rank)), rel[i]))

    reordered = list(libs)
    for slot, src in zip(slots, ordered):
        reordered[slot] = libs[src]
    env.Replace(LIBS=reordered)

    print("Link order: %d component libraries in CMake link-line order%s" % (
        len(slots), ", %d not on it (placed last, by path)" % unranked if unranked else ""))


main()
