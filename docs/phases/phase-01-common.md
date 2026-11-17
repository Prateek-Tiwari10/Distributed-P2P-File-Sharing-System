# Phase 1 — Common networking and utilities

## Goal
Shared helpers used by both binaries: framed TCP messages, socket setup, string utils, SHA1, config parsing, and the Makefile.

## Read first
`docs/DESIGN.md` sections 1 and 4 (framing only).

## Create
- `common/config.h` — all constants from DESIGN.md §1.
- `common/net.h`, `common/net.cpp`
- `common/util.h`, `common/util.cpp`
- `Makefile` — targets `all`, `tracker`, `client`, `clean`. For now `tracker` and `client` may be stub `main()`s that print usage.
- `tracker/main.cpp`, `client/main.cpp` — stubs only.
- `tests/net_test.cpp` + `make test` target.
- `tracker_info.txt` — the 3-tracker example from DESIGN.md.
- `.gitignore` — binaries, `*.o`, `*.log`, `*.progress`, `*.shares`.

## Functions to implement

`net.h`
```cpp
int  make_server(int port);                         // socket, SO_REUSEADDR, bind INADDR_ANY, listen(128); -1 on error
int  connect_to(const std::string& ip, int port, int timeout_sec = 0);  // -1 on error; timeout applies to connect+recv
int  connect_addr(const std::string& ip_port, int timeout_sec = 0);     // "ip:port"
void set_recv_timeout(int sock, int sec);
bool send_all(int sock, const char* buf, size_t n); // loops until n bytes sent
bool recv_all(int sock, char* buf, size_t n);       // loops until n bytes read; false on 0/error
bool send_msg(int sock, const std::string& msg);    // uint32 length (htonl) + body
bool recv_msg(int sock, std::string& msg);          // reverse; reject length > 64 MB
std::string request(const std::string& ip_port, const std::string& msg, int timeout_sec); // connect, send, recv one reply, close; "" on failure
```
Use `MSG_NOSIGNAL` on `send` (or ignore `SIGPIPE` in both mains).

`util.h`
```cpp
std::vector<std::string> split(const std::string& s, char sep = ' ');  // skips empty parts
std::string join(const std::vector<std::string>& v, char sep);
std::string sha1_hex(const char* data, size_t n);                    // 40 lowercase hex chars
std::string sha1_file(const std::string& path);                      // streaming, whole file
struct tracker_addr { int id; std::string ip; int client_port, sync_port; };
std::vector<tracker_addr> read_config(const std::string& path);
long long now_ms();
```

## Do not
- Do not write tracker or client logic yet.

## Done when
1. `make` builds `tracker`, `client` with no warnings.
2. `make test` runs `tests/net_test.cpp` which starts a server thread, sends 3 messages (empty, 10 bytes, 5 MB) with `send_msg`, receives them with `recv_msg` and checks equality; prints `net_test OK`.
3. `sha1_hex("abc", 3)` prints `a9993e364706816aba3e25717850c26c9cd0d89d` (checked in the test).
4. `read_config("tracker_info.txt")` returns 3 entries (checked in the test).
