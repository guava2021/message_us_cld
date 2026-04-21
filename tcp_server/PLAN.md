# TCP Client/Server Plan

## Purpose
Standard TCP client/server for handling user requests and sending status responses back.
Not ultra-low-latency — correctness, clarity, and robustness are the goals.

## Technology Stack

| Layer        | Choice                              |
|--------------|-------------------------------------|
| Language     | C++17                               |
| I/O model    | `epoll` edge-triggered (Linux)      |
| Threading    | Fixed-size thread pool, queue-based |
| Protocol     | Length-prefixed binary frames       |
| Serialization| Simple struct framing (no ext deps) |
| Build        | CMake + Clang                       |
| Timing       | Existing `TscClock`                 |

## Protocol

Length-prefixed framing:
```
[4-byte length (uint32, network byte order)][payload bytes]
```
- No external serialization library needed
- Handles TCP stream fragmentation correctly
- Status codes encoded in response payload

## Components

### 1. `TcpServer`
- `epoll` accept loop on main thread
- Dispatches accepted connections to worker thread pool
- Each worker handles read → process → write response
- Graceful shutdown via signal handler

### 2. `TcpClient`
- Blocking connect/send/recv (simple, correct)
- Sends length-prefixed request
- Waits for length-prefixed status response

### 3. Protocol Layer
- `MessageFrame` struct: length + payload
- `StatusCode` enum: OK, ERROR, TIMEOUT, etc.
- Encode/decode functions (handle partial reads/writes)

### 4. Thread Pool
- Fixed number of worker threads (configurable)
- SPSC or mutex-protected task queue
- Workers pull connection fds and process them

## Directory Layout

```
tcp_server/
  src/
    tcp_server.hpp / tcp_server.cpp
    tcp_client.hpp / tcp_client.cpp
    protocol.hpp / protocol.cpp
    thread_pool.hpp / thread_pool.cpp
  tests/
    unit/
      test_protocol.cpp       — framing, parsing, status codes
      test_thread_pool.cpp    — queue, worker lifecycle
    integration/
      test_loopback.cpp       — real server+client over loopback
      test_concurrency.cpp    — multiple clients, concurrent requests
      test_error_paths.cpp    — connection drop, partial recv, bad frames
  CMakeLists.txt
```

## Testing Strategy

| Type            | Included | Rationale                                              |
|-----------------|----------|--------------------------------------------------------|
| Unit tests      | Yes      | Protocol logic, framing, status codes — no network     |
| Integration tests | Yes    | Real loopback TCP — catches partial reads, resets, concurrency bugs |
| Mock tests      | No       | Mocking sockets adds complexity without catching real bugs |

## Phases

1. **Phase 1 — Basic**: `TcpServer` + `TcpClient` + protocol framing, unit tests
2. **Phase 2 — Robustness**: Thread pool, concurrent clients, integration tests
3. **Phase 3 — Hardening**: Error injection tests, graceful shutdown, ASan/MSan clean
