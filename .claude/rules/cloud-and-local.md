# Working on azahar: local PC vs cloud session

<!-- Managed by claude-cloud-kit. Edit the kit, re-run generate.py, re-run bootstrap. -->

**Cloud viability: Docs/review only - the real toolchain for this repo does not exist in a cloud session.**

Trunk branch: `master`

> This repo is a **fork** of an upstream project. Keep changes minimal and in separate commits so syncing with upstream stays clean. Do not open PRs against upstream without being asked.

## Where am I running?

Check `$CLAUDE_CODE_REMOTE`. It is `true` in a Claude Code cloud session and
unset locally. Never assume; the two environments differ in ways that matter.

| | Local (Windows PC) | Cloud session |
|---|---|---|
| OS | Windows | Ubuntu 24.04, root |
| Paths | `C:\Users\abhis\...` | `/home/...`, POSIX |
| Resources | your machine | ~4 vCPU, 16 GB RAM, 30 GB disk |
| Untracked local files | present | **absent** - only what is committed |
| GUI / emulators / devices | available | **not available** |
| Network | open | proxied allowlist (Trusted by default) |

Any command in this repo's docs written with a Windows path or a PowerShell
idiom needs translating before it runs in a cloud session. Translate it; do not
guess that it will work.

## Do this in a cloud session

- source reading, small patches

## Do NOT attempt this in a cloud session

- a full build -- too heavy for a cloud session; build locally

If a task lands in the second list, say so and stop rather than producing an
unverifiable result. "The harness passed" is not the same claim as "it works".

## Session hygiene (applies everywhere)

- Push after every commit, not just at the end of a session.
- Sequential work by default. Parallel subagents each cold-start and re-read
  shared context, and that redundancy is paid for. Only parallelise genuinely
  siloed tasks.
- Give unattended runs an explicit stop condition: same error 3x -> escalate
  once -> log it as blocked and move on. Never loop.
- Record which kind of verification actually happened (harness vs. real
  hardware) in the commit message, not just that "tests pass".
