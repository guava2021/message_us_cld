#pragma once

#include <cstdint>
#include <vector>

namespace tcp {

constexpr uint32_t kMaxPayloadBytes = 65535;

enum class StatusCode : uint8_t {
    Ok      = 0,
    Error   = 1,
    Timeout = 2,
    Invalid = 3,
    Busy    = 4,
};

struct Request {
    uint32_t id{0};
    uint8_t type{0};
    std::vector<uint8_t> payload;
};

struct Response {
    uint32_t id{0};
    StatusCode status{StatusCode::Ok};
    std::vector<uint8_t> payload;
};

// Wire format (request):  [4B length][4B id][1B type][payload]
// Wire format (response): [4B length][4B id][1B status][payload]
// length = 5 + payload.size(), big-endian

bool send_request(int fd, const Request &req);
bool send_response(int fd, const Response &resp);
bool recv_request(int fd, Request &out);
bool recv_response(int fd, Response &out);

}  // namespace tcp
