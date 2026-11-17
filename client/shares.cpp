#include "shares.h"

#include <fstream>
#include <vector>

#include "../common/util.h"


// My port is the part of my_addr after the ':', e.g. "127.0.0.1:9001" -> "client_9001.shares".
static std::string shares_path() {
    size_t colon = my_addr.rfind(':');
    std::string port = my_addr.substr(colon + 1);

    return "client_" + port + ".shares";
}


// Reads every line of client_<port>.shares. A missing file just gives an empty list.
static std::vector<std::string> read_lines() {
    std::vector<std::string> lines;

    std::ifstream in(shares_path());
    std::string line;

    while (std::getline(in, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    return lines;
}


// Overwrites client_<port>.shares with exactly these lines.
static void write_lines(const std::vector<std::string>& lines) {
    std::ofstream out(shares_path(), std::ios::trunc);

    for (const std::string& line : lines) {
        out << line << "\n";
    }
}


std::string share_key(const std::string& gid, const std::string& name) {
    return gid + "|" + name;
}


void put_share(std::shared_ptr<shared_file> f) {
    std::lock_guard<std::mutex> lk(shares_mtx);

    shares[share_key(f->gid, f->name)] = f;
}


void save_share(const shared_file& f) {
    // I keep shares_mtx while touching the file, so two threads never write it at the same time.
    std::lock_guard<std::mutex> lk(shares_mtx);

    // Uploading or downloading the same file twice should not write a second line.
    for (const std::string& line : read_lines()) {
        std::vector<std::string> parts = split(line);

        if (parts.size() >= 2 && parts[0] == f.gid && parts[1] == f.name) {
            return;
        }
    }

    std::ofstream out(shares_path(), std::ios::app);
    out << f.gid << " " << f.name << " " << f.path << "\n";
}


void add_share(std::shared_ptr<shared_file> f) {
    put_share(f);
    save_share(*f);
}


void remove_share(const std::string& gid, const std::string& name) {
    std::lock_guard<std::mutex> lk(shares_mtx);

    shares.erase(share_key(gid, name));

    // I keep the lines for other files and write the file back.
    std::vector<std::string> keep;

    for (const std::string& line : read_lines()) {
        std::vector<std::string> parts = split(line);

        bool same_file = parts.size() >= 2 && parts[0] == gid && parts[1] == name;
        if (!same_file) {
            keep.push_back(line);
        }
    }

    write_lines(keep);
}


void remove_group_shares(const std::string& gid) {
    std::lock_guard<std::mutex> lk(shares_mtx);

    for (auto it = shares.begin(); it != shares.end(); ) {
        if (it->second->gid == gid) {
            it = shares.erase(it);
        } else {
            ++it;
        }
    }

    std::vector<std::string> keep;

    for (const std::string& line : read_lines()) {
        std::vector<std::string> parts = split(line);

        if (parts.empty() || parts[0] != gid) {
            keep.push_back(line);
        }
    }

    write_lines(keep);
}


void set_have(const std::string& gid, const std::string& name, int i) {
    std::lock_guard<std::mutex> lk(shares_mtx);

    auto it = shares.find(share_key(gid, name));
    if (it == shares.end()) {
        return;   // the user stopped sharing it while I was downloading
    }

    shared_file& f = *it->second;
    if (i >= 0 && i < f.nchunks) {
        f.have[i] = true;
    }
}


std::vector<std::vector<std::string>> read_share_lines() {
    std::lock_guard<std::mutex> lk(shares_mtx);

    std::vector<std::vector<std::string>> out;

    for (const std::string& line : read_lines()) {
        std::vector<std::string> parts = split(line);

        if (parts.size() == 3) {
            out.push_back(parts);
        }
    }

    return out;
}
