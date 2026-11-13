/* SPDX-License-Identifier: MPL-2.0 */
#include "kernel/diagnostic_snapshot.hpp"
#include "../../mach/support.hpp"
#include <algorithm>

namespace ilemu::diagnostic_snapshot {
namespace {
void word(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value)
{
    mach_support::write_little_word(bytes, offset, value);
}
void wide(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value)
{
    word(bytes, offset, static_cast<std::uint32_t>(value));
    word(bytes, offset + 4U, static_cast<std::uint32_t>(value >> 32U));
}
void text(std::span<std::byte> bytes, std::string_view value)
{
    const auto count = std::min(bytes.size() - 1U, value.size());
    std::transform(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(count),
        bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
}
} // namespace

void KcData::item(std::uint32_t type, std::span<const std::byte> payload,
    std::uint64_t flags)
{
    const auto padding = (16U - payload.size() % 16U) % 16U;
    const auto begin = bytes_.size();
    bytes_.resize(begin + 16U + payload.size() + padding);
    auto destination = std::span { bytes_ }.subspan(begin);
    word(destination, 0U, type);
    word(destination, 4U, static_cast<std::uint32_t>(payload.size() + padding));
    const bool array = type >= 0x20U && type <= 0x2fU;
    if (!array && type != 0x13U && type != 0x14U &&
        type != 0x59a25807U && type != 0xf19158edU)
        flags |= 0x80U | padding;
    wide(destination, 8U, flags);
    std::copy(payload.begin(), payload.end(), destination.begin() + 16);
}

KcData::KcData(std::uint64_t now)
{
    item(0x59a25807U, { });
    std::array<std::byte, 8> value;
    wide(value, 0U, now);
    item(0x33U, value);
    word(value, 0U, 1U);
    word(value, 4U, 1U);
    item(0x32U, value);
}

void KcData::container(std::uint32_t type, std::uint64_t identifier)
{
    std::array<std::byte, 4> value;
    word(value, 0U, type);
    item(0x13U, value, identifier);
}

void KcData::task(const Task& task)
{
    container(0x903U, task.unique_id);
    std::array<std::byte, 120> value { };
    wide(value, 0U, task.unique_id);
    wide(value, 8U, task.flags);
    wide(value, 16U, task.terminated_user_ns);
    wide(value, 32U, task.start_wall_ns / 1'000'000'000U);
    wide(value, 40U, task.resident_bytes);
    wide(value, 48U, task.peak_resident_bytes);
    word(value, 56U, task.suspend_count);
    word(value, 84U, task.pid);
    text(std::span { value }.subspan(88U), task.name);
    item(0x905U, value);
}

void KcData::thread(const Thread& thread)
{
    container(0x904U, thread.id);
    std::array<std::byte, 104> value { };
    wide(value, 0U, thread.id);
    wide(value, 48U, thread.user_ns);
    wide(value, 64U, 0x8U | (thread.suspended ? 0x40U : 0U)); // PC-only
    word(value, 88U, thread.state);
    word(value, 96U, static_cast<std::uint16_t>(thread.base_priority) |
        (static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread.scheduled_priority)) << 16U));
    value[101] = static_cast<std::byte>(thread.requested_qos);
    item(0x906U, value);
    if (!thread.program_counters.empty()) {
        std::vector<std::byte> frames(thread.program_counters.size() * 4U);
        for (std::size_t i = 0; i < thread.program_counters.size(); ++i)
            word(frames, i * 4U, thread.program_counters[i]);
        const auto padding = (16U - frames.size() % 16U) % 16U;
        item(0x20U + static_cast<std::uint32_t>(padding), frames,
            (std::uint64_t { 0x915U } << 32U) | thread.program_counters.size());
    }
    end(thread.id);
}

void KcData::end(std::uint64_t identifier)
{
    item(0x14U, { }, identifier);
}

std::vector<std::byte> KcData::finish()
{
    item(0xf19158edU, { });
    return std::move(bytes_);
}

bool Microstackshots::claim_sample(std::uint64_t now)
{
    if (!enabled_.load(std::memory_order_relaxed))
        return false;
    auto deadline = next_sample_.load(std::memory_order_relaxed);
    return now >= deadline && next_sample_.compare_exchange_strong(
        deadline, now + 1'000'000'000U, std::memory_order_relaxed);
}

void Microstackshots::record(std::uint32_t processor, std::uint64_t wall_ns,
    const Task& task, const Thread& thread)
{
    Record bytes { };
    word(bytes, 0U, 0x31c54011U);
    word(bytes, 4U, processor);
    wide(bytes, 8U, wall_ns / 1'000'000'000U);
    wide(bytes, 16U, wall_ns % 1'000'000'000U / 1'000U);
    bytes[24] = std::byte { 4U }; // sampled userspace
    auto process = std::span { bytes }.subspan(27U, 293U);
    word(process, 0U, 0xdecafbadU);
    word(process, 4U, task.pid);
    wide(process, 8U, task.unique_id);
    wide(process, 16U, task.terminated_user_ns);
    word(process, 60U, task.suspend_count);
    word(process, 64U, static_cast<std::uint32_t>(task.resident_bytes / 4096U));
    word(process, 80U, static_cast<std::uint32_t>(task.flags));
    wide(process, 84U, task.start_wall_ns / 1'000'000'000U);
    wide(process, 92U, task.start_wall_ns % 1'000'000'000U / 1'000U);
    text(process.subspan(100U, 17U), task.name);
    auto running = std::span { bytes }.subspan(320U, 316U);
    word(running, 0U, 0xfeedfaceU);
    word(running, 8U, 1U);
    wide(running, 28U, thread.id);
    wide(running, 36U, thread.user_ns);
    word(running, 52U, thread.state);
    word(running, 56U, static_cast<std::uint32_t>(thread.base_priority));
    word(running, 60U, static_cast<std::uint32_t>(thread.scheduled_priority));
    running[68] = std::byte { 8U }; // kStacksPCOnly
    running[70] = static_cast<std::byte>(thread.requested_qos);
    word(bytes, 636U, thread.program_counters.front());
    const std::lock_guard lock { mutex_ };
    if ((records_.size() + 1U) * sizeof(Record) > capacity)
        records_.pop_front();
    records_.push_back(std::move(bytes));
}

std::vector<std::byte> Microstackshots::read() const
{
    const std::lock_guard lock { mutex_ };
    std::vector<std::byte> bytes;
    bytes.reserve(records_.size() * sizeof(Record));
    for (const auto& record : records_)
        bytes.insert(bytes.end(), record.begin(), record.end());
    return bytes;
}

} // namespace ilemu::diagnostic_snapshot
