"""Ask the team to test a pull request on hardware you don't have, and pick up such requests, in the Matrix room
#test-requests:magicbean.dev (copied to Discord, where each request shows who is testing it and the results).

    python -I scripts/test_requests.py request --pr 2711 --summary "Keep 8 GB cards inside their memory budget" \
        [--needs "8 GB NVIDIA or AMD card, Windows"] [--notes "what to check, where in the game"] [--commit <sha>]
    python -I scripts/test_requests.py list [--all] [--mine] [--json]
    python -I scripts/test_requests.py show T-7KQ2 [--json]
    python -I scripts/test_requests.py claim T-7KQ2 [--again] [--commit <confirmed-sha>]
    python -I scripts/test_requests.py verify T-7KQ2 --repo <test worktree>
    python -I scripts/test_requests.py renew [T-7KQ2]
    python -I scripts/test_requests.py release T-7KQ2
    python -I scripts/test_requests.py done T-7KQ2 --result pass|fail|mixed --comment <URL of your comment on the PR>
        [--commit <sha>]
    python -I scripts/test_requests.py update T-7KQ2 --commit <sha>
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

request pins the PR head (or --commit); claim records what to build, and done records what was tested. Commit and Head
lines follow the existing message text. update moves the pin only at the requester's request; old results stay in the
record, but do not count as tests of the new commit. Old messages without these lines still work.

list puts first what most needs a tester: requests nobody has tested or is testing, then the ones being tested with no
result yet, then the ones with results; oldest first within each. list checks approval and head once per PR, as does
show for one request. done records results even if its head check fails. Exit code 3: the action was refused (closed,
not yours, already tested by you).
Standard library only.
"""
import argparse
import calendar
import json
import os
import random
import re
import subprocess
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

LINE = re.compile(r"^%s\s*(T-[%s]{4})\s+(request|claim|renew|release|done|cancel|update)\b(?:\s+(pass|fail|mixed))?\s*:?\s*(.*)$"
                  % (MARK, ID_CHARS))
UNTIL = re.compile(r"until\s+(\d{4}-\d\d-\d\d)[ T](\d\d:\d\d)Z", re.I)
PR_URL = re.compile(r"https://github\.com/NotRexed/Hookshot/pull/(\d+)")
COMMENT_URL = re.compile(r"^https://github\.com/NotRexed/Hookshot/pull/(\d+)"
                         r"#(?:issuecomment-\d+|pullrequestreview-\d+|discussion_r\d+)$")
COMMIT = re.compile(r"[0-9a-fA-F]{7,40}")


def commit_arg(text):
    if not COMMIT.fullmatch(text):
        raise argparse.ArgumentTypeError("a commit must be 7 to 40 hexadecimal characters")
    return text.lower()


def commit_field(text, name="Commit"):
    value = None
    for line in text.splitlines():
        key, sep, sha = line.strip().partition(":")
        if sep and key.lower() == name.lower() and COMMIT.fullmatch(sha.strip()):
            value = sha.strip().lower()
    return value


def request_fields(text):
    """Only the appended footer is metadata; Commit/Head lines inside notes are ordinary text."""
    lines = text.splitlines()
    if len(lines) >= 2:
        commit, head = commit_field(lines[-2]), commit_field(lines[-1], "Head")
        if commit and head:
            return "\n".join(lines[:-2]), commit, head
    commit = commit_field(lines[-1]) if lines else None
    return ("\n".join(lines[:-1]), commit, None) if commit else (text, None, None)


def same_commit(a, b):
    return bool(a and b) and (a.startswith(b) or b.startswith(a))


def chosen_commit(commit, head):
    return head if not commit or same_commit(commit, head) else commit


def commit_notice(t):
    commit, head = t.get("commit"), t.get("head")
    if not commit or not head:
        return ""
    before = t.get("requested_head")
    if before and not same_commit(before, head):
        return ("head moved since the request: %s -> %s; requested commit is still %s. "
                "Ask the requester to confirm which commit is wanted." % (before[:8], head[:8], commit[:8]))
    if not same_commit(commit, head):
        return "requested commit %s is not the PR head %s (explicit choice)" % (commit[:8], head[:8])
    return ""


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
    try:
        return calendar.timegm(time.strptime(m.group(1) + " " + m.group(2), "%Y-%m-%d %H:%M"))
    except ValueError:  # a date that is not one (month 13, 99:99): the claim gets the default, as with none
        return None


def clear_claim(t, user):
    t["claims"].pop(user, None)
    t.get("claim_commits", {}).pop(user, None)
    t.get("claim_heads", {}).pop(user, None)


def expire(t, ts):
    """Drop the claims that ran out by ts."""
    for user, until in sorted(t["claims"].items(), key=lambda kv: kv[1]):
        if until <= ts:
            clear_claim(t, user)
            t["history"].append((until, user, "expired"))


def replay(events, now=None):
    """Requests by ID, from the room's messages oldest first: each event a dict with sender, origin_server_ts (ms)
    and body. Extra Commit/Head lines are optional; the first line keeps the original format."""
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
            more, commit, head = request_fields(more)
            for extra in more.splitlines():
                extra = extra.strip()
                if extra.lower().startswith("needs:") and not needs:
                    needs = extra[6:].strip()
                elif extra and not (pr and extra == pr.group(0)):
                    notes.append(extra)
            tests[tid] = {"id": tid, "requester": sender, "requested": ts, "summary": rest, "needs": needs,
                          "pr": int(pr.group(1)) if pr else None, "notes": "\n".join(notes), "status": "open",
                          "claims": {}, "results": {}, "history": [(ts, sender, action)]}
            if commit:
                tests[tid]["commit"] = commit
            if head:
                tests[tid].update(head=head, requested_head=head)
            continue
        if not t or t["status"] != "open":
            continue
        expire(t, ts)
        mine = sender in t["claims"]
        commit, head = commit_field(more), commit_field(more, "Head")
        if action == "claim" or action == "renew" and mine:
            until = until_time(rest)
            if until is None or until <= ts:
                until = ts + 60 * DEFAULT_TTL
            t["claims"][sender] = min(until, ts + 60 * MAX_TTL)
            if action == "claim" and (not mine or commit):
                if commit:
                    t.setdefault("claim_commits", {})[sender] = commit
                else:
                    t.get("claim_commits", {}).pop(sender, None)
                if head:
                    t.setdefault("claim_heads", {})[sender] = head
                else:
                    t.get("claim_heads", {}).pop(sender, None)
            action = "renew" if mine else "claim"
        elif action == "release" and mine:
            clear_claim(t, sender)
        elif action == "done" and result:
            link = COMMENT_URL.match(rest)
            if not link or t["pr"] and int(link.group(1)) != t["pr"]:
                continue
            clear_claim(t, sender)
            t["results"][sender] = {"result": result, "comment": rest, "time": ts}
            if commit:
                t["results"][sender]["commit"] = commit
                if head:
                    t["results"][sender]["head"] = head
        elif action == "update" and sender == t["requester"] and COMMIT.fullmatch(rest):
            t.update(commit=rest.lower(), requested_head=head or rest.lower())
        elif action == "cancel" and sender == t["requester"]:
            t["status"], t["claims"] = "closed", {}
            t.get("claim_commits", {}).clear()
            t.get("claim_heads", {}).clear()
        else:
            continue
        if head:
            t["head"] = head
        t["history"].append((ts, sender, action))
    for t in tests.values():
        if t["status"] == "open":
            expire(t, now)
    return tests


def current_results(t):
    """Keep old results in the record, but never count them as tests of a new requested commit."""
    if not t.get("commit"):
        return t["results"]
    return {u: r for u, r in t["results"].items() if same_commit(r.get("commit"), t["commit"])}


def need(t):
    """Sort key: what most needs a tester first; requests waiting for a maintainer last."""
    return (t.get("approved") is False, bool(current_results(t)), bool(t["claims"]), t["requested"])


def github_json(url, number):
    """One GitHub answer, with the same optional token and rate-limit care for every endpoint."""
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                                           "User-Agent": team_chat.USER_AGENT})
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        raise team_chat.ChatError("GitHub answered %d for PR #%d%s" % (
            error.code, number, " (rate limit: set GITHUB_TOKEN)" if error.code in (403, 429) else "")) from None
    except (urllib.error.URLError, OSError, ValueError) as error:
        raise team_chat.ChatError("cannot reach GitHub to check PR #%d: %s" % (number, error)) from None


def pr_info(number):
    """The PR's state, labels, author's standing and head commit, in one GitHub call."""
    pr = github_json(PR_API % number, number)
    state = "merged" if pr.get("merged_at") else "closed" if pr.get("state") == "closed" \
        else "draft" if pr.get("draft") else "open"
    head = (pr.get("head") or {}).get("sha", "")
    if not isinstance(head, str) or not COMMIT.fullmatch(head):
        raise team_chat.ChatError("GitHub did not return a head commit for PR #%d" % number)
    return {"state": state, "labels": [label.get("name") for label in pr.get("labels", [])],
            "author": (pr.get("user") or {}).get("login"), "association": pr.get("author_association"),
            "head": head.lower(), "base": (pr.get("base") or {}).get("sha")}


def approved(info):
    return info["association"] in TRUSTED or LABEL in info["labels"]


def pr_commit(number, commit, head):
    """Resolve a non-head commit and require that it is an ancestor of the PR head."""
    if not commit or same_commit(commit, head):
        return head
    url = "https://api.github.com/repos/NotRexed/Hookshot/compare/%s...%s?per_page=1" % (commit, head)
    comparison = github_json(url, number)
    sha = (comparison.get("base_commit") or {}).get("sha", "")
    if comparison.get("status") not in ("ahead", "identical") or not isinstance(sha, str) \
            or len(sha) != 40 or not COMMIT.fullmatch(sha) or not same_commit(commit, sha.lower()):
        raise team_chat.ChatError("commit %s is not a verified ancestor of PR #%d head %s" % (commit, number, head))
    return sha.lower()


def git(repo, *args):
    try:
        result = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=120)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise team_chat.ChatError("cannot check the test repository: %s" % error) from None
    if result.returncode:
        raise team_chat.ChatError("git %s failed: %s" % (" ".join(args), result.stderr.strip()))
    return result.stdout.strip()


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
        if info:
            t["head"] = info["head"]


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
    if t.get("commit"):
        lines.append("    commit: " + t["commit"][:8])
    notice = commit_notice(t)
    if notice:
        lines.append("    " + notice)
    if t["needs"]:
        lines.append("    needs: " + t["needs"])
    if t["claims"]:
        lines.append("    being tested by " + ", ".join(who(u) for u in sorted(t["claims"])))
    for user, r in sorted(t["results"].items(), key=lambda kv: kv[1]["time"]):
        detail = ""
        if r.get("commit"):
            detail = " (commit %s)" % r["commit"][:8]
            if r.get("head") and not same_commit(r["commit"], r["head"]):
                detail += " (tested an earlier commit; PR head at result: %s)" % r["head"][:8]
            if t.get("commit") and not same_commit(r["commit"], t["commit"]):
                detail += " (not the requested commit)"
        elif t.get("commit"):
            detail = " (commit not recorded)"
        lines.append("    %s by %s%s: %s" % (r["result"], who(user), detail, r["comment"]))
    if t.get("approved") is False:
        lines.append('    waiting for a maintainer to add the "%s" label on GitHub: not claimable yet' % LABEL)
    elif not t["claims"] and not current_results(t) and t["status"] == "open":
        lines.append("    nobody has tested %s yet" % ("this commit" if t.get("commit") else "it"))
    if t.get("pr_state") in ("merged", "closed"):
        lines.append("    the PR is %s" % t["pr_state"])
    if t["notes"]:
        lines += ["    " + line for line in t["notes"].splitlines()]
    return "\n".join(lines)


def public(t):
    result = {k: v for k, v in t.items() if k != "history"}
    result["current_results"] = current_results(t)
    return result


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
    try:
        info = pr_info(pr)  # the approval answer also supplies the pin
    except team_chat.ChatError as error:
        if not args.commit or len(args.commit) != 40:
            raise team_chat.ChatError("%s; cannot determine the PR head: retry when GitHub is available or "
                                      "use --commit with a full 40-character SHA" % error) from None
        info, head_error = None, str(error)
    commit = pr_commit(pr, args.commit, info["head"]) if info else args.commit
    tid = new_id(tests)
    text = "%s %s request: %s\n" % (MARK, tid, summary)
    if needs:
        text += "Needs: %s\n" % needs
    text += "%s/pull/%d" % (REPO, pr)
    if args.notes and args.notes.strip():
        text += "\n" + args.notes.strip()
    text += "\nCommit: %s" % commit
    if info:
        text += "\nHead: %s" % info["head"]
    send(config, rid, text)
    print("asked for testing as %s" % tid)
    print("commit to build: " + commit)
    if not info:
        print("note: PR head and approval could not be checked (%s); posted with the explicit commit, "
              "approval is unknown" % head_error)
        return 0
    notice = commit_notice({"commit": commit, "head": info["head"], "requested_head": info["head"]})
    if notice:
        print("note: " + notice)
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
    results = current_results(t)
    if me in results and not args.again:
        return refuse("you already tested the current request %s (%s); a moved PR head does not change its pin. "
                      "Wait for the card to be updated to a new commit, or --again to repeat this test deliberately"
                      % (t["id"], results[me]["result"]))
    commit = t.get("claim_commits", {}).get(me) if me in t["claims"] else None
    text = "%s %s claim: %s" % (MARK, t["id"], until_line())
    if t["pr"]:
        info = pr_info(t["pr"])  # no answer from GitHub: no claim
        if not approved(info):
            return refuse('%s waits for a maintainer: #%d is not from a collaborator and has no "%s" label'
                          % (t["id"], t["pr"], LABEL))
        if info["state"] in ("merged", "closed"):
            return refuse("#%d is %s" % (t["pr"], info["state"]))
        t["head"] = info["head"]
        commit = commit or t.get("commit") or info["head"]
        if args.commit and not same_commit(args.commit, commit):
            return refuse("--commit must confirm %s; ask the requester to update the card to choose another commit"
                          % commit)
        before = t.get("requested_head")
        changed = not same_commit(before, info["head"]) if before else not same_commit(commit, info["head"])
        if changed and not args.commit:
            return refuse("%s No claim sent; after the requester confirms, use claim %s --commit %s"
                          % (commit_notice(t), t["id"], commit))
        text += "\nCommit: %s\nHead: %s" % (commit, info["head"])
    send(config, rid, text)
    others = sorted(u for u in t["claims"] if u != me)
    print("claimed %s%s; renew while you test (`renew` renews all of yours)" % (
        t["id"], ", also being tested by " + ", ".join(others) if others else ""))
    if commit:
        print("commit to build: " + commit)
    notice = commit_notice(t)
    if notice:
        print("note: " + notice)
    return 0


def cmd_verify(args, config):
    """Check membership using the PR's fetched history, then print the exact pinned diff for review."""
    me = config["user_id"]
    rid, tests = state(config)
    t = find(tests, args.id)
    if me not in t["claims"]:
        return refuse("claim %s before verifying a build" % t["id"])
    commit = t.get("claim_commits", {}).get(me)
    try:
        if not t["pr"] or not commit:
            raise team_chat.ChatError("this claim has no recorded PR commit; use a current client to claim it")
        info = pr_info(t["pr"])
        if not approved(info) or info["state"] in ("merged", "closed"):
            raise team_chat.ChatError("the PR is no longer open and approved for testing")
        if t.get("claim_heads", {}).get(me) != info["head"]:
            raise team_chat.ChatError("the PR head moved since the claim; ask the requester and claim again")
        base = info.get("base") or ""
        if not isinstance(base, str) or not COMMIT.fullmatch(base):
            raise team_chat.ChatError("GitHub did not return the PR base commit")
        git(args.repo, "fetch", "--no-tags", "--no-recurse-submodules", REPO, "refs/pull/%d/head" % t["pr"])
        head = git(args.repo, "rev-parse", "--verify", "FETCH_HEAD^{commit}")
        if head != info["head"]:
            raise team_chat.ChatError("the PR head moved during verification; claim and check it again")
        pin = git(args.repo, "rev-parse", "--verify", commit + "^{commit}")
        try:
            git(args.repo, "merge-base", "--is-ancestor", pin, head)
        except team_chat.ChatError:
            raise team_chat.ChatError("commit %s is not a verified ancestor of PR #%d head %s"
                                      % (commit, t["pr"], head)) from None
        git(args.repo, "fetch", "--no-tags", "--no-recurse-submodules", REPO, base)
        diff = git(args.repo, "diff", "--no-ext-diff", "--no-textconv", base + "..." + pin, "--")
    except team_chat.ChatError as error:
        send(config, rid, "%s %s release" % (MARK, t["id"]))
        return refuse("%s; released %s, do not build it" % (error, t["id"]))
    print("verified commit: %s (ancestor of PR #%d head %s)" % (pin, t["pr"], head))
    print("Review this pinned commit's diff before checking it out or building:\n" + diff)
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
    commit = args.commit or t.get("claim_commits", {}).get(config["user_id"])
    if t.get("commit") and not commit:
        return refuse("name the tested commit with --commit (no live claim with a recorded commit)")
    text = "%s %s done %s: %s" % (MARK, t["id"], args.result, url)
    detail, info = "", None
    if t["pr"]:
        try:
            info = pr_info(t["pr"])
        except team_chat.ChatError as error:
            print("note: PR head could not be checked (%s); recording the result without a head comparison" % error)
    if commit:
        if info:
            commit = chosen_commit(commit, info["head"])
        text += "\nCommit: %s" % commit
        detail = ", commit %s" % commit[:8]
        if info:
            text += "\nHead: %s" % info["head"]
            if not same_commit(commit, info["head"]):
                detail += "; tested an earlier commit, current PR head %s" % info["head"][:8]
    send(config, rid, text)
    print("marked %s done (%s%s)" % (t["id"], args.result, detail))
    return 0


def cmd_update(args, config):
    rid, tests = state(config)
    t = find(tests, args.id)
    if t["requester"] != config["user_id"]:
        return refuse("only %s, who asked, can update %s" % (t["requester"], t["id"]))
    if t["status"] != "open":
        return refuse("%s is closed" % t["id"])
    if not t["pr"]:
        return refuse("%s has no PR" % t["id"])
    info = pr_info(t["pr"])
    commit = pr_commit(t["pr"], args.commit, info["head"])
    send(config, rid, "%s %s update: %s\nHead: %s" % (MARK, t["id"], commit, info["head"]))
    print("updated %s; commit to build: %s (existing claims keep their commit)" % (t["id"], commit))
    notice = commit_notice({"commit": commit, "head": info["head"], "requested_head": info["head"]})
    if notice:
        print("note: " + notice)
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
    p.add_argument("--commit", type=commit_arg, help="commit to test (default: PR head; 7 to 40 hex characters)")
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
    p.add_argument("--commit", type=commit_arg, help="confirm the pin after the requester confirms a moved head")
    p = sub.add_parser("verify", help="verify the claimed commit belongs to the PR and print its diff for review")
    p.add_argument("id")
    p.add_argument("--repo", required=True, help="test worktree; fetches only, does not check out or build")
    p = sub.add_parser("renew", help="keep your claims (all of them, or one)")
    p.add_argument("id", nargs="?")
    p = sub.add_parser("release", help="stop testing without a result")
    p.add_argument("id")
    p = sub.add_parser("done", help="finished: link your comment on the PR")
    p.add_argument("id")
    p.add_argument("--result", required=True, choices=("pass", "fail", "mixed"))
    p.add_argument("--comment", required=True)
    p.add_argument("--commit", type=commit_arg, help="commit tested (default: the one recorded for your live claim)")
    p = sub.add_parser("update", help="move your open request to another commit; keep claims and past results")
    p.add_argument("id")
    p.add_argument("--commit", required=True, type=commit_arg, help="new commit to test (7 to 40 hex characters)")
    p = sub.add_parser("cancel", help="close your request")
    p.add_argument("id")
    args = parser.parse_args(argv)
    commands = {"request": cmd_request, "list": cmd_list, "show": cmd_show, "claim": cmd_claim, "renew": cmd_renew,
                "release": cmd_release, "done": cmd_done, "cancel": cmd_cancel, "update": cmd_update,
                "verify": cmd_verify}
    try:
        return commands[args.command](args, team_chat.session()) or 0
    except team_chat.ChatError as error:
        print("test_requests: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
