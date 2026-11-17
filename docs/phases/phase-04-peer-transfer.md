# Phase 4 — Peer-to-peer chunk transfer

## Goal
Client serves chunks to other clients through a 4-thread serve pool, and downloads files with a transfer thread + 4 workers, verifying every chunk with SHA1.

## Read first
`docs/DESIGN.md` §3 (client table), §4.4, §5 (client state, one bitmap rule), §6.3 steps 1–4 and 6.

## Create / change
- `client/server.cpp` (+ `.h`) — listener thread + 4 serve threads using `serve_q`, `serve_mtx`, `serve_cv`.
- `client/download.cpp` (+ `.h`) — transfer thread and worker function.
- `client/commands.cpp` — `upload_file` hashing moves to a transfer thread with 4 workers; `download_file` starts a download transfer thread and returns to the prompt immediately.
- `client/main.cpp` — start listener + serve threads before connecting to the tracker; `show_downloads`.

## Serve side
- Listener: `accept` on `my_port`, push socket to `serve_q`, `notify_one`.
- Serve thread loop: `wait` until queue non-empty, pop, `set_recv_timeout(RECV_TIMEOUT)`, `recv_msg` → `GET gid name i`; look up `shares[gid|name]` and `have[i]` under `shares_mtx`; if missing → send `len = 0`; else `pread` from `path` at `i * CHUNK` (last chunk may be shorter), send `uint32 len` + bytes with `send_all`; close.
- Open the file inside the serve thread (`open` / `pread` / `close`), never share a file descriptor across threads.

## Download side
- Transfer thread: parse metadata reply into a `download_job`; register in `downloads`; `open(dest, O_RDWR|O_CREAT, 0644)` + `ftruncate(size)`; add to `shares` with `have` all false; push all chunk indexes into `todo` (order does not matter yet); start `WORKERS` threads; `join` them.
- Worker: pop index under `q_mtx` (exit when empty); pick a random peer from `peers[i]` that is not `my_addr`; `connect_addr(peer, RECV_TIMEOUT)`; `send_msg("GET gid name i")`; `recv_all` 4 bytes; reject `len == 0` or `len > CHUNK`; `recv_all` the body; `sha1_hex` must equal `chunk_hash[i]`; `pwrite` at `i * CHUNK`; then set `have[i] = true` under `shares_mtx`; `done++`. On any failure `tries[i]++` and push back if `< MAX_TRIES`.
- After join: if `done == nchunks` → `sha1_file(dest) == file_hash` → status `C` and append to `client_<port>.shares`; else status `F`. Print a one-line result.
- Upload hashing: 4 workers pull chunk indexes from a queue, each opens the file and `pread`s its chunk, writes the hash into `chunk_hash[i]` (pre-sized vector, distinct indexes → no lock needed).

## Do not
- No rarest-first, no `.progress` file, no `have` updates to the tracker yet (next phase).

## Done when
One tracker, A uploads a 50 MB random file; B downloads it:
1. `show_downloads` on B shows `[D]` while running and `[C] g1 big.bin 100/100` at the end.
2. `sha1sum` of A's file equals B's downloaded file.
3. Corrupt test: temporarily make A's serve thread flip one byte for chunk 3 (behind an env var `P2P_CORRUPT_CHUNK=3`); B retries and ends `[F]` after 3 tries for that chunk, others downloaded. Remove nothing else; keep the env-var hook for demos.
4. Kill A in the middle of a download: B's workers time out and B ends with `[F]` without hanging.
