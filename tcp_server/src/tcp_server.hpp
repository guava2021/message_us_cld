#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace tcp {

class ThreadPool;

using Handler = std::function<Response(const Request &)>;

class TcpServer {
   public:
    // num_workers: thread pool size; handler: maps request → response
    // recv_timeout_ms: per-connection recv timeout (0 = infinite); used for graceful shutdown
    TcpServer(uint16_t port, size_t num_workers, Handler handler, int recv_timeout_ms = 30000);
    ~TcpServer();

    TcpServer(const TcpServer &)            = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    void run();   // blocks until stop() is called
    void stop();  // signal run() to return; safe to call from any thread

    uint16_t port() const { return port_; }

   private:
    void accept_loop();
    void handle_connection(int client_fd);

    uint16_t port_;
    int server_fd_{-1};
    int recv_timeout_ms_;
    Handler handler_;
    std::unique_ptr<ThreadPool> pool_;
    int stop_pipe_[2]{-1, -1};
};

}  // namespace tcp
