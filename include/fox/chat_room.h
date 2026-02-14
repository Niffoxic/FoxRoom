#pragma once

#include <future>
#include <memory>
#include <optional>
#include <string>

#include "fox/chat_interfaces.h"

namespace fox
{
    struct chat_room_params
    {
        chat_config config{};
        std::shared_ptr<ITransportAdapter> transport{};
        std::shared_ptr<ITextChat> text_chat{};
        std::shared_ptr<IVoiceChat> voice_chat{};
        std::shared_ptr<IPlaylist> playlist{};
        std::shared_ptr<IFontManager> font_manager{};
        std::shared_ptr<IThemeManager> theme_manager{};
        std::shared_ptr<ISettingsStore> settings_store{};
    };

    class chat_room final : public transport_evt_listener
    {
    public:
        explicit chat_room(chat_room_params params = {});
        ~chat_room() override;

        chat_room(const chat_room&) = delete;
        chat_room& operator=(const chat_room&) = delete;
        chat_room(chat_room&&) noexcept;
        chat_room& operator=(chat_room&&) noexcept;

        void set_transport(std::shared_ptr<ITransportAdapter> transport);

        [[nodiscard]] std::future<bool> request_connect() const;
        [[nodiscard]] std::future<void> request_disconnect() const;
        [[nodiscard]] std::future<bool> request_create_room() const;
        [[nodiscard]] std::future<bool> request_join_room(join_code code) const;
        [[nodiscard]] std::future<void> request_leave_room() const;
        [[nodiscard]] std::future<bool> request_send_text(message msg) const;

        [[nodiscard]] room_state snapshot_room_state() const;
        [[nodiscard]] std::optional<std::string> last_error() const;

        void on_text(user_id uid, std::string text);

        void imgui_render();
        void imgui_main_menu() const;

        void on_transport_connected() override;
        void on_transport_disconnected() override;
        void on_transport_error(const std::string& error) override;
        void on_room_joined(const room_state& state) override;
        void on_room_left() override;
        void on_message_received(const message& msg) override;
        void on_user_joined(user_id id) override;
        void on_user_left(user_id id) override;
        void on_voice_data_received(const std::vector<std::uint8_t>& data) override;

    private:
        void imgui_draw_popups_() const;

    private:
        struct impl;
        std::unique_ptr<impl> impl_{};
    };
}
