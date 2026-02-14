#include "fox/chat_room.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "imgui.h"

#include "fox/winsock_transport.h"

namespace fox
{
    struct chat_room::impl
    {
        chat_config config_{};
        room_state room_state_{};
        std::optional<std::string> last_error_{};

        mutable std::shared_mutex state_mtx_{};
        mutable std::mutex transport_mtx_{};
        std::atomic<bool> transport_bound_{false};

        bool force_scroll_{false};
        bool refocus_message_input_ = true;
        std::array<char, 64> join_buf_{};
        std::array<char, 128> username_buf_{};
        std::array<char, 256> msg_buf_{};
        float pending_font_size_ = 24.0F;
        std::string pending_font_name_{};

        bool open_room_popup_ = false;
        bool open_theme_popup_ = false;
        bool open_font_popup_ = false;
        bool open_audio_popup_ = false;
        int selected_music_track_idx_ = 0;

        std::shared_ptr<ITransportAdapter> transport_{};
        std::shared_ptr<ITextChat> text_chat_{};
        std::shared_ptr<IVoiceChat> voice_chat_{};
        std::shared_ptr<IPlaylist> playlist_{};
        std::shared_ptr<IFontManager> font_manager_{};
        std::shared_ptr<IThemeManager> theme_manager_{};
        std::shared_ptr<ISettingsStore> settings_store_{};

        static std::future<bool> make_ready_bool(bool value)
        {
            std::promise<bool> promise;
            auto future = promise.get_future();
            promise.set_value(value);
            return future;
        }

        static std::future<void> make_ready_void()
        {
            std::promise<void> promise;
            auto future = promise.get_future();
            promise.set_value();
            return future;
        }
    };

    namespace
    {
        void sync_transport_display_name(const std::shared_ptr<ITransportAdapter>& transport, const std::string& display_name)
        {
            if (!transport)
                return;

            auto winsock = std::dynamic_pointer_cast<winsock_transport>(transport);
            if (winsock)
                winsock->set_display_name(display_name);
        }

        const char* state_label_from(const room_state& state) noexcept
        {
            if (state.transport == transport_state::connecting) return "connecting";
            if (state.transport == transport_state::online)     return "connected";
            return "disconnected";
        }

        void draw_room_code_copy_row(const room_state& state)
        {
            if (state.code.empty())
                return;

            char room_code_buf[256]{};
            std::snprintf(room_code_buf, sizeof(room_code_buf), "%s", state.code.c_str());

            ImGui::TextUnformatted("Room Code");
            ImGui::SetNextItemWidth(320.0f);
            ImGui::InputText("##room_code_display", room_code_buf, sizeof(room_code_buf), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("Copy"))
                ImGui::SetClipboardText(state.code.c_str());
        }
    }

    chat_room::chat_room(chat_room_params params)
        : impl_(std::make_unique<impl>())
    {
        impl_->config_ = std::move(params.config);
        impl_->transport_ = std::move(params.transport);
        impl_->text_chat_ = std::move(params.text_chat);
        impl_->voice_chat_ = std::move(params.voice_chat);
        impl_->playlist_ = std::move(params.playlist);
        impl_->font_manager_ = std::move(params.font_manager);
        impl_->theme_manager_ = std::move(params.theme_manager);
        impl_->settings_store_ = std::move(params.settings_store);

        if (impl_->transport_)
        {
            impl_->transport_->set_listener(this);
            impl_->transport_bound_.store(true, std::memory_order_release);
        }

        if (!impl_->config_.display_name.empty())
        {
            std::snprintf(impl_->username_buf_.data(), impl_->username_buf_.size(), "%s", impl_->config_.display_name.c_str());
        }

        if (impl_->settings_store_)
        {
            (void)impl_->settings_store_->load().get();

            if (const auto token = impl_->settings_store_->get_string("last_join_token"))
            {
                std::snprintf(impl_->join_buf_.data(), impl_->join_buf_.size(), "%s", token->c_str());
            }

            if (const auto name = impl_->settings_store_->get_string("last_username"))
            {
                impl_->config_.display_name = *name;
                std::snprintf(impl_->username_buf_.data(), impl_->username_buf_.size(), "%s", name->c_str());
            }

            if (const auto theme_name = impl_->settings_store_->get_string("theme_name"))
            {
                impl_->config_.preferred_theme = *theme_name;
                if (impl_->theme_manager_)
                {
                    (void)impl_->theme_manager_->apply(theme_spec{impl_->config_.preferred_theme});
                    impl_->config_.preferred_theme = impl_->theme_manager_->get_current();
                }
            }

            if (const auto font_name = impl_->settings_store_->get_string("font_name"))
            {
                impl_->pending_font_name_ = *font_name;
            }

            if (const auto font_size = impl_->settings_store_->get_float("font_size"))
            {
                impl_->pending_font_size_ = *font_size;
            }
        }

        if (impl_->theme_manager_)
        {
            const std::string selected_theme = impl_->config_.preferred_theme.empty()
                ? impl_->theme_manager_->get_current()
                : impl_->config_.preferred_theme;

            if (impl_->theme_manager_->apply(theme_spec{selected_theme}))
            {
                impl_->config_.preferred_theme = impl_->theme_manager_->get_current();
            }
        }

        if (impl_->font_manager_)
        {
            if (!impl_->pending_font_name_.empty())
            {
                impl_->font_manager_->set_font(impl_->pending_font_name_);
            }
            impl_->font_manager_->set_font_size(impl_->pending_font_size_);
            if (impl_->font_manager_->apply())
            {
                impl_->pending_font_name_ = impl_->font_manager_->current_font();
                impl_->pending_font_size_ = impl_->font_manager_->current_font_size();
                impl_->config_.preferred_font = impl_->pending_font_name_;
            }
        }
    }

    chat_room::~chat_room() = default;
    chat_room::chat_room(chat_room&&) noexcept = default;
    chat_room& chat_room::operator=(chat_room&&) noexcept = default;

    void chat_room::set_transport(std::shared_ptr<ITransportAdapter> transport)
    {
        std::scoped_lock lock(impl_->transport_mtx_);

        if (impl_->transport_)
        {
            impl_->transport_->set_listener(nullptr);
        }

        impl_->transport_ = std::move(transport);
        impl_->transport_bound_.store(false, std::memory_order_release);

        if (impl_->transport_)
        {
            impl_->transport_->set_listener(this);
            impl_->transport_bound_.store(true, std::memory_order_release);
        }
    }

    std::future<bool> chat_room::request_connect() const
    {
        std::lock_guard lock(impl_->transport_mtx_);
        if (!impl_->transport_)
            return impl::make_ready_bool(false);

        {
            std::unique_lock state_lock(impl_->state_mtx_);
            impl_->room_state_.transport = transport_state::connecting;
        }

        sync_transport_display_name(impl_->transport_, impl_->config_.display_name);
        return impl_->transport_->connect();
    }

    std::future<void> chat_room::request_disconnect() const
    {
        std::lock_guard lock(impl_->transport_mtx_);
        if (!impl_->transport_)
            return impl::make_ready_void();

        return impl_->transport_->disconnect();
    }

    std::future<bool> chat_room::request_create_room() const
    {
        std::lock_guard lock(impl_->transport_mtx_);
        if (!impl_->transport_)
            return impl::make_ready_bool(false);

        sync_transport_display_name(impl_->transport_, impl_->config_.display_name);
        return impl_->transport_->create_room();
    }

    std::future<bool> chat_room::request_join_room(join_code code) const
    {
        std::lock_guard lock(impl_->transport_mtx_);
        if (!impl_->transport_)
            return impl::make_ready_bool(false);

        sync_transport_display_name(impl_->transport_, impl_->config_.display_name);
        return impl_->transport_->join_room(std::move(code));
    }

    std::future<void> chat_room::request_leave_room() const
    {
        std::lock_guard lock(impl_->transport_mtx_);
        if (!impl_->transport_)
            return impl::make_ready_void();

        return impl_->transport_->leave_room();
    }

    std::future<bool> chat_room::request_send_text(message msg) const
    {
        std::lock_guard lock(impl_->transport_mtx_);
        if (!impl_->transport_)
            return impl::make_ready_bool(false);

        sync_transport_display_name(impl_->transport_, impl_->config_.display_name);
        return impl_->transport_->send_text(std::move(msg));
    }

    room_state chat_room::snapshot_room_state() const
    {
        std::shared_lock lock(impl_->state_mtx_);
        return impl_->room_state_;
    }

    std::optional<std::string> chat_room::last_error() const
    {
        std::shared_lock lock(impl_->state_mtx_);
        return impl_->last_error_;
    }

    void chat_room::on_text(user_id uid, std::string text)
    {
        if (!impl_->text_chat_ || text.empty())
            return;

        message msg{};
        msg.from = "uid:" + std::to_string(uid);
        msg.text = std::move(text);
        msg.timestamp = std::chrono::system_clock::now();
        impl_->text_chat_->add_message(msg);
        impl_->force_scroll_ = true;
    }

    void chat_room::imgui_main_menu() const
    {
        const auto state = snapshot_room_state();
        const bool is_online  = (state.transport == transport_state::online);

        if (ImGui::BeginMenu("Room"))
        {
            ImGui::Text("State: %s", state_label_from(state));
            ImGui::Separator();

            if (!state.code.empty())
            {
                draw_room_code_copy_row(state);
                ImGui::Separator();
            }

            if (ImGui::MenuItem("Room Controls"))
            {
                impl_->open_room_popup_ = true;
            }

            if (ImGui::MenuItem("Disconnect", nullptr, false, is_online))
            {
                if (impl_->voice_chat_ && impl_->voice_chat_->active())
                {
                    (void)impl_->voice_chat_->stop_capture().get();
                    {
                        std::unique_lock lock(impl_->state_mtx_);
                        impl_->room_state_.in_voice = false;
                    }
                }
                (void)request_leave_room();
                (void)request_disconnect();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Settings"))
        {
            if (ImGui::MenuItem("Theme Config"))
            {
                impl_->open_theme_popup_ = true;
            }

            if (ImGui::MenuItem("Font Config"))
            {
                impl_->open_font_popup_ = true;
            }

            if (ImGui::MenuItem("Save Settings", nullptr, false, impl_->settings_store_ != nullptr))
            {
                impl_->settings_store_->set_string("last_username", impl_->config_.display_name);
                impl_->settings_store_->set_bool("window_visible", true);
                impl_->settings_store_->set_int("window_width", 480);
                impl_->settings_store_->set_float("window_alpha", 1.0F);
                impl_->settings_store_->set_string("theme_name", impl_->config_.preferred_theme);
                impl_->settings_store_->set_string("font_name", impl_->pending_font_name_);
                impl_->settings_store_->set_float("font_size", impl_->pending_font_size_);
                (void)impl_->settings_store_->save();
            }

            if (ImGui::MenuItem("Load Settings", nullptr, false, impl_->settings_store_ != nullptr))
            {
                (void)impl_->settings_store_->load().get();

                if (const auto token = impl_->settings_store_->get_string("last_join_token"))
                    std::snprintf(impl_->join_buf_.data(), impl_->join_buf_.size(), "%s", token->c_str());

                if (const auto name = impl_->settings_store_->get_string("last_username"))
                {
                    impl_->config_.display_name = *name;
                    std::snprintf(impl_->username_buf_.data(), impl_->username_buf_.size(), "%s", name->c_str());
                }

                if (const auto theme_name = impl_->settings_store_->get_string("theme_name"))
                {
                    impl_->config_.preferred_theme = *theme_name;
                    if (impl_->theme_manager_)
                    {
                        (void)impl_->theme_manager_->apply(theme_spec{impl_->config_.preferred_theme});
                        impl_->config_.preferred_theme = impl_->theme_manager_->get_current();
                    }
                }

                if (const auto font_name = impl_->settings_store_->get_string("font_name"))
                    impl_->pending_font_name_ = *font_name;

                if (const auto font_size = impl_->settings_store_->get_float("font_size"))
                    impl_->pending_font_size_ = *font_size;
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Voice Chat"))
        {
            if (ImGui::MenuItem("Status Check"))
            {
                impl_->open_audio_popup_ = true;
            }

            if (impl_->voice_chat_)
            {
                const bool in_voice = impl_->voice_chat_->active();
                const bool muted = impl_->voice_chat_->is_muted();

                ImGui::Separator();
                ImGui::TextUnformatted("Voice Chat Settings");
                ImGui::Separator();

                if (ImGui::MenuItem("Join Voice", nullptr, false, is_online && !in_voice))
                {
                    impl_->voice_chat_->set_send_callback([this](const std::vector<std::uint8_t>& data) -> bool
                    {
                        if (impl_->transport_)
                            return impl_->transport_->send_voice_data(data);
                        return false;
                    });

                    (void)impl_->voice_chat_->start_capture();
                    {
                        std::unique_lock lock(impl_->state_mtx_);
                        impl_->room_state_.in_voice = true;
                    }
                }

                if (ImGui::MenuItem("Leave Voice", nullptr, false, is_online && in_voice))
                {
                    (void)impl_->voice_chat_->stop_capture().get();
                    {
                        std::unique_lock lock(impl_->state_mtx_);
                        impl_->room_state_.in_voice = false;
                    }
                }

                if (ImGui::MenuItem(muted ? "Unmute Mic" : "Mute Mic", nullptr, false, is_online && in_voice))
                {
                    impl_->voice_chat_->set_muted(!muted);
                }
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Music"))
        {
            ImGui::TextDisabled("its local only not networked");
            ImGui::Separator();

            if (impl_->playlist_)
            {
                impl_->playlist_->update_playback();

                if (ImGui::MenuItem("Scan FoxChat/musics"))
                {
                    const bool loaded = impl_->playlist_->load_from_directory("FoxChat/musics");
                    if (!loaded)
                    {
                        std::unique_lock lock(impl_->state_mtx_);
                        impl_->last_error_ = "Unable to scan FoxChat/musics";
                    }
                    impl_->selected_music_track_idx_ = 0;
                }

                float music_volume = impl_->playlist_->volume();
                ImGui::SetNextItemWidth(180.0F);
                if (ImGui::SliderFloat("Volume", &music_volume, 0.0F, 1.0F, "%.2f"))
                {
                    impl_->playlist_->set_volume(music_volume);
                }

                bool looping = impl_->playlist_->looping();
                if (ImGui::Checkbox("Loop", &looping))
                {
                    impl_->playlist_->set_looping(looping);
                }

                bool random_mode = impl_->playlist_->random();
                if (ImGui::Checkbox("Random Mode", &random_mode))
                {
                    impl_->playlist_->set_random(random_mode);
                }

                if (ImGui::MenuItem("Play Random", nullptr,
                    false, impl_->playlist_->track_count() > 0))
                {
                    (void)impl_->playlist_->play_random();
                }

                if (ImGui::MenuItem("Play Next", nullptr,
                    false, impl_->playlist_->track_count() > 0))
                {
                    (void)impl_->playlist_->play_next();
                }

                if (ImGui::MenuItem(impl_->playlist_->is_paused() ? "Resume" :
                    "Pause", nullptr, false,
                    impl_->playlist_->current_track().has_value()))
                {
                    (void)impl_->playlist_->pause_or_resume();
                }

                if (ImGui::MenuItem("Stop", nullptr, false,
                    impl_->playlist_->current_track().has_value()))
                {
                    impl_->playlist_->stop();
                }

                const auto tracks = impl_->playlist_->track_count();
                if (tracks > 0)
                {
                    ImGui::Separator();
                    ImGui::Text("Tracks: %zu", tracks);

                    if (impl_->selected_music_track_idx_ >= static_cast<int>(tracks))
                        impl_->selected_music_track_idx_ = 0;

                    std::string preview = "Select track";
                    if (const auto track = impl_->playlist_->track_at(static_cast<std::size_t>(impl_->selected_music_track_idx_)))
                    {
                        preview = std::filesystem::path(*track).filename().string();
                    }

                    if (ImGui::BeginMenu(preview.c_str()))
                    {
                        for (std::size_t i = 0; i < tracks; ++i)
                        {
                            auto track = impl_->playlist_->track_at(i);
                            if (!track)
                                continue;

                            const std::string label = std::filesystem::path(*track).filename().string();
                            if (ImGui::MenuItem(label.c_str(), nullptr, static_cast<int>(i) == impl_->selected_music_track_idx_))
                            {
                                impl_->selected_music_track_idx_ = static_cast<int>(i);
                            }
                        }
                        ImGui::EndMenu();
                    }

                    if (ImGui::MenuItem("Play Selected"))
                    {
                        (void)impl_->playlist_->play_track(static_cast<std::size_t>(impl_->selected_music_track_idx_));
                    }
                }

                if (const auto current = impl_->playlist_->current_track())
                {
                    ImGui::Separator();
                    const float len = impl_->playlist_->length_seconds();
                    const float pos = impl_->playlist_->position_seconds();
                    const float left = len > pos ? (len - pos) : 0.0F;

                    ImGui::Text("Now: %s", std::filesystem::path(*current).filename().string().c_str());
                    ImGui::Text("State: %s", impl_->playlist_->is_paused() ? "paused" : (impl_->playlist_->is_playing() ? "playing" : "stopped"));
                    ImGui::Text("Volume: %.0f%%", impl_->playlist_->volume() * 100.0F);

                    float seek_seconds = pos;
                    ImGui::SetNextItemWidth(220.0F);
                    if (ImGui::SliderFloat("Seek", &seek_seconds,
                        0.0F, len > 0.0F ? len : 0.0F, "%.1fs"))
                    {
                        (void)impl_->playlist_->seek_seconds(seek_seconds);
                    }

                    ImGui::Text("Elapsed: %.1fs", pos);
                    ImGui::Text("Time left: %.1fs", left);
                }
            }
            else
            {
                ImGui::TextDisabled("(no playlist backend)");
            }

            ImGui::EndMenu();
        }
    }

    void chat_room::imgui_render()
    {
        imgui_draw_popups_();

        const auto state = snapshot_room_state();
        const bool is_online  = (state.transport == transport_state::online);

        ImGui::Text("State: %s", state_label_from(state));
        if (!state.code.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", state.code.c_str());
        }

        if (auto err = last_error())
        {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                "Error: %s", err->c_str());
        }

        ImGui::Separator();

        // Chat log
        {
            const float footer_h = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
            if (ImGui::BeginChild("##chat_log", ImVec2(0, -footer_h), ImGuiChildFlags_Border))
            {
                if (impl_->text_chat_)
                {
                    constexpr float auto_scroll_threshold = 20.0f;
                    const bool was_near_bottom = (ImGui::GetScrollMaxY() - ImGui::GetScrollY()) <= auto_scroll_threshold;

                    const auto msgs = impl_->text_chat_->get_messages();
                    const int count = static_cast<int>(msgs.size());

                    ImGuiListClipper clipper;
                    clipper.Begin(count);
                    while (clipper.Step())
                    {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        {
                            const auto& m = msgs[static_cast<std::size_t>(i)];
                            ImGui::Text("[%s] %s", m.from.c_str(), m.text.c_str());
                        }
                    }

                    if (was_near_bottom || impl_->force_scroll_)
                    {
                        ImGui::SetScrollHereY(1.0f);
                        impl_->force_scroll_ = false;
                    }
                }
                else
                {
                    ImGui::TextDisabled("(no text chat backend)");
                }
            }
            ImGui::EndChild();
        }

        // Message input
        if (is_online)
        {
            const float send_w = ImGui::CalcTextSize("Send").x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - send_w - ImGui::GetStyle().ItemSpacing.x);

            if (impl_->refocus_message_input_)
            {
                ImGui::SetKeyboardFocusHere();
                impl_->refocus_message_input_ = false;
            }

            bool enter_pressed = ImGui::InputText(
                "##msg_input",
                impl_->msg_buf_.data(),
                impl_->msg_buf_.size(),
                ImGuiInputTextFlags_EnterReturnsTrue
            );

            ImGui::SameLine();

            if ((ImGui::Button("Send") || enter_pressed) && impl_->msg_buf_[0] != '\0')
            {
                message msg{};
                msg.text = impl_->msg_buf_.data();
                msg.from = impl_->config_.display_name.empty() ? "LocalUser" : impl_->config_.display_name;
                (void)request_send_text(std::move(msg));
                impl_->msg_buf_[0] = '\0';
                impl_->refocus_message_input_ = true;
            }
        }
        else
        {
            ImGui::TextDisabled("Connect via Room menu to chat.");
        }
    }

    void chat_room::on_transport_connected()
    {
        std::unique_lock lock(impl_->state_mtx_);
        impl_->room_state_.transport = transport_state::online;
    }

    void chat_room::on_transport_disconnected()
    {
        std::unique_lock lock(impl_->state_mtx_);
        impl_->room_state_.transport = transport_state::offline;
        impl_->room_state_.users.clear();
        impl_->room_state_.id.clear();
        impl_->room_state_.code.clear();
    }

    void chat_room::on_transport_error(const std::string& error)
    {
        std::unique_lock lock(impl_->state_mtx_);
        impl_->last_error_ = error;
    }

    void chat_room::on_room_joined(const room_state& state)
    {
        std::unique_lock lock(impl_->state_mtx_);
        impl_->room_state_ = state;

        if (impl_->settings_store_ && !state.code.empty())
        {
            impl_->settings_store_->set_string("last_join_token", state.code);
        }
    }

    void chat_room::on_room_left()
    {
        std::unique_lock lock(impl_->state_mtx_);
        impl_->room_state_.id.clear();
        impl_->room_state_.code.clear();
        impl_->room_state_.title.clear();
        impl_->room_state_.users.clear();
        impl_->room_state_.in_voice = false;
    }

    void chat_room::on_message_received(const message& msg)
    {
        if (impl_->text_chat_)
        {
            impl_->text_chat_->add_message(msg);
            impl_->force_scroll_ = true;
        }
    }

    void chat_room::on_user_joined(user_id id)
    {
        std::unique_lock lock(impl_->state_mtx_);
        if (std::find(impl_->room_state_.users.begin(), impl_->room_state_.users.end(), id) == impl_->room_state_.users.end())
        {
            impl_->room_state_.users.push_back(id);
        }
    }

    void chat_room::on_user_left(user_id id)
    {
        std::unique_lock lock(impl_->state_mtx_);
        std::erase(impl_->room_state_.users, id);
    }

    void chat_room::on_voice_data_received(const std::vector<std::uint8_t>& data)
    {
        if (impl_->voice_chat_)
        {
            impl_->voice_chat_->on_voice_data(data);
        }
    }

    void chat_room::imgui_draw_popups_() const
    {
        if (impl_->open_room_popup_)
        {
            ImGui::OpenPopup("Room Controls");
            impl_->open_room_popup_ = false;
        }

        if (impl_->open_theme_popup_)
        {
            ImGui::OpenPopup("Theme Settings");
            impl_->open_theme_popup_ = false;
        }

        if (impl_->open_font_popup_)
        {
            ImGui::OpenPopup("Font Settings");
            impl_->open_font_popup_ = false;
        }

        if (impl_->open_audio_popup_)
        {
            ImGui::OpenPopup("Status Check");
            impl_->open_audio_popup_ = false;
        }

        const auto state = snapshot_room_state();
        const bool is_offline = (state.transport == transport_state::offline);
        const bool is_online  = (state.transport == transport_state::online);

        if (ImGui::BeginPopupModal("Room Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("State: %s", state_label_from(state));
            ImGui::Separator();

            if (auto err = last_error())
            {
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Error: %s", err->c_str());
                ImGui::Separator();
            }

            ImGui::TextUnformatted("Username");
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::InputTextWithHint("##username_room", "Username",
                impl_->username_buf_.data(), impl_->username_buf_.size()))
            {
                impl_->config_.display_name = impl_->username_buf_.data();
                {
                    std::lock_guard lock(impl_->transport_mtx_);
                    sync_transport_display_name(impl_->transport_, impl_->config_.display_name);
                }
                if (impl_->settings_store_)
                    impl_->settings_store_->set_string("last_username", impl_->config_.display_name);
            }

            ImGui::Separator();

            if (is_offline)
            {
                if (ImGui::Button("Host"))
                {
                    (void)request_connect();
                    (void)request_create_room();
                }

                ImGui::SameLine();

                ImGui::SetNextItemWidth(260.0f);
                ImGui::InputTextWithHint("##join_token_popup", "FOX-1234-5678",
                    impl_->join_buf_.data(), impl_->join_buf_.size());
                ImGui::SameLine();

                if (ImGui::Button("Join") && impl_->join_buf_[0] != '\0')
                {
                    if (impl_->settings_store_)
                        impl_->settings_store_->set_string("last_join_token", impl_->join_buf_.data());

                    (void)request_connect();
                    (void)request_join_room(std::string(impl_->join_buf_.data()));
                }
            }
            else if (is_online)
            {
                draw_room_code_copy_row(state);
                ImGui::Separator();

                if (ImGui::Button("Disconnect"))
                {
                    if (impl_->voice_chat_ && impl_->voice_chat_->active())
                    {
                        (void)impl_->voice_chat_->stop_capture().get();
                        {
                            std::unique_lock lock(impl_->state_mtx_);
                            impl_->room_state_.in_voice = false;
                        }
                    }
                    (void)request_leave_room();
                    (void)request_disconnect();
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Close"))
            {
                ImGui::CloseCurrentPopup();
                impl_->open_room_popup_ = false;
            }

            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Theme Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (impl_->theme_manager_)
            {
                auto themes = impl_->theme_manager_->get_available_themes();
                if (!themes.empty())
                {
                    if (impl_->config_.preferred_theme.empty())
                        impl_->config_.preferred_theme = impl_->theme_manager_->get_current();

                    int current_theme_idx = 0;
                    for (std::size_t i = 0; i < themes.size(); ++i)
                    {
                        if (themes[i] == impl_->config_.preferred_theme)
                        {
                            current_theme_idx = static_cast<int>(i);
                            break;
                        }
                    }

                    std::vector<const char*> theme_names;
                    theme_names.reserve(themes.size());
                    for (const auto& t : themes) theme_names.push_back(t.c_str());

                    ImGui::TextUnformatted("Theme");
                    ImGui::SetNextItemWidth(260.0f);
                    if (ImGui::Combo("##theme_combo", &current_theme_idx, theme_names.data(), static_cast<int>(theme_names.size())))
                    {
                        impl_->config_.preferred_theme = themes[static_cast<std::size_t>(current_theme_idx)];
                        (void)impl_->theme_manager_->apply(theme_spec{impl_->config_.preferred_theme});
                        impl_->config_.preferred_theme = impl_->theme_manager_->get_current();
                        if (impl_->settings_store_)
                            impl_->settings_store_->set_string("theme_name", impl_->config_.preferred_theme);
                    }
                }
            }
            else
            {
                ImGui::TextDisabled("(no theme manager)");
            }

            ImGui::Separator();
            if (ImGui::Button("Close"))
            {
                ImGui::CloseCurrentPopup();
                impl_->open_theme_popup_ = false;
            }

            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Font Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (impl_->font_manager_)
            {
                auto fonts = impl_->font_manager_->get_available_fonts();

                ImGui::TextUnformatted("Font");

                if (!fonts.empty())
                {
                    if (impl_->pending_font_name_.empty())
                        impl_->pending_font_name_ = impl_->font_manager_->current_font();

                    int current_font_idx = 0;
                    for (std::size_t i = 0; i < fonts.size(); ++i)
                    {
                        if (fonts[i] == impl_->pending_font_name_)
                        {
                            current_font_idx = static_cast<int>(i);
                            break;
                        }
                    }

                    std::vector<const char*> font_names;
                    font_names.reserve(fonts.size());
                    for (const auto& f : fonts) font_names.push_back(f.c_str());

                    ImGui::SetNextItemWidth(260.0f);
                    if (ImGui::Combo("##font_combo", &current_font_idx, font_names.data(), static_cast<int>(font_names.size())))
                    {
                        impl_->pending_font_name_ = fonts[static_cast<std::size_t>(current_font_idx)];
                    }
                }

                ImGui::SetNextItemWidth(260.0f);
                ImGui::SliderFloat("##font_size", &impl_->pending_font_size_, 10.0F, 72.0F, "%.1f px");

                if (ImGui::Button("Apply Font"))
                {
                    impl_->font_manager_->set_font(impl_->pending_font_name_);
                    impl_->font_manager_->set_font_size(impl_->pending_font_size_);
                    if (impl_->font_manager_->apply())
                    {
                        impl_->pending_font_name_ = impl_->font_manager_->current_font();
                        impl_->pending_font_size_ = impl_->font_manager_->current_font_size();
                        impl_->config_.preferred_font = impl_->pending_font_name_;

                        if (impl_->settings_store_)
                        {
                            impl_->settings_store_->set_string("font_name", impl_->pending_font_name_);
                            impl_->settings_store_->set_float("font_size", impl_->pending_font_size_);
                        }
                    }
                }
            }
            else
            {
                ImGui::TextDisabled("(no font manager)");
            }

            ImGui::Separator();
            if (ImGui::Button("Close"))
            {
                ImGui::CloseCurrentPopup();
                impl_->open_font_popup_ = false;
            }

            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Status Check", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (impl_->voice_chat_)
            {
                const bool in_voice = impl_->voice_chat_->active();
                const bool muted = impl_->voice_chat_->is_muted();

                ImGui::Text("Voice: %s", in_voice ? "active" : "inactive");
                ImGui::Text("Mic: %s", muted ? "muted" : "on");
            }
            else
            {
                ImGui::TextDisabled("(no voice chat backend)");
            }

            ImGui::Separator();
            if (ImGui::Button("Close"))
            {
                ImGui::CloseCurrentPopup();
                impl_->open_audio_popup_ = false;
            }

            ImGui::EndPopup();
        }
    }

}
