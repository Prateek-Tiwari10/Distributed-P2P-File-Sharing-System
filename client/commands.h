#pragma once

#include <string>
#include <vector>

// Each cmd_ function returns the text to print for the user.
// If my tracker dies, they switch to the next tracker and return an ERR line.

std::string cmd_plain(const std::string& line, const std::vector<std::string>& a);  // control commands

std::string cmd_upload(const std::vector<std::string>& a);       // upload_file <path> <gid>
std::string cmd_download(const std::vector<std::string>& a);     // download_file <gid> <name> <dest_dir>
std::string cmd_stop_share(const std::vector<std::string>& a);   // stop_share <gid> <name>


// Helpers the transfer threads also use.

// Sends a DP line to the owner tracker on a temp connection, and to the backup
// if the owner can't be reached. "ERR file unavailable" if both fail.
std::string owner_request(const std::string& owner, const std::string& backup, const std::string& dp);
