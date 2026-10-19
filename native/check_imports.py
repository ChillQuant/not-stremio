#!/usr/bin/env python3
"""Lists functions the native app imports from modules the PS5 doesn't load
into an app. Those imports stay at address 0, so calling one crashes.

usage: check_imports.py [--warn-only] <linked pie.elf> <sdk target/lib> [<more stub dirs>...]

Exits 1 when an import has no SDK stub or resolves to a module the PS5 doesn't
load (--warn-only keeps the old behaviour: print and exit 0).

The loaded set is what the kernel log listed for a native media app on 11.60
(the module list printed with a crash)."""
import argparse, glob, os, subprocess, sys

LOADED = {
    "libkernel", "libkernel_web", "libkernel_sys", "libSceLibcInternal", "libc_stub_weak",
    "libSceSysmodule", "libSceAmpr", "libSceNet", "libSceIpmi", "libSceMbus", "libSceRegMgr",
    "libSceRtc", "libSceRazorCpu", "libSceAvSetting", "libSceVideoOut", "libSceAgcDriver",
    "libSceAgc", "libSceAudioOut", "libSceAudioIn", "libSceAjm", "libScePad", "libSceNetCtl",
    "libSceSsl", "libSceHttpCache", "libSceHttp", "libSceHttp2", "libSceNpCommon",
    "libSceNpManager", "libSceNpGameIntent", "libSceNpWebApi2", "libSceSaveData",
    "libSceSystemService", "libSceUserService", "libSceCommonDialog", "libSceSysUtil",
    # Loaded by the app itself before first use (native/ps5_modules.c,
    # src/hwdec_ps5.cpp).
    "libSceImeDialog", "libSceVideodec2",
}

NM = "llvm-nm-18"


def symbols(path, *flags):
    out = subprocess.run([NM, *flags, path], capture_output=True, text=True).stdout
    return {line.split()[-1] for line in out.splitlines() if line.strip()}


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Fail the build when the native app imports a function the PS5 won't resolve.")
    ap.add_argument("--warn-only", action="store_true",
                    help="print problems but exit 0 (manual/inspection runs)")
    ap.add_argument("pie", help="the linked pie.elf")
    ap.add_argument("libdirs", nargs="+", help="sdk target/lib and stub dirs")
    args = ap.parse_args(argv)
    pie, libdirs = args.pie, args.libdirs
    wanted = symbols(pie, "-D", "--undefined-only")
    owner = {}
    stubs = [so for d in libdirs for so in sorted(glob.glob(os.path.join(d, "*.so")))]
    for so in stubs:  # the link's search order
        module = os.path.basename(so)[:-3]
        for name in symbols(so, "-D", "--defined-only") & wanted:
            owner.setdefault(name, module)
    unresolved = sorted(wanted - owner.keys())
    missing = sorted((m, n) for n, m in owner.items() if m not in LOADED)
    if unresolved:
        print("imports with no SDK stub:", " ".join(unresolved))
    if missing:
        print("WARNING: these resolve to modules the PS5 doesn't load (address 0 at run time):")
        for module, name in missing:
            print(f"  {module}: {name}")
    else:
        print(f"imports ok: {len(owner)} functions, all from loaded modules")
    if (unresolved or missing) and not args.warn_only:
        sys.exit(1)


if __name__ == "__main__":
    main()
