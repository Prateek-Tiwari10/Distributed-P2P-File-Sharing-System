#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/util.h"

// All the client's shared data, shaped exactly as in DESIGN.md §5.
// I use C++17 `inline` globals, so every .cpp file shares one copy without a state.cpp.


// ----- Files I can serve to others -----

struct shared_file {
    std::string gid, name, path;
    uint64_t size;
    int nchunks;
    std::vector<bool> have;                                      // all true for uploads
};

inline std::unordered_map<std::string, std::shared_ptr<shared_file>> shares;  // key = gid + "|" + name
inline std::mutex shares_mtx;


// ----- Downloads in progress -----

struct download_job {
    std::string gid, name, dest;
    uint64_t size;
    int nchunks;

    std::string file_hash;
    std::vector<std::string> chunk_hash;

    std::vector<int> count;                                      // online seeders per chunk
    std::vector<std::vector<std::string>> peers;                 // up to 3 per chunk

    std::queue<int> todo;
    std::mutex q_mtx;
    std::vector<int> tries;

    std::vector<int> have_new;
    std::mutex have_mtx;

    int fd = -1;
    std::string owner_addr, backup_addr;

    std::atomic<int> done{0};
    std::atomic<char> status{'D'};                               // D / C / F
};

inline std::unordered_map<std::string, std::shared_ptr<download_job>> downloads;
inline std::mutex dl_mtx;


// ----- Serving chunks to other peers -----

inline std::queue<int> serve_q;
inline std::mutex serve_mtx;
inline std::condition_variable serve_cv;


// ----- Who I am and my tracker connection -----

inline std::string my_uid, my_addr;
inline int tracker_sock = -1;
inline int tracker_idx = 0;

inline std::vector<tracker_addr> tracker_list;                   // rows of tracker_info.txt, read-only after startup
