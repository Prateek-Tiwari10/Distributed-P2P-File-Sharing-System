#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The op log, tracker_<id>.log: one applied control op per line, "origin seq OP...".

void oplog_open();                                                   // open my log file for appending

// Writes one line and makes sure it is on disk (fflush + fsync). Caller holds log_mtx.
void oplog_append(int origin, uint64_t seq, const std::string& op);

// Re-applies every line of my log at startup, without logging or broadcasting again.
// Sets version[] and my_seq from what it read.
void oplog_replay();

// The log lines with this origin and seq >= from_seq, in seq order (for SYNC).
std::vector<std::string> oplog_read_from(int origin, uint64_t from_seq);
