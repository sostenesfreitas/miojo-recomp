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
import contextlib
import importlib.util
import io
import json
import os
import sys
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
spec = importlib.util.spec_from_file_location("test_requests", os.path.join(ROOT, "scripts", "test_requests.py"))
tr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tr)

T = "\N{TEST TUBE}"
PR = "https://github.com/NotRexed/Hookshot/pull/2711"
COMMENT = PR + "#issuecomment-123"
FAILS = []
ANN, BOB, CAT = ("@%s:magicbean.dev" % n for n in ("ann", "bob", "cat"))


def check(cond, what):
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


def fake_pr_info(number):
    got = GITHUB.get(number, {"state": "open", "labels": [], "author": "x", "association": "COLLABORATOR"})
    if isinstance(got, Exception):
        raise got
    return got


def run(events, user, *argv):
    """A command against a stand-in room and GitHub: (exit code, lines it sent, standard output)."""
    sent = []
    config = {"user_id": "@%s:magicbean.dev" % user, "base_url": "http://x", "access_token": "t"}
    out, err = io.StringIO(), io.StringIO()
    with mock.patch.object(tr.team_chat, "session", return_value=config), \
            mock.patch.object(tr.team_chat, "room_id", return_value="!room"), \
            mock.patch.object(tr, "history", side_effect=lambda c, r: list(events)), \
            mock.patch.object(tr, "pr_info", side_effect=fake_pr_info), \
            mock.patch.object(tr, "send", side_effect=lambda c, r, text: (sent.append(text), events.append(
                {"sender": config["user_id"], "origin_server_ts": tr.time.time() * 1000, "body": text}))), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = tr.main(list(argv))
    return code, sent, out.getvalue()


def test_commands():
    now_min = int(tr.time.time() // 60)
    events = [ev("ann", now_min - 5, REQ["body"])]
    code, sent, _ = run(events, "ann", "request", "--pr", "#2711", "--summary", "Fix  walls", "--needs",
                        "AMD  RX 6000+,  Linux", "--notes", "x")
    lines = sent[0].splitlines() if sent else []
    check(code == 0 and tr.LINE.match(lines[0]) and lines[0].endswith("request: Fix walls") and lines[1:] == [
        "Needs: AMD RX 6000+, Linux", PR, "x"], "request: summary, needs, PR link, notes: %r" % sent)
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
    check(code == 0 and sent == ["%s T-7KQ2 done mixed: %s" % (T, COMMENT)], "done posts: %r" % sent)
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
    stranger = {"state": "open", "labels": ["bug"], "author": "x", "association": "CONTRIBUTOR"}
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


for test in (test_parse, test_request, test_claims, test_done_cancel, test_order, test_commands, test_approval):
    test()
print("%d failure(s)" % len(FAILS) if FAILS else "all passed")
sys.exit(1 if FAILS else 0)
