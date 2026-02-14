#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace fox
{
    class imgui_hook final
    {
    public:
        using view_fn = std::function<void()>;
        using menu_fn = std::function<void()>;

        void rebuild_fonts_(float scale) noexcept;

        static imgui_hook& instance() noexcept;

        imgui_hook(const imgui_hook&) = delete;
        imgui_hook& operator=(const imgui_hook&) = delete;

        void init(void* hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx) noexcept;
        void shutdown() noexcept;

        void set_enabled(bool e) noexcept;
        [[nodiscard]] bool enabled() const noexcept;
        [[nodiscard]] bool initialized() const noexcept;

        [[nodiscard]] bool message_pump(void* hwnd, std::uint32_t msg,
            std::uint64_t wp, std::int64_t lp) noexcept;

        void set_view(view_fn fn);
        void set_view_title(std::string title);
        void clear_view() noexcept;

        void add_main_menu(std::string id, menu_fn fn);
        void remove_main_menu(const std::string& id) noexcept;
        void clear_main_menus() noexcept;

        void begin_frame(float dt_seconds) noexcept;
        void render() noexcept;

        void on_resize(std::uint32_t client_w, std::uint32_t client_h) noexcept;
        void on_dpi_changed(std::uint32_t dpi, float scale = 0.0f) noexcept;
        void on_device_lost_or_resize_begin() noexcept;
        void on_device_lost_or_resize_end() noexcept;
        void invalidate_device_objects() const noexcept;
        void recreate_device_objects() const noexcept;

        [[nodiscard]] bool reload_font(const std::string& font_path, float font_size_px) noexcept;
        [[nodiscard]] std::string current_font_path() const;
        [[nodiscard]] float current_font_size() const noexcept;

        void refresh_display_size_from_hwnd() noexcept;

        [[nodiscard]] std::uint32_t last_client_w() const noexcept;
        [[nodiscard]] std::uint32_t last_client_h() const noexcept;

    private:
        imgui_hook() = default;
        ~imgui_hook() = default;

        struct menu_entry
        {
            std::string id;
            menu_fn fn;
        };

        void apply_display_size_(std::uint32_t w, std::uint32_t h) noexcept;

        mutable std::mutex mtx_{};
        bool initialized_ = false;
        bool enabled_ = true;

        void* hwnd_ = nullptr;
        ID3D11Device* dev_ = nullptr;
        ID3D11DeviceContext* ctx_ = nullptr;

        std::uint32_t last_w_ = 0;
        std::uint32_t last_h_ = 0;

        float dpi_scale_ = 3.0f;
        float base_font_px_ = 32.0f;
        std::string font_path_ = "FoxChat/fonts/Rubik-Bold.ttf";
        bool style_scaled_ = false;
        bool font_reload_pending_ = false;

        std::string view_title_ = "View";
        view_fn view_{};
        std::vector<menu_entry> main_menus_{};
    };
}
