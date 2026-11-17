#include "data.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <random>

#include "../common/config.h"
#include "../common/util.h"
#include "control.h"
#include "state.h"
#include "sync.h"


// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Every tracker must pick the same owner for a new file, so I hash "gid|name"
// and take it modulo the fixed N from the config (never the live count).
static int hash_owner(const std::string& gid, const std::string& name) {
    size_t h = std::hash<std::string>{}(gid + "|" + name);
    return (int)(h % N);
}


static int backup_of(int owner) {
    return (owner + 1) % N;
}


// If the file already exists I use the owner stored with it,
// otherwise I fall back to the hash. Caller must hold g.mtx.
static int owner_of(const group_rec& g, const std::string& gid, const std::string& name) {
    auto it = g.files.find(name);

    if (it != g.files.end()) {
        return it->second.owner;
    }

    return hash_owner(gid, name);
}


// Turns a tracker id into the "ip:client_port" a client can connect to.
static std::string client_addr(int id) {
    for (const tracker_addr& t : trackers) {
        if (t.id == id) {
            return t.ip + ":" + std::to_string(t.client_port);
        }
    }

    return "";
}


// Returns the file's metadata, or nullptr if I don't have it. Caller must hold g.mtx.
static file_meta* find_meta(group_rec& g, const std::string& name) {
    auto it = g.files.find(name);

    if (it == g.files.end()) {
        return nullptr;
    }

    return it->second.meta.get();
}


// Reads a non-negative whole number. I return false on anything else
// so a bad message can't crash the tracker.
static bool to_number(const std::string& s, long long& out) {
    try {
        size_t used = 0;
        out = std::stoll(s, &used);
        return used == s.size() && out >= 0;
    } catch (...) {
        return false;
    }
}


// "3,7,9" -> {3, 7, 9}. False if any index is not a valid chunk of this file.
static bool parse_chunks(const std::string& list, int nchunks, std::vector<int>& out) {
    for (const std::string& part : split(list, ',')) {
        long long i = 0;

        if (!to_number(part, i) || i >= nchunks) {
            return false;
        }

        out.push_back((int)i);
    }

    return true;
}


// "0,1,2,...,n-1" — every chunk of a file.
static std::string all_chunks(int nchunks) {
    std::vector<std::string> parts;

    for (int i = 0; i < nchunks; i++) {
        parts.push_back(std::to_string(i));
    }

    return join(parts, ',');
}


// A fresh metadata record. `peer` (if not "-") seeds every chunk.
static std::shared_ptr<file_meta> make_meta(uint64_t size, int nchunks, const std::string& file_hash,
                                            const std::vector<std::string>& hashes, const std::string& peer) {
    auto meta = std::make_shared<file_meta>();
    meta->size       = size;
    meta->nchunks    = nchunks;
    meta->file_hash  = file_hash;
    meta->chunk_hash = hashes;
    meta->seeders.resize(nchunks);

    if (peer != "-") {
        for (auto& chunk_seeders : meta->seeders) {
            chunk_seeders.insert(peer);
        }
    }

    return meta;
}


// Sends one REPL message to the backup. Called with no lock held.
// If the backup is down I just print it. A restarted backup does not get this back (see heartbeat.cpp).
static void repl_to_backup(int owner, const std::string& msg) {
    int backup = backup_of(owner);

    if (backup == my_id) {
        return;
    }

    if (send_to(backup, "REPL " + msg) != "ACK") {
        fprintf(stderr, "[tracker %d] REPL to backup %d failed\n", my_id, backup);
    }
}


// ---------------------------------------------------------------------------
// upload_file / download_file / stop_share <gid> <name>  (client's own tracker)
// ---------------------------------------------------------------------------

std::string route_file_cmd(const std::string& uid, const std::vector<std::string>& a) {
    if (a.size() != 3) {
        return "ERR usage: " + a[0] + " <gid> <name>";
    }

    const std::string& gid  = a[1];
    const std::string& name = a[2];

    auto g = get_group(gid);
    if (!g) {
        return "ERR no group";
    }

    int owner;
    {
        std::lock_guard<std::mutex> lk(g->mtx);

        if (gone(*g)) {
            return "ERR no group";
        }
        if (!is_member(*g, uid)) {
            return "ERR not member";
        }

        owner = owner_of(*g, gid, name);
    }

    // If I'm the owner, the client can send the DP line to me.
    if (owner == my_id) {
        return "OK";
    }

    return "REDIRECT " + client_addr(owner) + " " + client_addr(backup_of(owner));
}


// ---------------------------------------------------------------------------
// DP handlers (run on the owner, or on the backup while the owner is down)
// ---------------------------------------------------------------------------

// What a DP handler hands back: the reply for the client, plus the REPL message
// (if any) that the owner must send to its backup once the group lock is released.
struct dp_result {
    std::string reply;
    std::string repl;
    bool new_file = false;
};


// DP <uid> upload_file <gid> <name> <size> <nchunks> <file_hash> <h0,h1,...> <peer>
// Caller holds g.mtx.
static dp_result dp_upload(group_rec& g, const std::vector<std::string>& a) {
    dp_result res;

    if (a.size() != 10) {
        res.reply = "ERR usage: DP <uid> upload_file <gid> <name> <size> <nchunks> <file_hash> <hashes> <peer>";
        return res;
    }

    const std::string& gid       = a[3];
    const std::string& name      = a[4];
    const std::string& file_hash = a[7];
    const std::string& peer      = a[9];

    long long size    = 0;
    long long nchunks = 0;
    if (!to_number(a[5], size) || !to_number(a[6], nchunks) || size == 0) {
        res.reply = "ERR bad size";
        return res;
    }

    // I double check the numbers agree, so later chunk indexes are always valid.
    std::vector<std::string> hashes = split(a[8], ',');
    long long expected = (size + CHUNK - 1) / CHUNK;

    if (nchunks != expected || (long long)hashes.size() != nchunks) {
        res.reply = "ERR bad chunk count";
        return res;
    }

    auto it = g.files.find(name);

    // First upload of this name: I build the metadata and the uploader seeds every chunk.
    if (it == g.files.end()) {
        file_entry entry;
        entry.owner = hash_owner(gid, name);
        entry.meta  = make_meta(size, nchunks, file_hash, hashes, peer);
        g.files[name] = entry;

        fprintf(stderr, "[tracker %d] new file %s/%s, owner %d, backup %d\n",
                my_id, gid.c_str(), name.c_str(), entry.owner, backup_of(entry.owner));

        res.reply    = "OK";
        res.new_file = true;
        res.repl     = "META " + gid + " " + name + " " + a[5] + " " + a[6] + " " + file_hash + " " + a[8] + " " + peer;
        return res;
    }

    file_meta* meta = it->second.meta.get();
    if (!meta) {
        res.reply = "ERR no file";
        return res;
    }

    // Same name but different content is not allowed.
    if (meta->file_hash != file_hash) {
        res.reply = "ERR exists with different hash";
        return res;
    }

    // Same file uploaded again (maybe by someone else): they become a seeder of every chunk.
    for (auto& chunk_seeders : meta->seeders) {
        chunk_seeders.insert(peer);
    }

    res.reply = "OK";
    res.repl  = "HAVE " + gid + " " + name + " " + peer + " " + all_chunks(meta->nchunks);
    return res;
}


// One random generator per thread, so shuffling needs no lock.
static std::mt19937& random_gen() {
    thread_local std::mt19937 gen(std::random_device{}());
    return gen;
}


// The seeder filter (DESIGN.md §4.2): keep only peers that are online right now
// and whose user is still a member of the group. Caller holds g.mtx and peers_mtx.
static std::vector<std::string> usable_seeders(const group_rec& g, const std::unordered_set<std::string>& seeders) {
    std::vector<std::string> out;

    for (const std::string& p : seeders) {
        if (online.count(p) == 0) {
            continue;
        }

        auto who = peer_uid.find(p);
        if (who == peer_uid.end() || !is_member(g, who->second)) {
            continue;
        }

        out.push_back(p);
    }

    return out;
}


// DP <uid> download_file <gid> <name>
// Reply: "OK <size> <nchunks> <file_hash> <h0,h1,...>" then one line per chunk "<i> <count> <p1,p2,...>"
// Caller holds g.mtx.
static dp_result dp_download(group_rec& g, const std::vector<std::string>& a) {
    dp_result res;

    if (a.size() != 5) {
        res.reply = "ERR usage: DP <uid> download_file <gid> <name>";
        return res;
    }

    file_meta* meta = find_meta(g, a[4]);
    if (!meta) {
        res.reply = "ERR no file";
        return res;
    }

    std::string out = "OK " + std::to_string(meta->size)
                    + " " + std::to_string(meta->nchunks)
                    + " " + meta->file_hash
                    + " " + join(meta->chunk_hash, ',');

    // Lock order: g.mtx (held by my caller) and then peers_mtx.
    std::lock_guard<std::mutex> lk(peers_mtx);

    for (int i = 0; i < meta->nchunks; i++) {
        std::vector<std::string> usable = usable_seeders(g, meta->seeders[i]);

        // count is every usable seeder; the list is at most PEERS_PER_CHUNK of them, at random.
        int count = usable.size();
        std::shuffle(usable.begin(), usable.end(), random_gen());

        if ((int)usable.size() > PEERS_PER_CHUNK) {
            usable.resize(PEERS_PER_CHUNK);
        }

        std::string peers = "-";
        if (!usable.empty()) {
            peers = join(usable, ',');
        }

        out += "\n" + std::to_string(i) + " " + std::to_string(count) + " " + peers;
    }

    res.reply = out;
    return res;
}


// DP <uid> have <gid> <name> <peer> <i,j,k>
// Caller holds g.mtx.
static dp_result dp_have(group_rec& g, const std::vector<std::string>& a) {
    dp_result res;

    if (a.size() != 7) {
        res.reply = "ERR usage: DP <uid> have <gid> <name> <peer> <i,j,k>";
        return res;
    }

    const std::string& peer = a[5];

    file_meta* meta = find_meta(g, a[4]);
    if (!meta) {
        res.reply = "ERR no file";
        return res;
    }

    // I check every index first, so a bad list changes nothing at all.
    std::vector<int> chunks;
    if (!parse_chunks(a[6], meta->nchunks, chunks)) {
        res.reply = "ERR bad chunk index";
        return res;
    }

    for (int i : chunks) {
        meta->seeders[i].insert(peer);
    }

    res.reply = "OK";
    res.repl  = "HAVE " + a[3] + " " + a[4] + " " + peer + " " + a[6];
    return res;
}


// DP <uid> stop_share <gid> <name> <peer>
// I always say OK: if the file is gone, there is nothing left to stop.
// Caller holds g.mtx.
static dp_result dp_stop_share(group_rec& g, const std::vector<std::string>& a) {
    dp_result res;

    if (a.size() != 6) {
        res.reply = "ERR usage: DP <uid> stop_share <gid> <name> <peer>";
        return res;
    }

    const std::string& peer = a[5];

    file_meta* meta = find_meta(g, a[4]);
    if (meta) {
        for (auto& chunk_seeders : meta->seeders) {
            chunk_seeders.erase(peer);
        }
    }

    res.reply = "OK";
    res.repl  = "STOP " + a[3] + " " + a[4] + " " + peer;
    return res;
}


std::string handle_dp(const std::vector<std::string>& a) {
    // Every DP form starts with: DP <uid> <cmd> <gid> <name> ...
    if (a.size() < 5) {
        return "ERR usage: DP <uid> <cmd> <gid> <name> ...";
    }

    const std::string& uid  = a[1];
    const std::string& cmd  = a[2];
    const std::string& gid  = a[3];
    const std::string& name = a[4];

    auto g = get_group(gid);
    if (!g) {
        return "ERR not member";
    }

    dp_result res;
    int owner;

    {
        std::lock_guard<std::mutex> lk(g->mtx);

        if (gone(*g) || !is_member(*g, uid)) {
            return "ERR not member";
        }

        // The owner handles DP lines; the backup does too, for when the owner is down.
        owner = owner_of(*g, gid, name);

        if (my_id != owner && my_id != backup_of(owner)) {
            return "ERR not owner";
        }

        if (cmd == "upload_file")        res = dp_upload(*g, a);
        else if (cmd == "download_file") res = dp_download(*g, a);
        else if (cmd == "have")          res = dp_have(*g, a);
        else if (cmd == "stop_share")    res = dp_stop_share(*g, a);
        else                             return "ERR unknown command";
    }

    // Everything below talks to other trackers, so the group lock is already released.
    // Only the owner replicates; a backup standing in for a dead owner keeps the change to itself.
    if (my_id == owner && !res.repl.empty() && res.reply.rfind("OK", 0) == 0) {
        repl_to_backup(owner, res.repl);
    }

    // Every tracker must learn a new file name, whoever stored it.
    if (res.new_file) {
        local_op({"ADD_FILE", gid, name, std::to_string(owner)});
    }

    return res.reply;
}


// ---------------------------------------------------------------------------
// Applying REPL lines (on the backup)
// ---------------------------------------------------------------------------

// META <gid> <name> <size> <nchunks> <file_hash> <hashes> <peer|->
static std::string repl_meta(group_rec& g, const std::vector<std::string>& a) {
    long long size = 0, nchunks = 0;

    if (a.size() != 8 || !to_number(a[3], size) || !to_number(a[4], nchunks)) {
        return "ERR bad META";
    }

    std::vector<std::string> hashes = split(a[6], ',');
    if ((long long)hashes.size() != nchunks) {
        return "ERR bad META";
    }

    const std::string& gid  = a[1];
    const std::string& name = a[2];
    const std::string& peer = a[7];

    auto it = g.files.find(name);

    if (it == g.files.end()) {
        // ADD_FILE hasn't reached me yet; the owner of a new file is always the hash.
        file_entry entry;
        entry.owner = hash_owner(gid, name);
        entry.meta  = make_meta(size, nchunks, a[5], hashes, peer);
        g.files[name] = entry;
        return "ACK";
    }

    file_entry& entry = it->second;

    if (!entry.meta) {
        entry.meta = make_meta(size, nchunks, a[5], hashes, peer);
        return "ACK";
    }

    // I already have it: only add the peer, and only if it really is the same file.
    if (entry.meta->file_hash == a[5] && peer != "-") {
        for (auto& chunk_seeders : entry.meta->seeders) {
            chunk_seeders.insert(peer);
        }
    }

    return "ACK";
}


// HAVE <gid> <name> <peer> <i,j,k>
static std::string repl_have(group_rec& g, const std::vector<std::string>& a) {
    if (a.size() != 5) {
        return "ERR bad HAVE";
    }

    file_meta* meta = find_meta(g, a[2]);
    if (!meta) {
        return "ACK";
    }

    std::vector<int> chunks;
    if (!parse_chunks(a[4], meta->nchunks, chunks)) {
        return "ERR bad HAVE";
    }

    for (int i : chunks) {
        meta->seeders[i].insert(a[3]);
    }

    return "ACK";
}


// STOP <gid> <name> <peer>
static std::string repl_stop(group_rec& g, const std::vector<std::string>& a) {
    if (a.size() != 4) {
        return "ERR bad STOP";
    }

    file_meta* meta = find_meta(g, a[2]);
    if (meta) {
        for (auto& chunk_seeders : meta->seeders) {
            chunk_seeders.erase(a[3]);
        }
    }

    return "ACK";
}


std::string apply_repl(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        return "ERR bad REPL";
    }

    // If the group doesn't exist here, there is nothing to attach the data to.
    auto g = get_group(a[1]);
    if (!g) {
        return "ACK";
    }

    std::lock_guard<std::mutex> lk(g->mtx);

    if (gone(*g)) {
        return "ACK";
    }

    if (a[0] == "META") return repl_meta(*g, a);
    if (a[0] == "HAVE") return repl_have(*g, a);
    if (a[0] == "STOP") return repl_stop(*g, a);

    return "ERR bad REPL";
}


// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

// One line per file: owner, whether I hold its metadata, and every seeder with
// the number of chunks it seeds, e.g. "big.bin owner 2 meta 100 chunks 127.0.0.1:9001(100)".
std::string debug_files(const std::vector<std::string>& a) {
    if (a.size() != 2) {
        return "ERR usage: debug_files <gid>";
    }

    auto g = get_group(a[1]);
    if (!g) {
        return "ERR no group";
    }

    std::lock_guard<std::mutex> lk(g->mtx);

    std::string out = "OK tracker " + std::to_string(my_id);

    for (auto& kv : g->files) {
        const file_entry& entry = kv.second;

        out += "\n" + kv.first + " owner " + std::to_string(entry.owner);

        if (!entry.meta) {
            out += " no meta";
            continue;
        }

        out += " meta " + std::to_string(entry.meta->nchunks) + " chunks";

        // A sorted map, so the output is easy to compare between runs.
        std::map<std::string, int> per_peer;
        for (auto& chunk_seeders : entry.meta->seeders) {
            for (const std::string& p : chunk_seeders) {
                per_peer[p]++;
            }
        }

        for (auto& pc : per_peer) {
            out += " " + pc.first + "(" + std::to_string(pc.second) + ")";
        }
    }

    return out;
}
