// tools/cpp/zeebo_control_server.h
//
// Out-of-band programmatic control channel for the Zeebo LLE System orchestrator.
// Ported cleanly from Zeebulator's core/control/control_server.h.
//
// Protocol (newline-delimited JSON request -> JSON response):
//   {"cmd":"ping"}                               -> {"ok":true,"pong":true}
//   {"cmd":"state"}                              -> {"ok":true,"cycle":C,"c0_pc":..,"c0_insns":..,"c1_pc":..,"c1_insns":..,"running":true}
//   {"cmd":"step","ticks":N}                     -> {"ok":true,"cycle":C,"c0_pc":..}
//   {"cmd":"reg","core":0,"n":15}                -> {"ok":true,"core":0,"n":15,"value":..}
//   {"cmd":"setreg","core":0,"n":0,"value":..}   -> {"ok":true,"core":0,"n":0,"value":..}
//   {"cmd":"read","core":0,"addr":..,"len":..}   -> {"ok":true,"core":0,"addr":..,"len":..,"hex":"...."}
//   {"cmd":"write","core":0,"addr":..,"hex":..}  -> {"ok":true,"core":0,"addr":..,"len":..}
//   {"cmd":"bp","core":0,"addr":..}              -> {"ok":true,"core":0,"addr":..}
//   {"cmd":"bpclear","core":0,"addr":..}         -> {"ok":true,"core":0,"addr":..}
//   {"cmd":"cont"}                               -> {"ok":true,"running":true}
//   {"cmd":"press","button":"button1"}           -> {"ok":true}
//   {"cmd":"screenshot","path":"/tmp/x.ppm"}     -> {"ok":true,"w":640,"h":480,"path":..}
//   {"cmd":"quit"}                               -> {"ok":true}

#ifndef ZEEBO_LLE_CONTROL_SERVER_H_
#define ZEEBO_LLE_CONTROL_SERVER_H_

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace zeebo_lle {

struct ControlRequest {
    std::string cmd;
    std::string button;
    std::string str_path;
    std::string str_hex;
    std::string str_mode;
    std::string str_action;
    unsigned long core = 0;
    unsigned long i0 = 0;       // ticks / n / addr / lo
    unsigned long i1 = 0;       // len
    unsigned long i2 = 0;       // hi
    unsigned long val = 0;      // value
    bool has_core = false;
    bool has_i0 = false;
    bool has_i1 = false;
    bool has_i2 = false;
    bool has_val = false;
    std::promise<std::string> reply;
};

class ControlServer {
public:
    ControlServer() = default;
    ~ControlServer() { Stop(); }

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    bool Start(int port) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            std::perror("[control] socket");
            return false;
        }
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = ::htons(static_cast<uint16_t>(port));
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::perror("[control] bind");
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        if (::listen(listen_fd_, 4) < 0) {
            std::perror("[control] listen");
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        running_.store(true);
        accept_thread_ = std::thread([this] { AcceptLoop(); });
        std::fprintf(stderr, "[control] listening on 127.0.0.1:%d\n", port);
        return true;
    }

    void Stop() {
        bool was = running_.exchange(false);
        if (!was) return;
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            while (!queue_.empty()) {
                try { queue_.front()->reply.set_value("{\"ok\":false,\"error\":\"shutdown\"}"); }
                catch (const std::future_error&) {}
                queue_.pop();
            }
        }
        const int client_fd = client_fd_.load();
        if (client_fd >= 0) ::shutdown(client_fd, SHUT_RDWR);
        if (accept_thread_.joinable()) accept_thread_.join();
    }

    std::vector<std::shared_ptr<ControlRequest>> Drain() {
        std::vector<std::shared_ptr<ControlRequest>> out;
        std::lock_guard<std::mutex> lk(mu_);
        while (!queue_.empty()) {
            out.push_back(std::move(queue_.front()));
            queue_.pop();
        }
        return out;
    }

    bool IsRunning() const { return running_.load(); }

private:
    bool Enqueue(const std::shared_ptr<ControlRequest>& req) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_.load()) return false;
        queue_.push(req);
        return true;
    }

    void AcceptLoop() {
        while (running_.load()) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (fd < 0) {
                if (!running_.load()) break;
                continue;
            }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            client_fd_.store(fd);
            HandleConnection(fd);
            client_fd_.store(-1);
            ::close(fd);
        }
    }

    void HandleConnection(int fd) {
        std::string buf;
        char chunk[1024];
        while (running_.load()) {
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                std::string reply = Dispatch(line);
                reply.push_back('\n');
                if (!WriteAll(fd, reply)) return;
            }
            ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) return;
            buf.append(chunk, static_cast<size_t>(n));
            if (buf.size() > 16384) {
                WriteAll(fd, "{\"ok\":false,\"error\":\"request_too_large\"}\n");
                return;
            }
        }
    }

    std::string Dispatch(const std::string& line) {
        auto req = std::make_shared<ControlRequest>();
        if (!ParseLine(line, *req)) {
            return "{\"ok\":false,\"error\":\"parse\"}";
        }
        std::future<std::string> fut = req->reply.get_future();
        if (!Enqueue(req)) return "{\"ok\":false,\"error\":\"shutdown\"}";
        if (fut.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
            return "{\"ok\":false,\"error\":\"timeout\"}";
        }
        return fut.get();
    }

    static bool ParseLine(const std::string& s, ControlRequest& req) {
        req.cmd = ExtractString(s, "cmd");
        if (req.cmd.empty()) return false;
        req.button = ExtractString(s, "button");
        req.str_path = ExtractString(s, "path");
        req.str_hex = ExtractString(s, "hex");
        req.str_mode = ExtractString(s, "mode");
        req.str_action = ExtractString(s, "action");

        unsigned long c;
        if (ExtractInt(s, "core", &c)) {
            req.core = c;
            req.has_core = true;
        }

        for (const char* k : {"ticks", "n", "addr", "port"}) {
            unsigned long v;
            if (ExtractInt(s, k, &v)) {
                req.i0 = v;
                req.has_i0 = true;
                break;
            }
        }
        unsigned long len;
        if (ExtractInt(s, "len", &len)) {
            req.i1 = len;
            req.has_i1 = true;
        }
        unsigned long value;
        if (ExtractInt(s, "value", &value) || ExtractInt(s, "val", &value)) {
            req.val = value;
            req.has_val = true;
        }
        unsigned long hi;
        if (ExtractInt(s, "hi", &hi)) {
            req.i2 = hi;
            req.has_i2 = true;
        }
        return true;
    }

    static std::string ExtractString(const std::string& s, const char* key) {
        std::string pat = "\"" + std::string(key) + "\"";
        size_t k = s.find(pat);
        if (k == std::string::npos) return {};
        size_t c = s.find(':', k + pat.size());
        if (c == std::string::npos) return {};
        size_t q = s.find('"', c + 1);
        if (q == std::string::npos) return {};
        size_t e = s.find('"', q + 1);
        if (e == std::string::npos) return {};
        return s.substr(q + 1, e - q - 1);
    }

    static bool ExtractInt(const std::string& s, const char* key, unsigned long* out) {
        std::string pat = "\"" + std::string(key) + "\"";
        size_t k = s.find(pat);
        if (k == std::string::npos) return false;
        size_t c = s.find(':', k + pat.size());
        if (c == std::string::npos) return false;
        size_t p = c + 1;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\"')) ++p;
        if (p >= s.size()) return false;
        int base = 10;
        if (p + 1 < s.size() && s[p] == '0' && (s[p + 1] == 'x' || s[p + 1] == 'X')) base = 16;
        char* end = nullptr;
        unsigned long long v = std::strtoull(s.c_str() + p, &end, base);
        if (end == s.c_str() + p) return false;
        *out = (unsigned long)v;
        return true;
    }

    static bool WriteAll(int fd, const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    int listen_fd_ = -1;
    std::atomic<int> client_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex mu_;
    std::queue<std::shared_ptr<ControlRequest>> queue_;
};

} // namespace zeebo_lle

#endif // ZEEBO_LLE_CONTROL_SERVER_H_
