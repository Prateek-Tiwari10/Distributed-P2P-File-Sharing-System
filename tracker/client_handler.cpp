#include "client_handler.h"

#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

#include "../common/net.h"
#include "../common/util.h"
#include "control.h"
#include "data.h"
#include "state.h"


// I look at the first word and hand the message to the right function.
static std::string dispatch(int sock, const std::vector<std::string>& a) {
    if (a.empty()) {
        return "ERR unknown command";
    }

    // While I'm rejoining the cluster my state may be stale, so I answer
    // every client command the same way, DP lines on temp connections included.
    if (my_state == RECOVERY) {
        return "ERROR: tracker in recovery mode, try again after some time";
    }

    const std::string& cmd = a[0];

    // These work without a login.
    if (cmd == "create_user") return create_user(a);
    if (cmd == "login")       return login(sock, a);

    // Hidden debug commands for the tests. They need no login.
    if (cmd == "debug_delay") return debug_delay(a);
    if (cmd == "debug_files") return debug_files(a);
    if (cmd == "debug_drop_next_bcast") return debug_drop_next_bcast(a);

    // DP lines can arrive on a redirected temp connection that never logged in.
    // The owner trusts the uid inside the message (a known limit in DESIGN.md §8).
    if (cmd == "DP") return handle_dp(a);

    // Everything below needs a logged-in session.
    std::string uid = session_uid(sock);
    if (uid.empty()) {
        return "ERR not logged in";
    }

    if (cmd == "logout")         return logout(sock);
    if (cmd == "create_group")   return create_group(uid, a);
    if (cmd == "join_group")     return join_group(uid, a);
    if (cmd == "leave_group")    return leave_group(uid, a);
    if (cmd == "list_requests")  return list_requests(uid, a);
    if (cmd == "accept_request") return accept_request(uid, a);
    if (cmd == "list_groups")    return list_groups(a);
    if (cmd == "list_files")     return list_files(uid, a);

    if (cmd == "upload_file" || cmd == "download_file" || cmd == "stop_share") {
        return route_file_cmd(uid, a);
    }

    return "ERR unknown command";
}


void cleanup(int sock) {
    end_session(sock);
    close(sock);

    fprintf(stderr, "[tracker %d] client %d disconnected, cleaned up\n", my_id, sock);
}


// One of these runs per connection: read a message, answer it, repeat until the client goes away.
void handle_client(int sock) {
    std::string msg;

    while (recv_msg(sock, msg)) {
        std::string reply = dispatch(sock, split(msg));

        if (!send_msg(sock, reply)) {
            break;
        }
    }

    cleanup(sock);
}
