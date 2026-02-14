#pragma once

#include "fox/chat_interfaces.h"

#include <memory>

namespace fox
{
    class music_playlist final : public IPlaylist
    {
    public:
        music_playlist();
        ~music_playlist() override;

        music_playlist(const music_playlist&) = delete;
        music_playlist& operator=(const music_playlist&) = delete;
        music_playlist(music_playlist&&) noexcept;
        music_playlist& operator=(music_playlist&&) noexcept;

        void add_track(std::string path) override;
        [[nodiscard]] std::size_t track_count() const override;
        [[nodiscard]] std::optional<std::string> track_at(std::size_t index) const override;
        void clear() override;
        [[nodiscard]] std::optional<std::string> current_track() const override;
        bool load_from_directory(const std::string& directory_path) override;
        bool play_track(std::size_t index) override;
        bool play_next() override;
        bool pause_or_resume() override;
        void stop() override;
        [[nodiscard]] bool is_playing() const override;
        [[nodiscard]] bool is_paused() const override;
        void set_volume(float volume) override;
        [[nodiscard]] float volume() const override;
        bool seek_seconds(float position_seconds) override;
        void set_looping(bool looping) override;
        [[nodiscard]] bool looping() const override;
        void set_random(bool random_mode) override;
        [[nodiscard]] bool random() const override;
        bool play_random() override;
        void update_playback() override;
        [[nodiscard]] float length_seconds() const override;
        [[nodiscard]] float position_seconds() const override;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
}
