#include "fox/font_manager.h"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>
#include <windows.h>

#include "imgui_hook.h"

namespace fox
{
    namespace
    {
        std::filesystem::path detect_executable_dir()
        {
            std::array<char, MAX_PATH> buffer{};
            const DWORD len = ::GetModuleFileNameA(nullptr, buffer.data(), buffer.size());
            if (len > 0 && len < buffer.size())
            {
                return std::filesystem::path(std::string(buffer.data(), len)).parent_path();
            }
            return {};
        }

        std::filesystem::path resolve_fonts_dir(std::filesystem::path configured)
        {
            auto is_valid_dir = [](const std::filesystem::path& p)
            {
                std::error_code ec;
                return !p.empty() && std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec);
            };

            if (is_valid_dir(configured))
                return configured;

            const std::filesystem::path exe_dir = detect_executable_dir();
            if (!exe_dir.empty())
            {
                const std::filesystem::path exe_relative = exe_dir / configured;
                if (is_valid_dir(exe_relative))
                    return exe_relative;

                const std::filesystem::path sibling_fonts = exe_dir / "fonts";
                if (is_valid_dir(sibling_fonts))
                    return sibling_fonts;
            }

            return configured;
        }

        bool is_ttf_file(const std::filesystem::directory_entry& entry)
        {
            if (!entry.is_regular_file())
            {
                return false;
            }

            std::string ext = entry.path().extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            return ext == ".ttf";
        }
    }

    font_manager::font_manager(std::filesystem::path fonts_dir)
        : fonts_dir_(resolve_fonts_dir(std::move(fonts_dir)))
    {
        std::lock_guard lock(mtx_);
        rescan_fonts_locked();
    }

    std::vector<std::string> font_manager::get_available_fonts() const
    {
        std::lock_guard lock(mtx_);
        const_cast<font_manager*>(this)->rescan_fonts_locked();
        return available_fonts_;
    }

    void font_manager::set_font(const std::string& font_name)
    {
        std::lock_guard lock(mtx_);
        selected_font_ = font_name;
    }

    void font_manager::set_font_size(const float font_size)
    {
        std::lock_guard lock(mtx_);
        selected_font_size_ = std::clamp(font_size, 10.0F, 72.0F);
    }

    float font_manager::current_font_size() const
    {
        std::lock_guard lock(mtx_);
        return selected_font_size_;
    }

    std::string font_manager::current_font() const
    {
        std::lock_guard lock(mtx_);
        return selected_font_;
    }

    bool font_manager::apply()
    {
        std::lock_guard lock(mtx_);
        rescan_fonts_locked();

        if (!selected_font_.empty())
        {
            const auto it = std::find(available_fonts_.begin(),
                available_fonts_.end(), selected_font_);
            if (it == available_fonts_.end())
            {
                selected_font_.clear();
            }
        }

        if (selected_font_.empty() && !available_fonts_.empty())
        {
            selected_font_ = available_fonts_.front();
        }

        const std::filesystem::path selected_font_path = selected_font_.empty()
            ? std::filesystem::path{}
            : fonts_dir_ / selected_font_;

        return imgui_hook::instance().reload_font(selected_font_path.string(),
            selected_font_size_);
    }

    void font_manager::rescan_fonts_locked()
    {
        available_fonts_.clear();

        std::error_code ec;
        if (!std::filesystem::exists(fonts_dir_, ec) ||
            !std::filesystem::is_directory(fonts_dir_, ec))
        {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(fonts_dir_, ec))
        {
            if (ec)
            {
                break;
            }

            if (is_ttf_file(entry))
            {
                available_fonts_.push_back(entry.path().filename().string());
            }
        }

        std::ranges::sort(available_fonts_);
    }
}
