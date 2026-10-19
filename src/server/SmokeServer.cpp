#include "redisdb/server/SmokeServer.hpp"

#include "redisdb/net/Connection.hpp"
#include "redisdb/net/ConnectionManager.hpp"
#include "redisdb/net/TcpListener.hpp"

#if defined(__APPLE__)
#include "redisdb/net/KqueuePoller.hpp"
#elif defined(__linux__)
#include "redisdb/net/EpollPoller.hpp"
#else
#error "Smoke server requires macOS or Linux"
#endif

#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace redisdb::server {

namespace {

#if defined(__APPLE__)
using NativePoller = net::KqueuePoller;
#else
using NativePoller = net::EpollPoller;
#endif

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) { stop_requested = 1; }

void install_signal_handlers() {
    if (std::signal(SIGINT, request_stop) == SIG_ERR) {
        throw std::runtime_error("failed to install SIGINT handler");
    }

    if (std::signal(SIGTERM, request_stop) == SIG_ERR) {
        throw std::runtime_error("failed to install SIGTERM handler");
    }
}

} // namespace

void run_smoke_echo(std::uint16_t port, std::size_t max_clients) {
    if (max_clients == 0) {
        throw std::invalid_argument("max_clients must be positive");
    }

    stop_requested = 0;

    NativePoller poller;

    auto listener = net::TcpListener::bind_loopback(port);

    net::ConnectionManager::Limits limits;
    limits.max_connections = max_clients;

    net::ConnectionManager manager(std::move(listener), poller, limits);

    manager.set_input_handler([](net::Connection& connection) {
        const std::string_view payload = connection.input();

        // Binary-safe transport echo.
        // No RESP parsing or command execution.
        connection.queue_output(payload);
        connection.consume_input(payload.size());
    });

    install_signal_handlers();

    // Machine-readable readiness signal.
    // Printed only after the listener and poller
    // have been successfully initialized.
    std::cout << "SMOKE_READY 127.0.0.1:" << manager.listener_port() << '\n' << std::flush;

    // Finite polling allows prompt shutdown even
    // when native pollers retry interrupted waits.
    constexpr auto PollInterval = std::chrono::milliseconds{100};

    while (stop_requested == 0) {
        (void)manager.poll_once(PollInterval);
    }

    // ConnectionManager destruction unregisters
    // its clients before their descriptors close.
    std::cout << "SMOKE_STOPPED\n" << std::flush;
}

} // namespace redisdb::server
