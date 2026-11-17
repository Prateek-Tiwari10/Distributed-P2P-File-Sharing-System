#include <chrono>
#include <cstdio>
#include <string>
#include <sys/socket.h>
#include <thread>

#include "../common/net.h"
#include "../common/util.h"
#include "client_handler.h"
#include "heartbeat.h"
#include "oplog.h"
#include "state.h"
#include "sync.h"


// Every client connection gets its own thread, and I don't wait for it.
static void client_listener(int server) {
    while (true) {
        int sock = accept(server, nullptr, nullptr);
        if (sock < 0) {
            continue;
        }

        std::thread t(handle_client, sock);
        t.detach();
    }
}


int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: ./bin/tracker tracker_info.txt <my_id>\n");
        return 1;
    }

    trackers = read_config(argv[1]);
    N = trackers.size();

    try {
        my_id = std::stoi(argv[2]);
    } catch (...) {
        fprintf(stderr, "[tracker] bad id %s\n", argv[2]);
        return 1;
    }


    // I find my own row in the config to know which port to listen on.
    const tracker_addr* me = nullptr;
    for (const tracker_addr& t : trackers) {
        if (t.id == my_id) {
            me = &t;
        }
    }

    if (me == nullptr) {
        fprintf(stderr, "[tracker %d] id not found in %s\n", my_id, argv[1]);
        return 1;
    }


    // Only this thread exists so far, so I can fill these in without locks.
    // I start in RECOVERY with an empty tracker list: I never assume I'm the master.
    version.assign(N, 0);
    last_seen.assign(N, 0);
    my_state = RECOVERY;

    read_test_switches();


    // My state comes back from my own op log before anyone can talk to me.
    oplog_replay();
    oplog_open();

    if (!start_sync_listener()) {
        fprintf(stderr, "[tracker %d] cannot listen on %d\n", my_id, me->sync_port);
        return 1;
    }


    // I take clients before recovery, so they get the recovery-mode error instead of nothing.
    int server = make_server(me->client_port);
    if (server < 0) {
        fprintf(stderr, "[tracker %d] cannot listen on %d\n", my_id, me->client_port);
        return 1;
    }

    std::thread clients(client_listener, server);
    clients.detach();

    fprintf(stderr, "[tracker %d] listening for clients on %d\n", my_id, me->client_port);


    // Cold start or rejoin. This returns once I'm ACTIVE.
    startup();
    start_heartbeat();


    // The other threads do the work from here on.
    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(1));
    }
}
