// Minimal non-blocking IPv4 UDP socket (Windows + POSIX).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using sock_t = SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#endif

namespace net {

inline bool same(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

class Udp {
 public:
  // port 0 = any free port; loopback_only binds 127.0.0.1 so nothing outside this PC can reach the socket.
  explicit Udp(uint16_t port = 0, bool loopback_only = false) {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
    s_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY); a.sin_port = htons(port);
    bound_ = ::bind(s_, (sockaddr*)&a, sizeof a) == 0;
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(s_, FIONBIO, &nb);
#else
    fcntl(s_, F_SETFL, fcntl(s_, F_GETFL, 0) | O_NONBLOCK);
#endif
  }
  ~Udp() {
#ifdef _WIN32
    closesocket(s_);
#else
    ::close(s_);
#endif
  }
  Udp(const Udp&) = delete;
  Udp& operator=(const Udp&) = delete;

  bool bound() const { return bound_; }
  bool resolve(const std::string& host, int port, sockaddr_in& out) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return false;
    out = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    return true;
  }
  void send(const sockaddr_in& to, const void* d, size_t n) {
    ::sendto(s_, (const char*)d, (int)n, 0, (const sockaddr*)&to, sizeof to);
  }
  // Returns the datagram length, or -1 when nothing is waiting.
  int recv(void* buf, size_t cap, sockaddr_in& from) {
#ifdef _WIN32
    int len = sizeof from;
#else
    socklen_t len = sizeof from;
#endif
    return (int)::recvfrom(s_, (char*)buf, (int)cap, 0, (sockaddr*)&from, &len);
  }

 private:
  sock_t s_;
  bool bound_ = false;
};

}  // namespace net
