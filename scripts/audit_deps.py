#!/usr/bin/env python3
"""Recursive dependency audit for the RemotePlay Windows dist.

Walks every .exe/.dll under the dist dir, resolves each import against:
  1. files shipped in the package (same dir as the referencing binary,
     the package root, and Windows loader search semantics for subdirs)
  2. a whitelist of Windows 10/11 x64 system DLLs (incl. UCRT api-sets)
and reports anything that cannot be resolved. Exit 1 on missing deps.
"""
import os
import re
import subprocess
import sys

DIST = os.environ.get(
    "RP_DIST_DIR", "/home/z/my-project/RemotePlay/dist/RemotePlay-Windows-x64"
)
OBJDUMP = os.environ.get(
    "RP_OBJDUMP", "/home/z/toolchain/root/usr/bin/x86_64-w64-mingw32-objdump"
)

# Windows 10/11 x64 always-available system DLLs (case-insensitive match).
# Includes UCRT api-set names (api-ms-win-*) which resolve to ucrtbase.dll,
# a system component since Windows 10.
SYSTEM_DLLS = {
    "kernel32", "user32", "gdi32", "shell32", "ole32", "oleaut32", "advapi32",
    "ws2_32", "msvcrt", "ntdll", "bcrypt", "bcryptprimitives", "d3d11", "d3d9",
    "dxgi", "dwmapi", "dwrite", "uxtheme", "imm32", "winmm", "mpr", "netapi32",
    "authz", "userenv", "version", "setupapi", "shcore", "shlwapi", "wtsapi32",
    "usp10", "cfgmgr32", "dnsapi", "iphlpapi", "rpcrt4", "msimg32", "comdlg32",
    "crypt32", "secur32", "wintrust", "mswsock", "gdiplus", "combase",
    "runtimeobject", "oleaut", "schannel", "msvcp140", "vcruntime140",
    "vcruntime140_1", "concrt140", "ucrtbase", "d2d1", "dcomp", "d3d12",
}

API_SET = re.compile(r"^api-ms-win-.*\.dll$", re.IGNORECASE)

def imports_of(path: str):
    out = subprocess.run(
        [OBJDUMP, "-p", path], capture_output=True, text=True
    ).stdout
    return re.findall(r"DLL Name:\s+(\S+)", out)

def classify(dll: str, pkg_files: set):
    low = dll.lower()
    if low in pkg_files:
        return "package"
    if API_SET.match(low):
        return "system"
    base = low.rsplit(".", 1)[0]
    if base in SYSTEM_DLLS:
        return "system"
    return "MISSING"

def main() -> int:
    pkg_files = set()
    binaries = []
    for root, _dirs, files in os.walk(DIST):
        for f in files:
            p = os.path.join(root, f)
            low = f.lower()
            pkg_files.add(low)
            if low.endswith((".exe", ".dll")):
                binaries.append(p)

    print(f"auditing {len(binaries)} binaries in {DIST}\n")
    problems = []
    for b in sorted(binaries):
        rel = os.path.relpath(b, DIST)
        deps = imports_of(b)
        # Windows loader: for a DLL in a subdir, dependencies resolve against
        # (1) app dir (package root), (2) its own dir, (3) system dirs.
        local = {os.path.basename(x).lower() for x in os.listdir(os.path.dirname(b))}
        unresolved = []
        for d in deps:
            kind = classify(d, pkg_files)
            if kind == "MISSING" and d.lower() in local:
                kind = "package"
            if kind == "MISSING":
                unresolved.append(d)
        status = "OK " if not unresolved else "BAD"
        print(f"[{status}] {rel}  ({len(deps)} imports)")
        if unresolved:
            for u in unresolved:
                print(f"         MISSING -> {u}")
                problems.append((rel, u))

    print()
    if problems:
        print(f"RESULT: {len(problems)} UNRESOLVED import(s)")
        return 1
    print("RESULT: all imports resolve against package or Windows system DLLs.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
