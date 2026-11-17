#include "sync.h"

#include <cstdio>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "../common/config.h"
#include "../common/net.h"
#include "../common/util.h"
#include "control.h"
#include "data.h"
#include "heartbeat.h"
#include "state.h"


// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

// "ip:sync_port" of a tracker id.
static std::string sync_addr(int id) {
    for (const tracker_addr& t : trackers) {
        if (t.id == id) {
            return t.ip + ":" + std::to_string(t.sync_port);
        }
    }

    return "";
}


std::string send_to(int id, const std::string& msg, int timeout_sec) {
    std::string addr = sync_addr(id);
    if (addr.empty()) {
        return "";
    }

    return request(addr, msg, timeout_sec);
}


std::vector<std::string> send_to_all(const std::string& msg) {
    std::vector<std::string> replies(N);

    // A dead tracker costs at most SYNC_TIMEOUT, then I move on to the next one.
    for (int id = 0; id < N; id++) {
        if (id == my_id) {
            continue;
        }

        replies[id] = send_to(id, msg);
    }

    return replies;
}


// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

// BCAST <origin> <seq> <OP...>
static std::string on_bcast(const std::vector<std::string>& a) {
    if (a.size() < 4) {
        return "ERR bad BCAST";
    }

    int origin;
    uint64_t seq;

    try {
        origin = std::stoi(a[1]);
        seq    = std::stoull(a[2]);
    } catch (...) {
        return "ERR bad BCAST";
    }

    if (origin < 0 || origin >= N) {
        return "ERR bad BCAST";
    }

    std::vector<std::string> op(a.begin() + 3, a.end());
    return apply_op(origin, seq, op, true);
}


// PEER_UP <peer> <uid>: a client logged in somewhere.
static std::string on_peer_up(const std::vector<std::string>& a) {
    if (a.size() != 3) {
        return "ERR bad PEER_UP";
    }

    std::lock_guard<std::mutex> lk(peers_mtx);
    online.insert(a[1]);
    peer_uid[a[1]] = a[2];

    return "ACK";
}


// PEER_DOWN <peer>: a client logged out or disconnected. Its seeder entries stay.
static std::string on_peer_down(const std::vector<std::string>& a) {
    if (a.size() != 2) {
        return "ERR bad PEER_DOWN";
    }

    std::lock_guard<std::mutex> lk(peers_mtx);
    online.erase(a[1]);

    return "ACK";
}


// I look at the first word of a tracker message and hand it to the right handler.
static std::string handle_sync(const std::string& msg) {
    std::vector<std::string> a = split(msg);

    if (a.empty()) {
        return "ERR empty";
    }

    if (a[0] == "BCAST") return on_bcast(a);

    // REPL <META|HAVE|STOP> ... from the owner of a file I back up.
    if (a[0] == "REPL") {
        std::vector<std::string> rest(a.begin() + 1, a.end());
        return apply_repl(rest);
    }

    if (a[0] == "PEER_UP")   return on_peer_up(a);
    if (a[0] == "PEER_DOWN") return on_peer_down(a);
    if (a[0] == "PING")          return on_ping(a);
    if (a[0] == "PING_REQ")      return on_ping_req(a);
    if (a[0] == "WHO_IS_MASTER") return on_who_is_master(a);
    if (a[0] == "HB")            return on_hb(a);
    if (a[0] == "SYNC")          return on_sync(a);
    if (a[0] == "PULL_DATA")     return on_pull_data(a);

    return "ERR unknown";
}


// One short-lived thread per incoming tracker message: read it, answer it, close.
static void sync_worker(int sock) {
    // A tracker that connects and then never sends shouldn't keep this thread forever.
    set_recv_timeout(sock, RECV_TIMEOUT);

    std::string msg;

    if (!recv_msg(sock, msg)) {
        close(sock);
        return;
    }

    std::string reply = handle_sync(msg);
    send_msg(sock, reply);
    close(sock);

    // A slave's HB may show it knows ops I don't. I fetch them only now,
    // after the HB_ACK is sent, so the slave isn't left waiting.
    if (msg.rfind("HB ", 0) == 0) {
        after_hb(split(msg));
    }
}


static void sync_listener(int server) {
    while (true) {
        int sock = accept(server, nullptr, nullptr);
        if (sock < 0) {
            continue;
        }

        std::thread t(sync_worker, sock);
        t.detach();
    }
}


bool start_sync_listener() {
    int port = -1;

    for (const tracker_addr& t : trackers) {
        if (t.id == my_id) {
            port = t.sync_port;
        }
    }

    int server = make_server(port);
    if (server < 0) {
        return false;
    }

    std::thread t(sync_listener, server);
    t.detach();

    fprintf(stderr, "[tracker %d] listening for trackers on %d\n", my_id, port);
    return true;
}
