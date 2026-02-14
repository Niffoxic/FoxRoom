#pragma once

#include "fox/chat_interfaces.h"

#include <filesystem>
#include <mutex>

namespace fox
{
    class settings_store final : public ISettingsStore
    {
    public:
        settings_store();

        [[nodiscard]] std::future<bool> load() override;
        [[nodiscard]] std::future<bool> save() const override;

        bool set_string(const std::string& key, std::string value) override;
        [[nodiscard]] std::optional<std::string> get_string(const std::string& key) const override;

        bool set_bool(const std::string& key, bool value) override;
        [[nodiscard]] std::optional<bool> get_bool(const std::string& key) const override;

        bool set_int(const std::string& key, int value) override;
        [[nodiscard]] std::optional<int> get_int(const std::string& key) const override;

        bool set_float(const std::string& key, float value) override;
        [[nodiscard]] std::optional<float> get_float(const std::string& key) const override;

    private:
        static constexpr const char* key_last_join_token = "last_join_token";
        static constexpr const char* key_last_username = "last_username";
        static constexpr const char* key_window_visible = "window_visible";
        static constexpr const char* key_window_width = "window_width";
        static constexpr const char* key_window_alpha = "window_alpha";
        static constexpr const char* key_theme_name = "theme_name";
        static constexpr const char* key_font_name = "font_name";
        static constexpr const char* key_font_size = "font_size";

        std::filesystem::path config_path_{};

        mutable std::mutex mtx_{};
        std::optional<std::string> last_join_token_{};
        std::optional<std::string> last_username_{};
        std::optional<bool> window_visible_{};
        std::optional<int> window_width_{};
        std::optional<float> window_alpha_{};
        std::optional<std::string> theme_name_{};
        std::optional<std::string> font_name_{};
        std::optional<float> font_size_{};
    };
}
