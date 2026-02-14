#include "fox/settings_store.h"

#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace fox
{
    namespace
    {
        std::future<bool> make_ready_bool(bool value)
        {
            std::promise<bool> p;
            p.set_value(value);
            return p.get_future();
        }

        std::string json_escape(const std::string& value)
        {
            std::string out;
            out.reserve(value.size());
            for (const char c : value)
            {
                switch (c)
                {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += c; break;
                }
            }
            return out;
        }

        std::optional<std::string> extract_raw_value(const std::string& src, const std::string& key)
        {
            const std::string needle = "\"" + key + "\"";
            const std::size_t key_pos = src.find(needle);
            if (key_pos == std::string::npos)
            {
                return std::nullopt;
            }

            const std::size_t colon = src.find(':', key_pos + needle.size());
            if (colon == std::string::npos)
            {
                return std::nullopt;
            }

            std::size_t start = colon + 1;
            while (start < src.size() && std::isspace(static_cast<unsigned char>(src[start])) != 0)
            {
                ++start;
            }

            if (start >= src.size())
            {
                return std::nullopt;
            }

            if (src[start] == '"')
            {
                std::size_t end = start + 1;
                while (end < src.size())
                {
                    if (src[end] == '"' && src[end - 1] != '\\')
                    {
                        break;
                    }
                    ++end;
                }

                if (end >= src.size())
                {
                    return std::nullopt;
                }

                return src.substr(start, end - start + 1);
            }

            std::size_t end = start;
            while (end < src.size() && src[end] != ',' && src[end] != '\n' && src[end] != '}')
            {
                ++end;
            }

            return src.substr(start, end - start);
        }

        std::optional<std::string> parse_string(const std::string& src, const std::string& key)
        {
            const auto raw = extract_raw_value(src, key);
            if (!raw || raw->size() < 2 || (*raw)[0] != '"' || (*raw)[raw->size() - 1] != '"')
            {
                return std::nullopt;
            }

            std::string out;
            for (std::size_t i = 1; i + 1 < raw->size(); ++i)
            {
                if ((*raw)[i] == '\\' && i + 1 < raw->size() - 1)
                {
                    ++i;
                    switch ((*raw)[i])
                    {
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back((*raw)[i]); break;
                    }
                    continue;
                }
                out.push_back((*raw)[i]);
            }
            return out;
        }

        std::optional<bool> parse_bool(const std::string& src, const std::string& key)
        {
            const auto raw = extract_raw_value(src, key);
            if (!raw)
            {
                return std::nullopt;
            }
            if (*raw == "true")
            {
                return true;
            }
            if (*raw == "false")
            {
                return false;
            }
            return std::nullopt;
        }

        std::optional<int> parse_int(const std::string& src, const std::string& key)
        {
            const auto raw = extract_raw_value(src, key);
            if (!raw)
            {
                return std::nullopt;
            }

            try
            {
                return std::stoi(*raw);
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        std::optional<float> parse_float(const std::string& src, const std::string& key)
        {
            const auto raw = extract_raw_value(src, key);
            if (!raw)
            {
                return std::nullopt;
            }

            try
            {
                return std::stof(*raw);
            }
            catch (...)
            {
                return std::nullopt;
            }
        }
    }

    settings_store::settings_store()
        : config_path_(std::filesystem::path("FoxChat") / "config.json")
    {
        std::error_code ec;
        std::filesystem::create_directories(config_path_.parent_path(), ec);
    }

    std::future<bool> settings_store::load()
    {
        std::lock_guard lock(mtx_);

        std::ifstream in(config_path_);
        if (!in.is_open())
        {
            return make_ready_bool(false);
        }

        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::string json = buffer.str();

        last_join_token_ = parse_string(json, key_last_join_token);
        last_username_ = parse_string(json, key_last_username);
        window_visible_ = parse_bool(json, key_window_visible);
        window_width_ = parse_int(json, key_window_width);
        window_alpha_ = parse_float(json, key_window_alpha);
        theme_name_ = parse_string(json, key_theme_name);
        font_name_ = parse_string(json, key_font_name);
        font_size_ = parse_float(json, key_font_size);

        return make_ready_bool(true);
    }

    std::future<bool> settings_store::save() const
    {
        std::lock_guard lock(mtx_);

        std::error_code ec;
        std::filesystem::create_directories(config_path_.parent_path(), ec);

        std::ofstream out(config_path_, std::ios::trunc);
        if (!out.is_open())
        {
            return make_ready_bool(false);
        }

        out << "{\n";

        bool wrote_field = false;
        const auto write_sep = [&]()
        {
            if (wrote_field)
            {
                out << ",\n";
            }
            wrote_field = true;
        };

        if (last_join_token_)
        {
            write_sep();
            out << "  \"" << key_last_join_token << "\": \"" << json_escape(*last_join_token_) << "\"";
        }
        if (last_username_)
        {
            write_sep();
            out << "  \"" << key_last_username << "\": \"" << json_escape(*last_username_) << "\"";
        }
        if (window_visible_)
        {
            write_sep();
            out << "  \"" << key_window_visible << "\": " << (*window_visible_ ? "true" : "false");
        }
        if (window_width_)
        {
            write_sep();
            out << "  \"" << key_window_width << "\": " << *window_width_;
        }
        if (window_alpha_)
        {
            write_sep();
            out << "  \"" << key_window_alpha << "\": " << std::fixed << std::setprecision(3) << *window_alpha_;
        }
        if (theme_name_)
        {
            write_sep();
            out << "  \"" << key_theme_name << "\": \"" << json_escape(*theme_name_) << "\"";
        }
        if (font_name_)
        {
            write_sep();
            out << "  \"" << key_font_name << "\": \"" << json_escape(*font_name_) << "\"";
        }
        if (font_size_)
        {
            write_sep();
            out << "  \"" << key_font_size << "\": " << std::fixed << std::setprecision(3) << *font_size_;
        }

        out << "\n}\n";

        return make_ready_bool(out.good());
    }

    bool settings_store::set_string(const std::string& key, std::string value)
    {
        std::lock_guard lock(mtx_);
        if (key == key_last_join_token)
        {
            last_join_token_ = std::move(value);
            return true;
        }
        if (key == key_last_username)
        {
            last_username_ = std::move(value);
            return true;
        }
        if (key == key_theme_name)
        {
            theme_name_ = std::move(value);
            return true;
        }
        if (key == key_font_name)
        {
            font_name_ = std::move(value);
            return true;
        }
        return false;
    }

    std::optional<std::string> settings_store::get_string(const std::string& key) const
    {
        std::lock_guard lock(mtx_);
        if (key == key_last_join_token)
        {
            return last_join_token_;
        }
        if (key == key_last_username)
        {
            return last_username_;
        }
        if (key == key_theme_name)
        {
            return theme_name_;
        }
        if (key == key_font_name)
        {
            return font_name_;
        }
        return std::nullopt;
    }

    bool settings_store::set_bool(const std::string& key, bool value)
    {
        std::lock_guard lock(mtx_);
        if (key == key_window_visible)
        {
            window_visible_ = value;
            return true;
        }
        return false;
    }

    std::optional<bool> settings_store::get_bool(const std::string& key) const
    {
        std::lock_guard lock(mtx_);
        if (key == key_window_visible)
        {
            return window_visible_;
        }
        return std::nullopt;
    }

    bool settings_store::set_int(const std::string& key, int value)
    {
        std::lock_guard lock(mtx_);
        if (key == key_window_width)
        {
            window_width_ = value;
            return true;
        }
        return false;
    }

    std::optional<int> settings_store::get_int(const std::string& key) const
    {
        std::lock_guard lock(mtx_);
        if (key == key_window_width)
        {
            return window_width_;
        }
        return std::nullopt;
    }

    bool settings_store::set_float(const std::string& key, float value)
    {
        std::lock_guard lock(mtx_);
        if (key == key_window_alpha)
        {
            window_alpha_ = value;
            return true;
        }
        if (key == key_font_size)
        {
            font_size_ = value;
            return true;
        }
        return false;
    }

    std::optional<float> settings_store::get_float(const std::string& key) const
    {
        std::lock_guard lock(mtx_);
        if (key == key_window_alpha)
        {
            return window_alpha_;
        }
        if (key == key_font_size)
        {
            return font_size_;
        }
        return std::nullopt;
    }
}
