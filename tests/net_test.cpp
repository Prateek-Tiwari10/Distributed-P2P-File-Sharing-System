#include "../common/net.h"
#include "../common/util.h"

#include <cstdio>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>


static const int TEST_PORT = 6999;


static void fail(const char* what) {
    fprintf(stderr, "net_test FAIL: %s\n", what);
    _exit(1);
}


// The server side of the test: accept one connection and echo back `count` messages.
static void echo_server(int server, size_t count) {
    int sock = accept(server, nullptr, nullptr);
    if (sock < 0) {
        fail("accept");
    }

    std::string m;

    for (size_t i = 0; i < count; i++) {
        if (!recv_msg(sock, m)) {
            fail("server recv_msg");
        }
        if (!send_msg(sock, m)) {
            fail("server send_msg");
        }
    }

    close(sock);
}


int main() {
    // I test three sizes: empty, small, and 5 MB (much bigger than one recv() call).
    std::vector<std::string> msgs;
    msgs.push_back("");
    msgs.push_back("0123456789");
    msgs.push_back(std::string(5 * 1024 * 1024, 'x'));

    // Fill the big one with a pattern, so a mixed-up byte order would be caught.
    for (size_t i = 0; i < msgs[2].size(); i++) {
        msgs[2][i] = (char)(i * 31 + 7);
    }


    int server = make_server(TEST_PORT);
    if (server < 0) {
        fail("make_server");
    }

    std::thread echo(echo_server, server, msgs.size());


    int sock = connect_to("127.0.0.1", TEST_PORT, 5);
    if (sock < 0) {
        fail("connect_to");
    }

    for (const std::string& m : msgs) {
        std::string back;

        if (!send_msg(sock, m)) {
            fail("send_msg");
        }
        if (!recv_msg(sock, back)) {
            fail("recv_msg");
        }
        if (back != m) {
            fail("echo mismatch");
        }
    }

    close(sock);
    echo.join();
    close(server);


    // Known answer for SHA1("abc").
    if (sha1_hex("abc", 3) != "a9993e364706816aba3e25717850c26c9cd0d89d") {
        fail("sha1_hex");
    }
    printf("sha1_hex(\"abc\") = %s\n", sha1_hex("abc", 3).c_str());


    std::vector<tracker_addr> cfg = read_config("tracker_info.txt");
    if (cfg.size() != 3) {
        fail("read_config");
    }
    printf("read_config: %zu trackers\n", cfg.size());


    printf("net_test OK\n");
    return 0;
}
