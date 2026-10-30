// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "graphics/gles_program_renderer.hpp"
#include "graphics/gles_resources.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace ilemu {
namespace {
    constexpr std::size_t command_limit = 64;
    constexpr std::size_t payload_limit = 64U * 1024U * 1024U;

    std::atomic_uint64_t completion_generation { 1 };
    std::mutex wake_mutex;
    std::condition_variable host_activity;
    std::function<void()> completion_wake;
    void notify_completion()
    {
        // Serialize the generation transition with idle wait's predicate and
        // atomic unlock-and-wait. No queue or backend lock is held here.
        std::lock_guard lock { wake_mutex };
        completion_generation.fetch_add(1, std::memory_order_release);
        host_activity.notify_all();
        // Only host wake primitives are allowed here, never kernel callbacks.
        if (completion_wake)
            completion_wake();
    }
    struct Credits {
        std::mutex mutex;
        std::condition_variable capacity;
        std::size_t count { }, bytes { }, results { }, result_bytes { };
    };

    struct Backend {
        std::unique_ptr<GlesProgramRenderer> renderer;
    };

    // One host-wide FIFO also orders different processes' EGL contexts. The
    // worker never acquires kernel, guest memory or SurfaceStore locks. Host
    // backend locks are taken only after releasing the command queue lock.
    class ProgramExecutor {
    public:
        ProgramExecutor()
            : thread_ { [this] { run(); } }
        {
        }
        ~ProgramExecutor()
        {
            {
                std::lock_guard lock { credits_->mutex };
                stopping_ = true;
            }
            available_.notify_all();
            credits_->capacity.notify_all();
            thread_.join();
        }

        template <typename F> auto submit(std::size_t bytes, F function)
        {
            using Result = std::invoke_result_t<F>;
            if (bytes > payload_limit)
                throw std::length_error {
                    "GLES command payload exceeds 64 MiB"
                };
            auto task = std::make_shared<std::packaged_task<Result()>>(
                std::move(function));
            auto result = task->get_future();
            std::unique_lock lock { credits_->mutex };
            credits_->capacity.wait(lock, [&] {
                return stopping_ ||
                       (credits_->count < command_limit &&
                           bytes <= payload_limit - credits_->bytes);
            });
            if (stopping_)
                throw std::runtime_error { "GLES render worker stopped" };
            if (next_sequence_ == 0)
                throw std::overflow_error { "GLES sequence space exhausted" };
            const auto sequence = next_sequence_;
            commands_.push_back({ sequence, bytes, [task] { (*task)(); } });
            ++next_sequence_;
            ++credits_->count;
            credits_->bytes += bytes;
            lock.unlock();
            available_.notify_one();
            return std::pair { sequence, std::move(result) };
        }

        template <typename F> auto call(F function, std::size_t bytes = 0)
        {
            return submit(bytes, std::move(function)).second.get();
        }

        bool capacity(std::size_t bytes)
        {
            std::lock_guard lock { credits_->mutex };
            return !stopping_ && credits_->count < command_limit &&
                   bytes <= payload_limit - credits_->result_bytes &&
                   credits_->results < command_limit;
        }
        std::shared_ptr<GlesWorkResult> submit_host(
            std::size_t bytes, std::function<bool(DisplayFrame&)> execute)
        {
            if (bytes > payload_limit)
                throw std::length_error { "GLES result exceeds 64 MiB" };
            auto result = std::make_shared<GlesWorkResult>();
            auto armed = std::make_shared<bool>(false);
            result->release_credit = [credits = credits_, bytes, armed] {
                if (!*armed)
                    return;
                {
                    std::lock_guard credit_lock { credits->mutex };
                    --credits->results;
                    credits->result_bytes -= bytes;
                }
                credits->capacity.notify_all();
                notify_completion();
            };
            std::unique_lock lock { credits_->mutex };
            if (stopping_ || credits_->count >= command_limit ||
                credits_->results >= command_limit ||
                bytes > payload_limit - credits_->result_bytes)
                return { };
            if (next_sequence_ == 0)
                throw std::overflow_error { "GLES sequence space exhausted" };
            const auto sequence = next_sequence_;
            commands_.push_back({ sequence, 0,
                [result, bytes, execute = std::move(execute)] {
                    bool success = false;
                    try {
                        success = execute(result->frame);
                    } catch (...) {
                    }
                    if (result->frame.pixels.size() >
                        bytes / sizeof(std::uint32_t)) {
                        std::vector<std::uint32_t>().swap(result->frame.pixels);
                        success = false;
                    }
                    result->status.store(success
                                             ? GlesWorkResult::Status::Completed
                                             : GlesWorkResult::Status::Failed,
                        std::memory_order_release);
                },
                true });
            result->sequence = sequence;
            ++next_sequence_;
            ++credits_->count;
            ++credits_->results;
            credits_->result_bytes += bytes;
            *armed = true;
            lock.unlock();
            available_.notify_one();
            return result;
        }

        // Only called on the render thread. Retention guarantees that even
        // allocation failure while publishing retirement cannot move an EGL
        // destructor to the core thread; final shutdown retires the remainder.
        void retain(const std::shared_ptr<Backend>& backend)
        {
            backends_.push_back(backend);
        }
        void retire(const std::shared_ptr<Backend>& backend)
        {
            backend->renderer.reset();
            std::erase(backends_, backend);
        }

    private:
        struct Command {
            std::uint64_t sequence;
            std::size_t bytes;
            std::function<void()> execute;
            bool retained_credit { };
        };
        void run()
        {
            for (;;) {
                Command command;
                {
                    std::unique_lock lock { credits_->mutex };
                    available_.wait(
                        lock, [&] { return stopping_ || !commands_.empty(); });
                    if (commands_.empty())
                        break;
                    command = std::move(commands_.front());
                    commands_.pop_front();
                }
                // packaged_task publishes a value or exception for every
                // accepted command. No queue lock spans driver/GPU execution.
                command.execute();
                command.execute = { };
                {
                    std::lock_guard lock { credits_->mutex };
                    completed_sequence_ = command.sequence;
                    --credits_->count;
                    credits_->bytes -= command.bytes;
                }
                credits_->capacity.notify_all();
                notify_completion();
            }
            backends_.clear();
        }

        std::shared_ptr<Credits> credits_ { std::make_shared<Credits>() };
        std::condition_variable available_;
        std::deque<Command> commands_;
        std::uint64_t next_sequence_ { 1 }, completed_sequence_ { };
        bool stopping_ { };
        std::vector<std::shared_ptr<Backend>> backends_;
        std::thread thread_;
    };

    std::mutex executor_mutex;
    std::shared_ptr<ProgramExecutor> executor_instance;
    std::shared_ptr<ProgramExecutor> shared_executor()
    {
        std::lock_guard lock { executor_mutex };
        if (!executor_instance)
            executor_instance = std::make_shared<ProgramExecutor>();
        return executor_instance;
    }

    struct DrawPacket {
        DisplayFrame frame;
        GlesRenderTargetKey target;
        GlesProgramDraw draw;
        GlesProgramState::Program program;
        GlesRasterState raster;
        GlesResourceStore textures;
        std::uint32_t mode;
    };

    // Logical payload bytes, including container entries. Check before any
    // snapshot allocation. One producer preparation packet can coexist with
    // the bounded queued/running set; neither can grow without a fixed limit.
    std::size_t payload_bytes(const DisplayFrame& frame,
        const GlesProgramDraw& draw, const GlesRasterState& raster)
    {
        std::size_t bytes = sizeof(DrawPacket);
        const auto add = [&](std::size_t size, std::size_t count = 1) {
            if (count > (payload_limit - bytes) / size)
                throw std::length_error { "GLES draw snapshot exceeds 64 MiB" };
            bytes += size * count;
        };
        add(sizeof(std::uint32_t), frame.pixels.size());
        add(sizeof(std::uint16_t), draw.indices.size());
        for (const auto& attribute : draw.attributes) {
            add(sizeof(attribute));
            add(1, attribute.buffer.empty() ? attribute.client.size()
                                            : attribute.buffer.size());
        }
        for (const auto& [location, uniform] : draw.state->uniforms) {
            static_cast<void>(location);
            add(sizeof(uniform) + 4 * sizeof(void*));
            add(1, uniform.name.size());
            add(sizeof(float), uniform.values.size());
        }
        std::set<std::uint32_t> seen;
        for (const auto name : draw.textures) {
            const auto* texture = raster.resources && seen.insert(name).second
                                      ? raster.resources->texture(name)
                                      : nullptr;
            if (!texture)
                continue;
            add(sizeof(*texture) + 4 * sizeof(void*));
            add(2 * sizeof(std::uint32_t) + 4 * sizeof(void*),
                texture->parameters.size());
            for (const auto& [level, image] : texture->levels) {
                static_cast<void>(level);
                add(sizeof(image) + 4 * sizeof(void*));
                add(sizeof(std::uint32_t), image.argb.size());
            }
        }
        return bytes;
    }

    std::shared_ptr<const DrawPacket> snapshot(const DisplayFrame& frame,
        GlesRenderTargetKey target, const GlesProgramDraw& draw,
        std::uint32_t mode, const GlesRasterState& raster)
    {
        auto packet = std::make_shared<DrawPacket>();
        // Do not copy DisplayFrame readers, native surfaces or presentation
        // leases: EGL consumes only dimensions and the already materialized CPU
        // pixels. Keep all guest/presentation callbacks on their original side.
        packet->frame.width = frame.width;
        packet->frame.height = frame.height;
        packet->frame.pixels = frame.pixels;
        packet->target = target;
        packet->mode = mode;
        packet->draw = draw;
        for (auto& attribute : packet->draw.attributes) {
            if (!attribute.buffer.empty()) {
                attribute.client.assign(
                    attribute.buffer.begin(), attribute.buffer.end());
                // Preserve the VBO stride; the executor distinguishes packed
                // guest client arrays from strided VBO snapshots by this view.
                attribute.buffer = attribute.client;
            }
        }
        // Preparation has consumed attributes/link metadata. Execution uses
        // only uniform values; never copy declarations or guest object stores.
        packet->program.uniforms = draw.state->uniforms;
        packet->draw.state = &packet->program;
        packet->raster = raster;
        if (raster.resources) {
            for (const auto name : draw.textures) {
                if (packet->textures.has_texture(name))
                    continue;
                const auto* texture = raster.resources->texture(name);
                if (!texture)
                    continue;
                packet->textures.ensure_texture(name);
                *packet->textures.texture(name) = *texture;
                for (auto& [level, image] :
                    packet->textures.texture(name)->levels) {
                    static_cast<void>(level);
                    image.host_surface.reset();
                    image.surface_id.reset();
                }
            }
        }
        packet->raster.resources = &packet->textures;
        return packet;
    }
} // namespace

std::uint64_t gles_completion_generation()
{
    return completion_generation.load(std::memory_order_acquire);
}
void set_gles_completion_wake(std::function<void()> wake)
{
    std::lock_guard lock { wake_mutex };
    completion_wake = std::move(wake);
}
void wait_gles_host_activity(
    std::uint64_t observed, std::chrono::nanoseconds timeout)
{
    std::unique_lock lock { wake_mutex };
    host_activity.wait_for(lock, timeout,
        [observed] { return gles_completion_generation() != observed; });
}
void shutdown_gles_program_worker()
{
    std::shared_ptr<ProgramExecutor> retiring;
    {
        std::lock_guard lock { executor_mutex };
        retiring = std::move(executor_instance);
    }
    retiring.reset();
}
bool gles_host_work_capacity(std::size_t bytes)
{
    return shared_executor()->capacity(bytes);
}
std::shared_ptr<GlesWorkResult> enqueue_gles_host_work(
    std::size_t bytes, std::function<bool(DisplayFrame&)> execute)
{
    return shared_executor()->submit_host(bytes, std::move(execute));
}

struct GlesProgramWorker::State {
    std::shared_ptr<ProgramExecutor> executor { shared_executor() };
    std::shared_ptr<Backend> backend { std::make_shared<Backend>() };
    std::string name;
    std::set<std::uint32_t> linked;
    struct Pending {
        std::uint64_t sequence;
        std::size_t issuer;
        std::uint64_t epoch;
        std::future<bool> result;
    };
    std::deque<Pending> pending;
    std::set<std::size_t> errors;
    std::map<std::size_t, std::uint64_t> epochs;

    void reap()
    {
        while (!pending.empty()) {
            auto& entry = pending.front();
            if (entry.result.wait_for(std::chrono::seconds { 0 }) !=
                std::future_status::ready)
                break;
            bool success = false;
            try {
                success = entry.result.get();
            } catch (...) {
            }
            if (!success && entry.epoch == epochs.at(entry.issuer))
                errors.insert(entry.issuer);
            pending.pop_front();
        }
    }
};

GlesProgramWorker::GlesProgramWorker(std::unique_ptr<State> state)
    : state_ { std::move(state) }
{
}

std::unique_ptr<GlesProgramWorker> GlesProgramWorker::create(std::string* error)
{
    try {
        auto worker = std::unique_ptr<GlesProgramWorker> {
            new GlesProgramWorker { std::make_unique<State>() }
        };
        auto* state = worker->state_.get();
        auto result = state->executor->call(
            [backend = state->backend, executor = state->executor.get()] {
                std::string failure;
                // Retain before creation: exceptions must also destroy on
                // worker.
                executor->retain(backend);
                backend->renderer = create_gles_program_renderer(&failure);
                return std::pair {
                    backend->renderer
                        ? std::string { backend->renderer->name() }
                        : std::string { },
                    std::move(failure)
                };
            });
        if (!state->backend->renderer) {
            if (error)
                *error = std::move(result.second);
            return { };
        }
        state->name = std::move(result.first) + " [render-worker]";
        return worker;
    } catch (const std::exception& failure) {
        if (error)
            *error = failure.what();
        return { };
    }
}

GlesProgramWorker::~GlesProgramWorker()
{
    try {
        state_->executor->call(
            [backend = state_->backend, executor = state_->executor.get()] {
                executor->retire(backend);
            });
    } catch (...) {
        // The executor still owns this backend and retires it at shutdown.
    }
}

std::string_view GlesProgramWorker::name() const { return state_->name; }
bool GlesProgramWorker::contains(std::uint32_t program) const
{
    return state_->linked.contains(program);
}
std::optional<GlesProgramWorker::Limits> GlesProgramWorker::limits()
{
    try {
        return state_->executor->call([backend = state_->backend] {
            return backend->renderer->limits();
        });
    } catch (...) {
        return std::nullopt;
    }
}
GlesProgramWorker::LinkResult GlesProgramWorker::link(std::uint32_t program,
    std::string_view vertex, std::string_view fragment,
    const std::map<std::string, std::uint32_t, std::less<>>& bindings)
{
    LinkResult result;
    try {
        if (vertex.size() > payload_limit ||
            fragment.size() > payload_limit - vertex.size())
            throw std::length_error { "GLES shader sources exceed 64 MiB" };
        auto bytes = vertex.size() + fragment.size();
        for (const auto& [name, location] : bindings) {
            static_cast<void>(location);
            constexpr auto entry_bytes =
                sizeof(std::string) + sizeof(std::uint32_t) + 4 * sizeof(void*);
            if (entry_bytes > payload_limit - bytes ||
                name.size() > payload_limit - bytes - entry_bytes)
                throw std::length_error { "GLES link packet exceeds 64 MiB" };
            bytes += entry_bytes + name.size();
        }
        auto linked = state_->executor->call(
            [backend = state_->backend, program,
                vertex = std::string { vertex },
                fragment = std::string { fragment }, bindings] {
                auto linked_result = backend->renderer->link(
                    program, vertex, fragment, bindings);
                return std::pair { std::move(linked_result),
                    backend->renderer->contains(program) };
            },
            bytes);
        result = std::move(linked.first);
        if (linked.second)
            state_->linked.insert(program);
        else
            state_->linked.erase(program);
    } catch (const std::exception& failure) {
        result.log = failure.what();
    }
    return result;
}
bool GlesProgramWorker::erase(std::uint32_t program)
{
    try {
        auto result = state_->executor->call([backend = state_->backend,
                                                 program] {
            const auto erased = backend->renderer->erase(program);
            return std::pair { erased, backend->renderer->contains(program) };
        });
        if (!result.second)
            state_->linked.erase(program);
        return result.first;
    } catch (...) {
        return false;
    }
}
std::optional<std::array<std::int32_t, 3>> GlesProgramWorker::precision(
    std::uint32_t shader, std::uint32_t precision)
{
    try {
        return state_->executor->call(
            [backend = state_->backend, shader, precision] {
                return backend->renderer->precision(shader, precision);
            });
    } catch (...) {
        return std::nullopt;
    }
}
std::uint64_t GlesProgramWorker::enqueue_draw(const DisplayFrame& frame,
    GlesRenderTargetKey target, const GlesProgramDraw& draw, std::uint32_t mode,
    const GlesRasterState& raster, std::size_t issuer)
{
    if (!draw.state)
        throw std::invalid_argument { "GLES draw has no uniform state" };
    state_->reap();
    const auto epoch = state_->epochs[issuer];
    const auto bytes = payload_bytes(frame, draw, raster);
    auto packet = snapshot(frame, target, draw, mode, raster);
    // Allocate the result record before publication so no accepted draw can
    // lose its error/result association if producer allocation fails.
    state_->pending.emplace_back();
    try {
        auto [sequence, result] = state_->executor->submit(
            bytes, [backend = state_->backend, packet = std::move(packet)] {
                return backend->renderer->draw(packet->frame, packet->target,
                    packet->draw, packet->mode, packet->raster);
            });
        state_->pending.back() = { sequence, issuer, epoch, std::move(result) };
        return sequence;
    } catch (...) {
        state_->pending.pop_back();
        throw;
    }
}
std::vector<std::size_t> GlesProgramWorker::collect_errors()
{
    state_->reap();
    std::vector<std::size_t> errors(
        state_->errors.begin(), state_->errors.end());
    state_->errors.clear();
    return errors;
}
void GlesProgramWorker::retire_issuer(std::size_t issuer)
{
    if (auto epoch = state_->epochs.find(issuer); epoch != state_->epochs.end())
        ++epoch->second;
    state_->errors.erase(issuer);
}
std::uint64_t GlesProgramWorker::issuer_boundary(std::size_t issuer) const
{
    std::uint64_t boundary = 0;
    for (const auto& entry : state_->pending)
        if (entry.issuer == issuer && entry.epoch == state_->epochs.at(issuer))
            boundary = entry.sequence;
    return boundary;
}
bool GlesProgramWorker::issuer_ready(
    std::size_t issuer, std::uint64_t through) const
{
    for (const auto& entry : state_->pending)
        if (entry.issuer == issuer && entry.sequence <= through &&
            entry.epoch == state_->epochs.at(issuer) &&
            entry.result.wait_for(std::chrono::seconds { 0 }) !=
                std::future_status::ready)
            return false;
    return true;
}
std::shared_ptr<GlesWorkResult> GlesProgramWorker::enqueue_readback(
    GlesRenderTargetKey target, std::size_t bytes,
    std::shared_ptr<GlesRenderer> invalidate_renderer)
{
    return state_->executor->submit_host(bytes,
        [backend = state_->backend, target,
            renderer = std::move(invalidate_renderer)](DisplayFrame& frame) {
            if (!backend->renderer->readback(frame, target))
                return false;
            if (renderer)
                renderer->invalidate(target);
            return true;
        });
}
bool GlesProgramWorker::readback(
    DisplayFrame& frame, GlesRenderTargetKey target)
{
    try {
        auto result =
            state_->executor->call([backend = state_->backend, target] {
                DisplayFrame pixels;
                const auto success =
                    backend->renderer->readback(pixels, target);
                return std::pair { success, std::move(pixels) };
            });
        if (!result.first)
            return false;
        frame.width = result.second.width;
        frame.height = result.second.height;
        frame.pixels = std::move(result.second.pixels);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace ilemu
