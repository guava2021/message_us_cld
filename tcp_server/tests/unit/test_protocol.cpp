// Build:  cmake -B build -DBUILD_TESTING=ON && cmake --build build
// Run:    ctest --test-dir build --output-on-failure
// ASan:   cmake -B build_asan -DBUILD_TESTING=ON -DSANITIZE=asan && cmake --build build_asan

// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" const char *__asan_default_options() {
    return "detect_leaks=0";
}

#include "protocol.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

static std::pair<int, int> make_pair() {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
        throw std::runtime_error("socketpair failed");
    return {fds[0], fds[1]};
}

TEST(Protocol, RoundtripEmptyRequest) {
    auto [a, b] = make_pair();
    tcp::Request req;
    req.id   = 42;
    req.type = 7;
    ASSERT_TRUE(tcp::send_request(a, req));
    tcp::Request out;
    ASSERT_TRUE(tcp::recv_request(b, out));
    EXPECT_EQ(out.id, 42u);
    EXPECT_EQ(out.type, 7u);
    EXPECT_TRUE(out.payload.empty());
    close(a);
    close(b);
}

TEST(Protocol, RoundtripRequestWithPayload) {
    auto [a, b] = make_pair();
    tcp::Request req;
    req.id          = 1;
    req.type        = 2;
    std::string msg = "hello world";
    req.payload.assign(msg.begin(), msg.end());
    ASSERT_TRUE(tcp::send_request(a, req));
    tcp::Request out;
    ASSERT_TRUE(tcp::recv_request(b, out));
    EXPECT_EQ(out.id, 1u);
    EXPECT_EQ(std::string(out.payload.begin(), out.payload.end()), msg);
    close(a);
    close(b);
}

TEST(Protocol, RoundtripResponse) {
    auto [a, b] = make_pair();
    tcp::Response resp;
    resp.id         = 99;
    resp.status     = tcp::StatusCode::Ok;
    std::string msg = "status: all good";
    resp.payload.assign(msg.begin(), msg.end());
    ASSERT_TRUE(tcp::send_response(a, resp));
    tcp::Response out;
    ASSERT_TRUE(tcp::recv_response(b, out));
    EXPECT_EQ(out.id, 99u);
    EXPECT_EQ(out.status, tcp::StatusCode::Ok);
    EXPECT_EQ(std::string(out.payload.begin(), out.payload.end()), msg);
    close(a);
    close(b);
}

TEST(Protocol, AllStatusCodes) {
    using SC = tcp::StatusCode;
    for (auto code : {SC::Ok, SC::Error, SC::Timeout, SC::Invalid, SC::Busy}) {
        auto [a, b] = make_pair();
        tcp::Response resp;
        resp.id     = 0;
        resp.status = code;
        ASSERT_TRUE(tcp::send_response(a, resp));
        tcp::Response out;
        ASSERT_TRUE(tcp::recv_response(b, out));
        EXPECT_EQ(out.status, code);
        close(a);
        close(b);
    }
}

TEST(Protocol, MaxPayload) {
    auto [a, b] = make_pair();
    tcp::Request req;
    req.id   = 5;
    req.type = 0;
    req.payload.assign(tcp::kMaxPayloadBytes, 0xAB);
    ASSERT_TRUE(tcp::send_request(a, req));
    tcp::Request out;
    ASSERT_TRUE(tcp::recv_request(b, out));
    EXPECT_EQ(out.payload.size(), tcp::kMaxPayloadBytes);
    EXPECT_EQ(out.payload[0], 0xABu);
    close(a);
    close(b);
}

TEST(Protocol, OversizedPayloadRejected) {
    auto [a, b] = make_pair();
    tcp::Request req;
    req.id   = 1;
    req.type = 0;
    req.payload.assign(tcp::kMaxPayloadBytes + 1, 0xFF);
    EXPECT_FALSE(tcp::send_request(a, req));
    close(a);
    close(b);
}

TEST(Protocol, RecvOnClosedSocket) {
    auto [a, b] = make_pair();
    close(a);
    tcp::Request out;
    EXPECT_FALSE(tcp::recv_request(b, out));
    close(b);
}

TEST(Protocol, IdPreservedAcrossWire) {
    auto [a, b] = make_pair();
    for (uint32_t id : {0u, 1u, 0xFFFFu, 0xFFFFFFFFu}) {
        tcp::Request req;
        req.id   = id;
        req.type = 0;
        ASSERT_TRUE(tcp::send_request(a, req));
        tcp::Request out;
        ASSERT_TRUE(tcp::recv_request(b, out));
        EXPECT_EQ(out.id, id);
    }
    close(a);
    close(b);
}

TEST(Protocol, MultipleSequentialRequests) {
    auto [a, b] = make_pair();
    for (uint32_t i = 0; i < 100; ++i) {
        tcp::Request req;
        req.id   = i;
        req.type = static_cast<uint8_t>(i % 256u);
        ASSERT_TRUE(tcp::send_request(a, req));
        tcp::Request out;
        ASSERT_TRUE(tcp::recv_request(b, out));
        EXPECT_EQ(out.id, i);
        EXPECT_EQ(out.type, static_cast<uint8_t>(i % 256u));
    }
    close(a);
    close(b);
}
