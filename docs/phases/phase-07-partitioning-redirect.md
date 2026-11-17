# Phase 7 — Partitioned file metadata, redirect, backup replica

## Goal
Each file's full metadata lives on its owner tracker plus one backup. Clients are redirected to the owner over a temporary connection and fall back to the backup when the owner is down.

## Read first
`docs/DESIGN.md` §2, §4.1 (`REDIRECT`), §4.2, §4.3 (`REPL META/HAVE/STOP`, `ADD_FILE`), §5 (`file_entry`), §6.2, §6.3 step 1.

## Change
- `tracker/data.cpp` — redirect decision, owner/backup roles, `REPL` sending and applying.
- `tracker/sync.cpp` — route `REPL ...` messages.
- `client/commands.cpp`, `client/download.cpp` — make sure every `DP` request uses: owner first, backup on connect failure; cache `owner_addr` / `backup_addr` in the `download_job` for `have` updates.

## Behaviour
- `owner_of`: stored owner if the name is known, else `hash % N`. `backup = (owner + 1) % N`.
- Tracker receiving `upload_file/download_file/stop_share`: if `owner == my_id` → `OK`; else `REDIRECT <owner> <backup>` (addresses from config, `client_port`).
- Tracker receiving a `DP` request accepts it if it is the owner **or** the backup of that file (backup serves when owner is down). Otherwise `ERR not owner`.
- Owner after a change: `REPL META` (new file), `REPL HAVE` (new seeder chunks from upload/have), `REPL STOP` (stop_share) to the backup, sent after releasing locks. Then for a new file `BCAST ADD_FILE`.
- Backup handling a `DP` itself (owner down): apply the change locally; do not try to replicate further.
- Backup applying `REPL`: create or update `meta` for that entry; ignore if the group does not exist.

## Do not
- No heartbeat-based detection — owner-down is detected by the client's failed connect.

## Done when
3 trackers, clients on different trackers:
1. Upload 5 files with different names; tracker logs show they land on different owners (`hash % 3`) and each backup holds a copy (add a debug command `debug_files <gid>` printing which files have `meta` here).
2. A client connected to tracker 1 downloads a file owned by tracker 2: it receives `REDIRECT`, opens a temp connection to tracker 2, and completes.
3. Kill the owner tracker of a file; a new `download_file` on another client completes via the backup.
4. While the owner is down, a client downloads from the backup and its `have` updates go to the backup; `download_file` from a third client shows that client as a seeder.
5. Kill owner **and** backup of a file → `ERR file unavailable` printed by the client (both connects failed).
