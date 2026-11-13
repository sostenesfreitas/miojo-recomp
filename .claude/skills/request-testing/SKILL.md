---
name: request-testing
description: Ask the team to test a Hookshot pull request on hardware you don't have (another GPU maker, OS, Steam Deck, 8 GB card), follow who is testing it, and read the results they post on the PR. Use when the user asks for testing on other hardware, says a PR needs a tester, asks whether anyone tested it or what the testers found, or wants to withdraw a request.
---

# Request testing

Requests go to the Matrix room `#test-requests:magicbean.dev`, which is copied to Discord: each request is one message
there that shows 🟢 open, 👀 who is testing it and until when, and ✅ the result with a link to the tester's comment on
the PR. Testers pick requests up with `/testing`. Everything goes through `scripts/test_requests.py` (standard library,
Windows and Linux; run it with `python -I` from the repository root). It uses the team chat sign-in: if
`python -I scripts/team_chat.py whoami` prints no user, sign in first as in `/team-updates`, section 1.

## 1. Ask

Only for a PR that is pushed, open and tested on your own hardware: a tester builds what is on GitHub, and their time
is the scarce part. Then:

```
python -I scripts/test_requests.py request --pr 2711 --summary "Keep 8 GB cards inside their memory budget" \
    --needs "AMD RX 6000 or newer, Windows" \
    --notes "Kakariko at night: frame time against main, and no flicker after a load. Log lines with [vram]."
```

- `--summary`: the change in one line, like a title (the PR's title, shortened if needed). It heads the Discord
  message.
- `--needs`: one line, what the test needs (GPU maker and generation, OS, VRAM size, Steam Deck); leave it out when
  any PC will do. Several setups wanted: one request each, so each gets its own tester.
- `--notes`: for the tester (shown by `show`, not on Discord), 1 to 4 lines they can follow without reading the
  whole PR: where in the game, what to compare, what counts as a pass, which log lines or numbers to copy. Before and
  after (main against the PR) when it is about speed.
- The rules of [CONTRIBUTING.md](../../../CONTRIBUTING.md) apply: no game files or anything made from them, no
  secrets or personal details.
- The script refuses a second open request from you for the same PR and needs.
- **The label is a request too:** any open PR labelled `testing wanted` gets a request posted for its author within a
  few minutes (the PR's title as the summary, no needs or notes: put what to test in the PR's description). Don't
  post a second one for the same PR; `list` shows it as asked by the relay. It closes when the label comes off or
  the PR is merged or closed.
- **Approval:** a request can be claimed only when the PR's author is a collaborator on GitHub, or a maintainer has put
  the `testing wanted` label on the PR. Otherwise the script says so, and the Discord card shows "Waiting for a
  maintainer" until the label is added (the card opens by itself within a few minutes). Tell the user to ask a
  maintainer for the label; don't add it yourself unless the user is a maintainer and asks you to.

Tell the user the request ID (`T-XXXX`). If they want it followed up, check back later (Claude Code:
`/loop 2h /request-testing check T-XXXX`); don't wait in a loop on your own.

## 2. Follow up and read the results

```
python -I scripts/test_requests.py list --mine --all     (your requests, with who is testing and the results)
python -I scripts/test_requests.py show T-XXXX --json
```

- Several people can test one request. `claims` lists who is testing it now (a claim clears by itself when its
  tester stops renewing it); `results` has each tester's result (pass, fail or mixed) and `comment`, the link to their
  comment on the PR. Read it:
  `gh api repos/NotRexed/Hookshot/issues/comments/<number after #issuecomment->` (a review:
  `gh api repos/NotRexed/Hookshot/pulls/<pr>/reviews/<number>`), or `gh pr view <pr> --comments`.
- Report it to the user in a few lines: who tested, on what, the result, the numbers or the failure. If it failed,
  continue with the failure on the PR (reproduce, fix, push), then close the request and ask again with a new one.

## 3. Close it

A request stays open, and more testers can pick it up, until you close it: when you have the results you need, or
the PR is merged or closed: `python -I scripts/test_requests.py cancel T-XXXX`. Only the person who asked can.

## The messages

Each message in the room is one line led by the ID and the action, so people can also type them in Element:

```
🧪 T-7KQ2 request: Keep 8 GB cards inside their memory budget
Needs: AMD RX 6000 or newer, Windows                (then the PR link, then the notes)
🧪 T-7KQ2 claim: until 2026-11-12 16:40Z
🧪 T-7KQ2 renew: until 2026-11-12 18:10Z
🧪 T-7KQ2 release
🧪 T-7KQ2 done pass: https://github.com/NotRexed/Hookshot/pull/2711#issuecomment-123
🧪 T-7KQ2 cancel
```

Errors: exit code 3 is a refusal with the reason (not yours, already done); `not signed in` or `no longer valid`:
sign in again; `cannot reach`: the server is down, tell the user.
