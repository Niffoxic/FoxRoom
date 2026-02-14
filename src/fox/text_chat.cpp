#include "fox/text_chat.h"

#include <algorithm>
#include <vector>

namespace fox
{
    struct text_chat::impl
    {
        std::size_t capacity_{};
        std::vector<message> messages_{};

        explicit impl(std::size_t cap)
            : capacity_(std::max<std::size_t>(1, cap))
        {
            messages_.reserve(capacity_);
        }
    };

    text_chat::text_chat(std::size_t capacity)
        : impl_(std::make_unique<impl>(capacity))
    {
    }

    text_chat::~text_chat() = default;
    text_chat::text_chat(text_chat&&) noexcept = default;
    text_chat& text_chat::operator=(text_chat&&) noexcept = default;

    void text_chat::add_message(const message& msg)
    {
        if (impl_->messages_.size() >= impl_->capacity_)
        {
            impl_->messages_.erase(impl_->messages_.begin());
        }
        impl_->messages_.push_back(msg);
    }

    std::span<const message> text_chat::get_messages() const
    {
        return impl_->messages_;
    }

    void text_chat::clear()
    {
        impl_->messages_.clear();
    }

    std::size_t text_chat::size() const
    {
        return impl_->messages_.size();
    }

    std::size_t text_chat::capacity() const
    {
        return impl_->capacity_;
    }
}
