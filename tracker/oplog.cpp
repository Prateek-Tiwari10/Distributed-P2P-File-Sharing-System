#include "oplog.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <unistd.h>

#include "../common/util.h"
#include "control.h"
#include "state.h"


static FILE* log_file = nullptr;


static std::string log_path() {
    return "tracker_" + std::to_string(my_id) + ".log";
}


// Splits "origin seq OP..." into its parts. False if the line is broken.
static bool parse_line(const std::string& line, int& origin, uint64_t& seq, std::vector<std::string>& op) {
    std::vector<std::string> parts = split(line);
    if (parts.size() < 3) {
        return false;
    }

    try {
        origin = std::stoi(parts[0]);
        seq    = std::stoull(parts[1]);
    } catch (...) {
        return false;
    }

    if (origin < 0 || origin >= N) {
        return false;
    }

    op.assign(parts.begin() + 2, parts.end());
    return true;
}


static std::vector<std::string> read_all_lines() {
    std::vector<std::string> lines;

    std::ifstream in(log_path());
    std::string line;

    while (std::getline(in, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    return lines;
}


void oplog_open() {
    log_file = fopen(log_path().c_str(), "a");

    if (log_file == nullptr) {
        fprintf(stderr, "[tracker %d] cannot open %s\n", my_id, log_path().c_str());
    }
}


void oplog_append(int origin, uint64_t seq, const std::string& op) {
    if (log_file == nullptr) {
        return;
    }

    fprintf(log_file, "%d %llu %s\n", origin, (unsigned long long)seq, op.c_str());

    // The client only gets OK after this, so the op must really be on disk first.
    fflush(log_file);
    fsync(fileno(log_file));
}


void oplog_replay() {
    int count = 0;

    for (const std::string& line : read_all_lines()) {
        int origin;
        uint64_t seq;
        std::vector<std::string> op;

        if (!parse_line(line, origin, seq, op)) {
            continue;
        }

        // Same apply code as a BCAST, just without logging it a second time.
        apply_change(op);

        std::lock_guard<std::mutex> lk(log_mtx);
        if (seq > version[origin]) {
            version[origin] = seq;
        }
        count++;
    }

    std::lock_guard<std::mutex> lk(log_mtx);
    my_seq = version[my_id];

    fprintf(stderr, "[tracker %d] replayed %d ops from %s\n", my_id, count, log_path().c_str());
}


std::vector<std::string> oplog_read_from(int origin, uint64_t from_seq) {
    // A map sorts by seq for me and also drops a line that got logged twice.
    std::map<uint64_t, std::string> picked;

    {
        std::lock_guard<std::mutex> lk(log_mtx);

        for (const std::string& line : read_all_lines()) {
            int o;
            uint64_t seq;
            std::vector<std::string> op;

            if (parse_line(line, o, seq, op) && o == origin && seq >= from_seq) {
                picked[seq] = line;
            }
        }
    }

    std::vector<std::string> out;
    for (auto& kv : picked) {
        out.push_back(kv.second);
    }

    return out;
}
