#pragma once

#include "fox/chat_interfaces.h"

#include <mutex>
#include <span>
#include <vector>

namespace fox
{
    class TextChatStub final : public ITextChat
    {
    public:
        void add_message(const message& msg) override
        {
            std::lock_guard lock(mtx_);
            if (history_.size() >= capacity_)
            {
                history_.erase(history_.begin());
            }
            history_.push_back(msg);
        }

        [[nodiscard]] std::span<const message> get_messages() const override
        {
            return history_;
        }

        void clear() override
        {
            std::lock_guard lock(mtx_);
            history_.clear();
        }

    private:
        static constexpr std::size_t capacity_ = 500;
        mutable std::mutex mtx_{};
        std::vector<message> history_{};
    };
}
