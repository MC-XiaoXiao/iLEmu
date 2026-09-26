// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Run scheduler-selected guest CPU batches on coordinated host
// execution lanes.

#include "foundation/guest_execution_coordinator.hpp"

#include <condition_variable>
#include <mutex>
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

void GuestExecutionCoordinator::run(std::span<GuestExecutionRequest*> requests)
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
    // The caller can execute a lane while its peers run instead of paying a
    // worker handoff only to wait idle. It uses the same deferred-SVC and
    // parallel-memory scope as a worker; kernel commits still wait for every
    // lane. submit()/wait() retain their independent channel semantics.
    try {
        for (auto* request : requests.subspan(1)) {
            submit(*request);
        }
        const auto previous_channel =
            std::exchange(execution_channel_active, true);
        execute(*requests.front());
        execution_channel_active = previous_channel;
    } catch (...) {
        while (wait()) { }
        throw;
    }
    while (wait()) { }
}

void GuestExecutionCoordinator::execute(GuestExecutionRequest& request) noexcept
{
    request.result = { };
    request.error = nullptr;
    try {
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
