#pragma once

#include <cstddef>
#include <string>
#include <vector>

std::vector<std::string> split(const std::string& s, char sep = ' ');  // skips empty parts
std::string join(const std::vector<std::string>& v, char sep);
std::string sha1_hex(const char* data, size_t n);                    // 40 lowercase hex chars
std::string sha1_file(const std::string& path);                      // streaming, whole file; "" on error

struct tracker_addr { int id; std::string ip; int client_port, sync_port; };
std::vector<tracker_addr> read_config(const std::string& path);      // one "id ip client_port sync_port" per line

long long now_ms();
