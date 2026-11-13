---
name: team-updates
description: Turn on automatic team updates, or post one, for Hookshot work (claims, findings, pull requests, blockers) in the team's Matrix room #agent-updates:magicbean.dev, and read what others posted. Use when the user asks to enable auto updates or team updates, to sign in to the team chat, to post or share an update, asks what others are working on, before claiming work, or when the hourly update loop fires.
---

# Team updates on Matrix

The team's agents and people share one room, `#agent-updates:magicbean.dev`: claims, progress, pull requests and calls for
help, the trail [docs/workflow.md](../../../docs/workflow.md) asks for. Everything goes through
`scripts/team_chat.py` (standard library only, Windows and Linux); run it with `python -I` from the repository root.

## The flow

1. **The user asks you to enable auto updates** ("turn on team updates", "post my progress to the team", ...).
2. **You send them to sign in on git.magicbean.dev with Discord** (section 1). This is the only thing they do.
3. **From then on you post to the room on your own**, about once an hour at most and only when something happened
   (sections 2 to 4). They can stop it, or ask for an update, at any time.

## 1. Sign in (once per person)

Check first: `python -I scripts/team_chat.py whoami`. If it prints a user ID, skip to section 4.

Otherwise tell the user, in two or three lines: you will post short updates about your work together to the team room
on their behalf, at most about once an hour and only when there is something worth sharing, and for that they sign in
once with Discord. Then:

1. Run `python -I scripts/team_chat.py login --no-browser` **in the background** (it waits up to 5 minutes) and give
   the user the link it prints. It opens **git.magicbean.dev**, where they click **Sign in with discord** and approve
   on Discord. The browser ends on "Signed in. You can close this tab." and the command prints
   `signed in as @name:magicbean.dev`.
2. **First time only:** signing in with Discord creates their account on git.magicbean.dev and on the team chat;
   nothing else to set up. If the command times out, run it again and give them the new link.
3. If Discord sign-in is not an option, they can sign in to Element (app.element.io, homeserver `magicbean.dev`), copy
   the token from Settings > Help & About > Access token, and run
   `python -I scripts/team_chat.py login --access-token-stdin` themselves. Never ask them to paste a token to you.
4. Confirm with `whoami`, then go straight on to section 4: they asked for auto updates.

The session is saved outside the repository (Linux `~/.config/hookshot/team_chat.json`, Windows
`%APPDATA%\Hookshot\team_chat.json`), readable by the user only. Never print it, commit it or copy it anywhere.

## 2. What is worth posting

Post when something changed that someone else could act on or would want to know:

- **A claim** before starting work (what, the worklist score, the hardware you can test on), and dropping one when you stop.
- **A finding**: a root cause, a reproduction, a regression and the commit that caused it, a measurement.
- **A pull request** opened, merged or needing review, with its link.
- **Blocked**: what you need (a test on other hardware, a decision, a review) and from whom.
- **Done** with a task, and what is left.

Do not post: "still working on it", test runs that changed nothing, plans, a recap of the last post, anything already
in the room. If nothing is worth posting, post nothing and don't say "no updates".

**Read before you post or claim**: `python -I scripts/team_chat.py read --limit 30`. Someone may have claimed the same
thing (two people fixing the same thing is the most common waste here) or already answered your question.

## 3. How to post

```
python -I scripts/team_chat.py status
python -I scripts/team_chat.py post "Intel Arc: claimed the missing walls, testing on an A770.
- Claimed: https://github.com/NotRexed/Hookshot/issues/1234 (worklist 36): missing walls on Intel Arc.
- Working on: a capture of the broken frame on an A770, Windows."
```

- **One message: a one-line summary, then a short list.**
  - The first line says in a few words what changed (it follows the script's 🤖 prefix).
  - Each line after it starts with `- ` and a label, using only the labels that apply, in this order: `Done:`,
    `Found:`, `Claimed:`, `PR:`, `Working on:`, `Blocked:`, `Wanted:`.
  - One plain sentence a line, with the numbers that matter and the link (issue or PR) as a full URL. At most about
    five list lines.
- A single short update can stay one line (`Claimed ...`); the list is for anything with more than one item.
- **The script allows one post an hour** (exit code 3 and the reason otherwise) and refuses the same text twice.
  `--force` skips the hour only when the user asked for an update; otherwise hold the update for the next window and
  fold it into one message.
- The rules of [CONTRIBUTING.md](../../../CONTRIBUTING.md) apply: **no game files or anything made from them**, no
  title keys, nothing from `work/`. A screenshot link and lines from `hookshot_log.txt` are fine. Also no secrets,
  tokens, local paths, user names or anything personal about the user.

## 4. Auto updates

Start a recurring check right after sign-in; don't post on a timer. In Claude Code: `/loop 60m /team-updates` (other
agents: their own scheduler, or check at each milestone). Tell the user it is on and how to stop it (end the loop, or
say "stop team updates"). Each time it fires:

1. Look at what changed since the last post: your session's work, `jj log`/`git log`, the PRs you opened.
2. `read` the room.
3. If something from section 2 happened, post one message; if not, do nothing.

The user can also say "post an update" at any time: write one from the work since the last post, and use `--force` if
the hour is not up.

## Errors

- `not signed in` or `no longer valid`: run the sign-in again (section 1).
- `cannot reach`: the server is down or offline; tell the user and don't retry in a loop.
- `M_FORBIDDEN` on join: the account is not allowed in the room yet; ask the user to ask an admin.
