# Phase 5 — Rarest-first, resume, becoming a seeder

## Goal
Downloads fetch rare chunks first, report progress to the tracker so partial downloaders serve chunks, and resume after a crash by fetching only missing chunks.

## Read first
`docs/DESIGN.md` §4.2 (`have`), §5 (files on disk), §6.3 steps 3, 5, 6, §6.6.

## Change
- `client/download.cpp`
- `client/shares.cpp`
- `client/main.cpp` (re-announce after login)

## Behaviour
- **Rarest-first:** before starting workers, collect missing chunk indexes and `stable_sort` them by `count[i]` ascending; push in that order.
- **HAVE updates:** worker pushes `i` into `have_new` (under `have_mtx`) after setting the bit. The transfer thread, while workers run, wakes every 2 s (sleep loop checking `done` / a `finished` flag): swaps out `have_new`, sends `DP uid have gid name my_addr i,j,k` to the owner (temp connection, backup fallback), and rewrites `dest.progress`. Do it once more after join.
- **Progress file:** `dest.progress` = `nchunks` characters of `0`/`1`. On start, if it exists and length matches, load it into `have` and mark those chunks done; queue only the missing ones. Delete it when the download completes (`C`).
- **Re-announce:** after a successful `login`, read `client_<port>.shares`; for each line whose file still exists, register it in `shares` (have all true) and send `have` for all chunks via the usual two-step owner lookup. Skip missing files silently.
- **Become seeder:** nothing extra — the bitmap is shared with serve threads (phase 4) and `have` puts the peer into seeder sets.

## Do not
- No multi-tracker work.

## Done when
One tracker, clients A, B, C in `g1`:
1. A uploads 50 MB. B starts download; while B is at ~50%, C starts downloading the same file: C's `download_file` metadata shows `127.0.0.1:9002` (B) for chunks B already has, and C's transfer uses B for some chunks (log the peer per chunk at debug level).
2. Kill B at ~50% (Ctrl+C). Restart B with the same port, `login`, `download_file` again: B fetches only the missing chunks (log prints `resume: N missing of 100`) and the final SHA1 matches.
3. Restart A, `login`: re-announce runs and a new downloader can fetch from A without A re-uploading.
4. With A as the only seeder of some chunks and B holding others, order of fetching for a new client starts with the chunks that have count 1.
