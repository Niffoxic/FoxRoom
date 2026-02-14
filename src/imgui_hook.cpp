#include "imgui_hook.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

extern LRESULT CALLBACK ImGui_ImplWin32_WndProcHandler(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
);

namespace fox
{
    static float clamp_scale(float s) noexcept
    {
        if (s < 0.75f) return 0.75f;
        if (s > 3.0f)  return 3.0f;
        return s;
    }

    void imgui_hook::rebuild_fonts_(float scale) noexcept
    {
        if (!ImGui::GetCurrentContext())
            return;

        scale = clamp_scale(scale);
        dpi_scale_ = scale;

        ImGuiStyle style;
        ImGui::StyleColorsDark(&style);

        if (style_scaled_)
        {
            const ImGuiStyle& current_style = ImGui::GetStyle();
            for (int i = 0; i < ImGuiCol_COUNT; ++i)
                style.Colors[i] = current_style.Colors[i];
        }

        style.ScaleAllSizes(dpi_scale_);
        ImGui::GetStyle() = style;
        style_scaled_ = true;

        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();

        const float font_px = base_font_px_ * dpi_scale_;

        io.FontDefault = nullptr;
        if (!font_path_.empty())
        {
            const std::filesystem::path path(font_path_);
            std::error_code ec;
            const bool font_file_available = std::filesystem::exists(path, ec)
                && std::filesystem::is_regular_file(path, ec);
            if (!ec && font_file_available)
            {
                if (FILE* file = std::fopen(font_path_.c_str(), "rb"))
                {
                    std::fclose(file);
                    io.FontDefault = io.Fonts->AddFontFromFileTTF(font_path_.c_str(), font_px);
                }
            }
        }

        if (!io.FontDefault)
            io.FontDefault = io.Fonts->AddFontDefault();

        io.Fonts->Build();
    }

    imgui_hook& imgui_hook::instance() noexcept
    {
        static imgui_hook g;
        return g;
    }

    void imgui_hook::init(void* hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx) noexcept
    {
        std::lock_guard lk(mtx_);

        if (initialized_)
            return;

        hwnd_ = hwnd;
        dev_  = dev;
        ctx_  = ctx;

        if (!hwnd_ || !dev_ || !ctx_)
            return;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGui::StyleColorsDark();

        ImGui_ImplWin32_Init((HWND)hwnd_);
        ImGui_ImplDX11_Init(dev_, ctx_);

        initialized_ = true;
        enabled_ = true;

        UINT dpi = 96;
        const auto hw = static_cast<HWND>(hwnd_);

        if (const HMODULE user32 = ::GetModuleHandleW(L"user32.dll"))
        {
            using get_dpi_for_window_fn = UINT(WINAPI*)(HWND);
            if (auto pGetDpiForWindow = reinterpret_cast<get_dpi_for_window_fn>(::GetProcAddress(user32, "GetDpiForWindow")))
                dpi = pGetDpiForWindow(hw);
        }

        const float scale = static_cast<float>(dpi) / 96.0f;
        rebuild_fonts_(scale);
        recreate_device_objects();

        refresh_display_size_from_hwnd();

    }

    void imgui_hook::shutdown() noexcept
    {
        std::lock_guard lk(mtx_);

        if (!initialized_)
            return;

        if (ImGui::GetCurrentContext())
        {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }

        view_ = {};
        view_title_ = "View";
        main_menus_.clear();

        hwnd_ = nullptr;
        dev_  = nullptr;
        ctx_  = nullptr;

        last_w_ = 0;
        last_h_ = 0;
        style_scaled_ = false;

        enabled_     = false;
        initialized_ = false;
    }

    void imgui_hook::set_enabled(bool e) noexcept
    {
        std::lock_guard lk(mtx_);
        enabled_ = e;
    }

    bool imgui_hook::enabled() const noexcept
    {
        std::lock_guard lk(mtx_);
        return enabled_;
    }

    bool imgui_hook::initialized() const noexcept
    {
        std::lock_guard lk(mtx_);
        return initialized_;
    }

    bool imgui_hook::message_pump(void* hwnd, std::uint32_t msg, std::uint64_t wp, std::int64_t lp) noexcept
    {
        if (!enabled_ || !initialized_)
            return false;

        return ImGui_ImplWin32_WndProcHandler(
            (HWND)hwnd,
            (UINT)msg,
            (WPARAM)wp,
            (LPARAM)lp
        ) != 0;
    }

    void imgui_hook::set_view(view_fn fn)
    {
        std::lock_guard lk(mtx_);
        view_ = std::move(fn);
    }

    void imgui_hook::set_view_title(std::string title)
    {
        std::lock_guard lk(mtx_);
        if (title.empty())
            title = "View";
        view_title_ = std::move(title);
    }

    void imgui_hook::clear_view() noexcept
    {
        std::lock_guard lk(mtx_);
        view_ = {};
        view_title_ = "View";
    }

    void imgui_hook::add_main_menu(std::string id, menu_fn fn)
    {
        std::lock_guard lk(mtx_);
        if (id.empty())
            return;

        for (auto& e : main_menus_)
        {
            if (e.id == id)
            {
                e.fn = std::move(fn);
                return;
            }
        }

        menu_entry e{};
        e.id = std::move(id);
        e.fn = std::move(fn);
        main_menus_.push_back(std::move(e));
    }

    void imgui_hook::remove_main_menu(const std::string& id) noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::erase_if(main_menus_,
                      [&](const menu_entry& e)
                      {
                          return e.id == id;
                      });
    }

    void imgui_hook::clear_main_menus() noexcept
    {
        std::lock_guard lk(mtx_);
        main_menus_.clear();
    }

    void imgui_hook::apply_display_size_(std::uint32_t w, std::uint32_t h) noexcept
    {
        if (!ImGui::GetCurrentContext())
            return;

        w = (std::max)(w, 1u);
        h = (std::max)(h, 1u);

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)w, (float)h);

        last_w_ = w;
        last_h_ = h;
    }

    void imgui_hook::on_resize(std::uint32_t client_w, std::uint32_t client_h) noexcept
    {
        std::lock_guard lk(mtx_);

        if (!initialized_)
            return;

        apply_display_size_(client_w, client_h);
    }

    void imgui_hook::on_dpi_changed(std::uint32_t dpi, float scale) noexcept
    {
        std::lock_guard lk(mtx_);

        if (!initialized_ || !ImGui::GetCurrentContext())
            return;

        if (scale <= 0.0f)
            scale = (float)dpi / 96.0f;

        invalidate_device_objects();
        rebuild_fonts_(scale);
        recreate_device_objects();
        refresh_display_size_from_hwnd();
    }

    void imgui_hook::on_device_lost_or_resize_begin() noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);

        if (!initialized_ || !ImGui::GetCurrentContext())
            return;

        invalidate_device_objects();
    }

    void imgui_hook::on_device_lost_or_resize_end() noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);

        if (!initialized_ || !ImGui::GetCurrentContext())
            return;

        recreate_device_objects();
        refresh_display_size_from_hwnd();
    }

    void imgui_hook::invalidate_device_objects() const noexcept
    {
        if (!initialized_ || !ImGui::GetCurrentContext())
            return;

        ImGui_ImplDX11_InvalidateDeviceObjects();
    }

    void imgui_hook::recreate_device_objects() const noexcept
    {
        if (!initialized_ || !ImGui::GetCurrentContext())
            return;

        ImGui_ImplDX11_CreateDeviceObjects();
    }

    bool imgui_hook::reload_font(const std::string& font_path, float font_size_px) noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);

        font_path_ = font_path;
        base_font_px_ = (std::max)(font_size_px, 1.0f);
        font_reload_pending_ = true;

        return initialized_;
    }

    std::string imgui_hook::current_font_path() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return font_path_;
    }

    float imgui_hook::current_font_size() const noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return base_font_px_;
    }

    void imgui_hook::refresh_display_size_from_hwnd() noexcept
    {
        if (!initialized_ || !hwnd_)
            return;

        RECT rc{};
        if (!::GetClientRect(static_cast<HWND>(hwnd_), &rc))
            return;

        const std::uint32_t w = (rc.right > rc.left) ? static_cast<std::uint32_t>(rc.right - rc.left) : 0u;
        const std::uint32_t h = (rc.bottom > rc.top) ? static_cast<std::uint32_t>(rc.bottom - rc.top) : 0u;

        apply_display_size_(w, h);
    }

    std::uint32_t imgui_hook::last_client_w() const noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_w_;
    }

    std::uint32_t imgui_hook::last_client_h() const noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_h_;
    }

    void imgui_hook::begin_frame(float dt_seconds) noexcept
    {
        view_fn view_local;
        std::string view_title_local;
        std::vector<menu_entry> menus_local;

        {
            std::lock_guard<std::mutex> lk(mtx_);

            if (!enabled_ || !initialized_)
                return;

            if (font_reload_pending_)
            {
                font_reload_pending_ = false;
                invalidate_device_objects();
                rebuild_fonts_(dpi_scale_);
                recreate_device_objects();
                refresh_display_size_from_hwnd();
            }

            ImGuiIO& io = ImGui::GetIO();
            if (dt_seconds > 0.0f) io.DeltaTime = dt_seconds;
            else if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;

            refresh_display_size_from_hwnd();

            view_local = view_;
            view_title_local = view_title_;
            menus_local = main_menus_;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Main menu bar
        if (!menus_local.empty())
        {
            if (ImGui::BeginMainMenuBar())
            {
                for (auto& m : menus_local)
                {
                    if (m.fn)
                        m.fn();
                }
                ImGui::EndMainMenuBar();
            }
        }

        {
            ImGuiIO& io = ImGui::GetIO();
            const float menu_bar_h = menus_local.empty() ? 0.0f : ImGui::GetFrameHeight();
            const ImVec2 pos  = ImVec2(0.0f, menu_bar_h);
            const ImVec2 size = ImVec2(io.DisplaySize.x, (std::max)(io.DisplaySize.y - menu_bar_h, 1.0f));

            ImGui::SetNextWindowPos(pos);
            ImGui::SetNextWindowSize(size);

            const ImGuiWindowFlags flags =
                ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoNavFocus;

            ImGui::Begin(view_title_local.c_str(), nullptr, flags);
            if (view_local)
                view_local();
            ImGui::End();
        }

        ImGui::Render();
    }

    void imgui_hook::render() noexcept
    {
        std::lock_guard<std::mutex> lk(mtx_);

        if (!enabled_ || !initialized_)
            return;

        ImDrawData* dd = ImGui::GetDrawData();
        if (!dd)
            return;

        ImGui_ImplDX11_RenderDrawData(dd);
    }
}
