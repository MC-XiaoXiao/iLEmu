/* SPDX-License-Identifier: MPL-2.0 */
#include "catalog_file_probe_batch.hpp"
#include "foundation/host_resource_controller.hpp"
#include <algorithm>
#include <thread>
#include <utility>
namespace ilemu::catalog_detail {
namespace {
    constexpr unsigned sample_window = 32;
    const auto promotion_cost = HostResourceBudget { }.deadline_reserve;
}
FileProbeBatch::FileProbeBatch(Read read, Generation generation)
    : read_(std::move(read))
    , generation_(std::move(generation))
{
}
FileProbeBatch::~FileProbeBatch() = default;
void FileProbeBatch::observe(Clock::duration elapsed)
{
    sampled_ += elapsed;
    if (++samples_ == sample_window) {
        if (sampled_ > promotion_cost)
            parallel_ = true;
        sampled_ = { };
        samples_ = 0;
    }
}
void FileProbeBatch::prepare(
    std::span<const std::filesystem::path* const> paths)
{
    if (workers_)
        workers_->wait_idle();
    if (!slots_.empty()) {
        Clock::duration total { };
        for (const auto& slot : slots_)
            total += slot.elapsed;
        // Stop speculative work when the remaining reads are cheap again.
        parallel_ = total > promotion_cost * slots_.size() / sample_window;
    }
    slots_.clear();
    if (!parallel_ || paths.size() < sample_window || paths.size() > capacity)
        return;
    try {
        if (!attempted_) {
            attempted_ = true;
            const auto available
                = std::max(1U, std::thread::hardware_concurrency());
            const auto lanes = std::min<std::size_t>(8, available - 1U);
            if (lanes > 1) {
                HostResourceBudget budget;
                budget.worker_count = lanes;
                workers_ = std::make_unique<HostResourceController>(budget);
            }
        }
        if (!workers_)
            return;
        slots_.resize(paths.size());
        for (std::size_t i = 0; i < paths.size(); ++i) {
            slots_[i].path = paths[i];
            slots_[i].token = workers_->submit(
                HostWorkKind::ForegroundPrepare, std::nullopt, [this, i] {
                    auto& slot = slots_[i];
                    const auto begin = Clock::now();
                    try {
                        slot.probe = read_(*slot.path);
                    } catch (...) {
                        // Failed probes are retried by the owner.
                    }
                    slot.elapsed = Clock::now() - begin;
                });
        }
    } catch (...) {
        if (workers_)
            workers_->wait_idle();
        slots_.clear();
    }
}
std::optional<FilePrefix> FileProbeBatch::read(
    std::size_t index, const std::filesystem::path& path)
{
    if (index < slots_.size()) {
        const auto& slot = slots_[index];
        if (slot.path == &path) {
            if (slot.token)
                slot.token->wait_finished();
            if (slot.probe && generation_(path) == slot.probe->generation)
                return slot.probe->prefix;
        }
    }
    const auto begin = Clock::now();
    const auto probe = read_(path);
    observe(Clock::now() - begin);
    return probe ? std::optional<FilePrefix> { probe->prefix } : std::nullopt;
}
}
