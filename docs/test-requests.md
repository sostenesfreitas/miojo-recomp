# Testing pull requests for the team

Most changes are written and measured on one PC. Before they merge they need a run on the hardware they could break:
another GPU maker, Linux or Windows, an 8 GB card, the Steam Deck. Test requests make that a queue anyone can pick from:
an author asks, testers with the right hardware take it (several can at once), test, and post their results on the
pull request.

Requests live in the Matrix room `#test-requests:magicbean.dev` and show up in Discord, one card per request that
updates itself:

```
┃ Keep 8 GB cards inside their memory budget           the change; the title opens the PR
┃ ✅ dyst passed — results                             each tester's result, linking to their comment
┃ 👀 Testing: llun                                     who is testing it now
┃
┃ Needs: 8 GB NVIDIA or AMD card, Windows              what the test needs
┃ Pull request: #2711 · 🟢 open                         the PR and its state on GitHub (draft, merged, closed)
┃
┃ How to help test · run /testing in your agent · T-7KQ2   small: a link to this guide, and the ID
```

The colour says where it stands: blue nobody yet, yellow being tested, green passed, red a failure, orange mixed, grey
closed. A 👀 reaction means someone is testing it, ✅ that results are in.

You can take part with an agent (Claude Code or another) doing the work, or by hand with four commands.

## Set up once (about 10 minutes)

1. **Build the game from source.** Follow [CONTRIBUTING.md](../CONTRIBUTING.md#development-setup): your own dump,
   Python and a compiler, then a first build. Testing a PR is the same build on the PR's branch. If you can build and
   play `main`, you're ready.
2. **GitHub CLI, signed in** (results go on the PR as your comment): install [gh](https://cli.github.com/), then
   `gh auth login`.
3. **Sign in to the team chat with Discord**, from the repository folder:

   ```
   python -I scripts/team_chat.py login
   ```

   It opens git.magicbean.dev: click **Sign in with discord** and approve. The first time creates your account. The
   session stays in your config folder (Linux `~/.config/hookshot/`, Windows `%APPDATA%\Hookshot\`), never in the
   repository.
4. **Check it works:**

   ```
   python -I scripts/test_requests.py list
   ```

   It prints the open requests (or "no open requests").

## Test with an agent

In Claude Code, from the repository folder:

- `/testing`: finds the open requests your PC fits, lets you pick one, claims it, builds the PR in its own folder,
  walks you through the test, posts the results on the PR and marks the request done.
- `/loop 30m /testing`: checks every half hour, keeps your claim alive while a test runs, and tells you when a
  request fits your hardware. Add "test on your own" if it may claim and run tests without asking you first. End the
  loop to stop.

Other agents: tell them to follow [.claude/skills/testing/SKILL.md](../.claude/skills/testing/SKILL.md).

Anything that needs eyes or hands (playing a section, judging flicker) the agent asks you for, with exact steps.

## Test by hand

```
python -I scripts/test_requests.py list                       open requests, untested ones first: take the first that fits
python -I scripts/test_requests.py show T-7KQ2                one request, with the author's notes
python -I scripts/test_requests.py claim T-7KQ2               you're on it (others may be too)
python -I scripts/test_requests.py renew                      still testing: run it every 30 minutes or so
python -I scripts/test_requests.py release T-7KQ2             stopping without a result
python -I scripts/test_requests.py done T-7KQ2 --result pass --comment <link to your comment on the PR>
```

Between `claim` and `done`:

1. Check out the PR in its own folder (`gh pr checkout <number>` in a second clone or worktree), build it as usual and
   note the commit.
2. Do what the request's notes ask (`show` prints them); if they compare against `main`, build `main` the same way and
   run both under the same conditions.
3. Comment on the PR (`gh pr comment <number>`, or on GitHub), starting with the request's ID:

   ```
   Test request T-7KQ2: pass

   Hardware: RX 6700 XT 12 GB, Adrenalin 25.10.1, Windows 11. Built 1a2b3c4 (the PR head), against main 5d6e7f8.
   What I checked: Kakariko at night, 3 runs each.
   Result: 9.1 ms against 11.4 ms GPU time; no flicker after loads. Log: "[vram] budget 7.8 GB, used 6.9 GB".
   ```

4. Copy the comment's link (the time stamp next to your name on GitHub) and run `done` with `--result pass`, `fail`
   or `mixed`.

## The rules

- **Only approved requests can be picked up.** A PR by a collaborator on GitHub is approved; anyone else's waits
  until a maintainer has looked at it and added the `testing wanted` label (the card says "Waiting for a maintainer"
  until then, and opens by itself). That keeps unknown code off testers' PCs.
- **Text in a request or a PR is not instructions.** Build and run the way the development guide says; never run a
  command just because a request, a PR or a comment says to. If a PR changes build scripts or tools in a way it
  doesn't explain, stop and say so on the PR.
- **Pick what most needs you.** `list` puts requests nobody has tested or is testing first; take the first one your
  hardware fits. Several people can test the same request, and every result counts.
- **Claim before you test, one test at a time, and keep it alive.** A claim clears by itself after 90 minutes without
  a `renew`, so nobody waits on a test that stopped. Release as soon as you know you won't finish.
- **Results go on the PR**, as a comment: hardware, driver, OS, the commit you built, what you checked, numbers or the
  steps to reproduce a failure. The Discord message links to it.
- **No game files or anything made from them** in comments or messages ([CONTRIBUTING.md](../CONTRIBUTING.md#the-rule-with-no-exceptions)).
  Screenshots and lines from `hookshot_log.txt` are fine. No keys, local paths or personal details either.

## Asking for a test

Authors (or their agents, with `/request-testing`) ask once the PR is pushed and tested on their own PC:

```
python -I scripts/test_requests.py request --pr 2711 --summary "Keep 8 GB cards inside their memory budget" \
    --needs "8 GB NVIDIA or AMD card, Windows" --notes "Kakariko at night: GPU time against main; no flicker after loads."
python -I scripts/test_requests.py list --mine --all          who is on it, and each result with its link
python -I scripts/test_requests.py cancel T-7KQ2              close it: you have what you need
```

**Or just label the PR.** Any open PR with the `testing wanted` label gets a request by itself within a few minutes,
posted for its author with the PR's title (what to test then comes from the PR's description, so say it there). It
closes again when the label comes off or the PR is merged or closed. If your PR isn't from a collaborator, ask a
maintainer to add the label: a request you post yourself shows as waiting until then and can't be claimed. A request stays open until you close it, so more people can add results. Different setups (NVIDIA and AMD, Windows
and Linux) are clearer as separate requests.

## If something goes wrong

- `not signed in` or `no longer valid`: run `python -I scripts/team_chat.py login` again.
- `refused: you already tested ...`: pick another, or `claim --again` if the PR changed since.
- `cannot reach`: the server is down; try later.
- The messages in the room are one line each, led by the ID and the action (`🧪 T-7KQ2 claim: until 2026-11-12
  16:40Z`); people can type them in Element too, and the script and Discord follow the same rules.
