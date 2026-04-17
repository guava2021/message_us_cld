// tcp_server.hpp — non-blocking TCP server with epoll for HFT
//
// Design (planned):
//  • Single-threaded event loop via epoll_wait — no lock contention
//  • Edge-triggered epoll (EPOLLET) — minimal syscalls per event
//  • Pre-allocated connection objects from memory_pool — no malloc on hot path
//  • Zero-copy recv path: read directly into application buffer
//  • TscClock timestamps on message arrival for latency measurement
//
// Intended use: market-data feed handler, exchange connectivity,
// fix engine gateway — any TCP path where per-message latency matters.
//
// Status: skeleton — implementation pending.

#pragma once

#include <cstdint>

namespace tcp {

// TODO: implement TcpServer, Connection, EpollLoop

}  // namespace tcp
