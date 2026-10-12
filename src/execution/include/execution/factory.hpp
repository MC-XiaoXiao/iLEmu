/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/run.hpp"
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

namespace ilemu::execution {
// Cache storage is a selected-backend property, independent of host architecture.
struct NativeCacheLayout {
    enum class Storage { None, Shared, Local };
    Storage storage = Storage::None;
    std::size_t minimum_bytes = 0;

    std::size_t mapping_count(std::size_t slots) const noexcept
    {
        return storage == Storage::None ? 0
            : storage == Storage::Shared ? 1 : slots;
    }
};
// Runtime slots request bounded backends; the host owns code allocation policy.
class ExecutorFactory {
public:
    using Create = std::function<std::unique_ptr<Executor>(std::size_t)>;
    ExecutorFactory() = default;
    ExecutorFactory(Create create, NativeCacheLayout layout)
        : create_(std::move(create)), layout_(layout) { }
    explicit operator bool() const noexcept { return bool(create_); }
    std::unique_ptr<Executor> operator()(std::size_t bytes) const
    {
        return create_(bytes);
    }
    NativeCacheLayout cache_layout() const noexcept { return layout_; }

private:
    Create create_;
    NativeCacheLayout layout_;
};
}
