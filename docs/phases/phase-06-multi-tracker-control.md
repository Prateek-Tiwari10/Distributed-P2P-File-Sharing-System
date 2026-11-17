# Phase 6 — Multiple trackers: control-plane replication

## Goal
Users, groups, membership and file names are replicated to every tracker. `create_user` / `create_group` are blocking and conflict-checked ("smaller id wins"). Every applied op is in a persistent op log.

## Read first
`docs/DESIGN.md` §2, §3 (sync listener, sync worker), §4.3 (`BCAST` and the op list), §5 (`version`, `my_seq`, `log_mtx`), §6.1.

## Create / change
- `tracker/sync.cpp` (+ `.h`) — sync listener on `sync_port`, short-lived worker per message, `send_to(id, msg)` and `send_to_all(msg)` helpers (use `request()` with `SYNC_TIMEOUT`, skip self).
- `tracker/oplog.cpp` (+ `.h`) — `append(origin, seq, op)` (write line + `fflush` + `fsync`), `replay()` at startup, `read_from(origin, from_seq)` (used in phase 8).
- `tracker/control.cpp` — route every control change through one function `apply_op(origin, seq, op_tokens, bool remote)` so local commands, `BCAST` and replay share the same code.
- `tracker/main.cpp` — start the sync listener thread; replay the log before accepting.

## Behaviour
- Local change: under the right locks, check + apply, `seq = ++my_seq`, append to log, `version[my_id] = seq`; release locks; then `BCAST my_id seq OP...` to all other trackers.
- **Creates (blocking):** follow DESIGN.md §6.1 exactly. The client thread waits for all replies (or timeouts) before answering the client. On any `CONFLICT`, remove the record only if its stored id is still mine, and reply `ERR creation failed, try again`.
- **Receiving `BCAST`:** skip if `seq <= version[origin]`. Creates: free → insert, ACK; existing with bigger id → replace (for a group: replace the whole record with a fresh one whose only member is the incoming creator), ACK; existing with smaller or equal id → `CONFLICT` (do not log, do not bump version). Other ops: apply, log, bump version, ACK.
- `join_group`, `accept_request`, `leave_group`: broadcast after local apply; replies ignored.
- `ADD_FILE <gid> <name> <owner_id>`: inserts `files[name] = {owner_id, nullptr}` if absent. In this phase emit it from the single-owner upload path (phase 3 code) after a brand-new file is stored.
- `apply_op` must be idempotent: re-applying an op leaves the same state.
- Replay at startup: read each line, call `apply_op(..., remote=true)` without broadcasting; set `version[origin] = max(...)` and `my_seq = version[my_id]`.

## Do not
- No heartbeat, no `SYNC`, no election, no partitioning/redirect between trackers (owner still computed but all trackers are expected up). No `PEER_UP/DOWN` yet.

## Done when
Start 3 trackers from `tracker_info.txt`. Client A on tracker 0, client B on tracker 1 (add a client flag `--tracker <idx>` to pick the starting tracker):
1. A: `create_user a p` then B: `login a p` (on tracker 1) → OK.
2. A: `create_group g1`; B (user b on tracker 1): `join_group g1`; A: `list_requests g1` → `OK b`; `accept_request g1 b`; B: `list_groups` → `OK g1`.
3. Conflict test: add a hidden debug command `debug_delay <ms>` on the tracker that sleeps after local insert before broadcasting. Run `create_group g2` on tracker 0 and tracker 1 at the same time → exactly one client gets OK, the other gets `ERR creation failed, try again`; all three trackers list the same single `g2` with the winner as owner.
4. Stop tracker 2 (Ctrl+C), `create_user c p` on tracker 0 → OK within ~1 s (tracker 2 skipped).
5. Restart tracker 0: users and groups created before the restart are still there (from `tracker_0.log`).
6. Upload on tracker 0 → `list_files g1` on tracker 1 shows the file name.
