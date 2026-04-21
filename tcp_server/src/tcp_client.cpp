#include "tcp_client.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tcp {

TcpClient::TcpClient(const std::string &host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *res = nullptr;
    int rc        = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0)
        throw std::runtime_error("getaddrinfo: " + std::string(gai_strerror(rc)));

    fd_ = socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC, res->ai_protocol);
    if (fd_ < 0) {
        freeaddrinfo(res);
        throw std::runtime_error("socket: " + std::string(strerror(errno)));
    }

    int nodelay = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    if (connect(fd_, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("connect: " + std::string(strerror(errno)));
    }
    freeaddrinfo(res);
}

TcpClient::~TcpClient() {
    if (fd_ >= 0)
        close(fd_);
}

}  // namespace tcp
