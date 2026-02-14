#include "fox/music_playlist.h"

#include <fmod.hpp>
#include <opus.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace fox
{
    namespace
    {
        bool is_supported_music(const std::filesystem::path& path)
        {
            if (!path.has_extension())
                return false;

            std::string ext = path.extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac" || ext == ".opus";
        }
    }

    struct music_playlist::impl
    {
        FMOD::System* system_ = nullptr;
        FMOD::Sound* current_sound_ = nullptr;
        FMOD::Channel* current_channel_ = nullptr;

        std::vector<std::string> tracks_{};
        std::optional<std::size_t> current_track_idx_{};
        mutable std::mutex mtx_{};

        bool paused_ = false;
        bool opus_ready_ = false;
        float volume_ = 1.0F;
        bool looping_ = false;
        bool random_mode_ = false;
        std::mt19937 rng_{std::random_device{}()};

        void release_sound()
        {
            if (current_channel_)
            {
                current_channel_->stop();
                current_channel_ = nullptr;
            }

            if (current_sound_)
            {
                current_sound_->release();
                current_sound_ = nullptr;
            }

            paused_ = false;
        }

        bool play_track_locked(std::size_t index)
        {
            if (!system_ || index >= tracks_.size())
                return false;

            release_sound();

            FMOD::Sound* sound = nullptr;
            if (system_->createSound(tracks_[index].c_str(), FMOD_DEFAULT, nullptr, &sound) != FMOD_OK || !sound)
                return false;

            FMOD::Channel* channel = nullptr;
            if (system_->playSound(sound, nullptr, false, &channel) != FMOD_OK || !channel)
            {
                sound->release();
                return false;
            }

            current_sound_ = sound;
            current_channel_ = channel;
            current_channel_->setVolume(volume_);
            current_track_idx_ = index;
            paused_ = false;
            return true;
        }
    };

    music_playlist::music_playlist()
        : impl_(std::make_unique<impl>())
    {
        if (FMOD::System_Create(&impl_->system_) != FMOD_OK || !impl_->system_)
            return;

        if (impl_->system_->init(256, FMOD_INIT_NORMAL, nullptr) != FMOD_OK)
        {
            impl_->system_->release();
            impl_->system_ = nullptr;
            return;
        }

        int err = OPUS_OK;
        OpusEncoder* encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
        OpusDecoder* decoder = opus_decoder_create(48000, 2, &err);
        if (encoder && decoder && err == OPUS_OK)
            impl_->opus_ready_ = true;

        if (encoder) opus_encoder_destroy(encoder);
        if (decoder) opus_decoder_destroy(decoder);
    }

    music_playlist::~music_playlist()
    {
        if (!impl_)
            return;

        std::lock_guard lock(impl_->mtx_);
        impl_->release_sound();

        if (impl_->system_)
        {
            impl_->system_->close();
            impl_->system_->release();
            impl_->system_ = nullptr;
        }
    }

    music_playlist::music_playlist(music_playlist&&) noexcept = default;
    music_playlist& music_playlist::operator=(music_playlist&&) noexcept = default;

    void music_playlist::add_track(std::string path)
    {
        std::lock_guard lock(impl_->mtx_);
        impl_->tracks_.push_back(std::move(path));
    }

    std::size_t music_playlist::track_count() const
    {
        std::lock_guard lock(impl_->mtx_);
        return impl_->tracks_.size();
    }

    std::optional<std::string> music_playlist::track_at(std::size_t index) const
    {
        std::lock_guard lock(impl_->mtx_);
        if (index >= impl_->tracks_.size())
            return std::nullopt;
        return impl_->tracks_[index];
    }

    void music_playlist::clear()
    {
        std::lock_guard lock(impl_->mtx_);
        impl_->tracks_.clear();
        impl_->current_track_idx_.reset();
        impl_->release_sound();
    }

    std::optional<std::string> music_playlist::current_track() const
    {
        std::lock_guard lock(impl_->mtx_);
        if (!impl_->current_track_idx_.has_value() || impl_->current_track_idx_.value() >= impl_->tracks_.size())
            return std::nullopt;
        return impl_->tracks_[impl_->current_track_idx_.value()];
    }

    bool music_playlist::load_from_directory(const std::string& directory_path)
    {
        std::vector<std::string> found{};
        std::error_code ec{};

        if (!std::filesystem::exists(directory_path, ec))
            return false;

        for (const auto& entry : std::filesystem::directory_iterator(directory_path, ec))
        {
            if (ec)
                break;

            if (entry.is_regular_file() && is_supported_music(entry.path()))
                found.push_back(entry.path().string());
        }

        std::sort(found.begin(), found.end());

        std::lock_guard lock(impl_->mtx_);
        impl_->tracks_ = std::move(found);
        impl_->current_track_idx_.reset();
        impl_->release_sound();
        return true;
    }

    bool music_playlist::play_track(std::size_t index)
    {
        std::lock_guard lock(impl_->mtx_);
        return impl_->play_track_locked(index);
    }

    bool music_playlist::play_next()
    {
        std::lock_guard lock(impl_->mtx_);
        if (impl_->tracks_.empty())
            return false;

        const std::size_t next_index = impl_->current_track_idx_.has_value()
            ? (impl_->current_track_idx_.value() + 1U) % impl_->tracks_.size()
            : 0;

        return impl_->play_track_locked(next_index);
    }

    bool music_playlist::pause_or_resume()
    {
        std::lock_guard lock(impl_->mtx_);
        if (!impl_->current_channel_)
            return false;

        impl_->paused_ = !impl_->paused_;
        impl_->current_channel_->setPaused(impl_->paused_);
        return true;
    }

    void music_playlist::stop()
    {
        std::lock_guard lock(impl_->mtx_);
        impl_->release_sound();
    }

    bool music_playlist::is_playing() const
    {
        std::lock_guard lock(impl_->mtx_);
        if (!impl_->current_channel_)
            return false;

        bool playing = false;
        impl_->current_channel_->isPlaying(&playing);
        return playing && !impl_->paused_;
    }

    bool music_playlist::is_paused() const
    {
        std::lock_guard lock(impl_->mtx_);
        return impl_->paused_;
    }

    void music_playlist::set_volume(float volume)
    {
        std::lock_guard lock(impl_->mtx_);
        impl_->volume_ = std::clamp(volume, 0.0F, 1.0F);
        if (impl_->current_channel_)
            impl_->current_channel_->setVolume(impl_->volume_);
    }

    float music_playlist::volume() const
    {
        std::lock_guard lock(impl_->mtx_);
        return impl_->volume_;
    }

    bool music_playlist::seek_seconds(float position_seconds)
    {
        std::lock_guard lock(impl_->mtx_);
        if (!impl_->current_channel_)
            return false;

        const float clamped = std::max(0.0F, position_seconds);
        const auto ms = static_cast<unsigned int>(clamped * 1000.0F);
        return impl_->current_channel_->setPosition(ms,
            FMOD_TIMEUNIT_MS) == FMOD_OK;
    }

    void music_playlist::set_looping(bool looping)
    {
        std::lock_guard lock(impl_->mtx_);
        impl_->looping_ = looping;
    }

    bool music_playlist::looping() const
    {
        std::lock_guard lock(impl_->mtx_);
        return impl_->looping_;
    }

    void music_playlist::set_random(bool random_mode)
    {
        std::lock_guard lock(impl_->mtx_);
        impl_->random_mode_ = random_mode;
    }

    bool music_playlist::random() const
    {
        std::lock_guard lock(impl_->mtx_);
        return impl_->random_mode_;
    }

    bool music_playlist::play_random()
    {
        std::lock_guard lock(impl_->mtx_);
        if (impl_->tracks_.empty())
            return false;

        std::uniform_int_distribution<std::size_t> dist(0, impl_->tracks_.size() - 1U);
        return impl_->play_track_locked(dist(impl_->rng_));
    }

    void music_playlist::update_playback()
    {
        std::lock_guard lock(impl_->mtx_);
        if (!impl_->system_)
            return;

        impl_->system_->update();

        if (!impl_->current_channel_ || impl_->paused_)
            return;

        bool playing = false;
        impl_->current_channel_->isPlaying(&playing);
        if (playing)
            return;

        if (impl_->tracks_.empty())
            return;

        if (impl_->looping_ && impl_->current_track_idx_.has_value())
        {
            (void)impl_->play_track_locked(impl_->current_track_idx_.value());
            return;
        }

        if (impl_->random_mode_)
        {
            std::uniform_int_distribution<std::size_t> dist(0, impl_->tracks_.size() - 1U);
            (void)impl_->play_track_locked(dist(impl_->rng_));
        }
    }

    float music_playlist::length_seconds() const
    {
        std::lock_guard lock(impl_->mtx_);
        if (!impl_->current_sound_)
            return 0.0F;

        unsigned int ms = 0;
        impl_->current_sound_->getLength(&ms, FMOD_TIMEUNIT_MS);
        return static_cast<float>(ms) / 1000.0F;
    }

    float music_playlist::position_seconds() const
    {
        std::lock_guard lock(impl_->mtx_);
        if (!impl_->current_channel_)
            return 0.0F;

        unsigned int ms = 0;
        impl_->current_channel_->getPosition(&ms, FMOD_TIMEUNIT_MS);
        return static_cast<float>(ms) / 1000.0F;
    }
}
