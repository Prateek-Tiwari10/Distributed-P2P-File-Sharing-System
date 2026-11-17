#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../common/util.h"

// All the tracker's shared data, shaped exactly as in DESIGN.md §5.
//
// Lock order I always follow (never the other way round):
//     groups_mtx -> group_rec::mtx -> peers_mtx -> log_mtx
// users_mtx, session_mtx and cluster_mtx are always taken on their own.


// ----- Users -----

struct user_rec {
    std::string pwd;
    uint64_t id;
};

extern std::unordered_map<std::string, user_rec> users;
extern std::mutex users_mtx;


// ----- Sessions: which socket is logged in as which user -----

extern std::unordered_map<int, std::pair<std::string, std::string>> sessions;  // sock -> (uid, peer)
extern std::mutex session_mtx;


// ----- Peers -----

extern std::unordered_set<std::string> online;                   // peers logged in right now
extern std::unordered_map<std::string, std::string> peer_uid;    // peer -> uid (kept after logout)
extern std::mutex peers_mtx;


// ----- Files and groups -----

struct file_meta {
    uint64_t size;
    int nchunks;
    std::string file_hash;
    std::vector<std::string> chunk_hash;
    std::vector<std::unordered_set<std::string>> seeders;        // one set per chunk, peer = "ip:port"
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

extern std::unordered_map<std::string, std::shared_ptr<group_rec>> groups;
extern std::mutex groups_mtx;


// ----- Op log and cluster -----

extern std::vector<uint64_t> version;
extern uint64_t my_seq;
extern std::mutex log_mtx;

extern int my_id, N;

// The seniority list: live trackers, oldest join first. The master is always
// tracker_order.front(); I never keep it in a separate variable that could go stale.
extern std::list<int> tracker_order;
extern std::unordered_map<int, std::list<int>::iterator> tracker_pos;
extern int tracker_order_version;

extern std::vector<long long> last_seen;                         // master only: last HB from each id
extern std::mutex cluster_mtx;                                   // guards the four above

// A restarting tracker is in RECOVERY until it has rejoined the master.
enum tracker_state { ACTIVE, RECOVERY };
extern std::atomic<tracker_state> my_state;

extern std::vector<tracker_addr> trackers;                       // config rows, read-only after startup
