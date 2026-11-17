#include "server.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../common/config.h"
#include "../common/net.h"
#include "../common/util.h"
#include "shares.h"
#include "state.h"


// ---------------------------------------------------------------------------
// Demo hook
// ---------------------------------------------------------------------------

// If P2P_CORRUPT_CHUNK=3 is set, I flip one byte of chunk 3 every time I send it.
// This is only for showing that the downloader catches a bad chunk with SHA1.
static int corrupt_chunk() {
    const char* v = getenv("P2P_CORRUPT_CHUNK");

    if (v == nullptr) {
        return -1;
    }

    return atoi(v);
}


// If P2P_SERVE_DELAY_MS=200 is set, I wait that long before sending each chunk.
// On localhost a 50 MB download takes a fraction of a second; this slows it down
// enough to kill a client "at 50%" in the tests.
static int serve_delay_ms() {
    const char* v = getenv("P2P_SERVE_DELAY_MS");

    if (v == nullptr) {
        return 0;
    }

    return atoi(v);
}


// ---------------------------------------------------------------------------
// Answering one GET
// ---------------------------------------------------------------------------

// A length of 0 means "I don't have that chunk".
static void send_nothing(int sock) {
    uint32_t zero = 0;
    send_all(sock, (const char*)&zero, 4);
}


// Looks up the file and its bitmap. If I have chunk i, I give back the path
// and how many bytes the chunk has (the last chunk may be shorter).
static bool find_chunk(const std::string& gid, const std::string& name, int i,
                       std::string& path, size_t& len) {
    std::lock_guard<std::mutex> lk(shares_mtx);

    auto it = shares.find(share_key(gid, name));
    if (it == shares.end()) {
        return false;
    }

    const shared_file& f = *it->second;

    if (i < 0 || i >= f.nchunks || !f.have[i]) {
        return false;
    }

    path = f.path;

    uint64_t start = (uint64_t)i * CHUNK;
    uint64_t left  = f.size - start;

    if (left < CHUNK) {
        len = left;
    } else {
        len = CHUNK;
    }

    return true;
}


// I open the file here in the serve thread and close it again,
// so no file descriptor is ever shared between threads.
static bool read_chunk(const std::string& path, int i, std::vector<char>& buf) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    off_t start = (off_t)i * CHUNK;
    size_t got = 0;

    // pread may return fewer bytes than asked, so I loop.
    while (got < buf.size()) {
        ssize_t r = pread(fd, buf.data() + got, buf.size() - got, start + got);
        if (r <= 0) {
            close(fd);
            return false;
        }
        got += r;
    }

    close(fd);
    return true;
}


static void serve_one(int sock) {
    set_recv_timeout(sock, RECV_TIMEOUT);

    std::string msg;
    if (!recv_msg(sock, msg)) {
        return;
    }

    // GET <gid> <name> <i>
    std::vector<std::string> a = split(msg);
    if (a.size() != 4 || a[0] != "GET") {
        send_nothing(sock);
        return;
    }

    int i = -1;
    try {
        i = std::stoi(a[3]);
    } catch (...) {
        send_nothing(sock);
        return;
    }

    std::string path;
    size_t len = 0;

    if (!find_chunk(a[1], a[2], i, path, len)) {
        send_nothing(sock);
        return;
    }

    std::vector<char> buf(len);
    if (!read_chunk(path, i, buf)) {
        send_nothing(sock);
        return;
    }

    if (i == corrupt_chunk()) {
        buf[0] ^= 1;
    }

    if (serve_delay_ms() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(serve_delay_ms()));
    }

    uint32_t net_len = htonl((uint32_t)len);

    if (!send_all(sock, (const char*)&net_len, 4)) {
        return;
    }
    send_all(sock, buf.data(), len);
}


// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

// Each serve thread sleeps until the listener hands it a socket.
static void serve_loop() {
    while (true) {
        int sock;

        {
            std::unique_lock<std::mutex> lk(serve_mtx);

            while (serve_q.empty()) {
                serve_cv.wait(lk);
            }

            sock = serve_q.front();
            serve_q.pop();
        }

        serve_one(sock);
        close(sock);
    }
}


// The listener only accepts and queues. The serve threads do the slow work.
static void listen_loop(int server) {
    while (true) {
        int sock = accept(server, nullptr, nullptr);
        if (sock < 0) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(serve_mtx);
            serve_q.push(sock);
        }

        serve_cv.notify_one();
    }
}


bool start_server(int port) {
    int server = make_server(port);
    if (server < 0) {
        return false;
    }

    std::thread listener(listen_loop, server);
    listener.detach();

    for (int k = 0; k < SERVE_THREADS; k++) {
        std::thread t(serve_loop);
        t.detach();
    }

    return true;
}
