#pragma once

#include <string>
#include <vector>

// Data-plane commands (DESIGN.md §4.1 last row, §4.2) and their replication (§4.3 REPL).

// upload_file / download_file / stop_share <gid> <name>, sent by a logged-in client
// to its own tracker. I answer OK if I own the file, otherwise REDIRECT <owner> <backup>.
std::string route_file_cmd(const std::string& uid, const std::vector<std::string>& a);

// "DP <uid> <cmd> ..." lines. These can come on a redirected temp connection
// that never logged in, so there is no session check for them.
// I only accept them if I am the file's owner or its backup.
std::string handle_dp(const std::vector<std::string>& a);

// One replicated data change, without the "REPL" word in front:
//   META <gid> <name> <size> <nchunks> <file_hash> <hashes> <peer|->
//   HAVE <gid> <name> <peer> <i,j,k>
//   STOP <gid> <name> <peer>
// Used for REPL messages from the owner.
std::string apply_repl(const std::vector<std::string>& a);

// debug_files <gid>: which files I hold metadata for, and their seeders.
std::string debug_files(const std::vector<std::string>& a);
