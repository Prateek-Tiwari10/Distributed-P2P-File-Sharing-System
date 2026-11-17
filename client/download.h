#pragma once

#include <string>

// Downloads (DESIGN.md §6.3). start_download returns at once; the transfer thread
// fetches the metadata from the owner (backup as fallback) and runs WORKERS workers.
void start_download(const std::string& uid, const std::string& gid, const std::string& name,
                    const std::string& dest, const std::string& owner, const std::string& backup);

// The show_downloads lines: "[D|C|F] <gid> <name> <done>/<total>".
std::string show_downloads();
