# How to run the phases with Claude Code

1. Put `CLAUDE.md` at the repo root and `docs/` next to it (this folder).
2. Start Claude Code in the repo root.
3. For each phase, paste exactly:

   > Implement `docs/phases/phase-NN-<name>.md`. Follow `CLAUDE.md`. Do not commit.

4. When it finishes, run every "Done when" check yourself.
5. Review `git diff`, then commit by hand, e.g. `git commit -m "phase 3: file metadata"`.
6. Only then start the next phase.

If a check fails, paste the failing output back with:

> Phase NN check K fails: <output>. Fix it within the same phase scope.

## Phases

| # | File | Result you can demo |
| --- | --- | --- |
| 1 | `phase-01-common.md` | framed TCP helpers, SHA1, config, Makefile |
| 2 | `phase-02-single-tracker-control.md` | users, login, groups on one tracker |
| 3 | `phase-03-file-metadata.md` | upload/download metadata, list_files |
| 4 | `phase-04-peer-transfer.md` | real P2P download with SHA1 checks |
| 5 | `phase-05-rarest-resume-seeder.md` | rarest-first, resume, downloader becomes seeder |
| 6 | `phase-06-multi-tracker-control.md` | 3 trackers, blocking creates, op log |
| 7 | `phase-07-partitioning-redirect.md` | owner/backup per file, redirect, fallback |
| 8 | `phase-08-heartbeat-recovery.md` | heartbeat, election, catch-up, restart recovery |
| 9 | `phase-09-online-security-failover.md` | online filter, 3 peers per chunk, leave security, client failover |
| 10 | `phase-10-demo-and-review.md` | one-command demo + final checklist |

After phase 5 you already have a complete single-tracker BitTorrent that is safe to push and demo.
