/* SPDX-License-Identifier: MPL-2.0 */
#include "arm64/shared_compilation_cache.hpp"
#include "compilation_key.hpp"
#include "published_trace.hpp"
#include <stdexcept>
#include <atomic>
#include <list>
#include <map>
#include <mutex>
namespace ilemu::execution {
class SharedCompilationCache::Impl {
public:
    Impl(std::shared_ptr<CodeAllocator> allocator, std::size_t budget)
        : allocator_(std::move(allocator)), budget_(budget)
    {
        if (!allocator_)
            throw std::invalid_argument("shared code cache requires an allocator");
    }
    struct Record {
        std::shared_ptr<const arm64::PublishedTrace> plan;
        std::size_t bytes;
        std::list<const arm64::CompilationKey*>::iterator recency;
    };
    using Plans = std::map<arm64::CompilationKey, Record>;
    void clear()
    {
        recent_.clear();
        plans_.clear();
        retained_.store(0, std::memory_order_relaxed);
    }
    // Declared first so code and records are released before the allocator.
    const std::shared_ptr<CodeAllocator> allocator_;
    std::mutex mutex_;
    const std::size_t budget_;
    bool pressured_ = false;
    std::atomic<std::size_t> retained_ {0};
    Plans plans_;
    std::list<const arm64::CompilationKey*> recent_;
};
SharedCompilationCache::SharedCompilationCache(
    std::shared_ptr<CodeAllocator> allocator, std::size_t budget)
    : impl_(std::make_unique<Impl>(std::move(allocator), budget)) {}
bool SharedCompilationCache::owns(const CodeAllocator& allocator) const noexcept
{ return impl_->allocator_.get() == &allocator; }
SharedCompilationCache::~SharedCompilationCache() = default;
std::uint64_t SharedCompilationCache::retained_bytes() const noexcept
{ return impl_->retained_.load(std::memory_order_relaxed); }
void SharedCompilationCache::clear()
{
    const std::lock_guard lock {impl_->mutex_};
    impl_->clear();
}
void SharedCompilationCache::set_pressure_limited(bool limited)
{
    const std::lock_guard lock {impl_->mutex_};
    impl_->pressured_ = limited;
    if (limited)
        impl_->clear();
}
std::shared_ptr<const arm64::PublishedTrace> SharedCompilationCache::find(
    const arm64::CompilationKey& key)
{
    const std::lock_guard lock {impl_->mutex_};
    const auto it = impl_->plans_.find(key);
    if (it == impl_->plans_.end())
        return {};
    impl_->recent_.splice(impl_->recent_.end(), impl_->recent_, it->second.recency);
    return it->second.plan;
}
void SharedCompilationCache::insert(const arm64::CompilationKey& key,
    std::shared_ptr<const arm64::PublishedTrace> plan)
{
    // Count the executable allocation and metadata. Evicted code retained by
    // local entries remains charged to their independently bounded caches.
    const auto bytes = plan->capacity() +
        sizeof(arm64::PublishedTrace) + sizeof(Impl::Plans::value_type) +
        12U * sizeof(void*);
    const std::lock_guard lock {impl_->mutex_};
    if (impl_->pressured_ || bytes > impl_->budget_ || impl_->plans_.contains(key))
        return;
    auto retained = impl_->retained_.load(std::memory_order_relaxed);
    while (retained > impl_->budget_ - bytes) {
        const auto oldest = impl_->plans_.find(*impl_->recent_.front());
        retained -= oldest->second.bytes;
        impl_->recent_.pop_front();
        impl_->plans_.erase(oldest);
        impl_->retained_.store(retained, std::memory_order_relaxed);
    }
    const auto it = impl_->plans_.emplace(key, Impl::Record {std::move(plan), bytes, {}}).first;
    try {
        it->second.recency = impl_->recent_.insert(impl_->recent_.end(), &it->first);
    } catch (...) {
        impl_->plans_.erase(it);
        throw;
    }
    impl_->retained_.store(retained + bytes, std::memory_order_relaxed);
}
}
