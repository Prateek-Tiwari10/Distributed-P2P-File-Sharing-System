#include "net.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>


// I refuse any message bigger than this, so a garbage length can't make me allocate gigabytes.
static const uint32_t MAX_MSG = 64u * 1024 * 1024;


// ---------------------------------------------------------------------------
// Setting up sockets
// ---------------------------------------------------------------------------

int make_server(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    // SO_REUSEADDR lets me restart a tracker/client right away on the same port.
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    if (listen(sock, 128) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}


void set_recv_timeout(int sock, int sec) {
    timeval tv{};
    tv.tv_sec = sec;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}


// Waits for a non-blocking connect to finish, for at most timeout_sec.
// Returns true only if the connection really succeeded.
static bool wait_for_connect(int sock, int timeout_sec) {
    pollfd pfd{};
    pfd.fd     = sock;
    pfd.events = POLLOUT;

    if (poll(&pfd, 1, timeout_sec * 1000) <= 0) {
        return false;   // timed out (or poll failed)
    }

    // The socket is "writable" both on success and on failure, so I ask which one it was.
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);

    return err == 0;
}


int connect_to(const std::string& ip, int port, int timeout_sec) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }


    // No timeout asked for: a normal blocking connect is fine.
    if (timeout_sec <= 0) {
        if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            return -1;
        }
        return sock;
    }


    // With a timeout, I connect in non-blocking mode and wait with poll.
    // That way a dead tracker/peer costs me at most timeout_sec,
    // not the kernel's very long default.
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, (sockaddr*)&addr, sizeof(addr));

    if (rc < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }

    if (rc < 0 && !wait_for_connect(sock, timeout_sec)) {
        close(sock);
        return -1;
    }

    // Back to blocking mode, and the same timeout now applies to every recv.
    fcntl(sock, F_SETFL, flags);
    set_recv_timeout(sock, timeout_sec);

    return sock;
}


// Same as connect_to, but takes "ip:port" in one string.
int connect_addr(const std::string& ip_port, int timeout_sec) {
    size_t colon = ip_port.rfind(':');
    if (colon == std::string::npos) {
        return -1;
    }

    std::string ip = ip_port.substr(0, colon);
    int port;

    try {
        port = std::stoi(ip_port.substr(colon + 1));
    } catch (...) {
        return -1;
    }

    return connect_to(ip, port, timeout_sec);
}


// ---------------------------------------------------------------------------
// Sending and receiving raw bytes
// ---------------------------------------------------------------------------

// send() may send only part of the buffer, so I keep going until it's all out.
// MSG_NOSIGNAL stops the process from dying with SIGPIPE if the other side is gone.
bool send_all(int sock, const char* buf, size_t n) {
    size_t sent = 0;

    while (sent < n) {
        ssize_t r = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);

        if (r < 0 && errno == EINTR) {
            continue;   // interrupted by a signal, just try again
        }
        if (r <= 0) {
            return false;
        }

        sent += r;
    }

    return true;
}


// Same idea as send_all: keep reading until I have exactly n bytes.
// 0 from recv means the other side closed the connection.
bool recv_all(int sock, char* buf, size_t n) {
    size_t got = 0;

    while (got < n) {
        ssize_t r = recv(sock, buf + got, n - got, 0);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false;
        }

        got += r;
    }

    return true;
}


// ---------------------------------------------------------------------------
// Framed messages: [4-byte length in network order][body]
// ---------------------------------------------------------------------------

bool send_msg(int sock, const std::string& msg) {
    uint32_t len = htonl((uint32_t)msg.size());

    if (!send_all(sock, (const char*)&len, 4)) {
        return false;
    }

    return send_all(sock, msg.data(), msg.size());
}


bool recv_msg(int sock, std::string& msg) {
    uint32_t len;

    if (!recv_all(sock, (char*)&len, 4)) {
        return false;
    }

    len = ntohl(len);
    if (len > MAX_MSG) {
        return false;
    }

    msg.assign(len, '\0');

    // An empty message is valid: there's simply nothing more to read.
    if (len == 0) {
        return true;
    }

    return recv_all(sock, &msg[0], len);
}


// One-shot: connect, send one message, read one reply, close. "" if anything fails.
std::string request(const std::string& ip_port, const std::string& msg, int timeout_sec) {
    int sock = connect_addr(ip_port, timeout_sec);
    if (sock < 0) {
        return "";
    }

    std::string reply;

    if (!send_msg(sock, msg) || !recv_msg(sock, reply)) {
        reply = "";
    }

    close(sock);
    return reply;
}
