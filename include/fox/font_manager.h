#pragma once

#include "fox/chat_interfaces.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace fox
{
    class font_manager final : public IFontManager
    {
    public:
        explicit font_manager(std::filesystem::path fonts_dir = std::filesystem::path("FoxChat") / "fonts");

        [[nodiscard]] std::vector<std::string> get_available_fonts() const override;
        void set_font(const std::string& font_name) override;
        void set_font_size(float font_size) override;
        [[nodiscard]] float current_font_size() const override;
        [[nodiscard]] std::string current_font() const override;
        [[nodiscard]] bool apply() override;

    private:
        void rescan_fonts_locked();

        mutable std::mutex mtx_{};
        std::filesystem::path fonts_dir_{};
        std::vector<std::string> available_fonts_{};
        std::string selected_font_{};
        float selected_font_size_ = 24.0F;
    };
}
