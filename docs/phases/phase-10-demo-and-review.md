# Phase 10 — Demo script and final review

## Goal
A one-command demo of the full system and a final check that the code matches DESIGN.md. **The owner writes README.md — do not create or edit it.**

## Read first
`docs/DESIGN.md` (all), every phase file's "Done when" list.

## Create / change
- `scripts/demo.sh` — builds, creates `demo/` with a 50 MB random file, starts 3 trackers and 3 clients in the background with logs in `demo/logs/`, drives the clients through a scripted session (use named pipes or `printf ... | ./client`), and prints PASS/FAIL per step.
- `scripts/clean.sh` — kills demo processes, removes `demo/`, `*.log`, `*.progress`, `*.shares`.
- Remove debug commands from normal help output (keep them working only when `P2P_DEBUG=1`).

## Demo steps (`demo.sh`)
1. 3 trackers up; tracker 0 is master.
2. A creates user + group on tracker 0, B and C join from trackers 1 and 2, A accepts.
3. A uploads the 50 MB file; B downloads; `sha1sum` match → PASS.
4. C starts downloading; B is used as a seeder for some chunks (grep C's log) → PASS.
5. Kill the file's owner tracker; C's download (or a new one) completes via backup → PASS.
6. Kill master; new master elected (grep logs) → PASS.
7. Restart the killed tracker; it catches up (grep `caught up`) → PASS.
8. B leaves the group; owner's `debug_files` no longer lists B → PASS.

## Final review checklist (report each as yes/no with file:line)
- No `send`/`recv`/`connect` inside a locked scope.
- Lock order respected everywhere (`groups_mtx` → `group_rec::mtx` → `peers_mtx` → `log_mtx`).
- Fixed `N` from config used in every hash (never live count).
- Owner never stored in `group_rec`.
- `group_rec.files` is the only file list.
- No outbox thread, no tracker thread pool, no quorum, no peer scoring.
- Every file descriptor and socket closed on all paths.
- `make` builds with `-Wall` and zero warnings.

## Done when
- `./scripts/demo.sh` prints PASS for all 8 steps on a clean checkout.
- Checklist reported with no "no".
