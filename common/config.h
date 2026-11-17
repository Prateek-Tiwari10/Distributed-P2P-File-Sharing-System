#pragma once

#include <cstddef>

// All constants from DESIGN.md §1.
constexpr size_t CHUNK = 512 * 1024;   // bytes per chunk
constexpr int RF = 2;                  // owner + backup
constexpr int HB_INTERVAL = 2;         // seconds between heartbeats
constexpr int HB_MISS = 3;             // missed HB_ACKs before I suspect the master
constexpr int INDIRECT_K = 3;          // most helpers asked to PING_REQ a suspected master
constexpr int PING_TIMEOUT = 1;        // seconds, a helper's own PING to the suspected master
constexpr int WORKERS = 4;             // transfer workers per upload/download
constexpr int SERVE_THREADS = 4;       // serve threads per client
constexpr int RECV_TIMEOUT = 5;        // seconds, on peer sockets
constexpr int SYNC_TIMEOUT = 1;        // seconds, tracker-to-tracker connect/recv
constexpr int MAX_TRIES = 3;           // attempts per chunk
constexpr int PEERS_PER_CHUNK = 3;     // peers returned per chunk
