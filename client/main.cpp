#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

#include "../common/net.h"
#include "../common/util.h"
#include "commands.h"
#include "download.h"
#include "server.h"
#include "state.h"


// "127.0.0.1:9001" -> 9001, or -1 if it doesn't look like ip:port.
static int port_of(const std::string& addr) {
    size_t colon = addr.rfind(':');
    if (colon == std::string::npos) {
        return -1;
    }

    try {
        return std::stoi(addr.substr(colon + 1));
    } catch (...) {
        return -1;
    }
}


// I look at the first word and pick the right command function.
static std::string run_command(const std::string& line, const std::vector<std::string>& a) {
    if (a[0] == "upload_file")    return cmd_upload(a);
    if (a[0] == "download_file")  return cmd_download(a);
    if (a[0] == "stop_share")     return cmd_stop_share(a);
    if (a[0] == "show_downloads") return show_downloads();

    return cmd_plain(line, a);
}


int main(int argc, char* argv[]) {
    // An optional "--tracker <idx>" picks which tracker in the config I start with.
    bool bad_args = (argc != 3 && argc != 5);
    if (argc == 5 && std::string(argv[3]) != "--tracker") {
        bad_args = true;
    }

    if (bad_args) {
        fprintf(stderr, "usage: ./bin/client <my_ip>:<my_port> tracker_info.txt [--tracker <idx>]\n");
        return 1;
    }

    my_addr = argv[1];

    int my_port = port_of(my_addr);
    if (my_port <= 0) {
        fprintf(stderr, "[client] bad address %s\n", argv[1]);
        return 1;
    }

    tracker_list = read_config(argv[2]);
    if (tracker_list.empty()) {
        fprintf(stderr, "[client] no trackers in %s\n", argv[2]);
        return 1;
    }


    // Other clients may ask me for chunks as soon as I log in, so the serve side starts first.
    if (!start_server(my_port)) {
        fprintf(stderr, "[client] cannot listen on %d\n", my_port);
        return 1;
    }


    tracker_idx = 0;
    if (argc == 5) {
        tracker_idx = atoi(argv[4]);
    }

    if (tracker_idx < 0 || tracker_idx >= (int)tracker_list.size()) {
        fprintf(stderr, "[client] no tracker with index %d\n", tracker_idx);
        return 1;
    }

    const tracker_addr& t = tracker_list[tracker_idx];

    tracker_sock = connect_to(t.ip, t.client_port);
    if (tracker_sock < 0) {
        fprintf(stderr, "[client] connect to tracker %d failed\n", t.id);
        return 1;
    }


    // I only print the "> " prompt when a person is typing, not when input is piped in.
    bool tty = isatty(0);
    std::string line;

    while (true) {
        if (tty) {
            printf("> ");
            fflush(stdout);
        }

        if (!std::getline(std::cin, line)) {
            break;
        }

        std::vector<std::string> a = split(line);
        if (a.empty()) {
            continue;
        }
        if (a[0] == "quit") {
            break;
        }

        // If my tracker died, the command functions have already moved me to the next one.
        std::string reply = run_command(line, a);

        printf("%s\n", reply.c_str());
        fflush(stdout);
    }

    // Transfer and serve threads are detached and still running, so I leave
    // with _exit instead of letting global destructors run under them.
    fflush(stdout);
    close(tracker_sock);
    _exit(0);
}
