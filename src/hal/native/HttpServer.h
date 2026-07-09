// HttpServer.h — tiny blocking HTTP/1.1 server for the emulator (native only).
// No external deps; POSIX sockets. One request at a time is plenty for a
// single-user dev emulator. Not for production use.
#pragma once
#include <string>
#include <functional>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace mmi {

struct HttpRequest { std::string method, path, body; };
struct HttpResponse { std::string contentType = "text/plain"; std::string body; int status = 200; };

class HttpServer {
public:
  explicit HttpServer(int port) : port_(port) {}

  using Handler = std::function<HttpResponse(const HttpRequest&)>;
  void onRequest(Handler h) { handler_ = std::move(h); }

  bool start() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return false;
    if (::listen(fd_, 8) < 0) return false;
    return true;
  }

  // Blocks forever, serving requests via the handler.
  void run() {
    while (true) {
      int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) continue;
      serveOne(c);
      ::close(c);
    }
  }

private:
  void serveOne(int c) {
    std::string req;
    char buf[4096];
    ssize_t n = ::recv(c, buf, sizeof(buf), 0);
    if (n <= 0) return;
    req.assign(buf, static_cast<size_t>(n));

    HttpRequest r;
    size_t sp1 = req.find(' ');
    size_t sp2 = req.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    r.method = req.substr(0, sp1);
    r.path   = req.substr(sp1 + 1, sp2 - sp1 - 1);

    // Read remaining body per Content-Length if not fully received.
    size_t hdrEnd = req.find("\r\n\r\n");
    size_t contentLen = 0;
    size_t clPos = req.find("Content-Length:");
    if (clPos != std::string::npos) contentLen = static_cast<size_t>(std::atoi(req.c_str() + clPos + 15));
    if (hdrEnd != std::string::npos) {
      r.body = req.substr(hdrEnd + 4);
      while (r.body.size() < contentLen) {
        n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        r.body.append(buf, static_cast<size_t>(n));
      }
    }

    HttpResponse resp = handler_ ? handler_(r) : HttpResponse{};
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " OK\r\n";
    out += "Content-Type: " + resp.contentType + "\r\n";
    out += "Access-Control-Allow-Origin: *\r\n";
    out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += resp.body;
    ::send(c, out.data(), out.size(), 0);
  }

  int port_;
  int fd_ = -1;
  Handler handler_;
};

} // namespace mmi
