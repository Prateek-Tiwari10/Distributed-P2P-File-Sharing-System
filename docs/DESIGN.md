# P2P File Sharing — Design Reference

This is the single source of truth for the implementation. Phase files point back here.

---

## 1. Processes and config

Two binaries:

- `./tracker tracker_info.txt <my_id>` — metadata server. Stores users, groups, file metadata and per-chunk seeders. **Never touches file bytes.**
- `./client <my_ip>:<my_port> tracker_info.txt` — command-line client **and** a small server that sends chunks to other clients.

`tracker_info.txt` (same file everywhere; one line per tracker; `N` = number of lines, fixed):

```
0 127.0.0.1 6000 7000
1 127.0.0.1 6001 7001
2 127.0.0.1 6002 7002
```

Columns: `id ip client_port sync_port`. `client_port` = clients (and redirected requests). `sync_port` = tracker-to-tracker.

Constants (put in `common/config.h`):

| Name | Value |
| --- | --- |
| `CHUNK` | 512 * 1024 bytes |
| `RF` | 2 (owner + backup) |
| `HB_INTERVAL` | 2 s |
| `HB_MISS` | 3 (master dead after 3 missed ACKs; tracker dead after 6 s without HB) |
| `WORKERS` | 4 transfer workers per upload/download |
| `SERVE_THREADS` | 4 serve threads per client |
| `RECV_TIMEOUT` | 5 s on peer sockets |
| `SYNC_TIMEOUT` | 1 s connect/recv timeout for tracker-to-tracker sends |
| `MAX_TRIES` | 3 attempts per chunk |
| `PEERS_PER_CHUNK` | 3 peers returned per chunk |

---

## 2. Two planes

| Plane | What | Where stored | Consistency |
| --- | --- | --- | --- |
| Control | users, groups, members, requests, **file names** | replicated to every tracker via `BCAST` | creates are blocking and conflict-checked; other ops are set operations |
| Data | full file metadata: size, chunk hashes, per-chunk seeders | only on owner tracker + backup | owner = `hash(gid + "|" + name) % N`, backup = `(owner + 1) % N` |
| Session | which socket is logged in as which user | local to the tracker the client is connected to | never replicated |

The owner of a file is computed **once**, at the first upload, and stored with the file name on every tracker (via `ADD_FILE`). Later requests look it up.
Use `std::hash<std::string>` on `gid + "|" + name`, modulo the **fixed** `N` (never the number of live trackers).

---

## 3. Threads

### Tracker — 3 always-on threads + 1 per client connection + short-lived sync workers

| Thread | Count | Job |
| --- | --- | --- |
| Accept (main) | 1 | `accept()` on `client_port`; for each connection create a thread and `detach` it |
| Client thread | 1 per connection | loop `recv_msg → dispatch → send_msg` for that connection; on disconnect run `cleanup()`. Redirected temp connections are just connections too. |
| Sync listener | 1 | `accept()` on `sync_port`; for each incoming tracker message create a short-lived detached thread |
| Sync worker | 1 per tracker message | handle that one message, send reply, close, exit |
| Heartbeat role thread | 1 | **master:** every 2 s mark trackers with `now - last_seen > 6 s` dead. **slave:** every 2 s send `HB` to master, wait for `HB_ACK`, `SYNC` if behind; 3 misses in a row → election. Switches role when election changes the master. |

Tracker-to-tracker sends (`BCAST`, `REPL`, `PEER_UP`, `PEER_DOWN`) are made **directly by the thread handling the request**, after releasing all locks, with `SYNC_TIMEOUT`. No outbox thread.

### Client — 2 always-on threads + serve pool of 4 + per-transfer pool of 4

| Thread | Count | Job |
| --- | --- | --- |
| CLI + tracker connection | 1 | read stdin commands; send on persistent tracker socket; handle `REDIRECT` (temp connection to owner); start a transfer thread for upload/download |
| Listener | 1 | `accept()` on `my_port`, push socket into `serve_q`, `notify_one` |
| Serve threads | 4 fixed | pop socket, read `GET`, check bitmap, `pread`, send `[len][bytes]`, close |
| Transfer thread | 1 per `upload_file` / `download_file` | upload: hash file with 4 workers, send metadata. download: get metadata, rarest-first queue, 4 workers, `HAVE` updates, final SHA1 |
| Transfer workers | 4 per transfer | download: fetch + verify + `pwrite` one chunk at a time. upload: SHA1 chunks in parallel |

---

## 4. Wire protocol

**Framing on every connection:** `[uint32 length, network byte order][body]`. Bodies are text, space separated; multi-line bodies use `\n`.
Replies start with `OK`, `ERR <reason>` or `REDIRECT <owner_ip:port> <backup_ip:port>`.
Chunk replies are the only binary body.

### 4.1 Client → its tracker (persistent connection)

| Command | Login | Reply |
| --- | --- | --- |
| `create_user <uid> <pwd>` | no | `OK` · `ERR user exists` · `ERR creation failed, try again` (blocking) |
| `login <uid> <pwd> <my_ip:my_port>` | no | `OK` · `ERR bad credentials`. Second device allowed. On OK: session stored, peer marked online, `PEER_UP` sent to other trackers. |
| `logout` | yes | `OK` (same as disconnect cleanup) |
| `create_group <gid>` | yes | `OK` · `ERR group exists` · `ERR creation failed, try again` (blocking) |
| `join_group <gid>` | yes | `OK request sent` · `ERR no group` · `ERR already member` |
| `leave_group <gid>` | yes | `OK` · `ERR not member` |
| `list_requests <gid>` | yes, owner | `OK <uid> ...` · `ERR not owner` |
| `accept_request <gid> <uid>` | yes, owner | `OK` · `ERR no such request` · `ERR not owner` |
| `list_groups` | yes | `OK <gid> ...` |
| `list_files <gid>` | yes, member | `OK <name> ...` (answered locally from `group_rec.files` keys) |
| `upload_file <gid> <name>` / `download_file <gid> <name>` / `stop_share <gid> <name>` | yes, member | `REDIRECT <owner> <backup>` if owner ≠ me, else `OK` (client then sends the `DP` form on the same connection) |

Owner of a group = `members.front()`; if the owner leaves, the next member becomes owner; if empty, the group is deleted.

Client-only commands (no network): `show_downloads` → lines `[D|C|F] <gid> <name> <done>/<total>`; `quit`.

### 4.2 Client → owner tracker, `DP` form

Sent on a temporary connection after `REDIRECT` (or on the persistent connection if the client's tracker is the owner). `<peer>` = sender's `ip:port`.

| Command | Reply |
| --- | --- |
| `DP <uid> upload_file <gid> <name> <size> <nchunks> <file_hash> <h0,h1,...> <peer>` | `OK` · `ERR not member` · `ERR exists with different hash` |
| `DP <uid> download_file <gid> <name>` | `OK <size> <nchunks> <file_hash> <h0,h1,...>` then one line per chunk: `<i> <count> <peer>,<peer>,<peer>` · `ERR no file` · `ERR not member` |
| `DP <uid> have <gid> <name> <peer> <i,j,k>` | `OK` · `ERR not member` (progress updates and re-announce) |
| `DP <uid> stop_share <gid> <name> <peer>` | `OK` |

Owner checks membership from its replicated group data. In the download reply, per chunk: `count` = number of online seeders; peers = at most `PEERS_PER_CHUNK`, chosen at random among seeders that are **online** and whose user is **still a member** (seeder filter). A chunk with no usable peer has `count 0` and `-` as the peer list.

### 4.3 Tracker ↔ tracker (`sync_port`, one message per connection)

| Message | Sent when | Receiver does | Reply |
| --- | --- | --- | --- |
| `BCAST <origin> <seq> <OP...>` | after any control-plane change | skip if `seq <= version[origin]`; else apply OP, append op log, `version[origin] = seq`. Creates: accept if name free or incoming id smaller (replace), else refuse | `ACK` · `CONFLICT` |
| `REPL META <gid> <name> <size> <nchunks> <file_hash> <hashes> <peer>` | owner after first upload | backup stores full metadata | `ACK` |
| `REPL HAVE <gid> <name> <peer> <i,j,k>` | owner after `have` / upload by another seeder | backup adds peer to chunks | `ACK` |
| `REPL STOP <gid> <name> <peer>` | owner after `stop_share` | backup removes peer from all chunks | `ACK` |
| `PEER_UP <peer> <uid>` | client logged in | `online.insert(peer)`, `peer_uid[peer] = uid` | `ACK` |
| `PEER_DOWN <peer>` | client logged out / disconnected | `online.erase(peer)` (seeder entries stay) | `ACK` |
| `HB <id> <hb_seq> <v0,v1,...>` | slave → master every 2 s | `last_seen[id] = now` | `HB_ACK <hb_seq> <v0,v1,...>` |
| `SYNC <origin> <from_seq>` | slave behind for origin | return ops from op log with that origin and `seq >= from_seq` | `OPS\n<op line>\n...` |
| `PULL_DATA <owner_id>` | restarted tracker | send every file record (meta + seeders) whose owner is `owner_id` | `DATA\n<META line>\n<HAVE line>...` |
| `PING` | election / startup | — | `PONG` |

Control-plane ops (inside `BCAST`, and one per line in the op log as `origin seq OP...`):

```
CREATE_USER <uid> <pwd> <id>
CREATE_GROUP <gid> <id> <creator_uid>
JOIN_REQ <gid> <uid>
ACCEPT <gid> <uid>
LEAVE <gid> <uid>
ADD_FILE <gid> <name> <owner_id>
```

`id = (uint64(origin) << 48) | seq` — unique without coordination, comparable for "smaller id wins".

Applying `LEAVE` on any tracker: remove uid from `members` (or `pending`), and for every file of that group that has `meta != nullptr` here, remove from every chunk's seeder set each peer `p` with `peer_uid[p] == uid`.

### 4.4 Client ↔ client (`my_port`)

| Request | Reply |
| --- | --- |
| `GET <gid> <name> <i>` (framed) | `[uint32 len][len bytes]`; `len = 0` means "I don't have it" |

---

## 5. Data structures

### Tracker (`tracker/state.h`)

```cpp
struct user_rec { std::string pwd; uint64_t id; };
std::unordered_map<std::string, user_rec> users;                 std::mutex users_mtx;

std::unordered_map<int, std::pair<std::string, std::string>> sessions;  // sock -> (uid, peer)
std::mutex session_mtx;

std::unordered_set<std::string> online;                          // peers logged in now
std::unordered_map<std::string, std::string> peer_uid;           // peer -> uid (kept after logout)
std::mutex peers_mtx;

struct file_meta {
    uint64_t size;
    int nchunks;
    std::string file_hash;
    std::vector<std::string> chunk_hash;
    std::vector<std::unordered_set<std::string>> seeders;        // per chunk, peer = "ip:port"
};

struct file_entry {
    int owner;                                                   // owner tracker id, fixed at first upload
    std::shared_ptr<file_meta> meta;                             // only on owner/backup, else nullptr
};

struct group_rec {
    std::mutex mtx;
    uint64_t id;
    std::list<std::string> members;                              // owner = members.front()
    std::unordered_map<std::string, std::list<std::string>::iterator> pos;
    std::unordered_set<std::string> pending;
    std::unordered_map<std::string, file_entry> files;           // name -> entry, on every tracker
};
std::unordered_map<std::string, std::shared_ptr<group_rec>> groups;  std::mutex groups_mtx;

std::vector<uint64_t> version;   uint64_t my_seq = 0;            std::mutex log_mtx;
int my_id, N, master_id;
std::vector<long long> last_seen;  std::vector<bool> alive;      std::mutex cluster_mtx;
```

Rules:
- `groups_mtx` is held only to find / insert / erase a `shared_ptr<group_rec>`; then release it and lock the group's own `mtx`.
- `group_rec.files` is the only list of files. Keys exist on every tracker; `meta` only on owner and backup.
- Group owner is never stored.

### Client (`client/state.h`)

```cpp
struct shared_file {
    std::string gid, name, path;
    uint64_t size; int nchunks;
    std::vector<bool> have;                                      // all true for uploads
};
std::unordered_map<std::string, std::shared_ptr<shared_file>> shares;  std::mutex shares_mtx;  // key gid + "|" + name

struct download_job {
    std::string gid, name, dest;
    uint64_t size; int nchunks;
    std::string file_hash; std::vector<std::string> chunk_hash;
    std::vector<int> count;                                      // online seeders per chunk
    std::vector<std::vector<std::string>> peers;                 // up to 3 per chunk
    std::queue<int> todo;  std::mutex q_mtx;
    std::vector<int> tries;
    std::vector<int> have_new;  std::mutex have_mtx;
    int fd = -1; std::string owner_addr, backup_addr;
    std::atomic<int> done{0};
    std::atomic<char> status{'D'};                               // D / C / F
};
std::unordered_map<std::string, std::shared_ptr<download_job>> downloads;  std::mutex dl_mtx;

std::queue<int> serve_q;  std::mutex serve_mtx;  std::condition_variable serve_cv;
std::string my_uid, my_addr;  int tracker_sock = -1;  int tracker_idx = 0;
```

A downloading file is put in `shares` at the start with `have` all false; workers set `have[i]` under `shares_mtx` **after** SHA1 matches and `pwrite` completes; serve threads read the same bitmap.

### Files on disk

| File | Writer | Content |
| --- | --- | --- |
| `tracker_<id>.log` | tracker | one op per line: `origin seq OP...`; flushed before replying OK |
| `<dest>.progress` | client | bitmap as `0/1` characters, rewritten every few chunks and at the end |
| `client_<port>.shares` | client | one line per shared file: `gid name path` |

---

## 6. Flows

### 6.1 Blocking create (create_user / create_group)

1. Client thread checks name locally. Taken → `ERR ... exists`.
2. Insert locally with `id = (my_id << 48) | ++my_seq`, append op log, bump `version[my_id]`.
3. Release locks. Send `BCAST my_id seq CREATE_... id` to every other tracker (skip on `SYNC_TIMEOUT`).
4. All replies `ACK` (or no reply) → client gets `OK`.
5. Any `CONFLICT` → remove the record **only if it still has my id**, reply `ERR creation failed, try again`.

Receiver of a create `BCAST`: name free → insert, `ACK`. Has same name with bigger id → replace with incoming, `ACK`. Has smaller id → `CONFLICT`.

`JOIN_REQ`, `ACCEPT`, `LEAVE`, `ADD_FILE` are sent the same way; replies are ignored.

### 6.2 Upload

1. Client (transfer thread): stat file, `nchunks = ceil(size / CHUNK)`, 4 workers compute chunk SHA1s, then whole-file SHA1.
2. `upload_file gid name` to its tracker → membership check → if file known use stored owner, else `hash % N` → `REDIRECT` or `OK`.
3. Client sends `DP uid upload_file ...` to owner (fallback backup).
4. Owner: membership check; new file → create `file_entry{owner, meta}`, uploader in every chunk set, `REPL META` to backup, `BCAST ADD_FILE` to all; same hash → add uploader as seeder (`REPL HAVE`); different hash → `ERR`.
5. Client adds to `shares` (have all true) and appends to `client_<port>.shares`.

### 6.3 Download

1. Transfer thread: `download_file` → `REDIRECT` → `DP uid download_file` → metadata + peers.
2. Open `dest` (`O_RDWR|O_CREAT`), `ftruncate(size)`, load `dest.progress` if present, register in `shares` with that bitmap.
3. Queue missing chunks sorted by `count` ascending (rarest first).
4. 4 workers: pop `i`; pick a random peer from `peers[i]` (not myself); connect, `GET`, read len, `recv_all`; check SHA1; `pwrite` at `i * CHUNK`; set `have[i]`; push `i` to `have_new`. On any failure: `tries[i]++`, push back if `< MAX_TRIES`.
5. Transfer thread every 2 s and at the end: send `DP uid have gid name peer i,j,k` with `have_new`, clear it; rewrite `.progress`.
6. All workers joined: all chunks → whole-file SHA1 check → status `C`, append to `.shares`. Otherwise status `F`, keep `.progress` for resume.

### 6.4 Logout / disconnect (one `cleanup` function)

Erase `sessions[sock]`; `online.erase(peer)` locally; send `PEER_DOWN peer` to all trackers; close socket. Seeder entries are **not** removed.

### 6.5 Leave group

Apply `LEAVE` locally (members + seeder removal as in 4.3), `BCAST` it. Client removes that group's entries from `shares` and from `client_<port>.shares`.

### 6.6 Login and re-announce

After `OK` to login, the client reads `client_<port>.shares`; for each file still on disk it sends `have` with all chunks it holds (via redirect).

### 6.7 Heartbeat, catch-up, election

- Slave every 2 s: `HB my_id hb_seq versions` → `HB_ACK hb_seq master_versions`. For each origin where master is ahead: `SYNC origin version[origin]+1`, apply returned ops in order (same apply function as `BCAST`).
- 3 missed ACKs → election: `PING` trackers in id order 0..N-1; the first that answers (or myself, if I come first) is master.
- Master: `last_seen[id] = now` on HB; every 2 s, trackers silent for > 6 s → `alive[id] = false`.

### 6.8 Tracker startup

1. Parse config, set `N`, `my_id`. 2. Replay `tracker_<id>.log`. 3. Start sync listener + heartbeat thread.
4. Find master by `PING` order. 5. If not master: one HB round + `SYNC` until caught up.
6. `PULL_DATA my_id` from `(my_id+1)%N`, and `PULL_DATA (my_id-1+N)%N` from that tracker. 7. Start accept on `client_port`.

> Changed: recovery pulls cluster-wide state from the master only; neighbour metadata pull removed; data-plane metadata recovery deferred.

### 6.9 Client tracker failover

If the persistent tracker socket fails, the client moves to the next tracker in `tracker_info.txt`, prints `tracker changed, please login again`, and continues.

---

## 7. Failure handling (summary)

| Failure | Handling |
| --- | --- |
| Client crashes | `recv_msg` fails → `cleanup()` |
| Peer dies mid-chunk / hangs | `recv_all` false / `RECV_TIMEOUT` → retry chunk with another random peer |
| Bad chunk | SHA1 mismatch → discard, retry |
| Owner tracker down | client uses backup from `REDIRECT` |
| Owner + backup down | `ERR file unavailable` (accepted limit) |
| Master down | 3 missed ACKs → election |
| Lost `BCAST` | version gap in `HB_ACK` → `SYNC` |
| Duplicate op | skipped by `seq <= version[origin]`; ops are idempotent anyway |

## 8. Known limitations (do not fix)

Owner trusts `uid` on redirected requests · seeder lists keep offline peers (filtered on send) · op log grows forever · thread per tracker connection · creates skip unreachable trackers (no quorum) · plain-text passwords, no TLS, no NAT traversal.
