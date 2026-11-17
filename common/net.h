#pragma once

#include <cstddef>
#include <string>

int  make_server(int port);                         // socket, SO_REUSEADDR, bind INADDR_ANY, listen(128); -1 on error
int  connect_to(const std::string& ip, int port, int timeout_sec = 0);  // -1 on error; timeout applies to connect+recv
int  connect_addr(const std::string& ip_port, int timeout_sec = 0);     // "ip:port"
void set_recv_timeout(int sock, int sec);
bool send_all(int sock, const char* buf, size_t n); // loops until n bytes sent
bool recv_all(int sock, char* buf, size_t n);       // loops until n bytes read; false on 0/error
bool send_msg(int sock, const std::string& msg);    // uint32 length (htonl) + body
bool recv_msg(int sock, std::string& msg);          // reverse; reject length > 64 MB
std::string request(const std::string& ip_port, const std::string& msg, int timeout_sec); // connect, send, recv one reply, close; "" on failure
