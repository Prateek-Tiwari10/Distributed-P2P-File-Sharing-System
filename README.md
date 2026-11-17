# Distributed P2P File Sharing System

A BitTorrent-style peer-to-peer file sharing system written in C++17. Files are split into 512 KB chunks, and peers download those chunks directly from each other, in parallel. A cluster of **trackers** coordinates users, groups and who holds which chunk, but never handles file data itself.

The system consists of two programs:

- **`tracker`**: the metadata server. Several trackers run together as a cluster, and a client can connect to any of them.
- **`client`**: a command-line client that also runs a small server, so it can send chunks to other clients.

## Contents

- [Features](#features)
- [Getting started](#getting-started)
- [Architecture](#architecture)
- [Tracker synchronization](#tracker-synchronization)
- [Users and sessions](#users-and-sessions)
- [Groups](#groups)
- [Uploading a file](#uploading-a-file)
- [Downloading a file](#downloading-a-file)
- [Failure handling](#failure-handling)
- [Reference](#reference)
- [Project structure](#project-structure)

## Features

- **Group-based sharing.** Files are shared within groups. Only members of a group can list, upload or download its files.
- **Parallel, verified downloads.** Four worker threads fetch chunks rarest-first, from up to three peers per chunk. Every chunk, and then the whole file, is verified with SHA-1.
- **Seeding while downloading.** A chunk can be served to other peers as soon as it has been verified.
- **Resumable downloads.** A `.progress` file records the completed chunks, so an interrupted download continues where it stopped.
- **Multiple trackers.** User and group data is replicated to every tracker. Each file's metadata is stored on one owner tracker and one backup tracker.
- **Fault tolerance.** The system handles peers that fail or send corrupt data, and the loss of the client's tracker, a file's owner tracker or the master tracker (see [Failure handling](#failure-handling)).

## Getting started

### Requirements

- Linux
- g++ with C++17 support
- OpenSSL (`libcrypto`)

On Ubuntu:

```bash
sudo apt install build-essential libssl-dev
```

### Build

```bash
make          # builds bin/tracker and bin/client
make test     # quick check of message framing, SHA-1 and config parsing
```

### Configure the trackers

`tracker_info.txt` lists every tracker, one per line, in the form `id ip client_port sync_port`. All trackers and clients read the same file. The number of lines, **N**, stays fixed while the system runs.

```text
0 127.0.0.1 6000 7000
1 127.0.0.1 6001 7001
2 127.0.0.1 6002 7002
```

### Run

Start tracker 0 first: on a cold start, the tracker on the first line becomes the master. The other trackers can be started at any time afterwards, and they join it.

```bash
./bin/tracker tracker_info.txt 0
./bin/tracker tracker_info.txt 1
./bin/tracker tracker_info.txt 2
```

Start each client with its own `ip:port`, the address on which it serves chunks to other clients. The optional `--tracker <index>` flag selects the tracker to connect to (default: 0).

```bash
./bin/client 127.0.0.1:9001 tracker_info.txt
./bin/client 127.0.0.1:9002 tracker_info.txt --tracker 1
```

### Example session

`A` is alice's client, connected to tracker 0, and `B` is bob's client, connected to tracker 1. The right-hand column shows what each client prints.

```text
A> create_user alice pw          OK
A> login alice pw                OK
A> create_group g1               OK
B> create_user bob pw            OK
B> login bob pw                  OK
B> join_group g1                 OK request sent
A> list_requests g1              OK bob
A> accept_request g1 bob         OK
A> upload_file a/big.bin g1      OK hashing big.bin (6 chunks)
                                 [upload] g1 big.bin: OK
B> list_files g1                 OK big.bin
B> download_file g1 big.bin dl   OK download started
                                 [download] g1 big.bin: complete (6/6)
B> show_downloads                [C] g1 big.bin 6/6
```

### Files created at runtime

The first two files are written to the working directory; the progress file is written next to the file being downloaded.

| File | Created by | Contents |
| --- | --- | --- |
| `tracker_<id>.log` | tracker | Every control-plane change, one per line; replayed on restart |
| `client_<port>.shares` | client | `gid name path` for each shared file; re-announced after login |
| `<dest_dir>/<name>.progress` | client | One `0` or `1` per chunk of an unfinished download; used to resume it |

To start from a clean state, stop all processes and delete `*.log`, `*.shares` and any `*.progress` files.

## Architecture

Clients get metadata from the trackers and file data from each other. The trackers communicate with each other to stay in sync.

```text
      +----------------------(3)----------------------+
      v                                               v
+-----------+    (3)    +-----------+    (3)    +-----------+
| tracker 0 |<--------->| tracker 1 |<--------->| tracker 2 |
|   master  |           |           |           |           |
+-----------+           +-----------+           +-----------+
   ^     ^                      ^
(1)|  (2)|                   (1)|
   |     +----------------+     |
   |                      |     |
+-----------+           +-----------+
|  client A |<--------->|  client B |
+-----------+    (4)    +-----------+
```

| Link | Connection | Carries |
| --- | --- | --- |
| (1) Client → its own tracker | Kept open for the whole session | Every command the user types |
| (2) Client → a file's owner tracker | Short-lived, one per request | `DP` requests: file metadata, `have`, `stop_share` |
| (3) Tracker ↔ tracker, on `sync_port` | Short-lived, one per message | `BCAST`, `REPL`, `PEER_UP` / `PEER_DOWN`, `HB`, `SYNC`, `WHO_IS_MASTER`, `PING` |
| (4) Client ↔ client | Short-lived, one per chunk | `GET <gid> <name> <i>` → `[len][bytes]` |

Every message is framed as `[4-byte length][body]`. Message bodies are space-separated text; only chunk data is sent as raw bytes.

The master is the oldest live tracker (tracker 0 after a cold start). It only coordinates heartbeats and recovery; clients can connect to any tracker.

### Where data lives

| Data | Stored on | Kept consistent by |
| --- | --- | --- |
| Users, groups, members, join requests and file names | Every tracker | `BCAST` and the op log, repaired by heartbeats (`SYNC`) |
| File metadata: size, chunk SHA-1s and the peers holding each chunk | The file's owner tracker and its backup | The owner sends `REPL` to the backup |
| Sessions (which connection is logged in as which user) | Only the tracker the client is connected to | Not replicated |
| Online peers | Every tracker | `PEER_UP` / `PEER_DOWN` |
| File data | Clients only | — |

This divides the system into two planes:

- The **control plane** (the first row) is replicated to every tracker, so any tracker can answer and the data stays available.
- The **data plane** (the second row) has a single owner per file, so each file's metadata is updated in exactly one place and stays consistent.

A file's owner tracker is `hash(gid + "|" + name) % N`, and its backup is `(owner + 1) % N`. `N` is the number of lines in `tracker_info.txt`, never the number of live trackers, so every tracker computes the same owner. The owner is fixed when the file is first uploaded and is stored with the file name on every tracker.

### Threads

| Process | Thread | Count | Responsibility |
| --- | --- | --- | --- |
| tracker | Client listener | 1 | Accepts client connections and starts one thread per connection |
| tracker | Client thread | 1 per connection | Reads commands and replies until the client disconnects, then cleans up its session |
| tracker | Sync listener | 1 | Accepts tracker messages and handles each one in a short-lived thread |
| tracker | Role thread | 1 | Every 2 s: the master removes silent trackers; a slave sends a heartbeat |
| client | Command loop | 1 | Reads user commands and owns the connection to the tracker |
| client | Listener and serve threads | 1 + 4 | The listener queues incoming connections; four threads send chunks |
| client | Transfer thread | 1 per upload or download | Runs four workers that hash or fetch chunks |

No lock is held during a network call, and locks are always taken in the same order: `groups_mtx` → the group's `mtx` → `peers_mtx` → `log_mtx`.

## Tracker synchronization

Each control-plane change is logged and broadcast by the tracker that makes it. Heartbeats to the master then repair anything a broadcast missed.

### Broadcasting a change

1. The tracker handling the command gives the change the next number from its own counter: `seq = ++my_seq`.
2. It applies the change, appends `origin seq OP...` to `tracker_<id>.log` (flushed to disk before the client receives `OK`) and sets `version[me] = seq`.
3. Without holding any lock, it sends `BCAST <origin> <seq> <OP...>` to every other tracker in turn. A tracker that does not respond within 1 s is skipped.
4. A receiving tracker ignores the op if `seq <= version[origin]`, because it has already applied it. Otherwise, it applies the op, logs it and sets `version[origin] = seq`.

`version` is a version vector: `version[k]` is the last op from tracker `k` that this tracker has applied. The replicated ops are:

```text
CREATE_USER  <uid> <pwd> <id>
CREATE_GROUP <gid> <id> <creator>
JOIN_REQ     <gid> <uid>
ACCEPT       <gid> <uid>
LEAVE        <gid> <uid>
ADD_FILE     <gid> <name> <owner>
```

Creates wait for every tracker's reply and can be rejected (see [Users and sessions](#users-and-sessions)). The other ops only add an entry to a set or remove one, so their order does not matter and their replies are ignored.

### Heartbeats and catch-up

Every 2 s, each slave sends `HB <id> <hb_seq> <versions>` to the master, which replies with `HB_ACK <hb_seq> <versions> <order_version> <order>`.

- If the master is ahead for some tracker `k`, the slave sends `SYNC <k> <from_seq>`, and the master returns the matching lines of its log. The slave applies them with the same code that handles a `BCAST`.
- If the slave is ahead (the master missed a `BCAST`), the master pulls the missing ops from the slave in the same way, right after replying.

As a result, a lost `BCAST` reaches every tracker within a few heartbeats, relayed through the master.

### The master and the tracker order

Every tracker keeps the same seniority list of live trackers, ordered from oldest to newest. The master is always the first tracker in this list.

- The master adds a joining tracker to the end of the list, and removes a tracker it has not heard from for more than 6 s (three heartbeats). Each change increments the list's version, and slaves adopt the newer list from the next `HB_ACK`.
- A restarted tracker rejoins at the end of the list, so a former master never takes over again when it comes back.

What happens when the master itself fails is covered under [Failure handling](#failure-handling).

### Starting a tracker

```text
start: read config, replay tracker_<id>.log, open both ports
       (clients get "tracker in recovery mode" until ACTIVE)
  |
  v
WHO_IS_MASTER to every other tracker
  |
  +-- nobody answers ------> cold start: order = config order,
  |                          the first line is master. ACTIVE
  |
  +-- "MASTER m" ----------> rejoin m:
  |                          1. HB with hb_seq 0, m puts me last
  |                          2. SYNC the ops I missed
  |                          3. PULL_DATA: who is online now
  |                          ACTIVE
  |
  +-- no usable answer ----> retry every 2 s; after 3 rounds with
                             no answer at all, become master alone
```

A tracker that is still recovering answers `WHO_IS_MASTER` and `PING` with `ERR recovering`, so it is never chosen as the master or counted as alive. A tracker that enters recovery later (see [Failure handling](#failure-handling)) runs the same loop, without the cold-start branch.

## Users and sessions

### Creating a user or group

`create_user` blocks until every tracker has replied, so two trackers can never both keep the same user name.

1. The client's tracker checks the name locally. If the name is taken, it replies `ERR user exists`.
2. It generates an id, `(tracker_id << 48) | seq`, which is unique without any coordination and can be compared with other ids.
3. It applies and logs the create, then sends `BCAST ... CREATE_USER <uid> <pwd> <id>` to every other tracker and waits for each reply (1 s timeout per tracker).
4. Each receiving tracker:
    - stores the user and replies `ACK` if the name is free;
    - replaces its own record and replies `ACK` if it holds the same name with a larger id;
    - replies `CONFLICT` if it holds the same name with a smaller id.
5. If any tracker replies `CONFLICT`, the creating tracker removes its own record (if that record still has its id) and replies `ERR creation failed, try again`. Otherwise, it replies `OK`.

The smaller id therefore always wins. If trackers 0 and 1 create `alice` at the same moment, tracker 0's id is smaller: tracker 1 replaces its own `alice` and tells its client to try again, while tracker 0's client receives `OK`. Every tracker ends up with the same `alice`.

`create_group` follows the same steps. The creator becomes the group's first member, and therefore its owner.

### Login

1. The client sends `login <uid> <pwd>` and appends its own `ip:port`, the address on which it serves chunks.
2. The tracker checks the password and stores the session (connection → uid and peer address) **on this tracker only**. A connection can hold one login, but the same user can log in from another client.
3. The peer is marked online, and `PEER_UP <peer> <uid>` is sent to every other tracker. This matters because a file's owner may be a different tracker, and owners only hand out online peers.
4. The client then re-announces its files. For each entry in `client_<port>.shares` whose file still exists on disk, it resumes serving the file and sends `DP <uid> have <gid> <name> <peer> 0,1,...,n-1` to the file's owner.

A restarted client therefore becomes a seeder again as soon as it logs in, without uploading anything again.

### Logout and disconnection

An explicit `logout`, closing the client and a client crash are all handled the same way on the tracker: the session is erased, the peer is marked offline and `PEER_DOWN <peer>` is sent to every other tracker.

The peer's seeder entries are **kept**. Offline peers are filtered out whenever a download asks for peers, and they become available again as soon as the user logs back in.

## Groups

A group is an ordered list of members, and the first member is the owner. A user joins by sending a request, which the owner accepts.

```text
               join_group            accept_request (by owner)
 not in group -----------> pending ----------------------------> member
      ^                       |                                    |
      |      leave_group      |            leave_group             |
      +-----------------------+------------------------------------+
```

| Command | Who can run it | Effect |
| --- | --- | --- |
| `create_group <gid>` | Any logged-in user | Blocking create, as for users; the creator becomes the first member and the owner |
| `join_group <gid>` | A non-member | `JOIN_REQ` op: adds the user to the pending set and replies `OK request sent` |
| `list_requests <gid>` | The owner | Lists the pending requests |
| `accept_request <gid> <uid>` | The owner | `ACCEPT` op: moves the user from the pending set to the end of the member list |
| `leave_group <gid>` | A member, or a user with a pending request | `LEAVE` op, described below |
| `list_groups` | Any logged-in user | Lists all groups |
| `list_files <gid>` | A member | Lists the group's file names, answered by the client's own tracker |

Each change is a replicated op, so every tracker ends up with the same members. Any tracker can answer `list_files`, because `ADD_FILE` stores every file name on every tracker.

When a `LEAVE` op is applied, each tracker:

- removes the user from the member list (or the pending set);
- makes the next member the owner if the user was the owner (the owner is always `members.front()` and is never stored separately);
- removes the user's peers from the seeder sets of the group's files, on the trackers that hold that metadata, so they are never handed out for this group again;
- deletes the group if no members remain.

The client that left stops serving the group's files and removes them from `client_<port>.shares`.

Members are stored in a `std::list` together with a map from each uid to its position in the list. Checking or removing a member takes O(1) time, and the list keeps the join order for choosing the next owner.

## Uploading a file

An upload sends only metadata to the file's owner tracker: the file size and a SHA-1 hash of each chunk. The data stays with the uploader, which becomes the first seeder.

```text
client         its tracker      owner tracker          backup, others
  |                 |                 |                       |
  |-- upload_file ->|                 |                       |
  |  g1 big.bin     |                 |                       |
  |                 | member? owner = |                       |
  |                 | stored or hash  |                       |
  |<- REDIRECT -----|                 |                       |
  |  owner backup   |                 |                       |
  |                 |                 |                       |
  | SHA1 each chunk |                 |                       |
  | (4 workers),    |                 |                       |
  | then the file   |                 |                       |
  |                 |                 |                       |
  |-- DP upload_file ... ------------>|                       |
  |                 |                 |-- REPL META --------->| backup
  |                 |                 |-- BCAST ADD_FILE ---->| all
  |<- OK -----------------------------|                       |
  |                 |                 |                       |
  | serve all chunks, add to client_<port>.shares
```

1. `upload_file <path> <gid>`: the client checks that the file exists and counts its 512 KB chunks. The file's name in the group is the last component of the path.
2. The client sends `upload_file <gid> <name>` to its own tracker, which checks membership and finds the owner: the owner stored with the file if the name already exists, otherwise `hash % N`. The tracker replies `OK` if it is the owner itself, or `REDIRECT <owner> <backup>` otherwise.
3. The client prints `OK hashing <name> (n chunks)` and returns to the prompt. A transfer thread computes each chunk's SHA-1 using four workers, followed by the SHA-1 of the whole file.
4. The client sends `DP <uid> upload_file <gid> <name> <size> <n> <file_sha1> <h0,h1,...> <ip:port>` to the owner on a short-lived connection, or to the backup if the owner cannot be reached.
5. The owner checks membership and the chunk count, then:
    - **New file name:** stores the metadata with the uploader as the seeder of every chunk, sends `REPL META` to the backup and broadcasts `ADD_FILE <gid> <name> <owner>`, so that every tracker can list the file.
    - **Same name, same SHA-1:** adds the uploader as a seeder of every chunk and sends `REPL HAVE` to the backup.
    - **Same name, different SHA-1:** replies `ERR exists with different hash`.
6. On `OK`, the client starts serving the file, adds `gid name path` to `client_<port>.shares` and prints `[upload] <gid> <name>: OK`.

`stop_share <gid> <name>` reverses an upload. It uses the same routing and then sends `DP <uid> stop_share ...`. The owner removes the peer from every chunk (and sends `REPL STOP` to the backup), and the client stops serving the file and removes its entry.

## Downloading a file

The owner tracker reports which peers hold each chunk. The client then fetches the chunks directly from those peers, four at a time and rarest first, and verifies every one.

```text
client B         its tracker      owner tracker               peers
  |                   |                 |                       |
  |-- download_file ->|                 |                       |
  |<- REDIRECT -------|                 |                       |
  |-- DP download_file ---------------->|                       |
  |<- SHA1s + peers per chunk ----------|                       |
  |                   |                 |                       |
  | 4 workers, rarest chunk first:      |                       |
  |-- GET g1 big.bin 7 ---------------------------------------->| A
  |<- [len][bytes] ---------------------------------------------|
  | check SHA1, write chunk 7, serve it too                     |
  |                   |                 |                       |
  | every 2 s:        |                 |                       |
  |-- DP have ... 7,2,9 --------------->|                       |
  |                   |                 | adds B as seeder      |
  |                   |                 |                       |
  | all chunks in: whole-file SHA1 -> complete
```

1. `download_file <gid> <name> <dest_dir>`: the client checks that the destination folder exists and that the file is not already being downloaded. It finds the owner through its tracker, as for an upload.
2. A transfer thread sends `DP <uid> download_file <gid> <name>` to the owner, or to the backup if the owner cannot be reached. The reply contains the file size, the file's SHA-1, each chunk's SHA-1 and one line per chunk: `<i> <count> <peer,peer,peer>`.
    - Only peers that are **currently online** and whose user is **still a group member** are counted.
    - `count` is the number of such peers holding the chunk. Up to three of them, chosen at random, are listed; if there are none, the line reads `0 -`.
3. The client creates `<dest_dir>/<name>` at its full size, so that each worker can write at its own offset. If a `<name>.progress` file from an earlier run is in the folder, the chunks it marks as complete are kept.
4. The client registers the file as shared, with the chunks it already has, and queues the missing chunks **rarest first** (lowest `count` first).
5. Four workers each take a chunk from the queue, pick a random listed peer (never the client itself), connect with a 5 s timeout and send `GET <gid> <name> <i>`. The peer replies with `[len][bytes]`; a `len` of 0 means it does not have the chunk.
6. The worker checks the length and the chunk's SHA-1, writes the chunk with `pwrite` at offset `i × 512 KB` and marks it as complete. From then on, the client serves that chunk to other peers.
7. Any failure (no peer available, a timeout, a wrong length or a SHA-1 mismatch) puts the chunk back in the queue for another attempt with a random listed peer, up to three attempts in total.
8. Every 2 s, the transfer thread reports newly completed chunks to the owner with `DP <uid> have <gid> <name> <ip:port> <i,j,k>`, so that later downloads receive this client as a peer. It also rewrites `<name>.progress`.
9. When the workers finish, the client checks the SHA-1 of the whole file. If it matches, `.progress` is deleted, the file is added to `client_<port>.shares` and its status becomes `C`. Otherwise, the status becomes `F` and `.progress` is kept, so running `download_file` again resumes the download.

`show_downloads` prints one line per download: its status (`[D]` downloading, `[C]` complete or `[F]` failed), followed by `<gid> <name> <done>/<total>` chunks.

On the serving side, a listener thread accepts each incoming `GET` connection and queues it. Four serve threads each take a connection from the queue, check that the requested chunk is complete, read it with `pread` and send it.

## Failure handling

Every failure leads to a retry, a switch to a replica, or a clear error message.

| Failure | Behaviour |
| --- | --- |
| A client crashes or exits | The tracker's `recv` fails, and it runs the same cleanup as for `logout`: the session is erased, the peer is marked offline and `PEER_DOWN` is sent to all trackers |
| A peer fails or stalls mid-chunk | After the 5 s connect/receive timeout, the chunk is retried with a random listed peer, up to three attempts in total |
| A peer sends a corrupt chunk | The SHA-1 check fails, so the chunk is discarded and retried |
| A download is interrupted | `.progress` keeps the verified chunks; the next `download_file` resumes the download and re-announces them |
| The client's tracker fails | The next command fails, so the client connects to the next tracker in `tracker_info.txt`, prints `tracker changed, please login again` and retries `create_user` or `login` once |
| A file's owner tracker is down | The client cannot connect to the owner, so it sends the `DP` request to the backup, which answers from its copy |
| Both the owner and the backup are down | The client receives `ERR file unavailable`, the accepted cost of having one owner per file |
| A `BCAST` is lost | The version vectors in the next heartbeat reveal the gap, and `SYNC` copies the missing ops |
| A slave tracker fails | The master removes it from the tracker order after 6 s without a heartbeat |
| The master fails | The slaves detect it with indirect pings (below), and the next oldest tracker becomes the master |
| A tracker restarts | It replays its log and rejoins at the end of the tracker order (see [Starting a tracker](#starting-a-tracker)). Until it has rejoined, its clients receive `ERROR: tracker in recovery mode, try again after some time` |

### Detecting master failure

A slave does not trust its own missed heartbeats alone: before declaring the master dead, it asks other trackers to check the master.

```text
slave: no HB_ACK from the master 3 times in a row (6 s)
  |
  v
PING_REQ <master> to up to 3 other trackers, oldest first
(each one PINGs the master itself and reports back)
  |
  +-- any says REACHABLE -----> the fault is on my side:
  |                             go to recovery and rejoin
  |
  +-- none reachable, at -----> the master is dead: remove it
  |   least one UNREACHABLE     from the front of the order;
  |                             the next oldest is master
  |
  +-- nobody answers ---------> I am the one cut off:
                                go to recovery
```

Every slave that sees the same failure removes the same tracker from the front of the order, so all copies of the order stay identical. A new master gives every tracker a full 6 s before it removes anyone. With only two trackers, there is no other tracker to ask, so the slave treats the master as dead.

## Reference

### Client commands

| Command | Requires | Output on success |
| --- | --- | --- |
| `create_user <uid> <pwd>` | — | `OK` |
| `login <uid> <pwd>` | — | Re-announce lines, then `OK` |
| `logout` | Login | `OK` |
| `create_group <gid>` | Login | `OK` |
| `join_group <gid>` | Login | `OK request sent` |
| `leave_group <gid>` | Membership or a pending request | `OK` |
| `list_requests <gid>` | Group ownership | `OK <uid> ...` |
| `accept_request <gid> <uid>` | Group ownership | `OK` |
| `list_groups` | Login | `OK <gid> ...` |
| `list_files <gid>` | Membership | `OK <name> ...` |
| `upload_file <path> <gid>` | Membership | `OK hashing <name> (n chunks)`, then `[upload] <gid> <name>: OK` |
| `download_file <gid> <name> <dest_dir>` | Membership | `OK download started`, then `[download] <gid> <name>: complete (n/n)` |
| `show_downloads` | — | One line per download: `[D]`, `[C]` or `[F]`, then `<gid> <name> <done>/<total>` |
| `stop_share <gid> <name>` | Membership | `OK` |
| `quit` | — | Exits the client |

Errors begin with `ERR`, for example `ERR not member`, `ERR not owner`, `ERR user exists` or `ERR file unavailable`.

### Configuration constants

Defined in `common/config.h`:

| Constant | Value | Meaning |
| --- | --- | --- |
| `CHUNK` | 512 KB | Chunk size |
| `WORKERS` | 4 | Hashing or download workers per transfer |
| `SERVE_THREADS` | 4 | Threads that send chunks to other peers |
| `PEERS_PER_CHUNK` | 3 | Peers listed per chunk in a download reply |
| `MAX_TRIES` | 3 | Attempts per chunk before giving up |
| `RECV_TIMEOUT` | 5 s | Timeout on peer connections and owner requests |
| `SYNC_TIMEOUT` | 1 s | Timeout on tracker-to-tracker messages |
| `HB_INTERVAL` | 2 s | Heartbeat interval |
| `HB_MISS` | 3 | Missed heartbeats before a tracker is suspected or removed |
| `INDIRECT_K` | 3 | Trackers asked to check an unresponsive master |
| `PING_TIMEOUT` | 1 s | Timeout for a helper's own `PING` to the master |
| `RF` | 2 | Copies of each file's metadata: the owner and the backup (always `owner + 1`) |

## Project structure

```text
common/   Message framing (send_msg / recv_msg), SHA-1, config reader, constants
tracker/  main.cpp            Start-up and client listener
          client_handler.cpp  Per-client loop: read, dispatch, reply
          control.cpp         Users, login, groups and replicated ops
          data.cpp            Owner routing, DP requests, REPL to the backup
          sync.cpp            Tracker-to-tracker listener and messages
          heartbeat.cpp       Master role, heartbeats, catch-up and recovery
          oplog.cpp           tracker_<id>.log: append, replay, SYNC reads
client/   main.cpp            Command loop
          commands.cpp        Tracker requests, re-announce, upload, stop_share
          download.cpp        Download jobs, workers, progress file
          server.cpp          Chunk server: listener and four serve threads
          shares.cpp          Shared files and client_<port>.shares
tests/    net_test.cpp        Framing, SHA-1 and config tests (make test)
docs/     DESIGN.md (full design), phases/ (build plan)
```
