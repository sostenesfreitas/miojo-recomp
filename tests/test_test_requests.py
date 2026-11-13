"""scripts/test_requests.py, game-free and offline: the message lines parse (a request: summary, then optional Needs,
PR link, notes; people's chat does not); replaying the room gives each request's state: several people can hold a
claim at once, a claim clears when it runs out (capped at 8 hours) and renew/release are the claimer's, done counts
from anyone with a link to a comment on the same PR and replaces that person's earlier result, cancel is the
requester's and nothing counts after it; list puts untested and unclaimed requests first and the ones waiting for a
maintainer last; a claim needs an approved PR (its author a collaborator, or the "testing wanted" label) that is still
open, and no claim goes out when GitHub can't be asked; and the commands refuse with exit code 3 what the replay would
ignore, and post the documented lines otherwise.

    python -I -B tests/test_test_requests.py
"""
import argparse
import contextlib
import importlib.util
import io
import json
import os
import sys
import subprocess
import tempfile
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", default=os.path.join(ROOT, "scripts", "test_requests.py"))
parser.add_argument("--old-source", help="also check compatibility with an older client's source")
parser.add_argument("--test", action="append", help="run only a named test (without test_)")
args = parser.parse_args()


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tr = load(args.source, "test_requests")

T = "\N{TEST TUBE}"
PR = "https://github.com/NotRexed/Hookshot/pull/2711"
COMMENT = PR + "#issuecomment-123"
FAILS = []
CHECKS = 0
STDERR = ""
HEAD = "bce3d17e" + "a" * 32
NEXT = "309e6c35" + "b" * 32
ANN, BOB, CAT = ("@%s:magicbean.dev" % n for n in ("ann", "bob", "cat"))


def check(cond, what):
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILS.append(what)
        print("FAIL: " + what)


def ev(sender, minute, body):
    return {"sender": "@%s:magicbean.dev" % sender, "origin_server_ts": minute * 60000, "body": body}


def at(minute):
    return minute * 60.0


def until(minute):
    return "until " + tr.stamp(at(minute))


REQ = ev("ann", 0, "%s T-7KQ2 request: Keep 8 GB cards in budget\nNeeds: NVIDIA RTX 30+, Windows\n%s\nCheck Kakariko."
         % (T, PR))


def state(events, now):
    return tr.replay([REQ] + events, now=at(now))["T-7KQ2"]


def test_parse():
    check(tr.parse("%s T-7KQ2 claim: until 2026-11-12 16:40Z" % T)[:2] == ("T-7KQ2", "claim"), "claim parses")
    check(tr.parse("%s T-7KQ2 done pass: %s" % (T, COMMENT))[2:4] == ("pass", COMMENT), "done parses result and link")
    check(tr.parse("%s T-7KQ2 release" % T)[1] == "release", "release without a colon parses")
    check(tr.parse("%s T-0OI1 claim" % T) is None, "IDs never use 0, O, 1, I")
    check(tr.parse("T-7KQ2 claim") is None, "a line without the mark is chat")
    check(tr.parse("I will claim T-7KQ2 later") is None, "chat mentioning an ID is ignored")
    check(tr.until_time("claim: until 2026-11-12 16:40Z") == 1794501600, "until is UTC")
    for _ in range(50):
        check(tr.LINE.match("%s %s cancel" % (T, tr.new_id({}))) is not None, "new IDs match the line format")


def test_request():
    t = state([], 1)
    check(t["pr"] == 2711 and t["summary"] == "Keep 8 GB cards in budget", "request: PR and summary")
    check(t["needs"] == "NVIDIA RTX 30+, Windows" and t["notes"] == "Check Kakariko.", "request: needs and notes")
    check(t["status"] == "open" and not t["claims"] and not t["results"], "request: open, untouched")
    bare = tr.replay([ev("ann", 0, "%s T-AAAA request: x\n%s" % (T, PR))])["T-AAAA"]
    check(bare["needs"] == "" and bare["notes"] == "" and bare["pr"] == 2711, "needs and notes are optional")
    dup = ev("bob", 1, "%s T-7KQ2 request: other\n%s" % (T, PR))
    check(state([dup], 2)["requester"] == ANN, "an ID is used once")


def test_claims():
    bob = ev("bob", 1, "%s T-7KQ2 claim: %s" % (T, until(61)))
    cat = ev("cat", 2, "%s T-7KQ2 claim" % T)
    t = state([bob, cat], 30)
    check(set(t["claims"]) == {BOB, CAT}, "two people test at once")
    check(t["claims"][BOB] == at(61) and t["claims"][CAT] == at(2 + tr.DEFAULT_TTL), "own until, else the default")
    t = state([bob, cat], 62)
    check(set(t["claims"]) == {CAT} and (at(61), BOB, "expired") in t["history"], "a claim clears when it runs out")
    check(not state([bob], 62)["claims"], "the last claim clears too")
    greedy = ev("bob", 1, "%s T-7KQ2 claim: %s" % (T, until(10000)))
    check(state([greedy], 2)["claims"][BOB] == at(1 + tr.MAX_TTL), "claims are capped")
    renew_other = ev("cat", 30, "%s T-7KQ2 renew: %s" % (T, until(200)))
    check(CAT not in state([bob, renew_other], 31)["claims"], "renew without a claim does nothing")
    renew = ev("bob", 30, "%s T-7KQ2 renew: %s" % (T, until(200)))
    t = state([bob, renew], 62)
    check(t["claims"][BOB] == at(200) and t["history"][-1][2] == "renew", "renew extends")
    again = ev("bob", 30, "%s T-7KQ2 claim: %s" % (T, until(150)))
    check(state([bob, again], 31)["history"][-1][2] == "renew", "a second claim by the holder is a renew")
    late = ev("bob", 70, "%s T-7KQ2 renew: %s" % (T, until(200)))
    check(BOB not in state([bob, late], 71)["claims"], "renew after it ran out does nothing")
    rel_other = ev("cat", 5, "%s T-7KQ2 release" % T)
    check(BOB in state([bob, rel_other], 6)["claims"], "release is the claimer's")
    rel = ev("bob", 5, "%s T-7KQ2 release" % T)
    check(not state([bob, rel], 6)["claims"], "release clears the claim")
    for typo in ("until 2026-13-12 16:40Z", "until 2026-11-12 99:99Z"):   # (a date strptime refuses: the default)
        t = state([ev("bob", 1, "%s T-7KQ2 claim: %s" % (T, typo))], 2)
        check(t["claims"].get(BOB) == at(1 + tr.DEFAULT_TTL), "a mistyped until (%s) is the default" % typo)


def test_done_cancel():
    bob = ev("bob", 1, "%s T-7KQ2 claim" % T)
    cat = ev("cat", 2, "%s T-7KQ2 claim" % T)
    done = ev("bob", 5, "%s T-7KQ2 done fail: %s" % (T, COMMENT))
    t = state([bob, cat, done], 6)
    check(t["results"][BOB]["result"] == "fail" and BOB not in t["claims"] and CAT in t["claims"],
          "done records a result, clears that claim only")
    walk_in = ev("dan", 3, "%s T-7KQ2 done pass: %s" % (T, COMMENT))
    check("@dan:magicbean.dev" in state([walk_in], 4)["results"], "done without a claim counts")
    redo = ev("bob", 9, "%s T-7KQ2 done pass: %s" % (T, PR + "#issuecomment-9"))
    check(state([bob, done, redo], 10)["results"][BOB]["result"] == "pass", "a later result replaces the earlier")
    wrong = ev("bob", 5, "%s T-7KQ2 done pass: https://github.com/NotRexed/Hookshot/pull/1#issuecomment-1" % T)
    check(not state([bob, wrong], 6)["results"], "done on another PR ignored")
    nolink = ev("bob", 5, "%s T-7KQ2 done pass: works" % T)
    check(not state([bob, nolink], 6)["results"], "done needs a comment link")
    noresult = ev("bob", 5, "%s T-7KQ2 done: %s" % (T, COMMENT))
    check(not state([bob, noresult], 6)["results"], "done needs a result")
    cancel_other = ev("bob", 2, "%s T-7KQ2 cancel" % T)
    check(state([cancel_other], 3)["status"] == "open", "cancel is the requester's")
    cancel = ev("ann", 3, "%s T-7KQ2 cancel" % T)
    t = state([bob, cancel], 4)
    check(t["status"] == "closed" and not t["claims"], "cancel closes and clears claims")
    after = ev("cat", 5, "%s T-7KQ2 claim" % T)
    check(not state([cancel, after], 6)["claims"], "nothing after cancel")


def test_order():
    reqs = [ev("ann", i, "%s T-AAA%s request: r%d\n%s" % (T, c, i, PR)) for i, c in enumerate("BCDE")]
    lines = reqs + [ev("bob", 10, "%s T-AAAB claim" % T),
                    ev("cat", 11, "%s T-AAAC done pass: %s" % (T, COMMENT))]
    order = [t["id"] for t in sorted(tr.replay(lines, now=at(12)).values(), key=tr.need)]
    check(order == ["T-AAAD", "T-AAAE", "T-AAAB", "T-AAAC"], "untested and unclaimed first: %s" % order)


GITHUB = {}  # PR number -> what pr_info returns, or an exception to raise
CALLS = []
COMPARE_CALLS = []


def fake_pr_info(number):
    CALLS.append(number)
    got = GITHUB.get(number, {"state": "open", "labels": [], "author": "x", "association": "COLLABORATOR",
                              "head": HEAD})
    if isinstance(got, Exception):
        raise got
    return got


def fake_github_json(url, number):
    check("/compare/" in url, "only a non-head membership comparison needs another GitHub answer")
    COMPARE_CALLS.append(url)
    commit = url.split("/compare/")[-1].split("...")[0]
    sha = next((sha for sha in (HEAD, NEXT) if sha.startswith(commit)), "c" * 40)
    return {"status": "ahead" if sha != "c" * 40 else "diverged", "base_commit": {"sha": sha}}


def run(events, user, *argv):
    """A command against a stand-in room and GitHub: (exit code, lines it sent, standard output)."""
    global STDERR
    sent = []
    config = {"user_id": "@%s:magicbean.dev" % user, "base_url": "http://x", "access_token": "t"}
    out, err = io.StringIO(), io.StringIO()
    with mock.patch.object(tr.team_chat, "session", return_value=config), \
            mock.patch.object(tr.team_chat, "room_id", return_value="!room"), \
            mock.patch.object(tr.urllib.request, "urlopen", side_effect=AssertionError("unexpected network call")), \
            mock.patch.object(tr, "history", side_effect=lambda c, r: list(events)), \
            mock.patch.object(tr, "pr_info", side_effect=fake_pr_info), \
            mock.patch.object(tr, "github_json", side_effect=fake_github_json, create=True), \
            mock.patch.object(tr, "send", side_effect=lambda c, r, text: (sent.append(text), events.append(
                {"sender": config["user_id"], "origin_server_ts": tr.time.time() * 1000, "body": text}))), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = tr.main(list(argv))
        except SystemExit as error:
            code = error.code
    STDERR = err.getvalue()
    return code, sent, out.getvalue()


def test_commands():
    now_min = int(tr.time.time() // 60)
    events = [ev("ann", now_min - 5, REQ["body"])]
    code, sent, _ = run(events, "ann", "request", "--pr", "#2711", "--summary", "Fix  walls", "--needs",
                        "AMD  RX 6000+,  Linux", "--notes", "x")
    lines = sent[0].splitlines() if sent else []
    check(code == 0 and tr.LINE.match(lines[0]) and lines[0].endswith("request: Fix walls") and lines[1:] == [
        "Needs: AMD RX 6000+, Linux", PR, "x", "Commit: " + HEAD, "Head: " + HEAD],
          "request: summary, needs, PR link, notes, then commit: %r" % sent)
    code, sent, _ = run(events, "ann", "request", "--pr", PR, "--summary", "again", "--needs", "amd rx 6000+, linux")
    check(code == 3 and not sent, "the same open request twice is refused")
    code, sent, _ = run(events, "bob", "claim", "t-7kq2")
    check(code == 0 and sent and " T-7KQ2 claim: until " in sent[0], "claim posts: %r" % sent)
    code, sent, out = run(events, "cat", "claim", "T-7KQ2")
    check(code == 0 and "also being tested by @bob" in out, "a second tester may claim, and is told who else")
    code, sent, _ = run(events, "dan", "renew", "T-7KQ2")
    check(code == 3 and not sent, "renew without a claim is refused")
    code, sent, _ = run(events, "bob", "renew")
    check(code == 0 and len(sent) == 1 and "T-7KQ2 renew: until " in sent[0], "renew with no ID renews yours")
    code, sent, _ = run(events, "bob", "done", "T-7KQ2", "--result", "pass", "--comment", PR)
    check(code == 3 and not sent, "done without a comment link is refused")
    code, sent, _ = run(events, "bob", "done", "T-7KQ2", "--result", "mixed", "--comment", COMMENT)
    check(code == 0 and sent == ["%s T-7KQ2 done mixed: %s\nCommit: %s\nHead: %s" % (T, COMMENT, HEAD, HEAD)],
          "done posts: %r" % sent)
    code, sent, _ = run(events, "bob", "claim", "T-7KQ2")
    check(code == 3 and not sent, "claiming what you already tested needs --again")
    code, sent, _ = run(events, "bob", "claim", "T-7KQ2", "--again")
    check(code == 0 and sent, "--again claims it")
    code, sent, _ = run(events, "bob", "cancel", "T-7KQ2")
    check(code == 3 and not sent, "only the requester cancels")
    code, sent, _ = run(events, "ann", "cancel", "T-7KQ2")
    check(code == 0 and sent == ["%s T-7KQ2 cancel" % T], "the requester cancels")
    code, sent, _ = run(events, "cat", "done", "T-7KQ2", "--result", "pass", "--comment", COMMENT)
    check(code == 3 and not sent, "done after cancel is refused")
    code, _, out = run(events, "bob", "list", "--all", "--json")
    check(code == 0 and any(t["id"] == "T-7KQ2" and t["status"] == "closed" for t in json.loads(out)), "list --json")


def test_approval():
    now_min = int(tr.time.time() // 60)
    stranger = {"state": "open", "labels": ["bug"], "author": "x", "association": "CONTRIBUTOR", "head": HEAD}
    events = [ev("ann", now_min - 9, "%s T-AAAB request: a\n%s/pull/1" % (T, tr.REPO)),
              ev("ann", now_min - 8, "%s T-AAAC request: b\n%s/pull/2" % (T, tr.REPO))]
    GITHUB.update({1: stranger, 2: dict(stranger, association="OWNER")})
    try:
        code, sent, _ = run(events, "bob", "claim", "T-AAAB")
        check(code == 3 and not sent, "a stranger's PR without the label can't be claimed")
        code, _, out = run(events, "bob", "list", "--json")
        got = [(t["id"], t["approved"]) for t in json.loads(out)]
        check(got == [("T-AAAC", True), ("T-AAAB", False)], "waiting requests list last: %s" % got)
        code, _, out = run(events, "bob", "list")
        check("waiting for a maintainer" in out, "list says it waits for a maintainer")
        GITHUB[1] = dict(stranger, labels=["bug", "testing wanted"])
        code, sent, _ = run(events, "bob", "claim", "T-AAAB")
        check(code == 0 and sent, "the label approves it")
        GITHUB[2] = dict(stranger, association="MEMBER", state="merged")
        code, sent, _ = run(events, "bob", "claim", "T-AAAC")
        check(code == 3 and not sent, "a merged PR can't be claimed")
        GITHUB[2] = tr.team_chat.ChatError("cannot reach GitHub")
        code, sent, _ = run(events, "bob", "claim", "T-AAAC")
        check(code == 1 and not sent, "no claim when GitHub can't be asked")
        code, _, out = run(events, "bob", "list", "--json")
        check(code == 0 and len(json.loads(out)) == 2, "list still works without GitHub")
    finally:
        GITHUB.clear()


def request(events, *extra):
    code, sent, out = run(events, "ann", "request", "--pr", "2711", "--summary", "Keep 8 GB cards in budget",
                          "--needs", "NVIDIA RTX 30+, Windows", "--notes", "Check Kakariko.", *extra)
    check(code == 0 and len(sent) == 1, "request succeeds: %s" % out)
    return tr.parse(sent[0])[0] if sent else "T-7KQ2"


def test_request_head():
    events = []
    CALLS.clear()
    tid = request(events)
    card = tr.replay(events)[tid]
    check(card.get("commit") == HEAD, "request records the head commit")
    check(card.get("requested_head") == HEAD and card.get("head") == HEAD, "request records the observed head")
    check(CALLS == [2711], "request reuses one GitHub answer for approval and commit: %r" % CALLS)
    check(events[0]["body"] == REQ["body"].replace("T-7KQ2", tid) + "\nCommit: %s\nHead: %s" % (HEAD, HEAD),
          "request appends metadata after every original line")
    check("commit: " + HEAD[:8] in tr.describe(card), "card renders a short commit")


def test_moved_head():
    events = []
    tid = request(events)
    GITHUB[2711] = dict(fake_pr_info(2711), head=NEXT)
    try:
        CALLS.clear()
        code, sent, out = run(events, "bob", "claim", tid)
        check(code == 3 and not sent, "an unattended claim is refused after the head moves")
        check("head moved since the request" in STDERR and HEAD[:8] in STDERR and NEXT[:8] in STDERR
              and "requester confirms" in STDERR, "refusal reports both heads and asks for confirmation")
        check(CALLS == [2711], "claim approval and moved-head check use one GitHub call")
        code, sent, _ = run(events, "bob", "claim", tid, "--commit", NEXT)
        check(code == 3 and not sent, "confirmation cannot silently substitute another commit")
        CALLS.clear()
        code, sent, out = run(events, "bob", "claim", tid, "--commit", HEAD[:8])
        check(code == 0 and len(sent) == 1 and CALLS == [2711], "explicit confirmation permits the pinned claim")
        check("commit to build: " + HEAD in out, "claim tells the tester the pinned commit, not the new head")
        card = tr.replay(events)[tid]
        check(card.get("commit") == HEAD and card["status"] == "open", "a moved head does not move or close the pin")
        check(card.get("claim_commits", {}).get(BOB) == HEAD, "claim stores the commit the tester was told")
        check(card.get("claim_heads", {}).get(BOB) == NEXT, "claim retains the head observed for this tester")
        check("head moved since the request" in tr.describe(card), "moved-head warning survives replay")
        CALLS.clear()
        code, _, out = run(events, "cat", "list")
        check(code == 0 and CALLS == [2711] and "head moved since the request" in out,
              "plain list checks the head with its existing approval call")
    finally:
        GITHUB.clear()


def test_done_commit():
    events = []
    tid = request(events)
    run(events, "bob", "claim", tid)
    GITHUB[2711] = dict(fake_pr_info(2711), head=NEXT)
    try:
        CALLS.clear()
        code, sent, out = run(events, "bob", "done", tid, "--result", "pass", "--comment", COMMENT)
        result = tr.replay(events)[tid]["results"].get(BOB, {})
        check(code == 0 and result.get("commit") == HEAD, "done defaults to the commit recorded at claim time")
        check(result.get("head") == NEXT and "tested an earlier commit" in out,
              "done records the current head and calls the result an earlier commit")
        check(CALLS == [2711], "done checks the head once")
        check(sent == ["%s %s done pass: %s\nCommit: %s\nHead: %s" % (T, tid, COMMENT, HEAD, NEXT)],
              "done leaves the result and link on the original first line")
        text = tr.describe(tr.replay(events)[tid])
        check("pass by " + BOB + " (commit " + HEAD[:8] in text and "tested an earlier commit" in text,
              "result card shows the tested commit and earlier-commit warning")
    finally:
        GITHUB.clear()


def test_explicit_commit():
    events = []
    tid = request(events, "--commit", NEXT[:8].upper())
    card = tr.replay(events)[tid]
    check(card.get("commit") == NEXT and card["notes"] == "Check Kakariko.", "explicit request commit is resolved")
    check("not the PR head" in tr.describe(card) and "explicit choice" in tr.describe(card),
          "explicit non-head choice is explained, not called a head move")
    code, _, out = run(events, "bob", "done", tid, "--result", "mixed", "--comment", COMMENT,
                        "--commit", NEXT[:7].upper())
    result = tr.replay(events)[tid]["results"].get(BOB, {})
    check(code == 0 and result.get("commit") == NEXT[:7] and "tested an earlier commit" in out,
          "done accepts a named non-head commit without a claim")
    code, _, out = run(events, "cat", "done", tid, "--result", "pass", "--comment", COMMENT,
                        "--commit", HEAD[:7].upper())
    result = tr.replay(events)[tid]["results"].get(CAT, {})
    check(code == 0 and result.get("commit") == HEAD and "earlier commit" not in out,
          "a head abbreviation is expanded, not reported as an earlier commit")
    other = []
    ident = request(other, "--commit", HEAD[:7].upper())
    check(tr.replay(other)[ident].get("commit") == HEAD, "request expands an abbreviation of the current head")
    for value in ("abcdef", "a" * 41, "g" * 8, "HEAD", "abc1234\n", " abc1234"):
        for command in (("request", "--pr", "2711", "--summary", "x"),
                        ("done", tid, "--result", "pass", "--comment", COMMENT), ("update", tid)):
            CALLS.clear()
            code, sent, out = run(events, "ann", *command, "--commit", value)
            check(code == 2 and not sent and not CALLS and "7 to 40 hexadecimal" in STDERR,
                  "%s rejects invalid commit %r before network or room writes" % (command[0], value))


def test_update_commit():
    events = []
    tid = request(events)
    run(events, "bob", "claim", tid)
    run(events, "bob", "done", tid, "--result", "fail", "--comment", COMMENT)
    run(events, "cat", "claim", tid)
    GITHUB[2711] = dict(fake_pr_info(2711), head=NEXT)
    try:
        code, sent, _ = run(events, "bob", "update", tid, "--commit", NEXT)
        check(code == 3 and not sent, "only the requester may update")
        CALLS.clear()
        code, sent, _ = run(events, "ann", "update", tid, "--commit", NEXT[:8])
        card = tr.replay(events)[tid]
        check(code == 0 and card.get("commit") == NEXT and card.get("requested_head") == NEXT,
              "update moves the pin to the named new head")
        check(CALLS == [2711], "update asks GitHub once")
        check(BOB in card["results"] and card["results"][BOB].get("commit") == HEAD
              and any(row[2] == "done" for row in card["history"]), "update preserves results and history")
        check(not tr.current_results(card) and not tr.need(card)[1], "old results do not satisfy the new request")
        check(CAT in card["claims"] and card.get("claim_commits", {}).get(CAT) == HEAD,
              "update does not change an existing tester's build")
        run(events, "cat", "renew", tid)
        code, _, out = run(events, "cat", "done", tid, "--result", "pass", "--comment", COMMENT)
        card = tr.replay(events)[tid]
        check(code == 0 and card["results"][CAT].get("commit") == HEAD and "earlier commit" in out,
              "done after update and renew still defaults to the claimed commit")
        check("nobody has tested this commit yet" in tr.describe(card)
              and "not the requested commit" in tr.describe(card), "updated card distinguishes historical results")
        code, _, out = run(events, "bob", "claim", tid)
        check(code == 0 and "commit to build: " + NEXT in out, "a new pin can be claimed without --again")
        run(events, "ann", "cancel", tid)
        code, sent, _ = run(events, "ann", "update", tid, "--commit", HEAD)
        check(code == 3 and not sent, "closed requests cannot be updated")
    finally:
        GITHUB.clear()


def test_old_card():
    # Stored before commit fields existed; exact old rendering, not one made with the new request command.
    card = tr.replay([REQ], now=at(1))["T-7KQ2"]
    expected = ("T-7KQ2  OPEN  %s  Keep 8 GB cards in budget\n"
                "    asked by %s on 1970-01-01 00:00Z\n"
                "    needs: NVIDIA RTX 30+, Windows\n"
                "    nobody has tested it yet\n"
                "    Check Kakariko.") % (PR, ANN)
    check("commit" not in card and tr.describe(card) == expected, "stored old card renders byte-for-byte unchanged")
    events = [REQ.copy()]
    code, sent, _ = run(events, "bob", "done", "T-7KQ2", "--result", "pass", "--comment", COMMENT)
    check(code == 0 and sent == ["%s T-7KQ2 done pass: %s" % (T, COMMENT)], "legacy unpinned done stays byte-identical")
    check("commit" not in tr.replay(events)["T-7KQ2"]["results"][BOB], "legacy results never invent a commit")
    pinned = []
    tid = request(pinned)
    code, sent, _ = run(pinned, "bob", "done", tid, "--result", "pass", "--comment", COMMENT)
    check(code == 3 and not sent, "pinned walk-in results must name the actual tested commit")
    pinned.append(ev("bob", int(tr.time.time() // 60), "%s %s done pass: %s" % (T, tid, COMMENT)))
    card = tr.replay(pinned)[tid]
    check("commit not recorded" in tr.describe(card) and not tr.current_results(card),
          "an old client's result is accepted, but never credited to an unrecorded commit")


def test_list_calls():
    events = []
    tid = request(events)
    run(events, "ann", "request", "--pr", "2711", "--summary", "other hardware", "--needs", "Linux")
    GITHUB[2711] = dict(fake_pr_info(2711), head=NEXT)
    try:
        CALLS.clear()
        code, _, out = run(events, "bob", "list", "--json")
        check(code == 0 and CALLS == [2711] and all(t.get("head") == NEXT and t.get("approved") is True
                                                  for t in json.loads(out)),
              "plain list checks head and approval once per PR, not once per card")
        CALLS.clear()
        code, _, out = run(events, "bob", "show", tid)
        check(code == 0 and CALLS == [2711] and "head moved since the request" in out, "show refreshes one PR")
        GITHUB[2711] = tr.team_chat.ChatError("GitHub answered 403 (rate limit: set GITHUB_TOKEN)")
        for command in (("claim", tid), ("update", tid, "--commit", NEXT)):
            CALLS.clear()
            code, sent, out = run(events, "ann", *command)
            check(code == 1 and not sent and CALLS == [2711] and "rate limit" in STDERR,
                  "%s sends nothing when GitHub cannot be checked" % command[0])
        CALLS.clear()
        code, _, out = run(events, "ann", "list", "--json")
        check(code == 0 and CALLS == [2711] and "warning:" in STDERR and '"approved": null' in out,
              "list keeps working with the existing rate-limit warning")
    finally:
        GITHUB.clear()


def test_done_unreachable():
    for error in ("cannot reach GitHub", "GitHub answered 403 (rate limit: set GITHUB_TOKEN)"):
        events = []
        tid = request(events)
        run(events, "bob", "claim", tid)
        GITHUB[2711] = tr.team_chat.ChatError(error)
        try:
            for user, extra, expected in (("bob", (), HEAD), ("cat", ("--commit", NEXT), NEXT)):
                CALLS.clear()
                code, sent, out = run(events, user, "done", tid, "--result", "pass", "--comment", COMMENT, *extra)
                result = tr.replay(events)[tid]["results"].get("@%s:magicbean.dev" % user, {})
                check(code == 0 and sent == ["%s %s done pass: %s\nCommit: %s" % (T, tid, COMMENT, expected)],
                      "done records %s's commit without Head when GitHub fails" % user)
                check(result.get("commit") == expected and "head" not in result,
                      "done never substitutes a stored head for a failed fresh check")
                check(CALLS == [2711] and "head could not be checked" in out and error in out,
                      "done checks once and explains the missing head comparison")
                check("earlier commit" not in out, "an unchecked result makes no earlier-commit claim")
        finally:
            GITHUB.clear()


def test_request_unreachable():
    for error in ("cannot reach GitHub", "GitHub answered 429 (rate limit: set GITHUB_TOKEN)"):
        events = []
        GITHUB[2711] = tr.team_chat.ChatError(error)
        try:
            for extra in ((), ("--commit", HEAD[:8])):
                CALLS.clear()
                code, sent, out = run(events, "ann", "request", "--pr", "2711", "--summary", "Offline request", *extra)
                check(code == 1 and not sent and CALLS == [2711] and "full 40-character SHA" in STDERR
                      and "retry when GitHub is available" in STDERR,
                      "request without a known full commit explains the offline way out")
            CALLS.clear()
            code, sent, out = run(events, "ann", "request", "--pr", "2711", "--summary", "Offline request",
                                  "--commit", HEAD.upper())
            tid = tr.parse(sent[0])[0] if sent else "T-7KQ2"
            check(code == 0 and sent == ["%s %s request: Offline request\n%s\nCommit: %s" % (T, tid, PR, HEAD)]
                  and CALLS == [2711], "explicit full commit posts without Head when GitHub fails")
            card = tr.replay(events).get(tid, {})
            check(card.get("commit") == HEAD and "head" not in card and "requested_head" not in card,
                  "offline request never invents a head at request time")
            check("approval is unknown" in out and error in out, "offline request explains unknown approval and head")
            code, _, out = run(events, "bob", "list", "--json")
            card = json.loads(out)[0]
            check(code == 0 and card.get("approved") is None and "head" not in card and "warning:" in STDERR,
                  "approval stays unknown while GitHub is unavailable")
            GITHUB[2711] = {"state": "open", "labels": [], "author": "x", "association": "CONTRIBUTOR", "head": NEXT}
            code, _, out = run(events, "bob", "list")
            check(code == 0 and "waiting for a maintainer" in out and "not the PR head" in out,
                  "a later successful list restores the maintainer gate and compares the explicit pin")
            check("head moved since the request" not in out, "an unobserved original head is not called a move")
            GITHUB[2711]["labels"] = ["testing wanted"]
            code, _, out = run(events, "bob", "list", "--json")
            check(code == 0 and json.loads(out)[0]["approved"] is True, "later approval opens the offline request")
        finally:
            GITHUB.clear()



def test_github_head():
    data = {"state": "open", "author_association": "COLLABORATOR", "labels": [], "user": {"login": "ann"},
            "head": {"sha": HEAD.upper()}}
    for environ, token in (({}, None), ({"GH_TOKEN": "second"}, "second"),
                           ({"GITHUB_TOKEN": "first", "GH_TOKEN": "second"}, "first")):
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = json.dumps(data).encode("utf-8")
        with mock.patch.dict(os.environ, environ, clear=True), \
                mock.patch.object(tr.urllib.request, "urlopen", return_value=response) as api:
            info = tr.pr_info(2711)
        check(api.call_count == 1 and info.get("head") == HEAD and tr.approved(info),
              "one GitHub response supplies the head and approval")
        req = api.call_args[0][0]
        check(req.full_url == tr.PR_API % 2711 and req.get_header("Authorization") == (
            "Bearer " + token if token else None), "existing optional-token precedence is unchanged")
    error = tr.urllib.error.HTTPError(tr.PR_API % 2711, 429, "rate limited", {}, None)
    with mock.patch.object(tr.urllib.request, "urlopen", side_effect=error) as api:
        try:
            tr.pr_info(2711)
            check(False, "rate limit must raise ChatError")
        except tr.team_chat.ChatError as exc:
            check("rate limit: set GITHUB_TOKEN" in str(exc) and api.call_count == 1,
                  "rate-limit handling does not retry")


def test_json_current_results():
    events = []
    tid = request(events)
    run(events, "bob", "claim", tid)
    run(events, "bob", "done", tid, "--result", "pass", "--comment", COMMENT)
    for command in (("list", "--json"), ("show", tid, "--json")):
        code, _, out = run(events, "bob", *command)
        card = json.loads(out)
        card = card[0] if isinstance(card, list) else card
        check(code == 0 and card.get("current_results") == card["results"] and BOB in card["results"],
              "JSON exposes current results before an update")
    GITHUB[2711] = dict(fake_pr_info(2711), head=NEXT)
    try:
        code, sent, _ = run(events, "bob", "claim", tid)
        check(code == 3 and not sent and "card to be updated to a new commit" in STDERR,
              "a moved PR head alone does not invalidate a current result")
        run(events, "ann", "update", tid, "--commit", NEXT)
        for command in (("list", "--json"), ("show", tid, "--json")):
            code, _, out = run(events, "bob", *command)
            card = json.loads(out)
            card = card[0] if isinstance(card, list) else card
            check(code == 0 and card.get("current_results") == {} and BOB in card["results"],
                  "JSON keeps historical results but exposes an empty current view after update")
        code, _, _ = run(events, "bob", "claim", tid)
        check(code == 0, "a result for the old pin does not block the updated card")
    finally:
        GITHUB.clear()
    legacy = state([ev("bob", 1, "%s T-7KQ2 done pass: %s" % (T, COMMENT))], 2)
    check(tr.public(legacy).get("current_results") == legacy["results"], "legacy JSON keeps all results current")


def test_request_membership():
    events = []
    COMPARE_CALLS.clear()
    tid = request(events)
    check(not COMPARE_CALLS, "a head request needs no membership call")
    for command in (("request", "--pr", "2711", "--summary", "outside", "--needs", "other"),
                    ("update", tid)):
        COMPARE_CALLS.clear()
        code, sent, _ = run(events, "ann", *command, "--commit", "c" * 40)
        check(code == 1 and not sent and len(COMPARE_CALLS) == 1 and "not a verified ancestor" in STDERR,
              "%s refuses an explicit commit outside the PR" % command[0])
    COMPARE_CALLS.clear()
    code, sent, _ = run(events, "ann", "update", tid, "--commit", NEXT[:7])
    check(code == 0 and len(COMPARE_CALLS) == 1 and tr.replay(events)[tid]["commit"] == NEXT,
          "update checks and resolves a non-head ancestor once")
    for status in ("behind", "diverged"):
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = json.dumps(
            {"status": status, "base_commit": {"sha": NEXT}}).encode("utf-8")
        with mock.patch.object(tr.urllib.request, "urlopen", return_value=response) as api:
            try:
                tr.pr_commit(2711, NEXT, HEAD)
                check(False, "a non-ancestor comparison must be refused")
            except tr.team_chat.ChatError:
                check(api.call_count == 1, "a %s comparison refuses without retry" % status)


def test_verify_membership():
    with tempfile.TemporaryDirectory(prefix="test-request-") as tmp, \
            mock.patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}):
        source, repo = os.path.join(tmp, "source"), os.path.join(tmp, "tester")
        os.mkdir(source)

        def fixture(*argv):
            result = subprocess.run(["git", "-C", source, "-c", "user.name=Test",
                                     "-c", "user.email=test@example.invalid", "-c", "commit.gpgsign=false", *argv],
                                    capture_output=True, text=True, check=True)
            return result.stdout.strip()

        def commit_file(name, text):
            with open(os.path.join(source, name), "w", encoding="utf-8") as f:
                f.write(text)
            fixture("add", name)
            fixture("commit", "-m", name)
            return fixture("rev-parse", "HEAD")

        fixture("init", "--initial-branch=main")
        base = commit_file("base.txt", "base\n")
        fixture("checkout", "-b", "pr")
        pin = commit_file("pinned.txt", "pinned change\n")
        head = commit_file("head-only.txt", "head-only change\n")
        fixture("update-ref", "refs/pull/2711/head", head)
        fixture("checkout", "-b", "outside", base)
        outsider = commit_file("outside.txt", "unrelated fork change\n")
        fixture("checkout", "pr")
        fixture("clone", "--no-hardlinks", source, repo)
        real_git = tr.git

        def local_git(path, *argv):
            return real_git(path, *(source if arg == tr.REPO else arg for arg in argv))

        GITHUB[2711] = {"state": "open", "labels": [], "author": "x", "association": "COLLABORATOR",
                        "head": head, "base": base}
        try:
            with mock.patch.object(tr, "git", side_effect=local_git):
                for chosen, valid in ((pin, True), (outsider, False)):
                    events = [ev("ann", int(tr.time.time() // 60), REQ["body"]
                                 + "\nCommit: %s\nHead: %s" % (chosen, head))]
                    run(events, "bob", "claim", "T-7KQ2")
                    before = real_git(repo, "rev-parse", "HEAD")
                    code, sent, out = run(events, "bob", "verify", "T-7KQ2", "--repo", repo)
                    if valid:
                        check(code == 0 and not sent and "verified commit: " + pin in out,
                              "tester verifies the pinned ancestor against the fetched PR head")
                        check("+pinned change" in out and "head-only change" not in out,
                              "tester reviews the pinned diff, not the newer head's changes")
                    else:
                        check(code == 3 and sent == ["%s T-7KQ2 release" % T] and "not a verified ancestor" in STDERR,
                              "tester refuses a fetchable outside commit and releases its claim")
                        check(BOB not in tr.replay(events)["T-7KQ2"]["claims"], "failed verification leaves no claim")
                    check(real_git(repo, "rev-parse", "HEAD") == before, "verification never checks out or builds code")
                events = [ev("ann", int(tr.time.time() // 60), REQ["body"]
                             + "\nCommit: %s\nHead: %s" % (pin, head))]
                run(events, "bob", "claim", "T-7KQ2")
                GITHUB[2711]["head"] = outsider
                code, sent, _ = run(events, "bob", "verify", "T-7KQ2", "--repo", repo)
                check(code == 3 and sent == ["%s T-7KQ2 release" % T] and "head moved since the claim" in STDERR,
                      "a head move after claiming stops verification and releases the claim")
        finally:
            GITHUB.clear()


def test_claim_commit_lifetime():
    requested = ev("ann", 0, REQ["body"] + "\nCommit: %s\nHead: %s" % (HEAD, HEAD))
    claimed = ev("bob", 1, "%s T-7KQ2 claim: %s\nCommit: %s\nHead: %s" % (T, until(61), HEAD, HEAD))
    for ending, minute in (([], 62), ([ev("bob", 2, "%s T-7KQ2 release" % T)], 3),
                           ([ev("ann", 2, "%s T-7KQ2 cancel" % T)], 3)):
        events = [requested, claimed] + ending
        card = tr.replay(events, now=at(minute))["T-7KQ2"]
        check(BOB not in card["claims"] and BOB not in card.get("claim_commits", {})
              and BOB not in card.get("claim_heads", {}), "ending a claim clears its commit and head snapshots")
        if card["status"] == "open":
            code, sent, _ = run(events, "bob", "done", "T-7KQ2", "--result", "pass", "--comment", COMMENT)
            check(code == 3 and not sent and "no live claim" in STDERR, "a gone claim cannot supply a done default")
            code, _, _ = run(events, "bob", "done", "T-7KQ2", "--commit", HEAD,
                              "--result", "pass", "--comment", COMMENT)
            check(code == 0, "a tester can still name the built commit after a claim ends")


def test_request_note_fields():
    notes = "Commit: %s\nHead: %s\nCommit: compare these builds\nHead: look at the logs" % (NEXT, NEXT)
    events = []
    tid = request(events, "--notes", notes)
    card = tr.replay(events)[tid]
    check(card["notes"] == notes and card["commit"] == HEAD and card["head"] == HEAD,
          "metadata-looking note lines survive before the actual request footer")
    for notes in ("Commit: %s\nHead: %s" % (NEXT, NEXT), "Commit: " + NEXT):
        events = []
        tid = request(events, "--notes", notes)
        check(tr.replay(events)[tid]["notes"] == notes, "even a trailing metadata-looking note is preserved")
    for notes in ("Commit: %s\nfollow-up text" % NEXT, "Head: " + NEXT):
        card = tr.replay([ev("ann", 0, "%s T-7KQ2 request: notes\n%s\n%s" % (T, PR, notes))])["T-7KQ2"]
        check(card["notes"] == notes and "commit" not in card and "head" not in card,
              "legacy note fields outside a commit footer are not swallowed")


def test_old_client():
    old = load(args.old_source, "old_test_requests")
    events = []
    tid = request(events)
    run(events, "bob", "claim", tid)
    run(events, "bob", "done", tid, "--result", "pass", "--comment", COMMENT)
    card = old.replay(events)[tid]
    check(card["pr"] == 2711 and card["summary"] == "Keep 8 GB cards in budget" and card["status"] == "open",
          "old client reads the new request")
    check("Commit: " + HEAD in card["notes"] and "Head: " + HEAD in card["notes"],
          "old client treats appended request fields as notes")
    check(card["results"][BOB]["result"] == "pass" and card["results"][BOB]["comment"] == COMMENT,
          "old client ignores extra done lines, keeps the exact result and URL")
    check(not card["claims"], "old client applies new claims and clears them on done")
    run(events, "ann", "update", tid, "--commit", NEXT)
    check(old.replay(events)[tid] == card, "old client ignores the unrecognised update action")
    print("OLD CLIENT CARD:\n" + old.describe(card))
    request(events, "--needs", "Linux")
    CALLS.clear()
    code, _, out = run(events, "cat", "list", "--json")
    new_calls = list(CALLS)
    check(code == 0 and len(json.loads(out)) == 2, "new list reads two open cards")
    CALLS.clear()
    with mock.patch.dict(globals(), {"tr": old}):
        code, _, out = run(events, "cat", "list", "--json")
    check(code == 0 and len(json.loads(out)) == 2 and CALLS == new_calls == [2711],
          "plain list makes exactly the same GitHub calls as the actual main client")
    print("LIST CALLS: main=%d branch=%d (two cards, one PR)" % (len(CALLS), len(new_calls)))


tests = (test_parse, test_request, test_claims, test_done_cancel, test_order, test_commands, test_approval,
         test_request_head, test_moved_head, test_done_commit, test_explicit_commit, test_update_commit,
         test_old_card, test_list_calls, test_done_unreachable, test_request_unreachable, test_github_head,
         test_json_current_results, test_request_membership, test_verify_membership, test_claim_commit_lifetime,
         test_request_note_fields)
if args.old_source:
    tests += (test_old_client,)
if args.test:
    names = {test.__name__[5:] for test in tests}
    if set(args.test) - names:
        parser.error("unknown test: " + ", ".join(sorted(set(args.test) - names)))
for test in tests:
    if args.test and test.__name__[5:] not in args.test:
        continue
    before = len(FAILS)
    test()
    print("%s %s" % ("pass" if len(FAILS) == before else "FAIL", test.__name__))
print("%d checks" % CHECKS)
print("%d failure(s)" % len(FAILS) if FAILS else "all passed")
sys.exit(1 if FAILS else 0)
