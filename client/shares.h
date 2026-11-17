#pragma once

#include <memory>
#include <string>
#include <vector>

#include "state.h"

// The `shares` map and its copy on disk, client_<port>.shares (one "gid name path" line per file).

std::string share_key(const std::string& gid, const std::string& name);   // "gid|name"

void put_share(std::shared_ptr<shared_file> f);                    // put in the map only
void save_share(const shared_file& f);                             // append a line to the file if it isn't there
void add_share(std::shared_ptr<shared_file> f);                    // both of the above
void remove_share(const std::string& gid, const std::string& name); // drop from the map and the file
void remove_group_shares(const std::string& gid);                  // same, for every file of a group

// The "gid name path" lines of client_<port>.shares, each split into its 3 parts.
std::vector<std::vector<std::string>> read_share_lines();

void set_have(const std::string& gid, const std::string& name, int i);   // mark one chunk as ready to serve
