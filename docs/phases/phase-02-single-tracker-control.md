# Phase 2 — Single tracker: users, sessions, groups

## Goal
One tracker (run with any config; ignore other trackers for now) that handles all control-plane commands from a real client CLI.

## Read first
`docs/DESIGN.md` §3 (tracker accept + client thread; client CLI thread), §4.1, §5 (users, sessions, online, peer_uid, group_rec without files), §6.4.

## Create / change
- `tracker/state.h`, `tracker/state.cpp` — structs and globals from DESIGN.md §5 (all of them, even unused ones).
- `tracker/main.cpp` — parse args, read config, find own row, accept loop on `client_port`, one detached thread per connection.
- `tracker/client_handler.cpp` (+ `.h`) — `handle_client(int sock)`: `recv_msg` loop, split, `dispatch`, `send_msg`; `cleanup(sock)` on exit.
- `tracker/control.cpp` (+ `.h`) — one function per command.
- `client/main.cpp` — connect to tracker 0 from config; CLI loop reading lines from stdin; send command, print reply. `login` must append the client's own `ip:port` automatically (user types `login <uid> <pwd>`).
- `client/state.h` — client globals from DESIGN.md §5 (all of them).

## Commands to implement (tracker)
`create_user`, `login`, `logout`, `create_group`, `join_group`, `leave_group`, `list_requests`, `accept_request`, `list_groups`, plus `list_files` returning `OK` with the keys of `group_rec.files` (empty for now).

Rules:
- Session required for everything except `create_user` and `login` → `ERR not logged in`.
- `create_group`: creator becomes first member. Assign `id = (my_id << 48) | ++my_seq` for users and groups already now (no broadcast yet).
- Members: `list` + `pos` map; join adds to `pending`; accept moves from `pending` to `members`; leave removes from members (or pending); owner = `members.front()`; delete group when empty.
- `list_requests` / `accept_request` only for the owner → `ERR not owner`.
- `login` from a second device is allowed: store `sessions[sock] = {uid, peer}`, `online.insert(peer)`, `peer_uid[peer] = uid`.
- `cleanup(sock)`: erase session, `online.erase(peer)`, close socket (no `PEER_DOWN` yet).
- Respect the lock order; no network I/O under locks.

## Do not
- No sync port, no broadcast, no op log, no files/upload yet.

## Done when
Run `./tracker tracker_info.txt 0` and two clients `./client 127.0.0.1:9001 tracker_info.txt`, `./client 127.0.0.1:9002 tracker_info.txt`:
1. A: `create_user a p` → OK; again → `ERR user exists`; `login a x` → `ERR bad credentials`; `login a p` → OK.
2. A: `create_group g1` → OK; B: `create_user b p`, `login b p`, `join_group g1` → `OK request sent`.
3. B: `list_requests g1` → `ERR not owner`; A: `list_requests g1` → `OK b`; `accept_request g1 b` → OK; `list_groups` → `OK g1`.
4. A: `leave_group g1` → OK; B: `list_requests g1` now works (B is owner).
5. Kill client B with Ctrl+C; tracker prints a cleanup line and keeps running.
6. Before login, `create_group x` → `ERR not logged in`.
