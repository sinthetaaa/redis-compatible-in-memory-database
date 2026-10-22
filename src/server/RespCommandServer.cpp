#include "redisdb/server/RespCommandServer.hpp"

#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/commands/CommandErrors.hpp"
#include "redisdb/commands/ConnectivityCommands.hpp"
#include "redisdb/net/Connection.hpp"
#include "redisdb/net/ConnectionManager.hpp"
#include "redisdb/net/TcpListener.hpp"
#include "redisdb/protocol/RespPipeline.hpp"

#if defined(__APPLE__)
#include "redisdb/net/KqueuePoller.hpp"
#elif defined(__linux__)
#include "redisdb/net/EpollPoller.hpp"
#else
#error "RESP2 server requires macOS or Linux"
#endif

#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace redisdb::server {
namespace {

#if defined(__APPLE__)
using NativePoller = net::KqueuePoller;
#else
using NativePoller = net::EpollPoller;
#endif

volatile std::sig_atomic_t command_stop_requested = 0;

void request_command_stop(int) { command_stop_requested = 1; }

void install_command_signals() {
    if (std::signal(SIGINT, request_command_stop) == SIG_ERR ||
        std::signal(SIGTERM, request_command_stop) == SIG_ERR) {
        throw std::runtime_error("unable to install RESP2 shutdown signal handlers");
    }
}

} // namespace

void run_resp2_server(std::uint16_t port, std::size_t max_clients) {
    if (max_clients == 0) {
        throw std::invalid_argument("max_clients must be positive");
    }

    command_stop_requested = 0;
    NativePoller poller;
    net::ConnectionManager::Limits limits;
    limits.max_connections = max_clients;

    net::ConnectionManager manager(net::TcpListener::bind_loopback(port), poller, limits);
    const commands::CommandDispatcher dispatcher(commands::connectivity_routes());
    const protocol::RespPipeline pipeline;

    manager.set_resumable_input_handler([&](net::Connection& connection) {
        // RESP2 input is framed and decoded through the existing bounded pipeline.
        // QUIT requests a graceful close only after its +OK reply has been sent.
        const auto outcome = pipeline.drain(connection, [&](const protocol::RespValue& request) {
            auto decoded = commands::decode_command(request);
            if (!decoded.command) {
                return protocol::RespValue::error("ERR expected RESP2 array of bulk strings");
            }

            auto dispatched = dispatcher.dispatch(*decoded.command);
            if (!dispatched.handled) {
                if (dispatched.error != commands::DispatchError::UnknownCommand) {
                    throw std::logic_error("unexpected dispatcher outcome");
                }
                return commands::unknown_command_reply(*decoded.command);
            }

            if (dispatched.handled->disposition ==
                commands::ConnectionDisposition::CloseAfterReply) {
                connection.request_close_after_flush();
            }
            return std::move(dispatched.handled->reply);
        });

        using Action = net::ConnectionManager::InputAction;
        switch (outcome.stop) {
        case protocol::PipelineStop::NeedMore:
        case protocol::PipelineStop::Closing:
            return Action::AwaitRead;
        case protocol::PipelineStop::BudgetExhausted:
            return Action::Continue;
        case protocol::PipelineStop::Backpressured:
            return Action::AwaitWrite;
        case protocol::PipelineStop::Invalid:
            // Malformed RESP2 is isolated to this client by ConnectionManager.
            throw std::runtime_error("invalid RESP2 request frame");
        }
        throw std::logic_error("unexpected pipeline result");
    });

    install_command_signals();
    std::cout << "RESP_READY 127.0.0.1:" << manager.listener_port() << '\n' << std::flush;
    constexpr auto PollInterval = std::chrono::milliseconds{100};
    while (command_stop_requested == 0) {
        (void)manager.poll_once(PollInterval);
    }
    std::cout << "RESP_STOPPED\n" << std::flush;
}

} // namespace redisdb::server
