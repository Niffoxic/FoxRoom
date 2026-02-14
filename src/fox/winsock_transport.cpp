#include "fox/winsock_transport.h"

#include <chrono>
#include <future>
#include <thread>
#include <utility>
#include <ctime>
#include <fstream>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Winhttp.lib")

#include <array>
#include <cstdint>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <vector>

namespace fox
{
    namespace
    {
        constexpr user_id local_user_id = 1;
        constexpr long recv_timeout_ms = 200;
        constexpr long select_timeout_us = 200000;

        std::mutex log_mtx{};

        std::string current_time_string()
        {
            std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &t);
            char out[32]{};
            std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &tm);
            return out;
        }

        void log_net(const std::string& message)
        {
            const std::string line = "[" + current_time_string() + "] [fox-net] " + message;
            {
                std::lock_guard lock(log_mtx);
                std::fprintf(stderr, "%s\n", line.c_str());
                std::ofstream file("foxchat_network.log", std::ios::app);
                if (file.is_open())
                {
                    file << line << std::endl;
                }
            }
            OutputDebugStringA((line + "\n").c_str());
        }

        std::string socket_addr_to_string(const sockaddr_in& addr)
        {
            std::array<char, INET_ADDRSTRLEN> ip{};
            inet_ntop(AF_INET, &addr.sin_addr, ip.data(), static_cast<DWORD>(ip.size()));
            return std::string(ip.data()) + ":" + std::to_string(ntohs(addr.sin_port));
        }

        std::string ipv4_to_string(const in_addr& addr)
        {
            std::array<char, INET_ADDRSTRLEN> ip{};
            inet_ntop(AF_INET, &addr, ip.data(), static_cast<DWORD>(ip.size()));
            return ip.data();
        }

        std::future<bool> make_ready_bool(bool value)
        {
            std::promise<bool> p;
            p.set_value(value);
            return p.get_future();
        }

        std::future<void> make_ready_void()
        {
            std::promise<void> p;
            p.set_value();
            return p.get_future();
        }

        class line_receiver
        {
        public:
            void feed(const char* data, int len) { buffer_.append(data, static_cast<std::size_t>(len)); }

            std::optional<std::string> get_line()
            {
                auto pos = buffer_.find('\n');
                if (pos == std::string::npos)
                {
                    return std::nullopt;
                }
                std::string line = buffer_.substr(0, pos);
                buffer_.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                return line;
            }

        private:
            std::string buffer_{};
        };

        bool send_all(SOCKET sock, const std::string& data)
        {
            const char* ptr = data.c_str();
            std::size_t remaining = data.size();
            while (remaining > 0)
            {
                const int sent = send(sock, ptr, static_cast<int>(remaining), 0);
                if (sent <= 0)
                {
                    return false;
                }
                ptr += sent;
                remaining -= static_cast<std::size_t>(sent);
            }
            return true;
        }

        bool recv_exact(SOCKET sock, char* buffer, int len)
        {
            int total_received = 0;
            while (total_received < len)
            {
                const int received = recv(sock, buffer + total_received, len - total_received, 0);
                if (received <= 0)
                {
                    if (received == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT)
                    {
                        continue;
                    }
                    return false;
                }
                total_received += received;
            }
            return true;
        }

        namespace base64
        {
            constexpr auto chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

            std::string encode(const std::vector<std::uint8_t>& data)
            {
                std::string result;
                int val = 0;
                int valb = -6;
                for (const std::uint8_t c : data)
                {
                    val = (val << 8) + c;
                    valb += 8;
                    while (valb >= 0)
                    {
                        result.push_back(chars[(val >> valb) & 0x3F]);
                        valb -= 6;
                    }
                }
                if (valb > -6)
                {
                    result.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
                }
                while (result.size() % 4 != 0)
                {
                    result.push_back('=');
                }
                return result;
            }

            std::vector<std::uint8_t> decode(const std::string& encoded)
            {
                std::vector<std::uint8_t> out;
                std::array<int, 256> table{};
                table.fill(-1);
                for (int i = 0; i < 64; ++i)
                {
                    table[static_cast<unsigned char>(chars[i])] = i;
                }
                int val = 0;
                int valb = -8;
                for (const char c : encoded)
                {
                    if (table[static_cast<unsigned char>(c)] == -1)
                    {
                        break;
                    }
                    val = (val << 6) + table[static_cast<unsigned char>(c)];
                    valb += 6;
                    if (valb >= 0)
                    {
                        out.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
                        valb -= 8;
                    }
                }
                return out;
            }
        }


        bool is_private_ipv4(const in_addr& addr)
        {
            const auto v = ntohl(addr.s_addr);
            const auto a = static_cast<unsigned>((v >> 24) & 0xFF);
            const auto b = static_cast<unsigned>((v >> 16) & 0xFF);
            if (a == 10 || a == 127)
            {
                return true;
            }
            if (a == 172 && b >= 16 && b <= 31)
            {
                return true;
            }
            if (a == 192 && b == 168)
            {
                return true;
            }
            return false;
        }

        std::string query_public_ip_winhttp(const wchar_t* host, const wchar_t* path)
        {
            HINTERNET h_session = WinHttpOpen(L"FoxChat/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!h_session)
            {
                return {};
            }

            std::string result;
            HINTERNET h_connect = WinHttpConnect(h_session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (h_connect)
            {
                HINTERNET h_request = WinHttpOpenRequest(h_connect, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
                if (h_request)
                {
                    const DWORD timeout_ms = 2500;
                    WinHttpSetTimeouts(h_request, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
                    if (WinHttpSendRequest(h_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(h_request, nullptr))
                    {
                        DWORD avail = 0;
                        while (WinHttpQueryDataAvailable(h_request, &avail) && avail > 0)
                        {
                            std::string chunk(avail, '\0');
                            DWORD read = 0;
                            if (!WinHttpReadData(h_request, chunk.data(), avail, &read) || read == 0)
                            {
                                break;
                            }
                            chunk.resize(read);
                            result += chunk;
                        }
                    }
                    WinHttpCloseHandle(h_request);
                }
                WinHttpCloseHandle(h_connect);
            }
            WinHttpCloseHandle(h_session);

            while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '\t'))
            {
                result.pop_back();
            }
            while (!result.empty() && (result.front() == ' ' || result.front() == '\t'))
            {
                result.erase(result.begin());
            }

            in_addr parsed{};
            if (!result.empty() && inet_pton(AF_INET, result.c_str(), &parsed) == 1 && !is_private_ipv4(parsed))
            {
                return result;
            }
            return {};
        }

        std::string guess_public_ip()
        {
            if (const auto ip = query_public_ip_winhttp(L"api.ipify.org", L"/"); !ip.empty())
            {
                return ip;
            }
            if (const auto ip = query_public_ip_winhttp(L"ifconfig.me", L"/ip"); !ip.empty())
            {
                return ip;
            }
            return {};
        }

        std::string host_ip_from_env()
        {
            const char* value = std::getenv("FOX_CHAT_HOST_IP");
            if (value == nullptr || value[0] == '\0')
            {
                return {};
            }
            in_addr parsed{};
            if (inet_pton(AF_INET, value, &parsed) == 1)
            {
                return value;
            }
            log_net("Ignoring FOX_CHAT_HOST_IP because it is not a valid IPv4 address: " + std::string(value));
            return {};
        }

        std::string guess_local_ip()
        {
            SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (probe != INVALID_SOCKET)
            {
                sockaddr_in target{};
                target.sin_family = AF_INET;
                target.sin_port = htons(53);
                inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);
                if (::connect(probe, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0)
                {
                    sockaddr_in local{};
                    int local_len = sizeof(local);
                    if (getsockname(probe, reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
                    {
                        closesocket(probe);
                        if (local.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
                        {
                            return ipv4_to_string(local.sin_addr);
                        }
                    }
                }
                closesocket(probe);
            }

            std::array<char, 256> hostname{};
            if (gethostname(hostname.data(), static_cast<int>(hostname.size())) == 0)
            {
                addrinfo hints{};
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                addrinfo* result = nullptr;
                if (getaddrinfo(hostname.data(), nullptr, &hints, &result) == 0 && result != nullptr)
                {
                    const addrinfo* it = result;
                    while (it != nullptr)
                    {
                        std::array<char, INET_ADDRSTRLEN> ip{};
                        const auto* addr = reinterpret_cast<sockaddr_in*>(it->ai_addr);
                        inet_ntop(AF_INET, &addr->sin_addr, ip.data(), static_cast<DWORD>(ip.size()));
                        if (std::string(ip.data()) != "127.0.0.1")
                        {
                            freeaddrinfo(result);
                            return ip.data();
                        }
                        it = it->ai_next;
                    }
                    freeaddrinfo(result);
                }
            }
            return "127.0.0.1";
        }

        std::string make_join_key(const std::string& public_ip, const std::string& lan_ip, const std::uint16_t port, const std::string& password)
        {
            const std::string payload = "v2|" + public_ip + "|" + lan_ip + "|" + std::to_string(port);
            std::vector<std::uint8_t> xored(payload.size());
            for (std::size_t i = 0; i < payload.size(); ++i)
            {
                xored[i] = static_cast<std::uint8_t>(payload[i] ^ password[i % password.size()]);
            }
            return base64::encode(xored);
        }

        std::optional<std::vector<std::pair<std::string, std::uint16_t>>> parse_join_key(const std::string& key, const std::string& password)
        {
            try
            {
                auto decoded = base64::decode(key);
                std::string payload(decoded.size(), '\0');
                for (std::size_t i = 0; i < decoded.size(); ++i)
                {
                    payload[i] = static_cast<char>(decoded[i] ^ password[i % password.size()]);
                }
                std::vector<std::pair<std::string, std::uint16_t>> endpoints;

                if (payload.rfind("v2|", 0) == 0)
                {
                    std::vector<std::string> parts;
                    std::size_t start = 0;
                    while (start <= payload.size())
                    {
                        const auto sep = payload.find('|', start);
                        if (sep == std::string::npos)
                        {
                            parts.push_back(payload.substr(start));
                            break;
                        }
                        parts.push_back(payload.substr(start, sep - start));
                        start = sep + 1;
                    }

                    if (parts.size() != 4)
                    {
                        return std::nullopt;
                    }

                    const int port = std::stoi(parts[3]);
                    if (port <= 0 || port > 65535)
                    {
                        return std::nullopt;
                    }

                    const auto add_endpoint = [&endpoints, port](const std::string& ip) {
                        if (ip.empty())
                        {
                            return;
                        }
                        in_addr parsed{};
                        if (inet_pton(AF_INET, ip.c_str(), &parsed) != 1)
                        {
                            return;
                        }
                        for (const auto& [existing_ip, existing_port] : endpoints)
                        {
                            if (existing_ip == ip && existing_port == static_cast<std::uint16_t>(port))
                            {
                                return;
                            }
                        }
                        endpoints.emplace_back(ip, static_cast<std::uint16_t>(port));
                    };

                    add_endpoint(parts[1]); // public IP first
                    add_endpoint(parts[2]); // LAN fallback second if didnt work
                }
                else
                {
                    //~ accept 1 key gen only its for testing only
                    const auto colon = payload.rfind(':');
                    if (colon == std::string::npos)
                    {
                        return std::nullopt;
                    }

                    const std::string ip = payload.substr(0, colon);
                    const int port = std::stoi(payload.substr(colon + 1));
                    if (port <= 0 || port > 65535)
                    {
                        return std::nullopt;
                    }
                    endpoints.emplace_back(ip, static_cast<std::uint16_t>(port));
                }

                if (endpoints.empty())
                {
                    return std::nullopt;
                }

                return endpoints;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        struct server_client
        {
            SOCKET socket = INVALID_SOCKET;
            std::string name{};
            std::atomic<bool> running{true};
            line_receiver receiver{};
            sockaddr_in udp_addr{};
            bool udp_registered = false;
        };

        class server
        {
        public:
            explicit server(std::string pwd, std::string join_key, std::uint16_t port)
                : password_(std::move(pwd)), join_key_(std::move(join_key)), port_(port)
            {
            }

            ~server() { stop(); }

            bool start()
            {
                if (join_key_.empty())
                {
                    log_net("Server join key is empty refusing to start insecure transport.");
                    return false;
                }

                listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (listen_socket_ == INVALID_SOCKET)
                {
                    log_net("Failed to create TCP listen socket. WSA=" + std::to_string(WSAGetLastError()));
                    return false;
                }

                int opt = 1;
                setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&opt), sizeof(opt));

                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = INADDR_ANY;
                addr.sin_port = htons(port_);

                if (bind(listen_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
                {
                    log_net("Failed to bind TCP listener on port " + std::to_string(port_) + ". WSA=" + std::to_string(WSAGetLastError()));
                    stop();
                    return false;
                }

                if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR)
                {
                    log_net("Failed to listen on TCP socket. WSA=" + std::to_string(WSAGetLastError()));
                    stop();
                    return false;
                }

                log_net("Host listening on 0.0.0.0:" + std::to_string(port_));

                // UDP voice
                udp_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (udp_socket_ != INVALID_SOCKET)
                {
                    sockaddr_in udp_addr{};
                    udp_addr.sin_family = AF_INET;
                    udp_addr.sin_addr.s_addr = INADDR_ANY;
                    udp_addr.sin_port = htons(static_cast<std::uint16_t>(port_ + 1));

                    if (bind(udp_socket_, reinterpret_cast<sockaddr*>(&udp_addr), sizeof(udp_addr)) == SOCKET_ERROR)
                    {
                        log_net("Failed to bind UDP relay on port " + std::to_string(static_cast<std::uint16_t>(port_ + 1)) + ". WSA=" + std::to_string(WSAGetLastError()));
                        closesocket(udp_socket_);
                        udp_socket_ = INVALID_SOCKET;
                    }
                    else
                    {
                        log_net("UDP relay listening on 0.0.0.0:" + std::to_string(static_cast<std::uint16_t>(port_ + 1)));
                    }
                }

                running_.store(true, std::memory_order_release);
                accept_thread_ = std::thread([this]() { accept_loop(); });

                if (udp_socket_ != INVALID_SOCKET)
                {
                    udp_thread_ = std::thread([this]() { udp_relay_loop(); });
                }

                return true;
            }

            void stop()
            {
                running_.store(false, std::memory_order_release);
                if (listen_socket_ != INVALID_SOCKET)
                {
                    closesocket(listen_socket_);
                    listen_socket_ = INVALID_SOCKET;
                }
                if (udp_socket_ != INVALID_SOCKET)
                {
                    closesocket(udp_socket_);
                    udp_socket_ = INVALID_SOCKET;
                }
                if (accept_thread_.joinable())
                {
                    accept_thread_.join();
                }
                if (udp_thread_.joinable())
                {
                    udp_thread_.join();
                }

                {
                    std::lock_guard lock(clients_mtx_);
                    for (auto& [sock, client] : clients_)
                    {
                        client->running.store(false, std::memory_order_release);
                        shutdown(sock, SD_BOTH);
                        closesocket(sock);
                    }
                }

                {
                    std::lock_guard lock(clients_mtx_);
                    clients_.clear();
                }

                {
                    std::lock_guard lock(udp_auth_mtx_);
                    authenticated_udp_peers_.clear();
                }

                std::vector<std::thread> threads;
                {
                    std::lock_guard lock(threads_mtx_);
                    threads.swap(client_threads_);
                }

                for (auto& thread : threads)
                {
                    if (thread.joinable())
                    {
                        thread.join();
                    }
                }
            }

        private:
            void accept_loop()
            {
                while (running_.load(std::memory_order_acquire))
                {
                    fd_set read_set;
                    FD_ZERO(&read_set);
                    FD_SET(listen_socket_, &read_set);

                    timeval tv{};
                    tv.tv_sec = 0;
                    tv.tv_usec = select_timeout_us;

                    const int ready = select(0, &read_set, nullptr, nullptr, &tv);
                    if (ready == SOCKET_ERROR)
                    {
                        if (!running_.load(std::memory_order_acquire))
                        {
                            break;
                        }
                        continue;
                    }
                    if (ready == 0)
                    {
                        continue;
                    }

                    sockaddr_in client_addr{};
                    int addr_len = sizeof(client_addr);
                    SOCKET client_sock = accept(listen_socket_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
                    if (client_sock == INVALID_SOCKET)
                    {
                        if (!running_.load(std::memory_order_acquire))
                        {
                            break;
                        }
                        continue;
                    }

                    log_net("Incoming TCP connection from " + socket_addr_to_string(client_addr));
                    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recv_timeout_ms), sizeof(recv_timeout_ms));

                    std::vector<char> supplied_key(join_key_.size());
                    if (!recv_exact(client_sock, supplied_key.data(), static_cast<int>(supplied_key.size())))
                    {
                        log_net("TCP join attempt failed while reading join key from " + socket_addr_to_string(client_addr));
                        closesocket(client_sock);
                        continue;
                    }

                    const std::string provided_key(supplied_key.begin(), supplied_key.end());
                    if (provided_key != join_key_)
                    {
                        log_net("Rejected TCP client due to invalid join key from " + socket_addr_to_string(client_addr));
                        closesocket(client_sock);
                        continue;
                    }
                    log_net("Accepted TCP join key from " + socket_addr_to_string(client_addr));

                    auto client = std::make_shared<server_client>();
                    client->socket = client_sock;
                    {
                        std::lock_guard lock(clients_mtx_);
                        clients_[client_sock] = std::move(client);
                    }

                    std::thread recv_thread([this, client_sock]()
                    {
                        std::shared_ptr<server_client> client;
                        {
                            std::lock_guard lock(clients_mtx_);
                            const auto it = clients_.find(client_sock);
                            if (it == clients_.end())
                            {
                                return;
                            }
                            client = it->second;
                        }

                        client_recv_loop(client);
                    });

                    {
                        std::lock_guard lock(threads_mtx_);
                        client_threads_.push_back(std::move(recv_thread));
                    }
                }
            }

            void client_recv_loop(const std::shared_ptr<server_client>& client)
            {
                char buf[1024];
                bool handshake_done = false;

                while (client->running.load(std::memory_order_acquire) && running_.load(std::memory_order_acquire))
                {
                    const int received = recv(client->socket, buf, sizeof(buf), 0);
                    if (received <= 0)
                    {
                        if (received == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT)
                        {
                            continue;
                        }
                        break;
                    }
                    client->receiver.feed(buf, received);

                    while (auto line = client->receiver.get_line())
                    {
                        if (!handshake_done)
                        {
                            if (line->rfind("PASS ", 0) == 0)
                            {
                                const std::string rest = line->substr(5);
                                const auto split = rest.find(' ');
                                if (split != std::string::npos)
                                {
                                    const std::string supplied = rest.substr(0, split);
                                    const std::string name = rest.substr(split + 1);
                                    if (supplied == password_ && !name.empty())
                                    {
                                        client->name = name;
                                        handshake_done = true;
                                        send_all(client->socket, "OK\n");
                                        log_net("Accepted client '" + name + "'.");
                                        broadcast("* " + name + " joined *\n", client->socket);
                                        continue;
                                    }
                                }
                            }
                            log_net("Rejected client handshake (bad password/name).");
                            send_all(client->socket, "ERR Invalid password or name\n");
                            client->running.store(false, std::memory_order_release);
                            break;
                        }

                        broadcast("[" + client->name + "] " + *line + "\n", INVALID_SOCKET);
                    }
                }

                if (handshake_done)
                {
                    log_net("Client disconnected: '" + client->name + "'.");
                    broadcast("* " + client->name + " left *\n", client->socket);
                }

                closesocket(client->socket);
                std::lock_guard lock(clients_mtx_);
                clients_.erase(client->socket);
            }

            void broadcast(const std::string& msg, SOCKET exclude)
            {
                std::lock_guard lock(clients_mtx_);
                for (auto& [sock, client] : clients_)
                {
                    if (sock != exclude && !client->name.empty())
                    {
                        (void)send_all(sock, msg);
                    }
                }
            }

            void udp_relay_loop()
            {
                char buf[4096];
                while (running_.load(std::memory_order_acquire))
                {
                    fd_set read_set;
                    FD_ZERO(&read_set);
                    FD_SET(udp_socket_, &read_set);

                    timeval tv{};
                    tv.tv_sec = 0;
                    tv.tv_usec = select_timeout_us;

                    const int ready = select(0, &read_set, nullptr, nullptr, &tv);
                    if (ready <= 0) continue;

                    sockaddr_in sender_addr{};
                    int sender_len = sizeof(sender_addr);
                    const int received = recvfrom(udp_socket_, buf, sizeof(buf), 0,
                        reinterpret_cast<sockaddr*>(&sender_addr), &sender_len);
                    if (received <= 0) continue;

                    const std::string sender_key = socket_addr_to_string(sender_addr);
                    int payload_offset = 0;
                    {
                        std::lock_guard lock(udp_auth_mtx_);
                        if (!authenticated_udp_peers_.contains(sender_key))
                        {
                            if (received < static_cast<int>(join_key_.size()) ||
                                std::memcmp(buf, join_key_.data(), join_key_.size()) != 0)
                            {
                                log_net("Rejected UDP packet from unauthenticated peer " + sender_key);
                                continue;
                            }

                            authenticated_udp_peers_.insert(sender_key);
                            payload_offset = static_cast<int>(join_key_.size());
                            log_net("Accepted UDP join key from " + sender_key);
                        }
                    }

                    const int payload_size = received - payload_offset;
                    if (payload_size <= 0)
                    {
                        continue;
                    }

                    // Register senders UDP address with their TCP client entry
                    {
                        std::lock_guard lock(clients_mtx_);
                        for (auto& [sock, client] : clients_)
                        {
                            if (!client->udp_registered && !client->name.empty())
                            {
                                // Match by IP address
                                sockaddr_in tcp_addr{};
                                int tcp_addr_len = sizeof(tcp_addr);
                                getpeername(sock, reinterpret_cast<sockaddr*>(&tcp_addr), &tcp_addr_len);
                                if (tcp_addr.sin_addr.s_addr == sender_addr.sin_addr.s_addr)
                                {
                                    client->udp_addr = sender_addr;
                                    client->udp_registered = true;
                                }
                            }
                        }
                    }

                    // Relay to all other registered UDP clients
                    {
                        std::lock_guard lock(clients_mtx_);
                        for (auto& [sock, client] : clients_)
                        {
                            if (client->udp_registered &&
                                (client->udp_addr.sin_addr.s_addr != sender_addr.sin_addr.s_addr ||
                                 client->udp_addr.sin_port != sender_addr.sin_port))
                            {
                                sendto(udp_socket_, buf + payload_offset, payload_size, 0,
                                    reinterpret_cast<sockaddr*>(&client->udp_addr),
                                    sizeof(client->udp_addr));
                            }
                        }
                    }
                }
            }

            SOCKET listen_socket_ = INVALID_SOCKET;
            SOCKET udp_socket_ = INVALID_SOCKET;
            std::atomic<bool> running_{false};
            std::thread accept_thread_{};
            std::thread udp_thread_{};
            std::mutex clients_mtx_{};
            std::unordered_map<SOCKET, std::shared_ptr<server_client>> clients_{};
            std::mutex threads_mtx_{};
            std::vector<std::thread> client_threads_{};
            std::string password_{};
            std::string join_key_{};
            std::uint16_t port_{};
            std::mutex udp_auth_mtx_{};
            std::set<std::string> authenticated_udp_peers_{};
        };
    }

    struct winsock_transport::impl
    {
        winsock_transport_options options_{};
        std::mutex options_mtx_{};
        transport_evt_listener* listener_ = nullptr;
        std::mutex listener_mtx_{};
        bool wsa_ok_ = false;

        std::unique_ptr<server> server_{};

        SOCKET client_socket_ = INVALID_SOCKET;
        std::thread recv_thread_{};
        std::atomic<bool> recv_running_{false};
        line_receiver receiver_{};

        // UDP voice
        SOCKET udp_socket_ = INVALID_SOCKET;
        sockaddr_in udp_server_addr_{};
        std::thread udp_recv_thread_{};
        std::atomic<bool> udp_recv_running_{false};
        std::uint16_t voice_port_{0};
    };

    winsock_transport::winsock_transport(winsock_transport_options options)
        : impl_(std::make_unique<impl>())
    {
        impl_->options_ = std::move(options);
        WSADATA wsa{};
        impl_->wsa_ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }

    winsock_transport::~winsock_transport()
    {
        (void)disconnect().get();
        if (impl_->wsa_ok_)
        {
            WSACleanup();
        }
    }

    winsock_transport::winsock_transport(winsock_transport&&) noexcept = default;
    winsock_transport& winsock_transport::operator=(winsock_transport&&) noexcept = default;

    void winsock_transport::set_listener(transport_evt_listener* listener)
    {
        std::lock_guard lock(impl_->listener_mtx_);
        impl_->listener_ = listener;
    }

    void winsock_transport::set_display_name(std::string display_name) const
    {
        if (display_name.empty())
        {
            display_name = "LocalUser";
        }

        std::lock_guard lock(impl_->options_mtx_);
        impl_->options_.display_name = std::move(display_name);
    }

    std::future<bool> winsock_transport::connect()
    {
        if (!impl_->wsa_ok_)
        {
            if (impl_->listener_)
            {
                impl_->listener_->on_transport_error("WSAStartup failed.");
            }
            return make_ready_bool(false);
        }
        if (impl_->listener_)
        {
            impl_->listener_->on_transport_connected();
        }
        log_net("Transport initialized (Winsock ready).");
        return make_ready_bool(true);
    }

    std::future<void> winsock_transport::disconnect()
    {
        impl_->recv_running_.store(false, std::memory_order_release);
        impl_->udp_recv_running_.store(false, std::memory_order_release);

        if (impl_->client_socket_ != INVALID_SOCKET)
        {
            shutdown(impl_->client_socket_, SD_BOTH);
            closesocket(impl_->client_socket_);
            impl_->client_socket_ = INVALID_SOCKET;
        }
        if (impl_->udp_socket_ != INVALID_SOCKET)
        {
            closesocket(impl_->udp_socket_);
            impl_->udp_socket_ = INVALID_SOCKET;
        }
        if (impl_->recv_thread_.joinable())
        {
            impl_->recv_thread_.join();
        }
        if (impl_->udp_recv_thread_.joinable())
        {
            impl_->udp_recv_thread_.join();
        }

        if (impl_->server_)
        {
            impl_->server_->stop();
            impl_->server_.reset();
        }

        impl_->voice_port_ = 0;

        if (impl_->listener_)
        {
            impl_->listener_->on_transport_disconnected();
        }

        return make_ready_void();
    }

    std::future<bool> winsock_transport::create_room()
    {
        const auto env_ip = host_ip_from_env();
        const auto public_ip = env_ip.empty() ? guess_public_ip() : std::string{};
        const auto local_ip = guess_local_ip();
        const auto advertised_ip = !env_ip.empty() ? env_ip : (!public_ip.empty() ? public_ip : local_ip);
        log_net("Selected host IP for room code: " + advertised_ip + (!env_ip.empty() ? " (from FOX_CHAT_HOST_IP)" : (!public_ip.empty() ?
            " (auto-detected public IPv4)" : " (fallback local IPv4)")));
        if (env_ip.empty() && public_ip.empty())
        {
            log_net("Could not auto-detect a public IPv4 set FOX_CHAT_HOST_IP to your public address for internet clients.");
        }
        log_net("Embedding dual endpoints in room key (public=" + advertised_ip + ", lan=" + local_ip + ").");
        const auto code = make_join_key(advertised_ip, local_ip, impl_->options_.host_port, impl_->options_.room_password);

        impl_->server_ = std::make_unique<server>(impl_->options_.room_password, code, impl_->options_.host_port);
        if (!impl_->server_->start())
        {
            if (impl_->listener_)
            {
                impl_->listener_->on_transport_error("Failed to start host server.");
            }
            return make_ready_bool(false);
        }

        auto join_future = join_room(code);
        if (!join_future.get())
        {
            return make_ready_bool(false);
        }

        room_state state{};
        state.id = "room-host";
        state.code = code;
        state.title = "Fox Chat";
        state.transport = transport_state::online;
        state.users.push_back(local_user_id);
        if (impl_->listener_)
        {
            impl_->listener_->on_room_joined(state);
        }

        return make_ready_bool(true);
    }

    std::future<bool> winsock_transport::join_room(join_code code)
    {
        const auto endpoint = parse_join_key(code, impl_->options_.room_password);
        if (!endpoint)
        {
            if (impl_->listener_)
            {
                impl_->listener_->on_transport_error("Invalid join key or wrong room password.");
            }
            return make_ready_bool(false);
        }

        const bool is_local_host_join = static_cast<bool>(impl_->server_);

        std::vector<std::pair<std::string, std::uint16_t>> endpoints = *endpoint;
        if (is_local_host_join)
        {
            const std::uint16_t local_port = endpoints.front().second;
            endpoints.clear();
            endpoints.emplace_back("127.0.0.1", local_port);
            log_net("Host local client attaching via 127.0.0.1:" + std::to_string(local_port) + " (room key also includes LAN/public endpoints for remote clients)");
        }

        std::string connect_ip;
        std::uint16_t port = 0;
        bool connected = false;
        int last_wsa_error = 0;
        for (const auto& [candidate_ip, candidate_port] : endpoints)
        {
            connect_ip = candidate_ip;
            port = candidate_port;
            impl_->client_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (impl_->client_socket_ == INVALID_SOCKET)
            {
                return make_ready_bool(false);
            }
            setsockopt(impl_->client_socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recv_timeout_ms), sizeof(recv_timeout_ms));

            log_net("Attempting to join " + connect_ip + ":" + std::to_string(port));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            inet_pton(AF_INET, connect_ip.c_str(), &addr.sin_addr);

            if (::connect(impl_->client_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            {
                connected = true;
                break;
            }

            last_wsa_error = WSAGetLastError();
            log_net("TCP connect failed to " + connect_ip + ":" + std::to_string(port) + " (WSA=" + std::to_string(last_wsa_error) + ")");
            closesocket(impl_->client_socket_);
            impl_->client_socket_ = INVALID_SOCKET;
        }

        if (!connected)
        {
            if (impl_->listener_)
            {
                impl_->listener_->on_transport_error("Unable to connect to host (WSA=" + std::to_string(last_wsa_error) + "). Check host IP/port forwarding and firewall.");
            }
            return make_ready_bool(false);
        }
        log_net("TCP connected to host.");

        std::string display_name;
        {
            std::lock_guard lock(impl_->options_mtx_);
            display_name = impl_->options_.display_name;
        }

        if (!send_all(impl_->client_socket_, code))
        {
            closesocket(impl_->client_socket_);
            impl_->client_socket_ = INVALID_SOCKET;
            log_net("Failed to send TCP join key to host.");
            return make_ready_bool(false);
        }

        const std::string handshake = "PASS " + impl_->options_.room_password + " " + display_name + "\n";
        if (!send_all(impl_->client_socket_, handshake))
        {
            closesocket(impl_->client_socket_);
            impl_->client_socket_ = INVALID_SOCKET;
            return make_ready_bool(false);
        }

        char response[256]{};
        const int received = recv(impl_->client_socket_, response, static_cast<int>(sizeof(response) - 1), 0);
        if (received <= 0 || std::string(response, response + received).find("OK") == std::string::npos)
        {
            closesocket(impl_->client_socket_);
            impl_->client_socket_ = INVALID_SOCKET;
            if (impl_->listener_)
            {
                impl_->listener_->on_transport_error("Server rejected join handshake.");
            }
            return make_ready_bool(false);
        }

        impl_->recv_running_.store(true, std::memory_order_release);
        impl_->recv_thread_ = std::thread([this]() {
            char buf[1024];
            while (impl_->recv_running_.load(std::memory_order_acquire))
            {
                int got = recv(impl_->client_socket_, buf, sizeof(buf), 0);
                if (got <= 0)
                {
                    if (got == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT)
                    {
                        continue;
                    }
                    break;
                }
                impl_->receiver_.feed(buf, got);
                while (auto line = impl_->receiver_.get_line())
                {
                    if (impl_->listener_)
                    {
                        message msg{};
                        msg.from = "room";
                        msg.text = *line;
                        msg.timestamp = std::chrono::system_clock::now();
                        impl_->listener_->on_message_received(msg);
                    }
                }
            }
            impl_->recv_running_.store(false, std::memory_order_release);
        });

        // UDP voice socket
        const auto udp_port = static_cast<std::uint16_t>(port + 1);
        impl_->voice_port_ = udp_port;

        impl_->udp_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (impl_->udp_socket_ != INVALID_SOCKET)
        {
            impl_->udp_server_addr_ = {};
            impl_->udp_server_addr_.sin_family = AF_INET;
            impl_->udp_server_addr_.sin_port = htons(udp_port);
            inet_pton(AF_INET, connect_ip.c_str(), &impl_->udp_server_addr_.sin_addr);

            // Send a registration packet so the server who knows our UDP address
            const std::string reg = code + "VOICEREG";
            sendto(impl_->udp_socket_, reg.data(), static_cast<int>(reg.size()), 0,
                reinterpret_cast<sockaddr*>(&impl_->udp_server_addr_),
                sizeof(impl_->udp_server_addr_));

            impl_->udp_recv_running_.store(true, std::memory_order_release);
            impl_->udp_recv_thread_ = std::thread([this]() {
                char ubuf[4096];
                while (impl_->udp_recv_running_.load(std::memory_order_acquire))
                {
                    fd_set read_set;
                    FD_ZERO(&read_set);
                    FD_SET(impl_->udp_socket_, &read_set);

                    timeval tv{};
                    tv.tv_sec = 0;
                    tv.tv_usec = select_timeout_us;

                    const int ready = select(0, &read_set, nullptr, nullptr, &tv);
                    if (ready <= 0) continue;

                    sockaddr_in from{};
                    int from_len = sizeof(from);
                    const int got = recvfrom(impl_->udp_socket_, ubuf, sizeof(ubuf), 0,
                        reinterpret_cast<sockaddr*>(&from), &from_len);
                    if (got <= 0) continue;

                    if (impl_->listener_)
                    {
                        std::vector<std::uint8_t> data(ubuf, ubuf + got);
                        impl_->listener_->on_voice_data_received(data);
                    }
                }
            });
        }

        room_state state{};
        state.id = "room-joined";
        state.code = std::move(code);
        state.title = "Fox Chat";
        state.transport = transport_state::online;
        state.users.push_back(local_user_id);
        if (impl_->listener_)
        {
            impl_->listener_->on_room_joined(state);
        }
        return make_ready_bool(true);
    }

    std::future<void> winsock_transport::leave_room()
    {
        return disconnect();
    }

    std::future<bool> winsock_transport::send_text(message msg)
    {
        if (impl_->client_socket_ == INVALID_SOCKET)
        {
            return make_ready_bool(false);
        }
        if (msg.text.empty())
        {
            return make_ready_bool(true);
        }
        if (msg.from.empty())
        {
            std::lock_guard lock(impl_->options_mtx_);
            msg.from = impl_->options_.display_name;
        }

        const bool ok = send_all(impl_->client_socket_, msg.text + "\n");
        return make_ready_bool(ok);
    }

    bool winsock_transport::send_voice_data(const std::vector<std::uint8_t>& data)
    {
        if (impl_->udp_socket_ == INVALID_SOCKET || data.empty())
        {
            return false;
        }

        const int sent = sendto(impl_->udp_socket_,
            reinterpret_cast<const char*>(data.data()),
            static_cast<int>(data.size()), 0,
            reinterpret_cast<sockaddr*>(&impl_->udp_server_addr_),
            sizeof(impl_->udp_server_addr_));
        return sent > 0;
    }

    std::uint16_t winsock_transport::voice_port() const
    {
        return impl_->voice_port_;
    }
}
