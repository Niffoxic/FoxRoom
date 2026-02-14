#pragma once

#include "fox/chat_interfaces.h"

#include <mutex>
#include <string>
#include <vector>

namespace fox
{
    class theme_manager final : public IThemeManager
    {
    public:
        theme_manager();

        [[nodiscard]] std::vector<std::string> get_available_themes() const override;
        [[nodiscard]] std::string get_current() const override;
        void set_current(const std::string& theme_name) override;
        bool apply(const theme_spec& spec) override;

    private:
        [[nodiscard]] static bool apply_theme_style(const std::string& theme_name);

        mutable std::mutex mtx_{};
        std::vector<std::string> available_themes_{};
        std::string current_theme_{};
    };
}
