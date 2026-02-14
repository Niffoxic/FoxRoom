#pragma once

#include "fox/chat_interfaces.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <random>
#include <sstream>
#include <string>

namespace fox
{
    class TransportStub final : public ITransportAdapter
    {
    public:
        void set_listener(transport_evt_listener* listener) override
        {
            listener_ = listener;
        }

        [[nodiscard]] std::future<bool> connect() override
        {
            if (listener_)
            {
                listener_->on_transport_connected();
            }
            return make_ready(true);
        }

        [[nodiscard]] std::future<void> disconnect() override
        {
            if (listener_)
            {
                listener_->on_transport_disconnected();
            }
            std::promise<void> p;
            p.set_value();
            return p.get_future();
        }

        [[nodiscard]] std::future<bool> create_room() override
        {
            auto code = generate_join_code();
            if (listener_)
            {
                room_state state{};
                state.id = "room-stub-1";
                state.code = code;
                state.title = "Stub Room";
                state.transport = transport_state::online;
                state.users.push_back(local_user_id_);
                listener_->on_room_joined(state);
            }
            return make_ready(true);
        }

        [[nodiscard]] std::future<bool> join_room(join_code code) override
        {
            if (listener_)
            {
                room_state state{};
                state.id = "room-stub-joined";
                state.code = code;
                state.title = "Joined Room";
                state.transport = transport_state::online;
                state.users.push_back(local_user_id_);
                listener_->on_room_joined(state);
            }
            return make_ready(true);
        }

        [[nodiscard]] std::future<void> leave_room() override
        {
            if (listener_)
            {
                listener_->on_room_left();
            }
            std::promise<void> p;
            p.set_value();
            return p.get_future();
        }

        [[nodiscard]] std::future<bool> send_text(message msg) override
        {
            if (listener_)
            {
                if (msg.from.empty())
                {
                    msg.from = "LocalUser";
                }
                msg.timestamp = std::chrono::system_clock::now();
                listener_->on_message_received(msg);
            }
            return make_ready(true);
        }

        [[nodiscard]] bool send_voice_data(const std::vector<std::uint8_t>&) override
        {
            return false;
        }

        [[nodiscard]] std::uint16_t voice_port() const override
        {
            return 0;
        }

    private:
        transport_evt_listener* listener_ = nullptr;
        user_id local_user_id_ = 1;

        static std::future<bool> make_ready(bool value)
        {
            std::promise<bool> p;
            p.set_value(value);
            return p.get_future();
        }

        static std::string generate_join_code()
        {
            std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(1000, 9999);
            std::ostringstream oss;
            oss << "FOX-" << dist(rng) << "-" << dist(rng);
            return oss.str();
        }
    };
}
