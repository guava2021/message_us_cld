#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <string>

namespace tcp {

class TcpClient {
   public:
    TcpClient(const std::string &host, uint16_t port);
    ~TcpClient();

    TcpClient(const TcpClient &)            = delete;
    TcpClient &operator=(const TcpClient &) = delete;

    bool send(const Request &req) { return send_request(fd_, req); }
    bool recv(Response &out) { return recv_response(fd_, out); }
    bool connected() const { return fd_ >= 0; }

   private:
    int fd_{-1};
};

}  // namespace tcp
