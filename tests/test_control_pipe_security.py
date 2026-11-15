"""The control bridge's Windows pipe DACL (host/control.c pipe_security), checked from Linux too, with no game.

tests/test_control.py runs the bridge's Windows code only on Windows (there it reads the pipe's DACL back and runs
HOS_CONTROL_PEER_SID's check, control_test peersid). This reads control.c's source the way other checks here do and
asserts what the Beanstalk Test Farm agent's two sandbox modes need, and what must not change:
  - the AppContainer branch: TokenIsAppContainer, then TokenAppContainerSid granted GA (a pipe's next instance is
    checked against the pipe's DACL with the game's token, and an AppContainer's check needs its own SID);
  - HOS_CONTROL_PEER_SID: read, checked by hos_control_peer_sid_check (ConvertStringSidToSidA, IsValidSid, the
    canonical string round trip, S-1-5-21- with five sub-authorities, a RID of 1000 or more, LookupAccountSidA
    saying SidTypeUser), the well-known groups refused by name, granted GRGW only, a refusal logged;
  - the rest kept: D:P (protected), the current user's GA, FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_REJECT_REMOTE_CLIENTS,
    and Linux's SO_PEERCRED check (Linux has no counterpart: the farm's bwrap runs the game as the same user).
Then, when there is a compiler for Windows (--cc, clang on Windows, or HOOKSHOT_MINGW=<llvm-mingw> on Linux),
host/control.c and tests/control_test.c are compiled for x86_64-w64-mingw32 (objects only, as test_control.py's flags,
-Werror); without one that half is skipped.

python -I -B tests/test_control_pipe_security.py [--cc <llvm-mingw>/bin/x86_64-w64-mingw32-clang]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

REFUSED = {"S-1-1-0": "Everyone", "S-1-5-11": "Authenticated Users", "S-1-5-32-545": "Users",
           "S-1-5-32-544": "Administrators"}


def body(src, start):
    """the text of the C function whose definition starts with start, to its closing brace at column 0"""
    i = src.index(start)
    return src[i:src.index("\n}\n", i) + 2]


def source_checks(src):
    fails = []

    def need(cond, what):
        if not cond:
            fails.append(what)

    win = src[src.index("/* Windows: overlapped pipe instances"):]
    sec = body(win, "static int pipe_security(void)")
    check = body(win, "int hos_control_peer_sid_check(")
    need('"D:P(A;;GA;;;%s)", user' in sec, "the DACL: protected, the current user's GA first")
    need("TokenIsAppContainer" in sec and "TokenAppContainerSid" in sec, "the AppContainer branch (TokenIsAppContainer, TokenAppContainerSid)")
    need('"(A;;GA;;;%s)", ac' in sec, "the AppContainer's SID granted GA")
    need('getenv("HOS_CONTROL_PEER_SID")' in sec, "HOS_CONTROL_PEER_SID read")
    need("hos_control_peer_sid_check(peer" in sec, "HOS_CONTROL_PEER_SID checked before it is used")
    grants = re.findall(r'"\(A;;([A-Z]+);;;%s\)", (\w+)', sec)
    need(("GRGW", "peer") in grants and all(r != "peer" or g == "GRGW" for g, r in grants), "the peer granted GRGW alone")
    need("HOS_CONTROL_PEER_SID refused" in sec, "a refused HOS_CONTROL_PEER_SID logged")
    for call in ("ConvertStringSidToSidA(s, &sid)", "IsValidSid(sid)", "ConvertSidToStringSidA(sid, &back)",
                 "strcmp(back, s)", 'strncmp(s, "S-1-5-21-", 9)', "GetSidSubAuthorityCount(sid) != 5",
                 "GetSidSubAuthority(sid, 4) < 1000", "LookupAccountSidA(", "use != SidTypeUser"):
        need(call in check, "the peer SID check: " + call)
    table = src[src.index("peer_sid_refused[][2] = {"):]
    table = table[:table.index("};")]
    for sid, name in REFUSED.items():
        need('{"%s", "%s"}' % (sid, name) in table, "the refused well-known SIDs: %s (%s)" % (name, sid))
    need("EqualSid(sid, group)" in check, "the well-known SIDs compared as SIDs (an SDDL alias like WD too)")
    listen = body(win, "static int pipe_listen(void)")
    need("FILE_FLAG_FIRST_PIPE_INSTANCE" in listen and "PIPE_REJECT_REMOTE_CLIENTS" in listen and "&pipe_sa" in listen,
         "the pipe: its first instance its own, remote clients refused, this DACL")
    need("SO_PEERCRED" in src and "cred.uid != getuid()" in src, "Linux: a client of another user turned away (SO_PEERCRED)")
    need("hos_control_peer_sid_check" in (ROOT / "tests" / "control_test.c").read_text(encoding="utf-8"),
         "control_test.c runs the peer SID check (peersid)")
    return fails


def windows_cc(given):
    if given:
        return given
    if os.name == "nt":
        return shutil.which("clang")
    mingw = os.environ.get("HOOKSHOT_MINGW", "")
    cc = os.path.join(mingw, "bin", "x86_64-w64-mingw32-clang")
    return cc if mingw and os.path.exists(cc) else None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", help="a C compiler for Windows")
    a = p.parse_args()
    fails = source_checks((ROOT / "host" / "control.c").read_text(encoding="utf-8"))
    for f in fails:
        print("FAIL: " + f, file=sys.stderr)
    if fails:
        return 1
    print("PASS: control.c's pipe DACL: the AppContainer's SID, HOS_CONTROL_PEER_SID's checked user, the rest kept")
    cc = windows_cc(a.cc)
    if not cc:
        print("SKIP compile: no compiler for Windows here (clang on Windows, or HOOKSHOT_MINGW=<llvm-mingw>)")
        return 0
    target = [] if os.name == "nt" or "mingw32" in os.path.basename(cc) else ["--target=x86_64-w64-mingw32"]
    with tempfile.TemporaryDirectory(prefix="ctl-win-") as tmp:
        for src in (ROOT / "host" / "control.c", ROOT / "tests" / "control_test.c"):
            r = subprocess.run([cc] + target + ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                                "-Wno-missing-field-initializers", "-D_CRT_SECURE_NO_WARNINGS", "-DHOS_CONTROL_NO_GAME",
                                "-I", str(ROOT / "host"), "-c", str(src), "-o", os.path.join(tmp, src.stem + ".o")],
                               capture_output=True, text=True)
            if r.returncode:
                print(r.stderr, end="", file=sys.stderr)
                print("FAIL: %s does not compile for Windows" % src.name, file=sys.stderr)
                return 1
    print("PASS: host/control.c and tests/control_test.c compile for x86_64-w64-mingw32 (-Werror)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
