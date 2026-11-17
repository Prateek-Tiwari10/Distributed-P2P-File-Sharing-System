#pragma once

#include <string>
#include <vector>

#include "../common/config.h"

// Tracker-to-tracker messages on sync_port (DESIGN.md §4.3). One message per connection.

// Starts the sync listener thread. Returns false if I cannot listen on my sync_port.
bool start_sync_listener();

// One message to one tracker, with SYNC_TIMEOUT (or the timeout I pass). "" if it didn't answer.
std::string send_to(int id, const std::string& msg, int timeout_sec = SYNC_TIMEOUT);

// The same message to every other tracker, one after the other.
// replies[k] is the answer of tracker k ("" for myself and for trackers that didn't answer).
std::vector<std::string> send_to_all(const std::string& msg);
