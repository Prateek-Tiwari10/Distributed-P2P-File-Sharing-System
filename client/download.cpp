#include "download.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../common/config.h"
#include "../common/net.h"
#include "../common/util.h"
#include "commands.h"
#include "shares.h"
#include "state.h"


// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// P2P_DEBUG=1 turns on the per-chunk log lines.
static bool debug_on() {
    return getenv("P2P_DEBUG") != nullptr;
}


// A random number in [0, n). Each thread has its own generator, so no lock is needed.
static size_t random_index(size_t n) {
    thread_local std::mt19937 gen(std::random_device{}());

    std::uniform_int_distribution<size_t> pick(0, n - 1);
    return pick(gen);
}


// How many bytes chunk i has. Only the last chunk can be shorter than CHUNK.
static size_t chunk_len(const download_job& job, int i) {
    uint64_t start = (uint64_t)i * CHUNK;
    uint64_t left  = job.size - start;

    if (left < CHUNK) {
        return left;
    }

    return CHUNK;
}


// ---------------------------------------------------------------------------
// Reading the owner's reply
// ---------------------------------------------------------------------------

// "OK <size> <nchunks> <file_hash> <h0,h1,...>" and then "<i> <count> <p1,p2,...>" per chunk.
static bool parse_metadata(const std::string& reply, download_job& job) {
    std::vector<std::string> lines = split(reply, '\n');
    if (lines.empty()) {
        return false;
    }

    std::vector<std::string> head = split(lines[0]);
    if (head.size() != 5 || head[0] != "OK") {
        return false;
    }

    try {
        job.size    = std::stoull(head[1]);
        job.nchunks = std::stoi(head[2]);
    } catch (...) {
        return false;
    }

    job.file_hash  = head[3];
    job.chunk_hash = split(head[4], ',');

    if (job.nchunks <= 0 || (int)job.chunk_hash.size() != job.nchunks) {
        return false;
    }

    job.count.assign(job.nchunks, 0);
    job.peers.assign(job.nchunks, std::vector<std::string>());
    job.tries.assign(job.nchunks, 0);

    for (size_t k = 1; k < lines.size(); k++) {
        std::vector<std::string> parts = split(lines[k]);
        if (parts.size() != 3) {
            continue;
        }

        int i, count;
        try {
            i     = std::stoi(parts[0]);
            count = std::stoi(parts[1]);
        } catch (...) {
            continue;
        }

        if (i < 0 || i >= job.nchunks) {
            continue;
        }

        job.count[i] = count;

        if (parts[2] != "-") {
            job.peers[i] = split(parts[2], ',');
        }
    }

    return true;
}


// ---------------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------------

// Fetches chunk i from one random peer, checks its SHA1 and writes it into place.
// Any problem (no peer, connect, timeout, bad length, bad hash, write) returns false.
static bool fetch_chunk(download_job& job, int i) {
    // I never ask myself for a chunk.
    std::vector<std::string> choices;
    for (const std::string& p : job.peers[i]) {
        if (p != my_addr) {
            choices.push_back(p);
        }
    }

    if (choices.empty()) {
        return false;
    }

    std::string peer = choices[random_index(choices.size())];

    if (debug_on()) {
        fprintf(stderr, "[download] %s chunk %d from %s\n", job.name.c_str(), i, peer.c_str());
    }


    // The RECV_TIMEOUT on this socket is what stops me hanging on a dead or stuck peer.
    int sock = connect_addr(peer, RECV_TIMEOUT);
    if (sock < 0) {
        return false;
    }

    bool ok = send_msg(sock, "GET " + job.gid + " " + job.name + " " + std::to_string(i));

    uint32_t len = 0;
    if (ok) {
        ok = recv_all(sock, (char*)&len, 4);
        len = ntohl(len);
    }

    // len 0 means the peer doesn't have it. Anything but the exact chunk size is wrong too.
    if (ok && len != chunk_len(job, i)) {
        ok = false;
    }

    std::vector<char> buf;
    if (ok) {
        buf.resize(len);
        ok = recv_all(sock, buf.data(), len);
    }

    close(sock);

    if (!ok) {
        return false;
    }


    if (sha1_hex(buf.data(), len) != job.chunk_hash[i]) {
        fprintf(stderr, "[download] %s chunk %d from %s: bad SHA1\n", job.name.c_str(), i, peer.c_str());
        return false;
    }

    // pwrite with an offset is safe from many threads on the same fd.
    off_t start = (off_t)i * CHUNK;
    size_t put = 0;

    while (put < len) {
        ssize_t r = pwrite(job.fd, buf.data() + put, len - put, start + put);
        if (r <= 0) {
            return false;
        }
        put += r;
    }

    return true;
}


// A worker keeps taking chunk indexes until the queue is empty.
// A failed chunk goes back on the queue until it has had MAX_TRIES attempts.
static void worker_loop(download_job* job) {
    while (true) {
        int i;
        {
            std::lock_guard<std::mutex> lk(job->q_mtx);

            if (job->todo.empty()) {
                return;
            }
            i = job->todo.front();
            job->todo.pop();
        }

        if (fetch_chunk(*job, i)) {
            // Only now is the chunk on disk and checked, so serve threads may hand it out.
            set_have(job->gid, job->name, i);
            job->done++;

            // The transfer thread tells the tracker about these every 2 s.
            std::lock_guard<std::mutex> lk(job->have_mtx);
            job->have_new.push_back(i);
            continue;
        }

        std::lock_guard<std::mutex> lk(job->q_mtx);

        job->tries[i]++;
        if (job->tries[i] < MAX_TRIES) {
            job->todo.push(i);
        } else {
            fprintf(stderr, "[download] %s chunk %d failed %d times, giving up\n",
                    job->name.c_str(), i, MAX_TRIES);
        }
    }
}


// I wrap the loop so the transfer thread can see how many workers are still running.
static void worker(download_job* job, std::atomic<int>* running) {
    worker_loop(job);
    (*running)--;
}


// ---------------------------------------------------------------------------
// Progress: the .progress file and HAVE updates
// ---------------------------------------------------------------------------

static std::string progress_path(const download_job& job) {
    return job.dest + ".progress";
}


// If <dest>.progress is there and has the right length, I copy it into the bitmap
// and return how many chunks it says I already have. Otherwise 0.
static int load_progress(const download_job& job, shared_file& f) {
    std::ifstream in(progress_path(job));
    std::string bits;

    if (!std::getline(in, bits) || (int)bits.size() != job.nchunks) {
        return 0;
    }

    int have = 0;

    for (int i = 0; i < job.nchunks; i++) {
        if (bits[i] == '1') {
            f.have[i] = true;
            have++;
        }
    }

    return have;
}


// Rewrites <dest>.progress as one '0'/'1' per chunk.
static void save_progress(const download_job& job) {
    std::string bits;
    {
        std::lock_guard<std::mutex> lk(shares_mtx);

        auto it = shares.find(share_key(job.gid, job.name));
        if (it == shares.end()) {
            return;
        }

        for (bool b : it->second->have) {
            bits += b ? '1' : '0';
        }
    }

    std::ofstream out(progress_path(job), std::ios::trunc);
    out << bits << "\n";
}


// Tells the owner (or the backup) about the chunks I got since the last update,
// so other downloaders can fetch them from me. The network part runs with no lock held.
static void send_have(download_job& job, const std::string& uid) {
    std::vector<int> fresh;
    {
        std::lock_guard<std::mutex> lk(job.have_mtx);
        fresh.swap(job.have_new);
    }

    if (fresh.empty()) {
        return;
    }

    std::vector<std::string> parts;
    for (int i : fresh) {
        parts.push_back(std::to_string(i));
    }

    std::string dp = "DP " + uid + " have " + job.gid + " " + job.name + " " + my_addr + " " + join(parts, ',');
    std::string reply = owner_request(job.owner_addr, job.backup_addr, dp);

    if (reply != "OK") {
        fprintf(stderr, "[download] %s have update: %s\n", job.name.c_str(), reply.c_str());
    }
}


// ---------------------------------------------------------------------------
// The transfer thread
// ---------------------------------------------------------------------------

static void finish(std::shared_ptr<download_job> job, char status) {
    job->status = status;

    printf("[download] %s %s: %s (%d/%d)\n", job->gid.c_str(), job->name.c_str(),
           status == 'C' ? "complete" : "failed", job->done.load(), job->nchunks);
    fflush(stdout);
}


// Puts the chunks I'm missing into the queue, rarest first (fewest online seeders).
// I sort (count, index) pairs: that orders by count, and chunks with the same count
// stay in index order, exactly like a stable_sort on count.
static int queue_missing(download_job& job, const shared_file& f) {
    std::vector<std::pair<int, int>> by_count;
    for (int i = 0; i < job.nchunks; i++) {
        if (!f.have[i]) {
            by_count.push_back({job.count[i], i});
        }
    }

    std::sort(by_count.begin(), by_count.end());

    std::vector<int> missing;
    for (auto& p : by_count) {
        missing.push_back(p.second);
    }

    if (debug_on() && !missing.empty()) {
        std::string order;
        for (size_t k = 0; k < missing.size() && k < 10; k++) {
            int i = missing[k];
            order += " " + std::to_string(i) + "(count " + std::to_string(job.count[i]) + ")";
        }
        fprintf(stderr, "[download] %s fetch order starts with:%s\n", job.name.c_str(), order.c_str());
    }

    for (int i : missing) {
        job.todo.push(i);
    }

    return missing.size();
}


static void download_thread(std::shared_ptr<download_job> job, std::string uid) {
    std::string dp = "DP " + uid + " download_file " + job->gid + " " + job->name;
    std::string reply = owner_request(job->owner_addr, job->backup_addr, dp);

    if (reply.rfind("OK ", 0) != 0) {
        printf("[download] %s %s: %s\n", job->gid.c_str(), job->name.c_str(), reply.c_str());
        fflush(stdout);
        job->status = 'F';
        return;
    }

    if (!parse_metadata(reply, *job)) {
        printf("[download] %s %s: ERR bad metadata from tracker\n", job->gid.c_str(), job->name.c_str());
        fflush(stdout);
        job->status = 'F';
        return;
    }

    if (debug_on()) {
        for (int i = 0; i < job->nchunks; i++) {
            fprintf(stderr, "[download] %s chunk %d: count %d, peers %s\n", job->name.c_str(), i,
                    job->count[i], job->peers[i].empty() ? "-" : join(job->peers[i], ',').c_str());
        }
    }


    // The file gets its full size right away, so every worker can pwrite at its own offset.
    // ftruncate to the same size keeps what an earlier, interrupted run already wrote.
    job->fd = open(job->dest.c_str(), O_RDWR | O_CREAT, 0644);
    if (job->fd < 0 || ftruncate(job->fd, job->size) != 0) {
        printf("[download] %s %s: ERR cannot open %s\n", job->gid.c_str(), job->name.c_str(), job->dest.c_str());
        fflush(stdout);
        job->status = 'F';
        return;
    }


    // I register the file as shared, with only the chunks from an earlier run (if any).
    auto f = std::make_shared<shared_file>();
    f->gid     = job->gid;
    f->name    = job->name;
    f->path    = job->dest;
    f->size    = job->size;
    f->nchunks = job->nchunks;
    f->have.assign(job->nchunks, false);

    int already = load_progress(*job, *f);
    job->done = already;

    // Chunks from an earlier run go in the first HAVE update too, in case the
    // last update before the crash never reached the tracker.
    for (int i = 0; i < job->nchunks; i++) {
        if (f->have[i]) {
            job->have_new.push_back(i);
        }
    }

    put_share(f);

    int missing = queue_missing(*job, *f);
    if (already > 0) {
        printf("[download] resume: %d missing of %d\n", missing, job->nchunks);
        fflush(stdout);
    }


    std::atomic<int> running{WORKERS};

    std::vector<std::thread> workers;
    for (int k = 0; k < WORKERS; k++) {
        workers.push_back(std::thread(worker, job.get(), &running));
    }

    // While the workers run, I wake up every HB_INTERVAL seconds (checking often so I
    // notice when they finish) to send HAVE updates and save the progress file.
    long long last_update = now_ms();

    while (running > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (now_ms() - last_update >= HB_INTERVAL * 1000) {
            send_have(*job, uid);
            save_progress(*job);
            last_update = now_ms();
        }
    }

    for (std::thread& t : workers) {
        t.join();
    }

    close(job->fd);
    job->fd = -1;

    send_have(*job, uid);
    save_progress(*job);


    // Every chunk passed its own SHA1, but I still check the whole file once at the end.
    // On a failure I keep the .progress file, so the next download_file resumes.
    if (job->done == job->nchunks && sha1_file(job->dest) == job->file_hash) {
        unlink(progress_path(*job).c_str());
        save_share(*f);
        finish(job, 'C');
    } else {
        finish(job, 'F');
    }
}


void start_download(const std::string& uid, const std::string& gid, const std::string& name,
                    const std::string& dest, const std::string& owner, const std::string& backup) {
    auto job = std::make_shared<download_job>();
    job->gid         = gid;
    job->name        = name;
    job->dest        = dest;
    job->size        = 0;
    job->nchunks     = 0;
    job->owner_addr  = owner;
    job->backup_addr = backup;

    {
        std::lock_guard<std::mutex> lk(dl_mtx);
        downloads[share_key(gid, name)] = job;
    }

    std::thread t(download_thread, job, uid);
    t.detach();
}


std::string show_downloads() {
    std::lock_guard<std::mutex> lk(dl_mtx);

    if (downloads.empty()) {
        return "no downloads";
    }

    std::string out;

    for (auto& kv : downloads) {
        const download_job& job = *kv.second;

        if (!out.empty()) {
            out += "\n";
        }

        out += "[";
        out += job.status.load();
        out += "] " + job.gid + " " + job.name + " "
             + std::to_string(job.done.load()) + "/" + std::to_string(job.nchunks);
    }

    return out;
}
