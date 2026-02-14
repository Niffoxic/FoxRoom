#include "fox/theme_manager.h"
#include "imgui.h"

namespace fox
{
    namespace
    {
        constexpr const char* dark_theme = "Dark";
        constexpr const char* light_theme = "Light";
        constexpr const char* high_contrast_theme = "HighContrast";

        constexpr const char* cyberpunk_theme = "Cyberpunk";
        constexpr const char* dracula_theme   = "Dracula";
        constexpr const char* nord_theme      = "Nord";
        constexpr const char* mocha_theme     = "Mocha";

        inline float clamp01(float v) { return (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v); }

        inline ImVec4 with_alpha(const ImVec4& c, float a)
        {
            return {c.x, c.y, c.z, clamp01(a)};
        }

        inline void apply_style_sizing(ImGuiStyle& style)
        {
            style.WindowPadding   = ImVec2(12.0f, 10.0f);
            style.FramePadding    = ImVec2(10.0f, 6.0f);
            style.ItemSpacing     = ImVec2(10.0f, 8.0f);
            style.ItemInnerSpacing= ImVec2(8.0f, 6.0f);

            style.WindowRounding  = 10.0f;
            style.ChildRounding   = 10.0f;
            style.FrameRounding   = 8.0f;
            style.PopupRounding   = 10.0f;
            style.ScrollbarRounding = 10.0f;
            style.GrabRounding    = 8.0f;
            style.TabRounding     = 8.0f;

            style.WindowBorderSize = 1.0f;
            style.FrameBorderSize  = 0.0f;
            style.PopupBorderSize  = 1.0f;
            style.TabBorderSize    = 0.0f;

            style.ScrollbarSize    = 14.0f;
            style.GrabMinSize      = 12.0f;
        }

        inline void apply_neon_accents(ImGuiStyle& style, const ImVec4& accent)
        {
            ImVec4* c = style.Colors;

            c[ImGuiCol_CheckMark]       = accent;
            c[ImGuiCol_SliderGrab]      = with_alpha(accent, 0.85f);
            c[ImGuiCol_SliderGrabActive]= accent;

            c[ImGuiCol_Separator]       = with_alpha(accent, 0.25f);
            c[ImGuiCol_SeparatorHovered]= with_alpha(accent, 0.60f);
            c[ImGuiCol_SeparatorActive] = with_alpha(accent, 0.90f);

            c[ImGuiCol_NavHighlight]    = with_alpha(accent, 0.60f);
            c[ImGuiCol_DragDropTarget]  = with_alpha(accent, 0.90f);

            c[ImGuiCol_PlotLines]       = with_alpha(accent, 0.85f);
            c[ImGuiCol_PlotLinesHovered]= accent;
            c[ImGuiCol_PlotHistogram]   = with_alpha(accent, 0.75f);
            c[ImGuiCol_PlotHistogramHovered]= accent;
        }
    }

    theme_manager::theme_manager()
        : available_themes_{ dark_theme, light_theme, high_contrast_theme,
                             cyberpunk_theme, dracula_theme, nord_theme, mocha_theme }
        , current_theme_(light_theme)
    {
    }

    std::vector<std::string> theme_manager::get_available_themes() const
    {
        std::lock_guard lock(mtx_);
        return available_themes_;
    }

    std::string theme_manager::get_current() const
    {
        std::lock_guard lock(mtx_);
        return current_theme_;
    }

    void theme_manager::set_current(const std::string& theme_name)
    {
        std::lock_guard lock(mtx_);
        for (const auto& theme : available_themes_)
        {
            if (theme == theme_name)
            {
                current_theme_ = theme_name;
                return;
            }
        }
    }

    bool theme_manager::apply(const theme_spec& spec)
    {
        if (spec.name.empty())
        {
            return false;
        }

        if (!apply_theme_style(spec.name))
        {
            return false;
        }

        set_current(spec.name);
        return true;
    }

    bool theme_manager::apply_theme_style(const std::string& theme_name)
    {
        if (!ImGui::GetCurrentContext())
        {
            return false;
        }

        ImGuiStyle& style = ImGui::GetStyle();

        if (theme_name == dark_theme)
        {
            ImGui::StyleColorsDark(&style);
            apply_style_sizing(style);
            return true;
        }

        if (theme_name == light_theme)
        {
            ImGui::StyleColorsLight(&style);
            apply_style_sizing(style);
            return true;
        }

        if (theme_name == high_contrast_theme)
        {
            ImGui::StyleColorsDark(&style);
            apply_style_sizing(style);

            ImVec4* colors = style.Colors;
            colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
            colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
            colors[ImGuiCol_PopupBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
            colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            colors[ImGuiCol_TextDisabled] = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
            colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.12f, 1.0f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.25f, 0.0f, 1.0f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.45f, 0.45f, 0.0f, 1.0f);
            colors[ImGuiCol_Button] = ImVec4(0.2f, 0.2f, 0.0f, 1.0f);
            colors[ImGuiCol_ButtonHovered] = ImVec4(0.7f, 0.7f, 0.0f, 1.0f);
            colors[ImGuiCol_ButtonActive] = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            colors[ImGuiCol_Header] = ImVec4(0.3f, 0.3f, 0.0f, 1.0f);
            colors[ImGuiCol_HeaderHovered] = ImVec4(0.7f, 0.7f, 0.0f, 1.0f);
            colors[ImGuiCol_HeaderActive] = ImVec4(1.0f, 1.0f, 0.1f, 1.0f);
            colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            colors[ImGuiCol_SliderGrab] = ImVec4(0.7f, 0.7f, 0.0f, 1.0f);
            colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            return true;
        }

        if (theme_name == cyberpunk_theme)
        {
            ImGui::StyleColorsDark(&style);
            apply_style_sizing(style);

            ImVec4* c = style.Colors;

            const ImVec4 bg0 = ImVec4(0.06f, 0.06f, 0.09f, 1.00f);
            const ImVec4 bg1 = ImVec4(0.10f, 0.10f, 0.15f, 1.00f);
            const ImVec4 panel = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
            const ImVec4 accent = ImVec4(0.95f, 0.20f, 0.85f, 1.00f); // neon magenta
            const ImVec4 accent2= ImVec4(0.10f, 0.85f, 0.95f, 1.00f); // neon cyan

            c[ImGuiCol_WindowBg]         = bg0;
            c[ImGuiCol_ChildBg]          = with_alpha(bg0, 0.80f);
            c[ImGuiCol_PopupBg]          = bg1;

            c[ImGuiCol_Border]           = with_alpha(accent2, 0.25f);
            c[ImGuiCol_BorderShadow]     = ImVec4(0,0,0,0);

            c[ImGuiCol_FrameBg]          = panel;
            c[ImGuiCol_FrameBgHovered]   = with_alpha(accent2, 0.20f);
            c[ImGuiCol_FrameBgActive]    = with_alpha(accent2, 0.30f);

            c[ImGuiCol_Button]           = with_alpha(accent, 0.22f);
            c[ImGuiCol_ButtonHovered]    = with_alpha(accent, 0.35f);
            c[ImGuiCol_ButtonActive]     = with_alpha(accent, 0.50f);

            c[ImGuiCol_Header]           = with_alpha(accent2, 0.20f);
            c[ImGuiCol_HeaderHovered]    = with_alpha(accent2, 0.30f);
            c[ImGuiCol_HeaderActive]     = with_alpha(accent2, 0.40f);

            c[ImGuiCol_Tab]              = with_alpha(panel, 0.95f);
            c[ImGuiCol_TabHovered]       = with_alpha(accent2, 0.30f);
            c[ImGuiCol_TabActive]        = with_alpha(accent2, 0.22f);
            c[ImGuiCol_TabUnfocused]     = with_alpha(panel, 0.80f);
            c[ImGuiCol_TabUnfocusedActive]=with_alpha(accent2, 0.18f);

            c[ImGuiCol_TitleBg]          = bg1;
            c[ImGuiCol_TitleBgActive]    = bg1;
            c[ImGuiCol_TitleBgCollapsed] = bg1;

            apply_neon_accents(style, accent2);
            c[ImGuiCol_CheckMark] = accent;

            return true;
        }

        if (theme_name == dracula_theme)
        {
            ImGui::StyleColorsDark(&style);
            apply_style_sizing(style);

            ImVec4* c = style.Colors;

            // Dracula palette
            const ImVec4 bg   = ImVec4(0.11f, 0.12f, 0.16f, 1.00f);
            const ImVec4 bg2  = ImVec4(0.15f, 0.16f, 0.22f, 1.00f);
            const ImVec4 panel= ImVec4(0.18f, 0.19f, 0.27f, 1.00f);

            const ImVec4 pink = ImVec4(1.00f, 0.33f, 0.74f, 1.00f);
            const ImVec4 cyan = ImVec4(0.55f, 0.91f, 0.99f, 1.00f);
            const ImVec4 purple=ImVec4(0.74f, 0.58f, 0.98f, 1.00f);

            c[ImGuiCol_WindowBg]       = bg;
            c[ImGuiCol_PopupBg]        = bg2;
            c[ImGuiCol_ChildBg]        = with_alpha(bg, 0.85f);

            c[ImGuiCol_Border]         = with_alpha(purple, 0.25f);

            c[ImGuiCol_FrameBg]        = panel;
            c[ImGuiCol_FrameBgHovered] = with_alpha(cyan, 0.15f);
            c[ImGuiCol_FrameBgActive]  = with_alpha(cyan, 0.22f);

            c[ImGuiCol_Button]         = with_alpha(purple, 0.20f);
            c[ImGuiCol_ButtonHovered]  = with_alpha(purple, 0.32f);
            c[ImGuiCol_ButtonActive]   = with_alpha(purple, 0.42f);

            c[ImGuiCol_Header]         = with_alpha(pink, 0.18f);
            c[ImGuiCol_HeaderHovered]  = with_alpha(pink, 0.28f);
            c[ImGuiCol_HeaderActive]   = with_alpha(pink, 0.38f);

            c[ImGuiCol_Tab]            = with_alpha(panel, 0.95f);
            c[ImGuiCol_TabHovered]     = with_alpha(pink, 0.25f);
            c[ImGuiCol_TabActive]      = with_alpha(pink, 0.18f);
            c[ImGuiCol_TabUnfocused]   = with_alpha(panel, 0.82f);
            c[ImGuiCol_TabUnfocusedActive]=with_alpha(pink, 0.14f);

            apply_neon_accents(style, purple);
            c[ImGuiCol_CheckMark] = pink;

            return true;
        }

        if (theme_name == nord_theme)
        {
            ImGui::StyleColorsDark(&style);
            apply_style_sizing(style);

            ImVec4* c = style.Colors;

            const ImVec4 bg   = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
            const ImVec4 bg2  = ImVec4(0.16f, 0.18f, 0.23f, 1.00f);
            const ImVec4 panel= ImVec4(0.20f, 0.22f, 0.28f, 1.00f);

            const ImVec4 ice  = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
            const ImVec4 frost= ImVec4(0.56f, 0.74f, 0.73f, 1.00f);

            c[ImGuiCol_WindowBg]       = bg;
            c[ImGuiCol_PopupBg]        = bg2;
            c[ImGuiCol_ChildBg]        = with_alpha(bg, 0.90f);

            c[ImGuiCol_Border]         = with_alpha(ice, 0.18f);

            c[ImGuiCol_FrameBg]        = panel;
            c[ImGuiCol_FrameBgHovered] = with_alpha(ice, 0.12f);
            c[ImGuiCol_FrameBgActive]  = with_alpha(ice, 0.18f);

            c[ImGuiCol_Button]         = with_alpha(frost, 0.18f);
            c[ImGuiCol_ButtonHovered]  = with_alpha(frost, 0.26f);
            c[ImGuiCol_ButtonActive]   = with_alpha(frost, 0.34f);

            c[ImGuiCol_Header]         = with_alpha(ice, 0.14f);
            c[ImGuiCol_HeaderHovered]  = with_alpha(ice, 0.22f);
            c[ImGuiCol_HeaderActive]   = with_alpha(ice, 0.30f);

            c[ImGuiCol_Tab]            = with_alpha(panel, 0.95f);
            c[ImGuiCol_TabHovered]     = with_alpha(ice, 0.18f);
            c[ImGuiCol_TabActive]      = with_alpha(ice, 0.14f);

            apply_neon_accents(style, ice);

            return true;
        }

        if (theme_name == mocha_theme)
        {
            ImGui::StyleColorsDark(&style);
            apply_style_sizing(style);

            ImVec4* c = style.Colors;

            const ImVec4 bg    = ImVec4(0.13f, 0.11f, 0.10f, 1.00f);
            const ImVec4 bg2   = ImVec4(0.18f, 0.15f, 0.13f, 1.00f);
            const ImVec4 panel = ImVec4(0.22f, 0.18f, 0.16f, 1.00f);

            const ImVec4 caramel = ImVec4(0.91f, 0.72f, 0.47f, 1.00f);
            const ImVec4 mint    = ImVec4(0.56f, 0.78f, 0.64f, 1.00f);

            c[ImGuiCol_WindowBg]       = bg;
            c[ImGuiCol_PopupBg]        = bg2;
            c[ImGuiCol_ChildBg]        = with_alpha(bg, 0.90f);

            c[ImGuiCol_Border]         = with_alpha(caramel, 0.18f);

            c[ImGuiCol_FrameBg]        = panel;
            c[ImGuiCol_FrameBgHovered] = with_alpha(caramel, 0.14f);
            c[ImGuiCol_FrameBgActive]  = with_alpha(caramel, 0.20f);

            c[ImGuiCol_Button]         = with_alpha(caramel, 0.18f);
            c[ImGuiCol_ButtonHovered]  = with_alpha(caramel, 0.26f);
            c[ImGuiCol_ButtonActive]   = with_alpha(caramel, 0.34f);

            c[ImGuiCol_Header]         = with_alpha(mint, 0.12f);
            c[ImGuiCol_HeaderHovered]  = with_alpha(mint, 0.18f);
            c[ImGuiCol_HeaderActive]   = with_alpha(mint, 0.24f);

            c[ImGuiCol_Tab]            = with_alpha(panel, 0.95f);
            c[ImGuiCol_TabHovered]     = with_alpha(caramel, 0.18f);
            c[ImGuiCol_TabActive]      = with_alpha(caramel, 0.14f);

            apply_neon_accents(style, caramel);
            c[ImGuiCol_CheckMark] = mint;

            return true;
        }

        return false;
    }
}
