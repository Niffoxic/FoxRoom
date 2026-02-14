#include "fox/voice_chat.h"

#include <fmod.hpp>
#include <opus.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace fox
{
    namespace
    {
        constexpr int sample_rate = 48000;
        constexpr int channels = 1;
        constexpr int frame_size = 960;
        constexpr int max_packet_size = 4000;
        constexpr int opus_bitrate = 24000;
        constexpr int playback_buffer_seconds = 1;
        constexpr std::uint64_t playback_stale_timeout_ms = 250;

        std::uint64_t steady_now_ms()
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }
    }

    struct voice_chat::impl
    {
        voice_send_callback send_cb_{};
        std::mutex send_cb_mtx_{};

        std::atomic<bool> running_{false};
        std::atomic<bool> muted_{false};
        std::thread capture_thread_{};

        FMOD::System* fmod_system_ = nullptr;

        // Playback
        FMOD::Sound* playback_sound_ = nullptr;
        FMOD::Channel* playback_channel_ = nullptr;
        std::mutex playback_mtx_{};
        std::vector<short> playback_ring_{};
        std::atomic<unsigned int> playback_write_pos_{0};
        std::atomic<std::uint64_t> last_voice_packet_ms_{0};
        std::atomic<bool> playback_has_data_{false};

        // Opus decoder for incoming audio
        OpusDecoder* decoder_ = nullptr;
        std::mutex decoder_mtx_{};

        static std::future<bool> make_ready_bool(bool value)
        {
            std::promise<bool> p;
            p.set_value(value);
            return p.get_future();
        }

        static std::future<void> make_ready_void()
        {
            std::promise<void> p;
            p.set_value();
            return p.get_future();
        }
    };

    voice_chat::voice_chat()
        : impl_(std::make_unique<impl>())
    {
    }

    voice_chat::~voice_chat()
    {
        if (impl_ && impl_->running_.load(std::memory_order_acquire))
        {
            (void)stop_capture().get();
        }
    }

    voice_chat::voice_chat(voice_chat&&) noexcept = default;
    voice_chat& voice_chat::operator=(voice_chat&&) noexcept = default;

    void voice_chat::set_send_callback(voice_send_callback cb)
    {
        std::lock_guard lock(impl_->send_cb_mtx_);
        impl_->send_cb_ = std::move(cb);
    }

    bool voice_chat::active() const
    {
        return impl_->running_.load(std::memory_order_acquire);
    }

    bool voice_chat::is_muted() const
    {
        return impl_->muted_.load(std::memory_order_acquire);
    }

    void voice_chat::set_muted(bool muted)
    {
        impl_->muted_.store(muted, std::memory_order_release);
    }

    std::future<bool> voice_chat::start_capture()
    {
        if (impl_->running_.load(std::memory_order_acquire))
        {
            return impl::make_ready_bool(false);
        }

        impl_->running_.store(true, std::memory_order_release);

        impl_->capture_thread_ = std::thread([this]()
        {
            FMOD::System* system = nullptr;
            if (FMOD::System_Create(&system) != FMOD_OK)
            {
                impl_->running_.store(false, std::memory_order_release);
                return;
            }

            if (system->init(512, FMOD_INIT_NORMAL, nullptr) != FMOD_OK)
            {
                system->release();
                impl_->running_.store(false, std::memory_order_release);
                return;
            }

            impl_->fmod_system_ = system;

            // Check for recording devices
            int num_drivers = 0, connected = 0;
            system->getRecordNumDrivers(&num_drivers, &connected);
            if (num_drivers <= 0)
            {
                system->close();
                system->release();
                impl_->fmod_system_ = nullptr;
                impl_->running_.store(false, std::memory_order_release);
                return;
            }

            // Recording sound in a circular buffer
            FMOD_CREATESOUNDEXINFO exinfo{};
            exinfo.cbsize = sizeof(exinfo);
            exinfo.numchannels = channels;
            exinfo.defaultfrequency = sample_rate;
            exinfo.format = FMOD_SOUND_FORMAT_PCM16;
            exinfo.length = sample_rate * channels * sizeof(short) * 2; // 2 seconds

            FMOD::Sound* record_sound = nullptr;
            system->createSound(nullptr, FMOD_OPENUSER | FMOD_LOOP_NORMAL, &exinfo, &record_sound);
            system->recordStart(0, record_sound, true);

            // Playback sound in a circular buffer for incoming audio
            constexpr unsigned int playback_samples = sample_rate * playback_buffer_seconds;
            {
                std::lock_guard lock(impl_->playback_mtx_);
                impl_->playback_ring_.resize(playback_samples * channels, 0);
                impl_->playback_write_pos_.store(0, std::memory_order_release);
                impl_->last_voice_packet_ms_.store(0, std::memory_order_release);
                impl_->playback_has_data_.store(false, std::memory_order_release);
            }

            FMOD_CREATESOUNDEXINFO pb_exinfo{};
            pb_exinfo.cbsize = sizeof(pb_exinfo);
            pb_exinfo.numchannels = channels;
            pb_exinfo.defaultfrequency = sample_rate;
            pb_exinfo.format = FMOD_SOUND_FORMAT_PCM16;
            pb_exinfo.length = playback_samples * channels * sizeof(short);

            FMOD::Sound* pb_sound = nullptr;
            system->createSound(nullptr, FMOD_OPENUSER | FMOD_LOOP_NORMAL, &pb_exinfo, &pb_sound);
            {
                std::lock_guard lock(impl_->playback_mtx_);
                impl_->playback_sound_ = pb_sound;
            }

            // Start playback
            FMOD::Channel* pb_channel = nullptr;
            system->playSound(pb_sound, nullptr, false, &pb_channel);
            {
                std::lock_guard lock(impl_->playback_mtx_);
                impl_->playback_channel_ = pb_channel;
            }

            // Opus encoder
            int err = OPUS_OK;
            OpusEncoder* encoder = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &err);
            if (err != OPUS_OK || !encoder)
            {
                system->recordStop(0);
                record_sound->release();
                pb_sound->release();
                system->close();
                system->release();
                impl_->fmod_system_ = nullptr;
                impl_->running_.store(false, std::memory_order_release);
                return;
            }
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(opus_bitrate));

            // Opus decoder for incoming audio
            {
                std::lock_guard lock(impl_->decoder_mtx_);
                impl_->decoder_ = opus_decoder_create(sample_rate, channels, &err);
            }

            // Capture loop
            unsigned int last_pos = 0;
            std::vector<short> capture_buffer;
            capture_buffer.reserve(frame_size * 2);
            std::vector<unsigned char> packet(max_packet_size);

            while (impl_->running_.load(std::memory_order_acquire))
            {
                system->update();

                unsigned int pos = 0;
                system->getRecordPosition(0, &pos);

                if (pos != last_pos)
                {
                    const unsigned int bytes_per_sample = sizeof(short) * channels;
                    const unsigned int offset = last_pos * bytes_per_sample;

                    unsigned int sample_count = 0;
                    if (pos > last_pos)
                    {
                        sample_count = pos - last_pos;
                    }
                    else
                    {
                        sample_count = (sample_rate * 2) - last_pos + pos;
                    }
                    const unsigned int length = sample_count * bytes_per_sample;

                    void* ptr1 = nullptr;
                    void* ptr2 = nullptr;
                    unsigned int len1 = 0, len2 = 0;

                    record_sound->lock(offset, length, &ptr1, &ptr2, &len1, &len2);

                    if (ptr1 && len1)
                    {
                        auto* s = static_cast<short*>(ptr1);
                        int samples = static_cast<int>(len1 / sizeof(short));
                        capture_buffer.insert(capture_buffer.end(), s, s + samples);
                    }
                    if (ptr2 && len2)
                    {
                        auto* s = static_cast<short*>(ptr2);
                        int samples = static_cast<int>(len2 / sizeof(short));
                        capture_buffer.insert(capture_buffer.end(), s, s + samples);
                    }

                    record_sound->unlock(ptr1, ptr2, len1, len2);
                    last_pos = pos;
                }

                // Encode full frames and send
                while (capture_buffer.size() >= static_cast<std::size_t>(frame_size))
                {
                    if (!impl_->muted_.load(std::memory_order_acquire))
                    {
                        const int encoded = opus_encode(encoder,
                            capture_buffer.data(),
                            frame_size,
                            packet.data(),
                            static_cast<opus_int32>(packet.size()));

                        if (encoded > 0)
                        {
                            std::vector<std::uint8_t> data(packet.begin(), packet.begin() + encoded);

                            std::lock_guard lock(impl_->send_cb_mtx_);
                            if (impl_->send_cb_)
                            {
                                (void)impl_->send_cb_(data);
                            }
                        }
                    }

                    capture_buffer.erase(capture_buffer.begin(), capture_buffer.begin() + frame_size);
                }

                // Writing any pending decoded audio into the playback sound buffer
                {
                    std::lock_guard lock(impl_->playback_mtx_);
                    if (impl_->playback_sound_ && !impl_->playback_ring_.empty())
                    {
                        const auto ring_size = static_cast<unsigned int>(impl_->playback_ring_.size());

                        if (impl_->playback_has_data_.load(std::memory_order_acquire))
                        {
                            const std::uint64_t last_packet_ms = impl_->last_voice_packet_ms_.load(std::memory_order_acquire);
                            const std::uint64_t now_ms = steady_now_ms();
                            if (last_packet_ms > 0 && (now_ms - last_packet_ms) > playback_stale_timeout_ms)
                            {
                                std::ranges::fill(impl_->playback_ring_, 0);
                                impl_->playback_write_pos_.store(0, std::memory_order_release);
                                impl_->playback_has_data_.store(false, std::memory_order_release);
                            }
                        }

                        // Copy from the ring buffer into the FMOD playback sound
                        void* p1 = nullptr;
                        void* p2 = nullptr;
                        unsigned int l1 = 0, l2 = 0;

                        const unsigned int total_bytes = ring_size * sizeof(short);
                        impl_->playback_sound_->lock(0, total_bytes, &p1, &p2, &l1, &l2);
                        if (p1 && l1)
                        {
                            std::memcpy(p1, impl_->playback_ring_.data(), l1);
                        }
                        impl_->playback_sound_->unlock(p1, p2, l1, l2);
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            // Cleanups
            system->recordStop(0);

            {
                std::lock_guard lock(impl_->decoder_mtx_);
                if (impl_->decoder_)
                {
                    opus_decoder_destroy(impl_->decoder_);
                    impl_->decoder_ = nullptr;
                }
            }

            opus_encoder_destroy(encoder);

            {
                std::lock_guard lock(impl_->playback_mtx_);
                impl_->playback_channel_ = nullptr;
                impl_->playback_sound_ = nullptr;
            }

            record_sound->release();
            pb_sound->release();
            system->close();
            system->release();
            impl_->fmod_system_ = nullptr;
        });

        return impl::make_ready_bool(true);
    }

    std::future<void> voice_chat::stop_capture()
    {
        impl_->running_.store(false, std::memory_order_release);
        if (impl_->capture_thread_.joinable())
        {
            impl_->capture_thread_.join();
        }
        return impl::make_ready_void();
    }

    void voice_chat::on_voice_data(const std::vector<std::uint8_t>& encoded_packet)
    {
        if (encoded_packet.empty()) return;

        std::vector<short> decoded(frame_size * channels);

        int decoded_samples = 0;
        {
            std::lock_guard lock(impl_->decoder_mtx_);
            if (!impl_->decoder_) return;

            decoded_samples = opus_decode(impl_->decoder_,
                encoded_packet.data(),
                static_cast<opus_int32>(encoded_packet.size()),
                decoded.data(),
                frame_size,
                0);
        }

        if (decoded_samples <= 0) return;

        // Writing decoded samples into the playback ring buffer
        {
            std::lock_guard lock(impl_->playback_mtx_);
            if (impl_->playback_ring_.empty()) return;

            const auto ring_size = static_cast<unsigned int>(impl_->playback_ring_.size());
            unsigned int write_pos = impl_->playback_write_pos_.load(std::memory_order_acquire);

            for (int i = 0; i < decoded_samples * channels; ++i)
            {
                impl_->playback_ring_[write_pos] = decoded[static_cast<std::size_t>(i)];
                write_pos = (write_pos + 1) % ring_size;
            }

            impl_->playback_write_pos_.store(write_pos, std::memory_order_release);
            impl_->last_voice_packet_ms_.store(steady_now_ms(), std::memory_order_release);
            impl_->playback_has_data_.store(true, std::memory_order_release);
        }
    }
}
