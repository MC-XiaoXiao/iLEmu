/* SPDX-License-Identifier: MPL-2.0 */
#include "code_pool.hpp"
#if defined(__linux__)
#include <oaknut/dual_code_block.hpp>
#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace ilemu::host::detail {
namespace {
    constexpr unsigned slots_per_chunk = 16;
    constexpr unsigned full_chunk = (1U << slots_per_chunk) - 1;

    struct Chunk {
        explicit Chunk(std::size_t page)
            : memory(page * slots_per_chunk)
        {
        }
        oaknut::DualCodeBlock memory;
        unsigned used = 0;
    };

    class Pool;
    class PooledCode final : public execution::ExecutableCode {
    public:
        PooledCode(std::shared_ptr<Pool>, std::shared_ptr<Chunk>, unsigned,
            std::size_t);
        ~PooledCode() override;
        std::size_t capacity() const noexcept override { return page_; }
        const void* entry() const noexcept override;
        void publish(std::span<const std::byte>) override;

    private:
        std::shared_ptr<Pool> pool_;
        std::shared_ptr<Chunk> chunk_;
        unsigned slot_;
        std::size_t page_;
        bool published_ = false;
    };

    // Page capacities preserve cache accounting. Reuse any vacant slot before
    // growing; retire empty chunks, keeping at most one for the next refill.
    // Backing is bounded by peak live pages rounded to a chunk. Code objects
    // own the pool so an allocator can be destroyed before its allocations.
    class Pool final : public std::enable_shared_from_this<Pool> {
    public:
        Pool(std::unique_ptr<execution::CodeAllocator> fallback, std::size_t page)
            : fallback_(std::move(fallback)), page_(page)
        {
        }
        std::unique_ptr<execution::ExecutableCode> allocate(std::size_t bytes)
        {
            if (bytes == 0 || bytes > page_)
                return fallback_->allocate(bytes);
            std::unique_lock lock(mutex_);
            auto available = std::find_if(chunks_.begin(), chunks_.end(),
                [](const auto& chunk) { return chunk->used != full_chunk; });
            if (available == chunks_.end() && !unavailable_) {
                try {
                    chunks_.push_back(std::make_shared<Chunk>(page_));
                    available = std::prev(chunks_.end());
                } catch (const std::bad_alloc&) {
                    // Host policy or unavailable memfd support must not prevent
                    // the existing anonymous-map publication path from working.
                    unavailable_ = true;
                }
            }
            if (available == chunks_.end()) {
                lock.unlock();
                return fallback_->allocate(bytes);
            }
            auto& chunk = *available;
            const auto slot = static_cast<unsigned>(std::countr_one(chunk->used));
            auto code = std::make_unique<PooledCode>(
                shared_from_this(), chunk, slot, page_);
            chunk->used |= 1U << slot;
            return code;
        }
        void release(const std::shared_ptr<Chunk>& chunk, unsigned slot)
        {
            std::lock_guard lock(mutex_);
            chunk->used &= ~(1U << slot);
            if (chunk->used == 0 && chunks_.size() > 1)
                std::erase(chunks_, chunk);
        }

    private:
        std::unique_ptr<execution::CodeAllocator> fallback_;
        const std::size_t page_;
        std::mutex mutex_;
        std::vector<std::shared_ptr<Chunk>> chunks_;
        bool unavailable_ = false;
    };

    PooledCode::PooledCode(std::shared_ptr<Pool> pool,
        std::shared_ptr<Chunk> chunk, unsigned slot, std::size_t page)
        : pool_(std::move(pool)), chunk_(std::move(chunk)), slot_(slot), page_(page)
    {
    }
    PooledCode::~PooledCode() { pool_->release(chunk_, slot_); }
    const void* PooledCode::entry() const noexcept
    {
        return published_
            ? reinterpret_cast<const std::byte*>(chunk_->memory.xptr()) +
                  slot_ * page_
            : nullptr;
    }
    void PooledCode::publish(std::span<const std::byte> bytes)
    {
        if (published_ || bytes.empty() || bytes.size() > page_)
            throw std::logic_error("invalid code publication");
        auto* writable = reinterpret_cast<char*>(chunk_->memory.wptr()) +
                         slot_ * page_;
        auto* executable = reinterpret_cast<char*>(chunk_->memory.xptr()) +
                           slot_ * page_;
        std::memcpy(writable, bytes.data(), bytes.size());
        // Clean the written alias before invalidating the execution alias.
        // Neither mapping changes permissions while other slots are executing.
        __builtin___clear_cache(writable, writable + bytes.size());
        __builtin___clear_cache(executable, executable + bytes.size());
        published_ = true;
    }

    class PooledAllocator final : public execution::CodeAllocator {
    public:
        PooledAllocator(std::unique_ptr<execution::CodeAllocator> fallback,
            std::size_t page)
            : pool_(std::make_shared<Pool>(std::move(fallback), page))
        {
        }
        std::unique_ptr<execution::ExecutableCode> allocate(
            std::size_t bytes) override
        {
            return pool_->allocate(bytes);
        }

    private:
        std::shared_ptr<Pool> pool_;
    };
}
std::unique_ptr<execution::CodeAllocator> make_pooled_code_allocator(
    std::unique_ptr<execution::CodeAllocator> fallback, std::size_t page_size)
{
    return std::make_unique<PooledAllocator>(std::move(fallback), page_size);
}
}
#endif
