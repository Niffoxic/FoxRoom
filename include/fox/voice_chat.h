#pragma once

#include "fox/chat_interfaces.h"

#include <atomic>
#include <future>
#include <memory>

namespace fox
{
    class voice_chat final : public IVoiceChat
    {
    public:
        voice_chat();
        ~voice_chat() override;

        voice_chat(const voice_chat&) = delete;
        voice_chat& operator=(const voice_chat&) = delete;
        voice_chat(voice_chat&&) noexcept;
        voice_chat& operator=(voice_chat&&) noexcept;

        void set_send_callback(voice_send_callback cb) override;
        [[nodiscard]] std::future<bool> start_capture() override;
        [[nodiscard]] std::future<void> stop_capture() override;
        [[nodiscard]] bool active() const override;
        void on_voice_data(const std::vector<std::uint8_t>& encoded_packet) override;
        [[nodiscard]] bool is_muted() const override;
        void set_muted(bool muted) override;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
}
