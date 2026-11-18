#!/usr/bin/env python3
"""Fail-fast check that the checkout contains every file the build needs.

CI spends ~30 minutes building the vcpkg libraries before CMake gets far
enough to notice that a source file was never committed ("Cannot find source
file: ..."). This script runs in about a second, before any of that, and names
exactly what is missing.

It checks that:
  1. every src/, tools/ and assets/ path written in CMakeLists.txt exists, and
     the icon referenced by the .rc file exists;
  2. every  #include "..."  in src/ and tools/ resolves to a file in the repo
     (headers fetched by CMake at configure time, such as imgui, are skipped).

Usage (from anywhere):  python tools/check_sources.py
Exit status: 0 = everything is present, 1 = something is missing.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Same search order the compiler gets from target_include_directories().
INCLUDE_DIRS = ["src", "third_party", "third_party/playfair", "tools"]
# Downloaded by FetchContent during configure, so not part of the checkout.
EXTERNAL_PREFIXES = ("imgui",)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def missing_cmake_files():
    text = re.sub(r"#.*", "", read(os.path.join(ROOT, "CMakeLists.txt")))
    # The lookbehind skips paths that are the tail of a longer one, such as
    # "${CMAKE_SOURCE_DIR}/third_party/..." (those are guarded by if(EXISTS)).
    pattern = r"(?<![\w/.}-])((?:src|tools|assets)/[\w./+-]+\.(?:cpp|c|h|rc|ico))"
    found = sorted(set(re.findall(pattern, text)))
    missing = [p for p in found if not os.path.isfile(os.path.join(ROOT, p))]

    # The linker needs one FairPlay blob: the real one, or else the stub.
    blobs = ["third_party/fairplay_blobs_real.cpp", "third_party/fairplay_blobs_stub.cpp"]
    if not any(os.path.isfile(os.path.join(ROOT, b)) for b in blobs):
        missing.append(" or ".join(blobs))

    # Resource files pulled in by the .rc (the icon), resolved next to the .rc.
    for rc in [p for p in found if p.endswith(".rc") and p not in missing]:
        rc_dir = os.path.dirname(os.path.join(ROOT, rc))
        for ref in re.findall(r'"([^"]+\.(?:ico|bmp|cur|png))"', read(os.path.join(ROOT, rc))):
            if not os.path.isfile(os.path.join(rc_dir, ref)):
                missing.append(os.path.relpath(os.path.join(rc_dir, ref), ROOT).replace(os.sep, "/")
                               + "  (referenced by " + rc + ")")
    return found, missing


def missing_includes():
    include = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
    scanned = 0
    missing = []
    for top in ("src", "tools"):
        for folder, _, names in os.walk(os.path.join(ROOT, top)):
            for name in sorted(names):
                if not name.endswith((".cpp", ".c", ".h")):
                    continue
                path = os.path.join(folder, name)
                for inc in include.findall(read(path)):
                    scanned += 1
                    if inc.startswith(EXTERNAL_PREFIXES):
                        continue
                    candidates = [os.path.join(folder, inc)]
                    candidates += [os.path.join(ROOT, d, inc) for d in INCLUDE_DIRS]
                    if not any(os.path.isfile(c) for c in candidates):
                        missing.append((os.path.relpath(path, ROOT).replace(os.sep, "/"), inc))
    return scanned, missing


def main():
    listed, missing_files = missing_cmake_files()
    scanned, missing_incs = missing_includes()
    on_github = os.environ.get("GITHUB_ACTIONS") == "true"

    problems = 0
    for p in missing_files:
        problems += 1
        print("MISSING FILE   : " + p)
        if on_github:
            print("::error file=CMakeLists.txt::Missing from the repository: " + p)
    for user, inc in missing_incs:
        problems += 1
        print("MISSING INCLUDE: \"%s\"  (included by %s)" % (inc, user))
        if on_github:
            print("::error file=%s::#include \"%s\" does not exist in the repository" % (user, inc))

    if problems:
        print("\n%d problem(s). These files exist in your working copy but did not reach the" % problems)
        print("repository. Run `git status`, then `git add -A`, commit and push, and make sure")
        print("the push includes every folder (also the hidden .github and .gitignore).")
        return 1

    print("OK: %d files named in CMakeLists.txt and %d #include lines all resolve." % (len(listed), scanned))
    return 0


if __name__ == "__main__":
    sys.exit(main())
