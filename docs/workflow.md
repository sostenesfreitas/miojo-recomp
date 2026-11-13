# Workflow: from report to release

How work moves through Hookshot, and how to decide what to work on next. It is written for people and for AI agents
alike (agents: [AGENTS.md](../AGENTS.md) sends you here). [CONTRIBUTING.md](../CONTRIBUTING.md) still holds the rules
that never bend, above all that no game files or anything made from them leave your PC.

The goal of this page is the order of work: with dozens of contributors and agents working at once, the project moves
fastest when everyone picks from the same priorities, claims before starting, and leaves a trail the next person can
follow.

## 1. Where work comes from

| Source | What lands there |
|---|---|
| GitHub issues (game problem, build problem, idea) | the record: version, hardware, place in the game, `hookshot_log.txt` |
| Tracking issues (`[Tracking] ...`) | one per family of reports (AMD rendering, no mesh shaders, Linux, VRAM, missing walls, FPS, ...) |
| Discord #bug-reporting (forum) | first reports, often without a log; turn the reproducible ones into issues |
| Discord #dev-work | claims, progress, review requests |
| Matrix `#agent-updates:magicbean.dev` | the same, posted by agents (`scripts/team_chat.py`, sign-in with Discord); copied to Discord #dev-agents |
| Matrix `#test-requests:magicbean.dev` | PRs that need a test on other hardware, who is testing them and the results ([test-requests.md](test-requests.md), `scripts/test_requests.py`); copied to Discord |
| Discord #dev-stuff | security scan results (Syntak): handled before anything else |
| CI | a red check on `main` is everyone's first problem |

## 2. Priority: what to work on first

Score a problem as **severity x reach x confidence** and work from the top. Write the three parts in the issue or the
claim, so others can see why it was picked. The scored, sorted list of open work is **[docs/worklist.md](worklist.md)**:
pick from its top, and add new problems there with their three parts.

### Severity

| Level | Weight | What it means |
|---|---|---|
| S0 | 8 | Security issue; crash, hang, device lost or a PC freeze; lost or corrupted saves; the builder cannot build |
| S1 | 5 | Cannot progress or cannot start: black screen, missing walls that block a dungeon, softlock, menus unusable |
| S2 | 3 | Wrong but playable: visual corruption, missing objects, audio out of sync, a large frame-rate loss (over 20%) |
| S3 | 1 | Polish: small visual issues, small frame-rate gains, settings, features |

### Reach: the systems people run

Reach is the share of players on the hardware the problem affects (1.0 when it affects everyone). Two numbers feed it,
and both change, so read them again rather than trusting old ones:

- **Who reports problems:** `python -I scripts/hardware_report.py` counts the GPU vendor, card class and OS in the
  issue forms (it needs the `gh` CLI). As of 2026-11-10, of 399 issues that named their hardware: AMD 51%, NVIDIA 35%,
  Intel 3.5%; by class RX 6000 16%, AMD APUs and handhelds 12%, RX 9000 10%, RTX 40 9%, RTX 50 8.5%, RTX 30 7%, cards
  without mesh shaders 14.5%; Windows 57%, Linux and SteamOS 17%. This over-counts hardware where things break.
- **Who plays:** the [Steam Hardware Survey](https://store.steampowered.com/hwsurvey/) is the best public picture of
  PC gaming hardware (vendor, card, OS). Use its current numbers for the installed base.

A problem that hits a large installed base, or that dominates the reports, ranks high either way.

### Confidence

| | Weight |
|---|---|
| Reproduced by you, or by a maintainer, with a log | 1.0 |
| Two or more reporters with logs | 0.7 |
| One report, or no log | 0.4 (ask for the log first) |

### Hardware tiers: stability across machines

A change must not break a higher tier to help a lower one.

| Tier | Hardware | What it means |
|---|---|---|
| 1 | NVIDIA RTX 20-50 and AMD RX 6000-9000, on Windows; the same on Linux (NVIDIA driver, Mesa RADV); Steam Deck | Must keep working: test here before merge, regressions are S0/S1 |
| 2 | AMD APUs and handhelds (ROG Ally, Legion Go, 780M/890M/8060S), Intel Arc | Fixed when reported, tested when someone with the hardware can |
| 3 | Cards without mesh shaders (GTX 10/16, RX 5000 and older) | Best effort through the fallback; never at a Tier 1 cost |

Today Tier 1 testing is mostly NVIDIA; AMD files half the reports. AMD stability is the largest gap, so an AMD Tier 1
problem with a log outranks an NVIDIA one of the same severity.

## 3. The lifecycle

1. **Triage.** Reproduce if you can. Find the tracking issue it belongs to, or open the issue (with the log). Score it
   (section 2).
2. **Claim.** Before starting, say so in Discord #dev-work and on the issue: what, why (the score), and on what
   hardware you can test. Search open PRs first: two people fixing the same thing is the most common waste here. Drop
   a claim out loud when you stop.
3. **Fix.** A branch from current `main`, one topic. Find the cause before the fix: a log, a trace, a measurement.
4. **Test.**
   - A game-free test that fails without the change (see AGENTS.md: tests read the source).
   - `python -I -B tests/run_all.py`, all of it.
   - In the game, on your hardware: what you did and what you saw, with numbers from `hookshot_log.txt` for performance.
   - Add a row to [tested configurations](tested-configurations.md) for each run you measured.
   - Say what you could not test (another OS, another vendor, real input devices).
5. **Pull request.** Open the description with why the PR is needed; then what changed, how it was tested, what was
   not tested, and screenshots when it shows on screen. Link the issue and the tracking issue. Post the link in
   #dev-work.
6. **Review.** Answer every comment in the PR. Rebase onto `main` before asking for review again: a PR that does not
   merge cleanly waits.
7. **Merge and release.** Release branches are merged back into `main` right after a release, so that `main`'s builds
   carry the release's fixes and the right version string (`git describe` only sees tags on `main`'s history).
8. **Verify.** Ask the reporter to retest on the release, then close the issue, or keep it open on the tracking issue
   with what is left.

## 4. The security lane

Security comes first: a scan result in #dev-stuff is triaged before other work.

- Check every finding against the code; post the finding and the verdict for each (real / false positive, and why).
- Fix the real ones in a PR, marked as security in the title.
- Record false positives in the scanner as not exploitable, with the reason, so the next scan shows only new ones.
- Never paste secrets, tokens or the scanner's private links in public channels.

## 5. Keeping the board readable

Suggested labels (a maintainer creates them): `priority:P0` to `priority:P3` (from the score), `hw:nvidia`, `hw:amd`,
`hw:intel`, `hw:no-mesh-shaders`, `os:linux`, `os:steamdeck`, `needs-log`, `reproduced`, `claimed`. With them, the
next thing to work on is the top of `is:open label:reproduced -label:claimed sort:priority`.

## For agents

- Start here, then AGENTS.md, then CONTRIBUTING.md.
- Pick from the top of [docs/worklist.md](worklist.md); add a scored row there for a new problem you find.
- Prefer problems you can reproduce on the hardware you run on.
- Claim before working, report progress where you claimed, and post the PR there. Agents post to the Matrix room
  `#agent-updates:magicbean.dev` (AGENTS.md, *Team updates*): read it before claiming.
- Measure, don't guess: every claim about speed or correctness carries the log line, the test or the screenshot.
- Never open a PR whose tests you have not run, or that has not been rebased onto `main`.
