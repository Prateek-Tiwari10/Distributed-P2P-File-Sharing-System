#include "commands.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <queue>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "../common/config.h"
#include "../common/net.h"
#include "../common/util.h"
#include "download.h"
#include "shares.h"
#include "state.h"


// ---------------------------------------------------------------------------
// Talking to the tracker
// ---------------------------------------------------------------------------

// My tracker is gone: I move to the next tracker in tracker_info.txt (round robin,
// skipping any that don't answer). My session died with the old tracker, so I forget my uid.
static void switch_tracker() {
    close(tracker_sock);
    tracker_sock = -1;
    my_uid.clear();

    int n = tracker_list.size();

    for (int k = 1; k <= n; k++) {
        int idx = (tracker_idx + k) % n;
        const tracker_addr& t = tracker_list[idx];

        int sock = connect_to(t.ip, t.client_port, RECV_TIMEOUT);
        if (sock < 0) {
            continue;
        }

        // connect_to put a recv timeout on it, but a blocking create can take a while,
        // so the persistent socket goes back to waiting as long as it needs.
        set_recv_timeout(sock, 0);

        tracker_sock = sock;
        tracker_idx  = idx;

        printf("tracker changed, please login again\n");
        fflush(stdout);
        return;
    }

    fprintf(stderr, "[client] no tracker is reachable\n");
}


// Send one message on my persistent tracker socket and wait for the reply.
// If the socket is broken I switch trackers and return "".
static std::string ask_tracker(const std::string& msg) {
    std::string reply;

    if (tracker_sock >= 0 && send_msg(tracker_sock, msg) && recv_msg(tracker_sock, reply)) {
        return reply;
    }

    switch_tracker();
    return "";
}


// The "ip:client_port" of a row in tracker_info.txt.
static std::string tracker_client_addr(int idx) {
    const tracker_addr& t = tracker_list[idx];

    return t.ip + ":" + std::to_string(t.client_port);
}


// Step 1 of every file command: ask my own tracker who owns the file.
// On "OK" (my tracker is the owner) or "REDIRECT" I fill in owner and backup and return "OK".
// Otherwise I return the tracker's error, or "" if the connection broke.
//
// Design: data-plane commands go to the owner tracker, which is usually not the client's home tracker. A temporary connection matches redirect-based routing; the persistent socket stays dedicated to the home tracker.
static std::string route(const std::string& first, std::string& owner, std::string& backup) {
    std::string reply = ask_tracker(first);

    if (reply.empty()) {
        return "";
    }

    if (reply == "OK") {
        owner  = tracker_client_addr(tracker_idx);
        backup = tracker_client_addr((tracker_idx + 1) % tracker_list.size());
        return "OK";
    }

    // Anything that is not a redirect is an error from my tracker, so I pass it on.
    if (reply.rfind("REDIRECT ", 0) != 0) {
        return reply;
    }

    std::vector<std::string> parts = split(reply);
    if (parts.size() != 3) {
        return "ERR bad redirect";
    }

    owner  = parts[1];
    backup = parts[2];

    if (getenv("P2P_DEBUG") != nullptr) {
        fprintf(stderr, "[client] %s -> REDIRECT owner %s, backup %s\n", first.c_str(), owner.c_str(), backup.c_str());
    }

    return "OK";
}


std::string owner_request(const std::string& owner, const std::string& backup, const std::string& dp) {
    std::string answer = request(owner, dp, RECV_TIMEOUT);

    if (answer.empty()) {
        answer = request(backup, dp, RECV_TIMEOUT);
    }
    if (answer.empty()) {
        return "ERR file unavailable";
    }

    return answer;
}


// ---------------------------------------------------------------------------
// Re-announce after login (DESIGN.md §6.6)
// ---------------------------------------------------------------------------

// For one line of client_<port>.shares: if the file is still on disk, I serve it again
// and tell its owner I have every chunk. Returns false only if my tracker connection broke.
static bool reannounce_one(const std::string& gid, const std::string& name, const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
        return true;   // the file is gone, so I skip it quietly
    }

    uint64_t size = st.st_size;
    int nchunks = (size + CHUNK - 1) / CHUNK;

    auto f = std::make_shared<shared_file>();
    f->gid     = gid;
    f->name    = name;
    f->path    = path;
    f->size    = size;
    f->nchunks = nchunks;
    f->have.assign(nchunks, true);
    put_share(f);


    // Any of the three file commands works for the owner lookup; I use download_file.
    std::string owner, backup;
    std::string r = route("download_file " + gid + " " + name, owner, backup);

    if (r.empty()) {
        return false;
    }
    if (r != "OK") {
        printf("[re-announce] %s %s: %s\n", gid.c_str(), name.c_str(), r.c_str());
        return true;
    }

    std::vector<std::string> all;
    for (int i = 0; i < nchunks; i++) {
        all.push_back(std::to_string(i));
    }

    std::string dp = "DP " + my_uid + " have " + gid + " " + name + " " + my_addr + " " + join(all, ',');
    std::string reply = owner_request(owner, backup, dp);

    printf("[re-announce] %s %s: %s\n", gid.c_str(), name.c_str(), reply.c_str());
    return true;
}


static bool reannounce() {
    for (const std::vector<std::string>& line : read_share_lines()) {
        if (!reannounce_one(line[0], line[1], line[2])) {
            return false;
        }
    }

    fflush(stdout);
    return true;
}


// ---------------------------------------------------------------------------
// Plain control commands (create_user, login, groups, ...)
// ---------------------------------------------------------------------------

std::string cmd_plain(const std::string& line, const std::vector<std::string>& a) {
    std::string msg = line;

    // The tracker needs my ip:port for its peer tables, so I add it to login myself.
    if (a[0] == "login") {
        msg += " " + my_addr;
    }

    std::string reply = ask_tracker(msg);

    // After a tracker change I try create_user and login once more on the new tracker.
    // Anything else needs a login first, so it is not worth retrying.
    if (reply.empty() && tracker_sock >= 0 && (a[0] == "create_user" || a[0] == "login")) {
        reply = ask_tracker(msg);
    }

    if (reply.empty()) {
        return "ERR no reply from tracker";
    }

    // Remember who I am, because the DP lines carry my uid.
    if (reply.rfind("OK", 0) == 0) {
        if (a[0] == "login" && a.size() >= 2) {
            my_uid = a[1];

            // Files I shared before get announced again. The tracker's OK comes
            // back to the user only after that, so its lines appear just above it.
            if (!reannounce()) {
                return "ERR no reply from tracker";
            }
        }
        if (a[0] == "logout") {
            my_uid.clear();
        }

        // I left the group, so I must not serve its files any more (DESIGN.md §6.5).
        if (a[0] == "leave_group" && a.size() >= 2) {
            remove_group_shares(a[1]);
        }
    }

    return reply;
}


// ---------------------------------------------------------------------------
// upload_file <path> <gid>
// ---------------------------------------------------------------------------

// "./test/big.bin" -> "big.bin"
static std::string base_name(const std::string& path) {
    size_t slash = path.rfind('/');

    if (slash == std::string::npos) {
        return path;
    }

    return path.substr(slash + 1);
}


// Everything one upload's hashing workers share.
struct hash_work {
    std::string path;
    uint64_t size;

    std::queue<int> todo;
    std::mutex q_mtx;

    std::vector<std::string> chunk_hash;   // pre-sized, each worker writes only its own indexes
    std::atomic<bool> failed{false};
};


// One hashing worker: take a chunk index, pread that chunk, SHA1 it, repeat.
// Each worker opens the file itself, so no file descriptor is shared.
static void hash_worker(hash_work* w) {
    int fd = open(w->path.c_str(), O_RDONLY);
    if (fd < 0) {
        w->failed = true;
        return;
    }

    std::vector<char> buf(CHUNK);

    while (true) {
        int i;
        {
            std::lock_guard<std::mutex> lk(w->q_mtx);

            if (w->todo.empty()) {
                break;
            }
            i = w->todo.front();
            w->todo.pop();
        }

        uint64_t start = (uint64_t)i * CHUNK;
        size_t len = CHUNK;
        if (w->size - start < CHUNK) {
            len = w->size - start;
        }

        size_t got = 0;
        while (got < len) {
            ssize_t r = pread(fd, buf.data() + got, len - got, start + got);
            if (r <= 0) {
                break;
            }
            got += r;
        }

        if (got != len) {
            w->failed = true;
            break;
        }

        // Distinct indexes per worker, so this write needs no lock.
        w->chunk_hash[i] = sha1_hex(buf.data(), len);
    }

    close(fd);
}


// The upload transfer thread: hash with WORKERS threads, then send the metadata to the owner.
static void upload_thread(std::string uid, std::string path, std::string gid, std::string name,
                          uint64_t size, int nchunks, std::string owner, std::string backup) {
    hash_work w;
    w.path = path;
    w.size = size;
    w.chunk_hash.resize(nchunks);

    for (int i = 0; i < nchunks; i++) {
        w.todo.push(i);
    }

    std::vector<std::thread> workers;
    for (int k = 0; k < WORKERS; k++) {
        workers.push_back(std::thread(hash_worker, &w));
    }
    for (std::thread& t : workers) {
        t.join();
    }

    std::string file_hash = sha1_file(path);

    if (w.failed || file_hash.empty()) {
        printf("[upload] %s %s: ERR cannot read file\n", gid.c_str(), name.c_str());
        fflush(stdout);
        return;
    }


    std::string dp = "DP " + uid + " upload_file " + gid + " " + name
                   + " " + std::to_string(size)
                   + " " + std::to_string(nchunks)
                   + " " + file_hash
                   + " " + join(w.chunk_hash, ',')
                   + " " + my_addr;

    std::string reply = owner_request(owner, backup, dp);

    if (reply == "OK") {
        // The tracker accepted it, so now I'm a seeder of every chunk.
        auto f = std::make_shared<shared_file>();
        f->gid     = gid;
        f->name    = name;
        f->path    = path;
        f->size    = size;
        f->nchunks = nchunks;
        f->have.assign(nchunks, true);

        add_share(f);
    }

    printf("[upload] %s %s: %s\n", gid.c_str(), name.c_str(), reply.c_str());
    fflush(stdout);
}


std::string cmd_upload(const std::vector<std::string>& a) {
    if (a.size() != 3) {
        return "ERR usage: upload_file <path> <gid>";
    }

    const std::string& path = a[1];
    const std::string& gid  = a[2];

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return "ERR no such file";
    }
    if (st.st_size == 0) {
        return "ERR empty file";
    }

    uint64_t size = st.st_size;
    int nchunks = (size + CHUNK - 1) / CHUNK;
    std::string name = base_name(path);


    // I ask for the owner first (on the persistent socket, from this thread),
    // then the transfer thread does the slow hashing and sends the DP line.
    std::string owner, backup;
    std::string r = route("upload_file " + gid + " " + name, owner, backup);
    if (r.empty()) {
        return "ERR no reply from tracker";
    }
    if (r != "OK") {
        return r;
    }

    std::thread t(upload_thread, my_uid, path, gid, name, size, nchunks, owner, backup);
    t.detach();

    return "OK hashing " + name + " (" + std::to_string(nchunks) + " chunks)";
}


// ---------------------------------------------------------------------------
// download_file <gid> <name> <dest_dir>
// ---------------------------------------------------------------------------

std::string cmd_download(const std::vector<std::string>& a) {
    if (a.size() != 4) {
        return "ERR usage: download_file <gid> <name> <dest_dir>";
    }

    const std::string& gid  = a[1];
    const std::string& name = a[2];
    const std::string& dir  = a[3];

    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return "ERR no such directory";
    }

    // One download of the same file at a time.
    {
        std::lock_guard<std::mutex> lk(dl_mtx);

        auto it = downloads.find(share_key(gid, name));
        if (it != downloads.end() && it->second->status == 'D') {
            return "ERR already downloading";
        }
    }

    std::string owner, backup;
    std::string r = route("download_file " + gid + " " + name, owner, backup);
    if (r.empty()) {
        return "ERR no reply from tracker";
    }
    if (r != "OK") {
        return r;
    }

    start_download(my_uid, gid, name, dir + "/" + name, owner, backup);
    return "OK download started";
}


// ---------------------------------------------------------------------------
// stop_share <gid> <name>
// ---------------------------------------------------------------------------

std::string cmd_stop_share(const std::vector<std::string>& a) {
    if (a.size() != 3) {
        return "ERR usage: stop_share <gid> <name>";
    }

    const std::string& gid  = a[1];
    const std::string& name = a[2];

    std::string owner, backup;
    std::string r = route("stop_share " + gid + " " + name, owner, backup);
    if (r.empty()) {
        return "ERR no reply from tracker";
    }
    if (r != "OK") {
        return r;
    }

    std::string dp = "DP " + my_uid + " stop_share " + gid + " " + name + " " + my_addr;
    std::string reply = owner_request(owner, backup, dp);

    // Only forget the file locally once the owner has dropped me as a seeder.
    if (reply == "OK") {
        remove_share(gid, name);
    }

    return reply;
}
