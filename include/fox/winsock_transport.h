#pragma once

#include "fox/chat_interfaces.h"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace fox
{
    struct winsock_transport_options
    {
        std::string display_name{"LocalUser"};
        std::string room_password{"foxchat"};
        std::uint16_t host_port{48000};
    };

    class winsock_transport final : public ITransportAdapter
    {
    public:
        explicit winsock_transport(winsock_transport_options options = {});
        ~winsock_transport() override;

        winsock_transport(const winsock_transport&) = delete;
        winsock_transport& operator=(const winsock_transport&) = delete;
        winsock_transport(winsock_transport&&) noexcept;
        winsock_transport& operator=(winsock_transport&&) noexcept;

        void set_listener(transport_evt_listener* listener) override;
        void set_display_name(std::string display_name) const;

        [[nodiscard]] std::future<bool> connect() override;
        [[nodiscard]] std::future<void> disconnect() override;
        [[nodiscard]] std::future<bool> create_room() override;
        [[nodiscard]] std::future<bool> join_room(join_code code) override;
        [[nodiscard]] std::future<void> leave_room() override;
        [[nodiscard]] std::future<bool> send_text(message msg) override;
        [[nodiscard]] bool send_voice_data(const std::vector<std::uint8_t>& data) override;
        [[nodiscard]] std::uint16_t voice_port() const override;

    private:
        struct impl;
        std::unique_ptr<impl> impl_{};
    };
}
