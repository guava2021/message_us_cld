#include "protocol.hpp"

#include <cstddef>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tcp {

static bool recv_exact(int fd, void *buf, size_t n) {
    auto *ptr        = static_cast<uint8_t *>(buf);
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t r = recv(fd, ptr, remaining, 0);
        if (r <= 0)
            return false;
        ptr += r;
        remaining -= static_cast<size_t>(r);
    }
    return true;
}

static bool send_exact(int fd, const void *buf, size_t n) {
    const auto *ptr  = static_cast<const uint8_t *>(buf);
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t r = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (r <= 0)
            return false;
        ptr += r;
        remaining -= static_cast<size_t>(r);
    }
    return true;
}

bool send_request(int fd, const Request &req) {
    if (req.payload.size() > kMaxPayloadBytes)
        return false;
    uint32_t body_len = 5U + static_cast<uint32_t>(req.payload.size());
    uint32_t net_len  = htonl(body_len);
    uint32_t net_id   = htonl(req.id);

    if (!send_exact(fd, &net_len, 4))
        return false;
    if (!send_exact(fd, &net_id, 4))
        return false;
    if (!send_exact(fd, &req.type, 1))
        return false;
    if (!req.payload.empty())
        if (!send_exact(fd, req.payload.data(), req.payload.size()))
            return false;
    return true;
}

bool send_response(int fd, const Response &resp) {
    if (resp.payload.size() > kMaxPayloadBytes)
        return false;
    uint32_t body_len = 5U + static_cast<uint32_t>(resp.payload.size());
    uint32_t net_len  = htonl(body_len);
    uint32_t net_id   = htonl(resp.id);
    auto status_byte  = static_cast<uint8_t>(resp.status);

    if (!send_exact(fd, &net_len, 4))
        return false;
    if (!send_exact(fd, &net_id, 4))
        return false;
    if (!send_exact(fd, &status_byte, 1))
        return false;
    if (!resp.payload.empty())
        if (!send_exact(fd, resp.payload.data(), resp.payload.size()))
            return false;
    return true;
}

bool recv_request(int fd, Request &out) {
    uint32_t net_len = 0;
    if (!recv_exact(fd, &net_len, 4))
        return false;
    uint32_t body_len = ntohl(net_len);
    if (body_len < 5U || (body_len - 5U) > kMaxPayloadBytes)
        return false;

    uint32_t net_id = 0;
    if (!recv_exact(fd, &net_id, 4))
        return false;
    out.id = ntohl(net_id);

    if (!recv_exact(fd, &out.type, 1))
        return false;

    size_t payload_len = body_len - 5U;
    out.payload.resize(payload_len);
    if (payload_len > 0)
        if (!recv_exact(fd, out.payload.data(), payload_len))
            return false;
    return true;
}

bool recv_response(int fd, Response &out) {
    uint32_t net_len = 0;
    if (!recv_exact(fd, &net_len, 4))
        return false;
    uint32_t body_len = ntohl(net_len);
    if (body_len < 5U || (body_len - 5U) > kMaxPayloadBytes)
        return false;

    uint32_t net_id = 0;
    if (!recv_exact(fd, &net_id, 4))
        return false;
    out.id = ntohl(net_id);

    uint8_t status_byte = 0;
    if (!recv_exact(fd, &status_byte, 1))
        return false;
    out.status = static_cast<StatusCode>(status_byte);

    size_t payload_len = body_len - 5U;
    out.payload.resize(payload_len);
    if (payload_len > 0)
        if (!recv_exact(fd, out.payload.data(), payload_len))
            return false;
    return true;
}

}  // namespace tcp
