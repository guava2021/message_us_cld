// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" const char *__asan_default_options() {
    return "detect_leaks=0";
}

#include "tcp_client.hpp"
#include "tcp_server.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

static tcp::Response echo_handler(const tcp::Request &req) {
    tcp::Response resp;
    resp.id      = req.id;
    resp.status  = tcp::StatusCode::Ok;
    resp.payload = req.payload;
    return resp;
}

static uint16_t free_port() {
    int fd  = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
    close(fd);
    return ntohs(addr.sin_port);
}

static int raw_connect(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct ErrorFixture : ::testing::Test {
    uint16_t port{};
    std::unique_ptr<tcp::TcpServer> server;
    std::thread server_thread;

    void SetUp() override {
        port = free_port();
        // short recv timeout so workers unblock quickly during shutdown
        server = std::make_unique<tcp::TcpServer>(port, 4, echo_handler, /*recv_timeout_ms=*/200);
        server_thread = std::thread([this] { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    void TearDown() override {
        server->stop();
        if (server_thread.joinable())
            server_thread.join();
    }
};

// Send a length field claiming a payload too large for our protocol
TEST_F(ErrorFixture, MalformedLengthTooLarge) {
    int fd = raw_connect(port);
    ASSERT_GE(fd, 0);

    uint32_t bad_len = htonl(0xFFFFFFFFu);
    send(fd, &bad_len, 4, MSG_NOSIGNAL);

    // server should detect invalid length, close connection
    char buf[4];
    ssize_t r = recv(fd, buf, sizeof(buf), 0);
    EXPECT_LE(r, 0);  // EOF or error
    close(fd);
}

// Send a length field claiming fewer than 5 bytes (below minimum)
TEST_F(ErrorFixture, MalformedLengthTooSmall) {
    int fd = raw_connect(port);
    ASSERT_GE(fd, 0);

    uint32_t bad_len = htonl(3u);  // min valid is 5
    send(fd, &bad_len, 4, MSG_NOSIGNAL);

    char buf[4];
    ssize_t r = recv(fd, buf, sizeof(buf), 0);
    EXPECT_LE(r, 0);
    close(fd);
}

// Send length header then immediately close — server must not crash
TEST_F(ErrorFixture, TruncatedAfterLength) {
    int fd = raw_connect(port);
    ASSERT_GE(fd, 0);

    uint32_t len = htonl(10u);  // claims 10 bytes coming
    send(fd, &len, 4, MSG_NOSIGNAL);
    close(fd);  // close before sending body

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // server must still be alive — serve a good client
    tcp::TcpClient good("127.0.0.1", port);
    tcp::Request req;
    req.id   = 42;
    req.type = 0;
    ASSERT_TRUE(good.send(req));
    tcp::Response resp;
    ASSERT_TRUE(good.recv(resp));
    EXPECT_EQ(resp.id, 42u);
}

// Send garbage bytes — server must close gracefully
TEST_F(ErrorFixture, GarbageData) {
    int fd = raw_connect(port);
    ASSERT_GE(fd, 0);

    const char garbage[] = "XXXXXXXXXXXXXXXXXXXX";
    send(fd, garbage, sizeof(garbage), MSG_NOSIGNAL);

    char buf[4];
    ssize_t r = recv(fd, buf, sizeof(buf), 0);
    EXPECT_LE(r, 0);
    close(fd);
}

// Rapid connect/disconnect without sending any data
TEST_F(ErrorFixture, RapidConnectDisconnect) {
    for (int i = 0; i < 20; ++i) {
        int fd = raw_connect(port);
        if (fd >= 0)
            close(fd);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // server must still be responsive
    tcp::TcpClient good("127.0.0.1", port);
    tcp::Request req;
    req.id   = 7;
    req.type = 0;
    ASSERT_TRUE(good.send(req));
    tcp::Response resp;
    ASSERT_TRUE(good.recv(resp));
    EXPECT_EQ(resp.id, 7u);
}

// Stop server while clients are mid-flight — must not hang or crash
// Uses its own server lifecycle so cleanup is deterministic.
TEST(ErrorInjection, ShutdownUnderLoad) {
    const uint16_t port = free_port();
    auto server = std::make_unique<tcp::TcpServer>(port, 4, echo_handler, /*recv_timeout_ms=*/200);
    std::thread server_thread([&] { server->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    constexpr int kClients = 4;
    std::atomic<bool> go{true};
    std::vector<std::thread> clients;
    clients.reserve(kClients);

    for (int c = 0; c < kClients; ++c) {
        clients.emplace_back([&, c] {
            try {
                tcp::TcpClient client("127.0.0.1", port);
                uint32_t id = 0;
                while (go.load(std::memory_order_relaxed)) {
                    tcp::Request req;
                    req.id   = static_cast<uint32_t>(c * 10000 + id++);
                    req.type = 0;
                    if (!client.send(req))
                        break;
                    tcp::Response resp;
                    if (!client.recv(resp))
                        break;
                }
            } catch (...) {
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Stop server with active clients — must complete within recv_timeout_ms
    server->stop();
    if (server_thread.joinable())
        server_thread.join();

    go.store(false, std::memory_order_relaxed);
    for (auto &t : clients)
        t.join();
    // No crash/hang = pass
}
