# Phase 3 — File metadata on a single tracker

## Goal
`upload_file`, `download_file` (metadata only), `stop_share`, `have`, `list_files` working against one tracker. No bytes move between clients yet.

## Read first
`docs/DESIGN.md` §2, §4.1 (last row), §4.2, §5 (`file_meta`, `file_entry`, client `shared_file`), §6.2 steps 1–5.

## Create / change
- `tracker/data.cpp` (+ `.h`) — owner lookup and the four `DP` handlers.
- `tracker/client_handler.cpp` — route `upload_file/download_file/stop_share` and `DP ...` lines.
- `client/commands.cpp` (+ `.h`) — upload preparation and the two-step "ask tracker → send DP" logic.
- `client/shares.cpp` (+ `.h`) — `shares` map + `client_<port>.shares` file append/remove.
- `client/main.cpp` — new commands.

## Tracker behaviour
- `owner_of(gid, name)`: if `files` has the name → stored `owner`; else `std::hash<std::string>{}(gid + "|" + name) % N`.
- `upload_file/download_file/stop_share <gid> <name>`: check session + membership; if owner ≠ `my_id` → `REDIRECT <owner ip:client_port> <backup ip:client_port>`; else `OK`. (With one tracker running, start it with a 1-line config so owner is always 0.)
- `DP <uid> upload_file ...`: membership check by `uid`; new name → `files[name] = {owner, meta}` with uploader in every chunk set; existing with same `file_hash` → add uploader to every chunk; different hash → `ERR exists with different hash`.
- `DP <uid> download_file <gid> <name>`: reply format exactly as DESIGN.md §4.2. For now return **all** seeders (no online filter, no 3-peer cap); `count` = set size.
- `DP <uid> have <gid> <name> <peer> <i,j,k>`: add peer to those chunks.
- `DP <uid> stop_share <gid> <name> <peer>`: remove peer from every chunk.
- `list_files <gid>`: member only; keys of `files`.

## Client behaviour
- `upload_file <path> <gid>` (user syntax): stat file (error if missing/empty), compute chunk SHA1s **in the CLI thread for now** (workers come in phase 4), whole-file SHA1, then `upload_file <gid> <basename>` → on `OK` send `DP ...` on the same socket; on `REDIRECT` open a temp connection to owner (fallback to backup), send `DP ...`, close. On `OK` add to `shares` with `have` all true and append to `client_<port>.shares`.
- `download_file <gid> <name> <dest_dir>`: same two-step; print the parsed metadata (size, nchunks, peers per chunk). No transfer yet.
- `stop_share <gid> <name>`: two-step, remove from `shares` and the `.shares` file.

## Do not
- No peer-to-peer transfer, no workers, no replication.

## Done when
1-line config, `./tracker one.txt 0`, clients A (9001) and B (9002) logged in, both in `g1`:
1. A: `upload_file ./test/big.bin g1` (create a 3 MB random file first) → OK; `list_files g1` → `OK big.bin`.
2. B: `download_file g1 big.bin ./dl` → prints size, 6 chunks, each chunk listing `127.0.0.1:9001`.
3. A: upload a different file with the same name → `ERR exists with different hash`.
4. A non-member trying `download_file g1 big.bin` → `ERR not member`.
5. A: `stop_share g1 big.bin`; B's next `download_file` shows `count 0` for every chunk.
