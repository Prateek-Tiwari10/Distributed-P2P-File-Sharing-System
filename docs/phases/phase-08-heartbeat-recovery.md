# Phase 8 — Heartbeat, catch-up, master election, tracker recovery

## Goal
Trackers detect failures, elect a new master deterministically, repair missed broadcasts, and a restarted tracker rebuilds its state before taking clients.

## Read first
`docs/DESIGN.md` §3 (heartbeat role thread), §4.3 (`HB`, `HB_ACK`, `SYNC`, `PULL_DATA`, `PING`), §6.7, §6.8.

## Create / change
- `tracker/heartbeat.cpp` (+ `.h`) — the role thread, master checker logic, election.
- `tracker/sync.cpp` — handlers for `HB`, `SYNC`, `PULL_DATA`, `PING`.
- `tracker/oplog.cpp` — `read_from(origin, from_seq)`.
- `tracker/main.cpp` — startup order exactly as DESIGN.md §6.8.

## Behaviour
- Role thread loop (every `HB_INTERVAL`):
  - if `master_id == my_id`: for each other id, `alive[id] = (now - last_seen[id] <= 3 * HB_INTERVAL)`.
  - else: `request(master, "HB my_id hb_seq v0,...", SYNC_TIMEOUT)`; valid only if reply is `HB_ACK` with the same `hb_seq`. Reset `missed` on success and call `catch_up(master_versions)`; else `missed++`, and at `HB_MISS` run `elect()`.
- `catch_up`: for each origin where master's version > mine, `SYNC origin version[origin]+1` to the master, apply each returned op with `apply_op(remote=true)` (which logs it and bumps version).
- `elect()`: for id = 0..N-1: if id == my_id → master = me, stop; else if `request(id, "PING")` returns `PONG` → master = id, stop. Print `[tracker X] master is now Y`.
- `HB` handler (master): `last_seen[id] = now_ms()`, reply `HB_ACK hb_seq <my versions>`. If a slave is ahead for some origin, the master pulls with `SYNC` from that slave (same `catch_up`, after replying).
- `SYNC` handler: `OPS` + matching log lines in seq order.
- `PULL_DATA <owner_id>` handler: for every group/file entry with `owner == owner_id` and `meta != nullptr`, send one `META` line and one `HAVE <peer> <chunks>` line per seeder.
- Startup: replay log → start sync listener + role thread → elect → if slave, one HB + catch_up → `PULL_DATA my_id` from `(my_id+1)%N` and `PULL_DATA (my_id-1+N)%N` from that tracker → start client accept.

## Do not
- No quorum, no voting rounds, no leases.

## Done when
3 trackers:
1. Tracker 0 is master at start (all log `master is now 0`).
2. Kill tracker 0: within ~6 s trackers 1 and 2 both log `master is now 1`; heartbeats continue to tracker 1.
3. While tracker 2 is stopped, create users/groups on tracker 1; start tracker 2: it logs `caught up` and has those users (log in on tracker 2 works).
4. Simulate a lost broadcast with a debug command `debug_drop_next_bcast` on tracker 1, then `create_user z p`: tracker 2 gets `z` within ~2 s via HB + SYNC.
5. Kill a tracker that owns some files, restart it: after startup `debug_files` shows the metadata it owns again (pulled from its backup), and downloads with it as owner work.
