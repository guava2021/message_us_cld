#include "tcp_server.hpp"

#include "protocol.hpp"
#include "thread_pool.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tcp {

static int make_server_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        throw std::runtime_error("socket: " + std::string(strerror(errno)));

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("bind: " + std::string(strerror(errno)));
    }
    if (listen(fd, 128) < 0) {
        close(fd);
        throw std::runtime_error("listen: " + std::string(strerror(errno)));
    }
    return fd;
}

TcpServer::TcpServer(uint16_t port, size_t num_workers, Handler handler, int recv_timeout_ms)
    : port_(port),
      recv_timeout_ms_(recv_timeout_ms),
      handler_(std::move(handler)),
      pool_(std::make_unique<ThreadPool>(num_workers)) {
    server_fd_ = make_server_socket(port);
    if (pipe(stop_pipe_) < 0) {
        close(server_fd_);
        throw std::runtime_error("pipe: " + std::string(strerror(errno)));
    }
}

TcpServer::~TcpServer() {
    if (server_fd_ >= 0)
        close(server_fd_);
    if (stop_pipe_[0] >= 0)
        close(stop_pipe_[0]);
    if (stop_pipe_[1] >= 0)
        close(stop_pipe_[1]);
}

void TcpServer::stop() {
    char b                      = 1;
    [[maybe_unused]] ssize_t rc = write(stop_pipe_[1], &b, 1);
}

void TcpServer::run() {
    accept_loop();
}

void TcpServer::accept_loop() {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        throw std::runtime_error("epoll_create1: " + std::string(strerror(errno)));

    auto add_fd = [epfd](int fd, uint32_t events) {
        epoll_event ev{};
        ev.events  = events;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    };

    add_fd(server_fd_, EPOLLIN);
    add_fd(stop_pipe_[0], EPOLLIN);

    epoll_event events[64];
    while (true) {
        int n = epoll_wait(epfd, events, 64, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == stop_pipe_[0]) {
                close(epfd);
                return;
            }
            if (events[i].data.fd == server_fd_) {
                sockaddr_in client_addr{};
                socklen_t len = sizeof(client_addr);
                int client_fd = accept4(server_fd_, reinterpret_cast<sockaddr *>(&client_addr),
                                        &len, SOCK_CLOEXEC);
                if (client_fd < 0)
                    continue;
                int nodelay = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                if (recv_timeout_ms_ > 0) {
                    timeval tv{};
                    tv.tv_sec  = recv_timeout_ms_ / 1000;
                    tv.tv_usec = static_cast<suseconds_t>((recv_timeout_ms_ % 1000) * 1000);
                    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                }
                pool_->submit([this, client_fd] { handle_connection(client_fd); });
            }
        }
    }
    close(epfd);
}

void TcpServer::handle_connection(int client_fd) {
    while (true) {
        Request req;
        if (!recv_request(client_fd, req))
            break;
        Response resp = handler_(req);
        if (!send_response(client_fd, resp))
            break;
    }
    close(client_fd);
}

}  // namespace tcp
