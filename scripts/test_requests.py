"""Ask the team to test a pull request on hardware you don't have, and pick up such requests, in the Matrix room
#test-requests:magicbean.dev (copied to Discord, where each request shows who is testing it and the results).

    python -I scripts/test_requests.py request --pr 2711 --summary "Keep 8 GB cards inside their memory budget" \
        [--needs "8 GB NVIDIA or AMD card, Windows"] [--notes "what to check, where in the game"]
    python -I scripts/test_requests.py list [--all] [--mine] [--json]
    python -I scripts/test_requests.py show T-7KQ2 [--json]
    python -I scripts/test_requests.py claim T-7KQ2 [--again]
    python -I scripts/test_requests.py renew [T-7KQ2]
    python -I scripts/test_requests.py release T-7KQ2
    python -I scripts/test_requests.py done T-7KQ2 --result pass|fail|mixed --comment <URL of your comment on the PR>
    python -I scripts/test_requests.py cancel T-7KQ2

It uses the sign-in of scripts/team_chat.py (run `team_chat.py login` once). Every message is one line that starts
with the request's ID and the action, so people can read it and type it by hand in Element:

    🧪 T-7KQ2 request: Keep 8 GB cards inside their memory budget
    Needs: 8 GB NVIDIA or AMD card, Windows          (then the PR link, then the notes)
    🧪 T-7KQ2 claim: until 2026-11-12 16:40Z
    🧪 T-7KQ2 renew: until 2026-11-12 18:10Z
    🧪 T-7KQ2 release
    🧪 T-7KQ2 done pass: https://github.com/NotRexed/Hookshot/pull/2711#issuecomment-123
    🧪 T-7KQ2 cancel

The room's messages are the only record: list and every check below replay them in order. A request stays open until
the person who asked cancels it, and several people can test it at once: each holds their own claim and posts their
own result. A claim lasts 90 minutes (at most 8 hours) and simply clears when it runs out, so a tester renews while
testing (`renew` with no ID renews all of yours). done counts from anyone while the request is open, links to a comment
on the request's PR, and replaces that person's earlier result. release is the claimer's; cancel is the requester's.
A message that breaks these rules changes nothing. (The approval below is GitHub's, so the script checks it before
claiming; a claim typed by hand on a request that isn't approved shows nowhere on Discord.)

Only approved requests can be claimed: the PR's author is a collaborator on GitHub (owner, member or collaborator),
or a maintainer put the "testing wanted" label on it. Anything else waits for a maintainer to add the label (the card
on Discord says so, and list shows it last); this keeps strangers' code off testers' PCs until someone has looked at
it. The script asks GitHub (no sign-in needed; GITHUB_TOKEN or GH_TOKEN is used when set).

list puts first what most needs a tester: requests nobody has tested or is testing, then the ones being tested with no
result yet, then the ones with results; oldest first within each. Exit code 3: the action was refused (closed, not
yours, already tested by you).
Standard library only.
"""
import argparse
import calendar
import json
import os
import random
import re
import sys
import time
import urllib.error
import urllib.request
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import team_chat  # noqa: E402  (same folder; python -I does not add it to the path)

ROOM = "#test-requests:magicbean.dev"
REPO = "https://github.com/NotRexed/Hookshot"
MARK = "\N{TEST TUBE}"
ID_CHARS = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # no 0/O, 1/I
DEFAULT_TTL = 90
MAX_TTL = 480
PR_API = "https://api.github.com/repos/NotRexed/Hookshot/pulls/%d"
LABEL = "testing wanted"
TRUSTED = ("OWNER", "MEMBER", "COLLABORATOR")
HISTORY_LIMIT = 50000  # messages read back; renews add one per tester every half hour
EXIT_REFUSED = team_chat.EXIT_REFUSED

LINE = re.compile(r"^%s\s*(T-[%s]{4})\s+(request|claim|renew|release|done|cancel)\b(?:\s+(pass|fail|mixed))?\s*:?\s*(.*)$"
                  % (MARK, ID_CHARS))
UNTIL = re.compile(r"until\s+(\d{4}-\d\d-\d\d)[ T](\d\d:\d\d)Z", re.I)
PR_URL = re.compile(r"https://github\.com/NotRexed/Hookshot/pull/(\d+)")
COMMENT_URL = re.compile(r"^https://github\.com/NotRexed/Hookshot/pull/(\d+)"
                         r"#(?:issuecomment-\d+|pullrequestreview-\d+|discussion_r\d+)$")


def parse(body):
    """(id, action, result, rest of the first line, following lines) or None."""
    if not isinstance(body, str):
        return None
    first, _, more = body.strip().partition("\n")
    m = LINE.match(first.strip())
    if not m:
        return None
    return m.group(1), m.group(2), m.group(3), m.group(4).strip(), more.strip()


def until_time(text):
    m = UNTIL.search(text)
    if not m:
        return None
    return calendar.timegm(time.strptime(m.group(1) + " " + m.group(2), "%Y-%m-%d %H:%M"))


def expire(t, ts):
    """Drop the claims that ran out by ts."""
    for user, until in sorted(t["claims"].items(), key=lambda kv: kv[1]):
        if until <= ts:
            del t["claims"][user]
            t["history"].append((until, user, "expired"))


def replay(events, now=None):
    """Requests by ID, from the room's messages oldest first: each event a dict with sender, origin_server_ts (ms)
    and body. The same rules as the Discord relay."""
    now = time.time() if now is None else now
    tests = {}
    for ev in events:
        p = parse(ev.get("body"))
        if not p:
            continue
        tid, action, result, rest, more = p
        sender, ts = ev.get("sender"), ev.get("origin_server_ts", 0) / 1000
        t = tests.get(tid)
        if action == "request":
            if t:
                continue  # an ID is used once
            pr = PR_URL.search(rest + "\n" + more)
            needs, notes = "", []
            for extra in more.splitlines():
                extra = extra.strip()
                if extra.lower().startswith("needs:") and not needs:
                    needs = extra[6:].strip()
                elif extra and not (pr and extra == pr.group(0)):
                    notes.append(extra)
            tests[tid] = {"id": tid, "requester": sender, "requested": ts, "summary": rest, "needs": needs,
                          "pr": int(pr.group(1)) if pr else None, "notes": "\n".join(notes), "status": "open",
                          "claims": {}, "results": {}, "history": [(ts, sender, action)]}
            continue
        if not t or t["status"] != "open":
            continue
        expire(t, ts)
        mine = sender in t["claims"]
        if action == "claim" or action == "renew" and mine:
            until = until_time(rest)
            if until is None or until <= ts:
                until = ts + 60 * DEFAULT_TTL
            t["claims"][sender] = min(until, ts + 60 * MAX_TTL)
            action = "renew" if mine else "claim"
        elif action == "release" and mine:
            del t["claims"][sender]
        elif action == "done" and result:
            link = COMMENT_URL.match(rest)
            if not link or t["pr"] and int(link.group(1)) != t["pr"]:
                continue
            t["claims"].pop(sender, None)
            t["results"][sender] = {"result": result, "comment": rest, "time": ts}
        elif action == "cancel" and sender == t["requester"]:
            t["status"], t["claims"] = "closed", {}
        else:
            continue
        t["history"].append((ts, sender, action))
    for t in tests.values():
        if t["status"] == "open":
            expire(t, now)
    return tests


def need(t):
    """Sort key: what most needs a tester first; requests waiting for a maintainer last."""
    return (t.get("approved") is False, bool(t["results"]), bool(t["claims"]), t["requested"])


def pr_info(number):
    """The PR's state (open, draft, merged, closed), its labels and its author's standing, from GitHub."""
    req = urllib.request.Request(PR_API % number, headers={"Accept": "application/vnd.github+json",
                                                           "User-Agent": team_chat.USER_AGENT})
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            pr = json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        raise team_chat.ChatError("GitHub answered %d for PR #%d%s" % (
            error.code, number, " (rate limit: set GITHUB_TOKEN)" if error.code in (403, 429) else "")) from None
    except (urllib.error.URLError, OSError, ValueError) as error:
        raise team_chat.ChatError("cannot reach GitHub to check PR #%d: %s" % (number, error)) from None
    state = "merged" if pr.get("merged_at") else "closed" if pr.get("state") == "closed" \
        else "draft" if pr.get("draft") else "open"
    return {"state": state, "labels": [label.get("name") for label in pr.get("labels", [])],
            "author": (pr.get("user") or {}).get("login"), "association": pr.get("author_association")}


def approved(info):
    return info["association"] in TRUSTED or LABEL in info["labels"]


def annotate(tests):
    """Add each open request's PR state and approval (one call per PR); unknown when GitHub can't be asked."""
    seen = {}
    for t in tests:
        if t["status"] != "open" or not t["pr"]:
            continue
        if t["pr"] not in seen:
            try:
                seen[t["pr"]] = pr_info(t["pr"])
            except team_chat.ChatError as error:
                print("warning: %s" % error, file=sys.stderr)
                seen[t["pr"]] = None
        info = seen[t["pr"]]
        t["pr_state"] = info and info["state"]
        t["approved"] = info and approved(info)


def history(config, rid):
    """Every message in the room, oldest first (up to HISTORY_LIMIT)."""
    events, start = [], None
    while len(events) < HISTORY_LIMIT:
        path = "/rooms/%s/messages?dir=b&limit=1000" % team_chat.quote(rid)
        if start:
            path += "&from=" + team_chat.quote(start)
        page = team_chat.request("GET", team_chat.client(config["base_url"], path), config["access_token"])
        for ev in page.get("chunk", []):
            content = ev.get("content", {})
            if ev.get("type") == "m.room.message" and "m.new_content" not in content:
                events.append({"sender": ev.get("sender"), "origin_server_ts": ev.get("origin_server_ts", 0),
                               "body": content.get("body"), "event_id": ev.get("event_id")})
        start = page.get("end")
        if not start or not page.get("chunk"):
            break
    events.reverse()
    return events


def state(config):
    rid = team_chat.room_id(config, config.get("test_room", ROOM))
    return rid, replay(history(config, rid))


def send(config, rid, text):
    path = "/rooms/%s/send/m.room.message/%s" % (team_chat.quote(rid), uuid.uuid4().hex)
    team_chat.request("PUT", team_chat.client(config["base_url"], path), config["access_token"],
                      body={"msgtype": "m.text", "body": text})


def new_id(taken):
    while True:
        tid = "T-" + "".join(random.SystemRandom().choice(ID_CHARS) for _ in range(4))
        if tid not in taken:
            return tid


def pr_number(text):
    m = PR_URL.search(text) or re.match(r"^#?(\d+)$", text.strip())
    if not m:
        raise team_chat.ChatError("--pr takes a number or %s/pull/<number>" % REPO)
    return int(m.group(1))


def stamp(ts):
    return time.strftime("%Y-%m-%d %H:%MZ", time.gmtime(ts))


def until_line():
    return "until %s" % stamp(time.time() + 60 * DEFAULT_TTL)


def describe(t, me=None):
    def who(user):
        return "you" if user == me else user
    head = "%s  %s  %s/pull/%s  %s" % (t["id"], t["status"].upper(), REPO, t["pr"], t["summary"])
    lines = [head, "    asked by %s on %s" % (who(t["requester"]), stamp(t["requested"]))]
    if t["needs"]:
        lines.append("    needs: " + t["needs"])
    if t["claims"]:
        lines.append("    being tested by " + ", ".join(who(u) for u in sorted(t["claims"])))
    for user, r in sorted(t["results"].items(), key=lambda kv: kv[1]["time"]):
        lines.append("    %s by %s: %s" % (r["result"], who(user), r["comment"]))
    if t.get("approved") is False:
        lines.append('    waiting for a maintainer to add the "%s" label on GitHub: not claimable yet' % LABEL)
    elif not t["claims"] and not t["results"] and t["status"] == "open":
        lines.append("    nobody has tested it yet")
    if t.get("pr_state") in ("merged", "closed"):
        lines.append("    the PR is %s" % t["pr_state"])
    if t["notes"]:
        lines += ["    " + line for line in t["notes"].splitlines()]
    return "\n".join(lines)


def public(t):
    return {k: v for k, v in t.items() if k != "history"}


def find(tests, tid):
    tid = tid.upper()
    if tid not in tests:
        raise team_chat.ChatError("no request %s in %s" % (tid, ROOM))
    return tests[tid]


def refuse(why):
    print("refused: " + why, file=sys.stderr)
    return EXIT_REFUSED


def cmd_request(args, config):
    pr = pr_number(args.pr)
    summary, needs = " ".join(args.summary.split()), " ".join((args.needs or "").split())
    if not summary:
        raise team_chat.ChatError("--summary is the change in one line")
    rid, tests = state(config)
    for t in tests.values():
        if t["pr"] == pr and t["requester"] == config["user_id"] and t["status"] == "open" \
                and t["needs"].lower() == needs.lower():
            return refuse("%s already asks for this" % t["id"])
    tid = new_id(tests)
    text = "%s %s request: %s\n" % (MARK, tid, summary)
    if needs:
        text += "Needs: %s\n" % needs
    text += "%s/pull/%d" % (REPO, pr)
    if args.notes and args.notes.strip():
        text += "\n" + args.notes.strip()
    send(config, rid, text)
    print("asked for testing as %s" % tid)
    try:
        info = pr_info(pr)
    except team_chat.ChatError:
        return 0
    if not approved(info):
        print('note: #%d is not from a collaborator and has no "%s" label, so nobody can claim it until a maintainer '
              "adds the label on GitHub" % (pr, LABEL))
    return 0


def cmd_list(args, config):
    _, tests = state(config)
    me = config["user_id"]
    shown = [t for t in tests.values()
             if (args.all or t["status"] == "open")
             and (not args.mine or me == t["requester"] or me in t["claims"] or me in t["results"])]
    annotate(shown)
    shown.sort(key=need)
    if args.json:
        print(json.dumps([public(t) for t in shown], indent=2))
    elif not shown:
        print("no requests" if args.all else "no open requests")
    else:
        print("\n\n".join(describe(t, me) for t in shown))
    return 0


def cmd_show(args, config):
    _, tests = state(config)
    t = find(tests, args.id)
    annotate([t])
    print(json.dumps(public(t), indent=2) if args.json else describe(t, config["user_id"]))
    return 0


def cmd_claim(args, config):
    me = config["user_id"]
    rid, tests = state(config)
    t = find(tests, args.id)
    if t["status"] != "open":
        return refuse("%s is closed" % t["id"])
    if me in t["results"] and not args.again:
        return refuse("you already tested %s (%s); --again to test it again" % (t["id"], t["results"][me]["result"]))
    if t["pr"]:
        info = pr_info(t["pr"])  # no answer from GitHub: no claim
        if not approved(info):
            return refuse('%s waits for a maintainer: #%d is not from a collaborator and has no "%s" label'
                          % (t["id"], t["pr"], LABEL))
        if info["state"] in ("merged", "closed"):
            return refuse("#%d is %s" % (t["pr"], info["state"]))
    send(config, rid, "%s %s claim: %s" % (MARK, t["id"], until_line()))
    others = sorted(u for u in t["claims"] if u != me)
    print("claimed %s%s; renew while you test (`renew` renews all of yours)" % (
        t["id"], ", also being tested by " + ", ".join(others) if others else ""))
    return 0


def cmd_renew(args, config):
    me = config["user_id"]
    rid, tests = state(config)
    held = [t for t in tests.values() if me in t["claims"]]
    if args.id:
        t = find(tests, args.id)
        if t not in held:
            return refuse("you hold no claim on %s (it may have run out: claim it again)" % t["id"])
        held = [t]
    if not held:
        print("no claims to renew")
        return 0
    for t in held:
        send(config, rid, "%s %s renew: %s" % (MARK, t["id"], until_line()))
    print("renewed " + ", ".join(t["id"] for t in held))
    return 0


def cmd_release(args, config):
    me = config["user_id"]
    rid, tests = state(config)
    t = find(tests, args.id)
    if me not in t["claims"]:
        return refuse("you hold no claim on %s" % t["id"])
    send(config, rid, "%s %s release" % (MARK, t["id"]))
    print("released %s" % t["id"])
    return 0


def cmd_done(args, config):
    url = args.comment.strip()
    rid, tests = state(config)
    t = find(tests, args.id)
    if t["status"] != "open":
        return refuse("%s is closed" % t["id"])
    link = COMMENT_URL.match(url)
    if not link:
        return refuse("--comment must be the link to your comment on the PR (%s/pull/<n>#issuecomment-...)" % REPO)
    if t["pr"] and int(link.group(1)) != t["pr"]:
        return refuse("that comment is on #%s, the request is for #%s" % (link.group(1), t["pr"]))
    send(config, rid, "%s %s done %s: %s" % (MARK, t["id"], args.result, url))
    print("marked %s done (%s)" % (t["id"], args.result))
    return 0


def cmd_cancel(args, config):
    me = config["user_id"]
    rid, tests = state(config)
    t = find(tests, args.id)
    if t["requester"] != me:
        return refuse("only %s, who asked, can cancel %s" % (t["requester"], t["id"]))
    if t["status"] != "open":
        return refuse("%s is already closed" % t["id"])
    send(config, rid, "%s %s cancel" % (MARK, t["id"]))
    print("closed %s" % t["id"])
    return 0


def main(argv=None):
    team_chat.unicode_output()  # summaries and notes are anyone's text: no crash on Windows' code page in a pipe
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("request", help="ask for a PR to be tested")
    p.add_argument("--pr", required=True, help="PR number or link")
    p.add_argument("--summary", required=True, help="the change in one line, like a title")
    p.add_argument("--needs", help='what testing needs, one line, e.g. "AMD RX 6000 or newer, Linux"')
    p.add_argument("--notes", help="what to check and where (a few lines, for the tester)")
    p = sub.add_parser("list", help="open requests, what most needs a tester first (--all: closed ones too)")
    p.add_argument("--all", action="store_true")
    p.add_argument("--mine", action="store_true", help="only the ones you asked for, are testing or tested")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("show", help="one request")
    p.add_argument("id")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("claim", help="start testing a request")
    p.add_argument("id")
    p.add_argument("--again", action="store_true", help="test a request you already reported on")
    p = sub.add_parser("renew", help="keep your claims (all of them, or one)")
    p.add_argument("id", nargs="?")
    p = sub.add_parser("release", help="stop testing without a result")
    p.add_argument("id")
    p = sub.add_parser("done", help="finished: link your comment on the PR")
    p.add_argument("id")
    p.add_argument("--result", required=True, choices=("pass", "fail", "mixed"))
    p.add_argument("--comment", required=True)
    p = sub.add_parser("cancel", help="close your request")
    p.add_argument("id")
    args = parser.parse_args(argv)
    commands = {"request": cmd_request, "list": cmd_list, "show": cmd_show, "claim": cmd_claim, "renew": cmd_renew,
                "release": cmd_release, "done": cmd_done, "cancel": cmd_cancel}
    try:
        return commands[args.command](args, team_chat.session()) or 0
    except team_chat.ChatError as error:
        print("test_requests: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
