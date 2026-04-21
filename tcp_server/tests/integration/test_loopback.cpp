// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" const char *__asan_default_options() {
    return "detect_leaks=0";
}

#include "tcp_client.hpp"
#include "tcp_server.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
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

struct ServerFixture : ::testing::Test {
    uint16_t port{};
    std::unique_ptr<tcp::TcpServer> server;
    std::thread server_thread;

    void SetUp() override {
        port   = free_port();
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

TEST_F(ServerFixture, SingleRequestResponse) {
    tcp::TcpClient client("127.0.0.1", port);
    tcp::Request req;
    req.id          = 1;
    req.type        = 0;
    std::string msg = "ping";
    req.payload.assign(msg.begin(), msg.end());

    ASSERT_TRUE(client.send(req));
    tcp::Response resp;
    ASSERT_TRUE(client.recv(resp));
    EXPECT_EQ(resp.id, 1u);
    EXPECT_EQ(resp.status, tcp::StatusCode::Ok);
    EXPECT_EQ(std::string(resp.payload.begin(), resp.payload.end()), msg);
}

TEST_F(ServerFixture, MultipleSequentialRequests) {
    tcp::TcpClient client("127.0.0.1", port);
    for (uint32_t i = 0; i < 50; ++i) {
        tcp::Request req;
        req.id   = i;
        req.type = 1;
        ASSERT_TRUE(client.send(req));
        tcp::Response resp;
        ASSERT_TRUE(client.recv(resp));
        EXPECT_EQ(resp.id, i);
        EXPECT_EQ(resp.status, tcp::StatusCode::Ok);
    }
}

TEST_F(ServerFixture, ConcurrentClients) {
    constexpr int kClients  = 8;
    constexpr int kRequests = 10;
    std::atomic<int> successes{0};

    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int c = 0; c < kClients; ++c) {
        clients.emplace_back([&, c] {
            try {
                tcp::TcpClient client("127.0.0.1", port);
                for (int r = 0; r < kRequests; ++r) {
                    tcp::Request req;
                    req.id   = static_cast<uint32_t>(c * 1000 + r);
                    req.type = 0;
                    if (!client.send(req))
                        return;
                    tcp::Response resp;
                    if (!client.recv(resp))
                        return;
                    if (resp.id == req.id)
                        successes.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
            }
        });
    }
    for (auto &t : clients)
        t.join();
    EXPECT_EQ(successes.load(), kClients * kRequests);
}

TEST_F(ServerFixture, ClientDisconnectMidStream) {
    {
        tcp::TcpClient client("127.0.0.1", port);
        (void)client;
        // destructor closes immediately without sending a frame
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // server must survive and keep accepting
    tcp::TcpClient client2("127.0.0.1", port);
    tcp::Request req;
    req.id   = 99;
    req.type = 0;
    ASSERT_TRUE(client2.send(req));
    tcp::Response resp;
    ASSERT_TRUE(client2.recv(resp));
    EXPECT_EQ(resp.id, 99u);
}

TEST_F(ServerFixture, LargePayloadEcho) {
    tcp::TcpClient client("127.0.0.1", port);
    tcp::Request req;
    req.id = 7;
    req.payload.assign(tcp::kMaxPayloadBytes, 0xCC);
    ASSERT_TRUE(client.send(req));
    tcp::Response resp;
    ASSERT_TRUE(client.recv(resp));
    ASSERT_EQ(resp.payload.size(), tcp::kMaxPayloadBytes);
    EXPECT_EQ(resp.payload[0], 0xCCu);
    EXPECT_EQ(resp.payload.back(), 0xCCu);
}

TEST_F(ServerFixture, EmptyPayloadRequest) {
    tcp::TcpClient client("127.0.0.1", port);
    tcp::Request req;
    req.id   = 0;
    req.type = 5;
    ASSERT_TRUE(client.send(req));
    tcp::Response resp;
    ASSERT_TRUE(client.recv(resp));
    EXPECT_EQ(resp.id, 0u);
    EXPECT_TRUE(resp.payload.empty());
}
