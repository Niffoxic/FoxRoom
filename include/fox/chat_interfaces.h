#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fox
{
    using user_id   = std::uint64_t;
    using join_code = std::string;
    using room_id   = std::string;

    enum class role
    {
        host,
        participant,
        observer
    };

    enum class transport_state
    {
        offline,
        connecting,
        online
    };

    struct message
    {
        std::string from{};
        std::string text{};
        std::chrono::system_clock::time_point timestamp{};
    };

    struct room_state
    {
        room_id         id          {};
        join_code       code        {};
        std::string     title       {};
        transport_state transport   = transport_state::offline;
        bool            in_voice    = false;

        std::vector<user_id> users{};
    };

    struct chat_config
    {
        std::string display_name    {};
        std::string server_url      {};
        std::string preferred_font  {};
        std::string preferred_theme {};
        bool voice_enabled = false;
    };

    struct theme_spec
    {
        std::string name{};
    };

    class transport_evt_listener
    {
    public:
        virtual ~transport_evt_listener() = default;

        virtual void on_transport_connected     () = 0;
        virtual void on_transport_disconnected  () = 0;

        virtual void on_transport_error (const std::string& error)  = 0;
        virtual void on_room_joined     (const room_state& state)   = 0;
        virtual void on_room_left() = 0;

        virtual void on_message_received    (const message& msg)                    = 0;
        virtual void on_voice_data_received (const std::vector<std::uint8_t>& data) = 0;

        virtual void on_user_joined (user_id id) = 0;
        virtual void on_user_left   (user_id id) = 0;
    };

    class ITransportAdapter
    {
    public:
        virtual ~ITransportAdapter() = default;

        virtual void set_listener(transport_evt_listener* listener) = 0;

        [[nodiscard]] virtual std::future<bool> connect    () = 0;
        [[nodiscard]] virtual std::future<void> disconnect () = 0;
        [[nodiscard]] virtual std::future<bool> create_room() = 0;
        [[nodiscard]] virtual std::future<void> leave_room () = 0;

        [[nodiscard]] virtual std::future<bool> join_room(join_code code) = 0;
        [[nodiscard]] virtual std::future<bool> send_text(message msg)    = 0;
        [[nodiscard]] virtual bool send_voice_data(const std::vector<std::uint8_t>& data) = 0;
        [[nodiscard]] virtual std::uint16_t voice_port() const = 0;
    };

    class ITextChat
    {
    public:
        virtual ~ITextChat() = default;

        virtual void add_message(const message& msg) = 0;
        [[nodiscard]] virtual std::span<const message> get_messages() const = 0;
        virtual void clear() = 0;
    };

    using voice_send_callback = std::function<bool(const std::vector<std::uint8_t>&)>;

    class IVoiceChat
    {
    public:
        virtual ~IVoiceChat() = default;

        virtual void set_send_callback(voice_send_callback cb) = 0;
        [[nodiscard]] virtual std::future<bool> start_capture() = 0;
        [[nodiscard]] virtual std::future<void> stop_capture() = 0;
        [[nodiscard]] virtual bool active() const = 0;
        virtual void on_voice_data(const std::vector<std::uint8_t>& encoded_packet) = 0;
        [[nodiscard]] virtual bool is_muted() const = 0;
        virtual void set_muted(bool muted) = 0;
    };

    class IPlaylist
    {
    public:
        virtual ~IPlaylist() = default;

        virtual void add_track(std::string path) = 0;
        [[nodiscard]] virtual std::size_t track_count() const = 0;
        [[nodiscard]] virtual std::optional<std::string> track_at(std::size_t index) const = 0;
        virtual void clear() = 0;
        [[nodiscard]] virtual std::optional<std::string> current_track() const = 0;
        virtual bool load_from_directory(const std::string& directory_path) = 0;
        virtual bool play_track(std::size_t index) = 0;
        virtual bool play_next() = 0;
        virtual bool pause_or_resume() = 0;
        virtual void stop() = 0;
        [[nodiscard]] virtual bool is_playing() const = 0;
        [[nodiscard]] virtual bool is_paused() const = 0;
        virtual void set_volume(float volume) = 0;
        [[nodiscard]] virtual float volume() const = 0;
        virtual bool seek_seconds(float position_seconds) = 0;
        virtual void set_looping(bool looping) = 0;
        [[nodiscard]] virtual bool looping() const = 0;
        virtual void set_random(bool random_mode) = 0;
        [[nodiscard]] virtual bool random() const = 0;
        virtual bool play_random() = 0;
        virtual void update_playback() = 0;
        [[nodiscard]] virtual float length_seconds() const = 0;
        [[nodiscard]] virtual float position_seconds() const = 0;
    };

    class IFontManager
    {
    public:
        virtual ~IFontManager() = default;

        [[nodiscard]] virtual std::vector<std::string> get_available_fonts() const = 0;
        virtual void set_font(const std::string& font_name) = 0;
        virtual void set_font_size(float font_size) = 0;
        [[nodiscard]] virtual float current_font_size() const = 0;
        [[nodiscard]] virtual std::string current_font() const = 0;
        [[nodiscard]] virtual bool apply() = 0;
    };

    class IThemeManager
    {
    public:
        virtual ~IThemeManager() = default;

        [[nodiscard]] virtual std::vector<std::string> get_available_themes() const = 0;
        [[nodiscard]] virtual std::string get_current() const = 0;
        virtual void set_current(const std::string& theme_name) = 0;
        virtual bool apply(const theme_spec& spec) = 0;
    };

    class ISettingsStore
    {
    public:
        virtual ~ISettingsStore() = default;

        [[nodiscard]] virtual std::future<bool> load() = 0;
        [[nodiscard]] virtual std::future<bool> save() const = 0;

        virtual bool set_string(const std::string& key, std::string value) = 0;
        [[nodiscard]] virtual std::optional<std::string> get_string(const std::string& key) const = 0;

        virtual bool set_bool(const std::string& key, bool value) = 0;
        [[nodiscard]] virtual std::optional<bool> get_bool(const std::string& key) const = 0;

        virtual bool set_int(const std::string& key, int value) = 0;
        [[nodiscard]] virtual std::optional<int> get_int(const std::string& key) const = 0;

        virtual bool set_float(const std::string& key, float value) = 0;
        [[nodiscard]] virtual std::optional<float> get_float(const std::string& key) const = 0;
    };
}
