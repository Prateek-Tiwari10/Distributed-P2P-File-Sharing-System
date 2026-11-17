# Phase 9 — Online peers, seeder filter, leave security, client failover

## Goal
Only online members are handed out as seeders (max 3 per chunk), leaving a group removes a user as seeder everywhere, logout only marks a peer offline, and a client survives the loss of its tracker.

## Read first
`docs/DESIGN.md` §4.1 (`login`, `leave_group`), §4.2 (download reply rules), §4.3 (`PEER_UP`, `PEER_DOWN`, `LEAVE`), §6.4, §6.5, §6.9.

## Change
- `tracker/control.cpp` — `login` sends `PEER_UP`; `cleanup` sends `PEER_DOWN`; `LEAVE` apply removes the user's peers from seeder sets.
- `tracker/sync.cpp` — `PEER_UP` / `PEER_DOWN` handlers.
- `tracker/data.cpp` — seeder filter in the download reply.
- `client/main.cpp`, `client/shares.cpp` — leave cleanup, tracker failover.

## Behaviour
- `login` OK → `online.insert(peer)`, `peer_uid[peer] = uid` (under `peers_mtx`), then send `PEER_UP peer uid` to all trackers (after unlock).
- `cleanup` (logout or disconnect) → erase session, `online.erase(peer)`, send `PEER_DOWN peer` to all. Seeder sets are **not** touched.
- Download reply, per chunk: walk the seeder set; keep peers that are in `online` and whose `peer_uid` is a current member of the group; `count` = number kept; pick up to `PEERS_PER_CHUNK` at random from them; none → `0 -`. Lock order: group `mtx` then `peers_mtx`.
- `LEAVE` apply (local and remote): remove from members/pending, then for each file in that group with `meta != nullptr`, erase every seeder `p` where `peer_uid[p] == uid`.
- Client `leave_group gid` OK → remove all `shares` entries of that group and their lines from `client_<port>.shares`.
- Client tracker failover: if `send_msg`/`recv_msg` on the persistent tracker socket fails, close it, connect to the next tracker in config (round robin), print `tracker changed, please login again`, clear `my_uid`, and retry the command once if it was `create_user` or `login`.
- `upload_file`/`have` from a non-member are already rejected by the owner (phase 3) — keep it.

## Done when
3 trackers, clients A, B, C, D in `g1` (on different trackers):
1. A and B seed a file. B logs out. C's `download_file` lists only A for every chunk; B logs in again → C sees B again (no re-upload needed).
2. Five seeders online → each chunk line lists at most 3 peers, `count` shows 5.
3. B runs `leave_group g1`: on the owner tracker `debug_files` no longer lists B in any chunk; B's serve threads answer `len 0` for that file.
4. A non-member E tries `download_file g1 file` and `upload_file` into `g1` → `ERR not member`.
5. Kill the tracker that client D is connected to: D prints `tracker changed, please login again`, logs in, and downloads normally.
