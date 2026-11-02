// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Run scheduler-selected guest CPU batches on coordinated host
// execution lanes.

#include "foundation/guest_execution_coordinator.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace ilemu {

namespace {
thread_local bool execution_channel_active { };
}

bool GuestExecutionCoordinator::in_execution_channel() noexcept
{
    return execution_channel_active;
}

struct GuestExecutionCoordinator::Impl {
    enum class State { Idle, Submitted, Running, Completed };
    struct Channel {
        std::condition_variable work_available;
        std::thread worker;
        GuestExecutionRequest* request { };
        State state { State::Idle };
    };
    std::mutex mutex;
    std::condition_variable completion_available;
    std::vector<std::unique_ptr<Channel>> channels;
    std::size_t outstanding { };
    bool stopping { };
};

GuestExecutionCoordinator::GuestExecutionCoordinator(std::size_t worker_count)
    : impl_ { std::make_unique<Impl>() }
{
    if (worker_count == 0U)
        throw std::invalid_argument { "guest execution requires a channel" };
    impl_->channels.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            impl_->channels.push_back(std::make_unique<Impl::Channel>());
            impl_->channels.back()->worker =
                std::thread([this, index] { worker_loop(index); });
        }
    } catch (...) {
        stop();
        throw;
    }
}

GuestExecutionCoordinator::~GuestExecutionCoordinator()
{
    stop();
}

void GuestExecutionCoordinator::stop() noexcept
{
    {
        std::lock_guard lock { impl_->mutex };
        impl_->stopping = true;
    }
    for (auto& channel : impl_->channels)
        channel->work_available.notify_one();
    for (auto& channel : impl_->channels)
        if (channel->worker.joinable())
            channel->worker.join();
}

void GuestExecutionCoordinator::submit(GuestExecutionRequest& request)
{
    std::lock_guard lock { impl_->mutex };
    if (!request.cpu || request.execution_slot >= impl_->channels.size())
        throw std::invalid_argument { "guest request requires a CPU and valid channel" };
    if (impl_->stopping)
        throw std::logic_error { "guest execution channels are stopping" };
    auto& channel = *impl_->channels[request.execution_slot];
    if (channel.state != Impl::State::Idle)
        throw std::logic_error { "guest execution channel is occupied" };
    for (const auto& other : impl_->channels)
        if (other->request && other->request->cpu == request.cpu)
            throw std::logic_error { "guest CPU already has an outstanding request" };
    request.cooperation_signal = host_completion_pending_ ? &host_completions_ : nullptr;
    request.completion_generation = polled_completion_generation_;
    channel.request = &request;
    channel.state = Impl::State::Submitted;
    ++impl_->outstanding;
    channel.work_available.notify_one();
}

GuestExecutionRequest* GuestExecutionCoordinator::wait()
{
    std::unique_lock lock { impl_->mutex };
    if (impl_->outstanding == 0U)
        return nullptr;
    impl_->completion_available.wait(lock, [this] {
        for (const auto& channel : impl_->channels)
            if (channel->state == Impl::State::Completed)
                return true;
        return false;
    });
    for (auto& channel : impl_->channels) {
        if (channel->state != Impl::State::Completed)
            continue;
        auto* request = channel->request;
        channel->request = nullptr;
        channel->state = Impl::State::Idle;
        --impl_->outstanding;
        return request;
    }
    std::terminate();
}

void GuestExecutionCoordinator::poll_host_completions()
{
    polled_completion_generation_ = host_completions_.generation();
}

void GuestExecutionCoordinator::arm_host_completions(bool pending)
{
    host_completion_pending_ = pending;
}

void GuestExecutionCoordinator::notify_host_completion() noexcept
{
    host_completions_.notify();
}

void GuestExecutionCoordinator::run(std::span<GuestExecutionRequest*> requests,
    const std::function<void()>& service_host)
{
    {
        std::lock_guard lock { impl_->mutex };
        if (impl_->outstanding != 0U)
            throw std::logic_error { "guest execution windows cannot overlap" };
    }
    // Validate before dispatch so a malformed window cannot partially execute.
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (!requests[i] || !requests[i]->cpu ||
            requests[i]->execution_slot >= impl_->channels.size())
            throw std::invalid_argument { "invalid guest execution request" };
        for (std::size_t j = 0; j < i; ++j)
            if (requests[i]->cpu == requests[j]->cpu ||
                requests[i]->execution_slot == requests[j]->execution_slot)
                throw std::invalid_argument { "duplicate guest execution channel or CPU" };
    }
    if (requests.empty())
        return;
    try {
        for (auto* request : requests)
            submit(*request);
        for (;;) {
            std::unique_lock lock { impl_->mutex };
            if (impl_->outstanding == 0U)
                break;
            const auto completed = [this] {
                return std::any_of(impl_->channels.begin(), impl_->channels.end(),
                    [](const auto& channel) {
                        return channel->state == Impl::State::Completed;
                    });
            };
            if (service_host)
                impl_->completion_available.wait_for(
                    lock, std::chrono::milliseconds { 1 }, completed);
            else
                impl_->completion_available.wait(lock, completed);
            for (auto& channel : impl_->channels) {
                if (channel->state == Impl::State::Completed) {
                    channel->request = nullptr;
                    channel->state = Impl::State::Idle;
                    --impl_->outstanding;
                }
            }
            const bool pending = impl_->outstanding != 0U;
            lock.unlock();
            if (pending && service_host)
                service_host();
        }
    } catch (...) {
        while (wait()) { }
        throw;
    }
}

void GuestExecutionCoordinator::execute(GuestExecutionRequest& request) noexcept
{
    request.result = { };
    request.error = nullptr;
    try {
        const execution::GuestCooperationScope cooperation {
            request.cooperation_signal, request.completion_generation };
        std::optional<AddressSpace::ExclusiveAccess> access;
        if (request.exclusive_memory)
            access.emplace(*request.exclusive_memory);
        request.result = request.single_step
                             ? request.cpu->step(request.execution_slot)
                             : request.cpu->run_cooperatively(
                                   request.tick_budget,
                                   request.host_slice_budget,
                                   request.execution_slot);
    } catch (...) {
        request.error = std::current_exception();
    }
}

void GuestExecutionCoordinator::worker_loop(std::size_t index)
{
    // Each slot has a stable host worker and memory view; host affinity remains
    // the OS scheduler's responsibility and is not part of Guest CPU identity.
    auto& channel = *impl_->channels[index];
    std::unique_lock lock { impl_->mutex };
    for (;;) {
        channel.work_available.wait(lock, [this, &channel] {
            return impl_->stopping || channel.state == Impl::State::Submitted;
        });
        if (channel.state != Impl::State::Submitted)
            return;
        channel.state = Impl::State::Running;
        auto* request = channel.request;
        lock.unlock();
        execution_channel_active = true;
        execute(*request);
        execution_channel_active = false;
        lock.lock();
        channel.state = Impl::State::Completed;
        impl_->completion_available.notify_one();
    }
}

} // namespace ilemu
