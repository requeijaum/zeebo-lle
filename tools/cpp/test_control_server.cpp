#include "zeebo_control_server.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

int main() {
    constexpr int port = 49129;
    zeebo_lle::ControlServer server;
    assert(server.Start(port));

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = ::htons(port);
    assert(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // Give AcceptLoop time to enter recv() on an idle client. Stop must unblock
    // that recv rather than hanging forever in the destructor/join.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto start = std::chrono::steady_clock::now();
    server.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(elapsed < std::chrono::seconds(1));

    ::close(fd);

    // A parsed request left in the queue must also be completed during Stop;
    // otherwise Dispatch waits for its 30-second promise timeout.
    constexpr int port2 = port + 1;
    assert(server.Start(port2));
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    addr.sin_port = ::htons(port2);
    assert(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const char request[] = "{\"cmd\":\"ping\"}\n";
    assert(::send(fd, request, sizeof(request) - 1, 0) ==
           static_cast<ssize_t>(sizeof(request) - 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto queued_start = std::chrono::steady_clock::now();
    server.Stop();
    assert(std::chrono::steady_clock::now() - queued_start < std::chrono::seconds(1));
    ::close(fd);
    return 0;
}
