#pragma once

#include "fox/chat_interfaces.h"

#include <cstddef>
#include <memory>

namespace fox
{
    class text_chat final : public ITextChat
    {
    public:
        static constexpr std::size_t default_capacity = 500;

        explicit text_chat(std::size_t capacity = default_capacity);
        ~text_chat() override;

        text_chat(const text_chat&) = delete;
        text_chat& operator=(const text_chat&) = delete;
        text_chat(text_chat&&) noexcept;
        text_chat& operator=(text_chat&&) noexcept;

        void add_message(const message& msg) override;
        [[nodiscard]] std::span<const message> get_messages() const override;
        void clear() override;

        [[nodiscard]] std::size_t size() const;
        [[nodiscard]] std::size_t capacity() const;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
}
