// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "graphics/gles_program_renderer.hpp"
#include "graphics/gles_renderer_owner.hpp"
#include "graphics/gles_resources.hpp"

#include <algorithm>
#include <array>
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
        std::size_t preparations { }, preparation_bytes { };
    };

    struct PayloadCredit {
        std::function<void()> release;
        ~PayloadCredit()
        {
            if (release)
                release();
        }
    };

    constexpr std::size_t owner_limit = GlesRendererOwner::maximum_namespaces;
    struct RendererOwnerRecord {
        std::shared_ptr<GlesRenderer> renderer;
        std::uint64_t owner { }, through { };
        std::optional<HostSurfaceKey> target;
        bool retiring { }, failed { };
    };

    struct Backend {
        std::atomic_bool retiring { false };
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
            // future's shared state can outlive execution. Empty its callable
            // on the worker so ready boolean results do not retain deep draw
            // snapshots or Backend references after command-credit release.
            auto task = std::make_shared<std::packaged_task<Result()>>(
                [pending = std::make_unique<F>(
                     std::move(function))]() mutable -> Result {
                    auto execute = std::move(pending);
                    return (*execute)();
                });
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

        bool capacity(std::size_t bytes, std::size_t command_bytes = 0)
        {
            std::lock_guard lock { credits_->mutex };
            return !stopping_ && credits_->count < command_limit &&
                   bytes <= payload_limit - credits_->result_bytes &&
                   command_bytes <= payload_limit - credits_->bytes &&
                   credits_->results < command_limit;
        }
        bool preparation_capacity(std::size_t bytes)
        {
            std::lock_guard lock { credits_->mutex };
            return !stopping_ && credits_->preparations < command_limit &&
                   bytes <= payload_limit - credits_->preparation_bytes;
        }
        std::shared_ptr<PayloadCredit> reserve_preparation(std::size_t bytes)
        {
            if (bytes > payload_limit)
                throw std::length_error { "GLES preparation exceeds 64 MiB" };
            auto credit = std::make_shared<PayloadCredit>();
            auto armed = std::make_shared<bool>(false);
            credit->release = [credits = credits_, bytes, armed] {
                if (!*armed)
                    return;
                {
                    std::lock_guard lock { credits->mutex };
                    --credits->preparations;
                    credits->preparation_bytes -= bytes;
                }
                notify_completion();
            };
            std::lock_guard lock { credits_->mutex };
            if (stopping_ || credits_->preparations >= command_limit ||
                bytes > payload_limit - credits_->preparation_bytes)
                return { };
            ++credits_->preparations;
            credits_->preparation_bytes += bytes;
            *armed = true;
            return credit;
        }
        std::shared_ptr<GlesWorkResult> submit_host(std::size_t bytes,
            std::function<bool(DisplayFrame&)> execute,
            std::size_t command_bytes = 0)
        {
            if (bytes > payload_limit || command_bytes > payload_limit)
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
                command_bytes > payload_limit - credits_->bytes ||
                bytes > payload_limit - credits_->result_bytes)
                return { };
            if (next_sequence_ == 0)
                throw std::overflow_error { "GLES sequence space exhausted" };
            const auto sequence = next_sequence_;
            commands_.push_back({ sequence, command_bytes,
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
                } });
            result->sequence = sequence;
            ++next_sequence_;
            ++credits_->count;
            credits_->bytes += command_bytes;
            ++credits_->results;
            credits_->result_bytes += bytes;
            *armed = true;
            lock.unlock();
            available_.notify_one();
            return result;
        }

        bool owner_capacity()
        {
            std::lock_guard lock { credits_->mutex };
            return !stopping_ && owners_used_ < owner_limit;
        }
        bool retain_owner(const std::shared_ptr<RendererOwnerRecord>& owner)
        {
            std::lock_guard lock { credits_->mutex };
            if (stopping_ || owners_used_ >= owner_limit)
                return false;
            const auto slot = std::ranges::find(owners_, nullptr);
            *slot = owner;
            ++owners_used_;
            return true;
        }
        void request_owner_retirement(
            std::shared_ptr<RendererOwnerRecord> owner)
        {
            {
                std::lock_guard lock { credits_->mutex };
                // The single FIFO's completed_sequence_ is a continuous host
                // execution prefix. Native release separately waits its actual
                // GPU submission fence; host execution is not GPU completion.
                owner->through = next_sequence_ - 1U;
                owner->retiring = true;
                ++owners_retiring_;
                owner.reset();
                maintenance_ = true;
            }
            available_.notify_one();
        }

        // Only called on the render thread. Retention guarantees that even
        // allocation failure while publishing retirement cannot move an EGL
        // destructor to the core thread; final shutdown retires the remainder.
        void retain(const std::shared_ptr<Backend>& backend)
        {
            backends_.push_back(backend);
        }
        void request_retirement(std::shared_ptr<Backend> backend)
        {
            {
                std::lock_guard lock { credits_->mutex };
                backend->retiring.store(true, std::memory_order_release);
                // Drop the publisher's final reference before making the
                // maintenance predicate visible. Otherwise a wake could see
                // use_count==2 and miss the final drop with an empty FIFO.
                backend.reset();
                maintenance_ = true;
            }
            available_.notify_one();
        }

    private:
        struct Command {
            std::uint64_t sequence;
            std::size_t bytes;
            std::function<void()> execute;
        };
        void run()
        {
            for (;;) {
                Command command;
                {
                    std::unique_lock lock { credits_->mutex };
                    available_.wait(lock, [&] {
                        return stopping_ || maintenance_ || !commands_.empty();
                    });
                    maintenance_ = false;
                    if (commands_.empty()) {
                        const auto stop = stopping_;
                        lock.unlock();
                        retire_ready_backends();
                        retire_ready_owners();
                        if (stop)
                            break;
                        continue;
                    }
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
                retire_ready_backends();
                retire_ready_owners();
                credits_->capacity.notify_all();
                notify_completion();
            }
            backends_.clear();
            owners_.fill(nullptr);
        }

        void retire_ready_backends()
        {
            // Only the worker touches this list. Queued/running jobs retain
            // Backend too, so a retired publisher cannot destroy EGL state
            // until every accepted user has really released its ownership.
            std::erase_if(backends_, [](const auto& backend) {
                return backend->retiring.load(std::memory_order_acquire) &&
                       backend.use_count() == 1;
            });
        }

        void retire_ready_owners()
        {
            // Slots stay reserved across the driver call, including failure.
            // No registration/queue lock spans native release or a GPU wait.
            for (std::size_t i = 0; i < owners_.size(); ++i) {
                std::shared_ptr<RendererOwnerRecord> owner;
                {
                    std::lock_guard lock { credits_->mutex };
                    if (owners_retiring_ == 0)
                        return;
                    const auto& entry = owners_[i];
                    if (!entry || !entry->retiring || entry->failed ||
                        completed_sequence_ < entry->through)
                        continue;
                    owner = entry;
                }
                bool released = false;
                try {
                    released =
                        owner->target
                            ? owner->renderer->release(
                                  std::span { &*owner->target, 1U })
                            : owner->renderer->release_owner(owner->owner);
                } catch (...) {
                }
                {
                    std::lock_guard lock { credits_->mutex };
                    if (released) {
                        owners_[i].reset();
                        --owners_used_;
                    } else {
                        // Quarantine failed native namespaces until renderer
                        // shutdown. Their budget cannot fund unbounded retries.
                        owner->failed = true;
                    }
                    --owners_retiring_;
                }
                notify_completion();
            }
        }

        std::array<std::shared_ptr<RendererOwnerRecord>, owner_limit> owners_;
        std::size_t owners_used_ { }, owners_retiring_ { };
        std::shared_ptr<Credits> credits_ { std::make_shared<Credits>() };
        std::condition_variable available_;
        std::deque<Command> commands_;
        std::uint64_t next_sequence_ { 1 }, completed_sequence_ { };
        bool stopping_ { }, maintenance_ { };
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

    template <typename Value, typename Execute>
    std::shared_ptr<GlesValueResult<Value>> submit_value(
        const std::shared_ptr<ProgramExecutor>& executor, std::size_t bytes,
        Execute execute)
    {
        auto result = std::make_shared<GlesValueResult<Value>>();
        result->completion = executor->submit_host(
            bytes, [result, execute = std::move(execute)](
                       DisplayFrame&) { return execute(result->value); });
        return result->completion ? result : nullptr;
    }

    bool bounded_link_result(const GlesProgramRenderer::LinkResult& result)
    {
        std::size_t bytes = sizeof(result);
        const auto add = [&](std::size_t size) {
            if (size > payload_limit - bytes)
                return false;
            bytes += size;
            return true;
        };
        if (!add(result.log.size()))
            return false;
        for (const auto& [name, location] : result.attributes) {
            static_cast<void>(location);
            if (!add(sizeof(name) + sizeof(location) + 4 * sizeof(void*)) ||
                !add(name.size()))
                return false;
        }
        for (const auto* variables :
            { &result.active_attributes, &result.active_uniforms })
            for (const auto& variable : *variables)
                if (!add(sizeof(variable)) || !add(variable.name.size()))
                    return false;
        return true;
    }

    struct DrawPacket {
        std::shared_ptr<PayloadCredit> credit;
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
        const GlesProgramDraw& draw, const GlesRasterState& raster,
        bool read_target)
    {
        std::size_t bytes = sizeof(DrawPacket);
        const auto add = [&](std::size_t size, std::size_t count = 1) {
            if (count > (payload_limit - bytes) / size)
                throw std::length_error { "GLES draw snapshot exceeds 64 MiB" };
            bytes += size * count;
        };
        if (read_target) {
            // Retained output plus the worker's temporary readback frame. An
            // accelerated input frame can carry dimensions with no CPU pixels.
            const auto pixels =
                static_cast<std::uint64_t>(frame.width) * frame.height;
            if (pixels > payload_limit / (2U * sizeof(std::uint32_t)))
                throw std::length_error {
                    "GLES target readback exceeds 64 MiB"
                };
            add(sizeof(std::uint32_t), std::max(frame.pixels.size(),
                                           static_cast<std::size_t>(pixels)));
            add(sizeof(std::uint32_t), static_cast<std::size_t>(pixels));
        } else {
            add(sizeof(std::uint32_t), frame.pixels.size());
        }
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

    std::shared_ptr<DrawPacket> snapshot(const DisplayFrame& frame,
        GlesRenderTargetKey target, const GlesProgramDraw& draw,
        std::uint32_t mode, const GlesRasterState& raster,
        std::shared_ptr<PayloadCredit> credit)
    {
        auto packet = std::make_shared<DrawPacket>();
        packet->credit = std::move(credit);
        // Do not copy DisplayFrame readers, native surfaces or presentation
        // leases: EGL consumes dimensions and owned CPU pixels. A separately
        // accepted target readback can fill this frame before draw admission.
        // Keep guest/presentation callbacks on their original side.
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

struct GlesRendererOwner::State {
    std::shared_ptr<RendererOwnerRecord> record;
    std::shared_ptr<ProgramExecutor> executor;
    bool active { };
};

GlesRendererOwner::GlesRendererOwner(
    std::shared_ptr<GlesRenderer> renderer, std::uint64_t owner)
    : state_ { std::make_unique<State>() }
{
    state_->record = std::make_shared<RendererOwnerRecord>();
    state_->record->renderer = std::move(renderer);
    state_->record->owner = owner;
}

GlesRendererOwner::GlesRendererOwner(
    std::shared_ptr<GlesRenderer> renderer, HostSurfaceKey target)
    : state_ { std::make_unique<State>() }
{
    state_->record = std::make_shared<RendererOwnerRecord>();
    state_->record->renderer = std::move(renderer);
    state_->record->target = target;
}

GlesRendererOwner::~GlesRendererOwner()
{
    if (state_->active)
        state_->executor->request_owner_retirement(std::move(state_->record));
}

bool GlesRendererOwner::activate()
{
    if (state_->active)
        return true;
    if (!state_->executor)
        state_->executor = shared_executor();
    state_->active = state_->executor->retain_owner(state_->record);
    return state_->active;
}

bool GlesRendererOwner::active() const { return state_->active; }
bool GlesRendererOwner::capacity()
{
    return shared_executor()->owner_capacity();
}

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
    const auto changed = [observed] {
        return gles_completion_generation() != observed;
    };
    if (timeout == std::chrono::nanoseconds::max())
        host_activity.wait(lock, changed);
    else
        host_activity.wait_for(lock, timeout, changed);
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
bool gles_draw_preparation_capacity(std::size_t bytes)
{
    return shared_executor()->preparation_capacity(bytes);
}
std::shared_ptr<GlesWorkResult> enqueue_gles_host_work(
    std::size_t bytes, std::function<bool(DisplayFrame&)> execute)
{
    return shared_executor()->submit_host(bytes, std::move(execute));
}

class GlesProgramWorker::PreparedDraw {
    friend class GlesProgramWorker;
    std::weak_ptr<Backend> backend;
    std::shared_ptr<DrawPacket> packet;
    std::shared_ptr<GlesWorkResult> target_readback;
    bool read_target { };
    std::size_t bytes { }, issuer { };
    std::uint64_t epoch { };
    std::shared_ptr<GlesWorkResult> accepted;
};

struct GlesProgramWorker::State {
    std::shared_ptr<ProgramExecutor> executor { shared_executor() };
    std::shared_ptr<Backend> backend { std::make_shared<Backend>() };
    std::string name;
    bool published { }, initialized { };
    std::set<std::uint32_t> linked;
    struct Pending {
        std::uint64_t sequence;
        std::size_t issuer;
        std::uint64_t epoch;
        std::shared_ptr<GlesWorkResult> result;
    };
    std::deque<Pending> pending;
    std::set<std::size_t> errors;
    std::map<std::size_t, std::uint64_t> epochs;

    void reap()
    {
        while (!pending.empty()) {
            auto& entry = pending.front();
            if (!entry.result->ready())
                break;
            const auto success = entry.result->succeeded();
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

std::unique_ptr<GlesProgramWorker> GlesProgramWorker::begin_create(
    std::shared_ptr<GlesValueResult<Initialization>>& result)
{
    auto worker = std::unique_ptr<GlesProgramWorker> { new GlesProgramWorker {
        std::make_unique<State>() } };
    auto& state = *worker->state_;
    result = submit_value<Initialization>(state.executor, initialization_bytes,
        [backend = state.backend, executor = state.executor.get()](
            Initialization& info) {
            // Publish retention before anything can create a driver object.
            executor->retain(backend);
            backend->renderer = create_gles_program_renderer(&info.error);
            if (backend->renderer)
                info.name = std::string { backend->renderer->name() };
            if (info.name.size() > initialization_bytes - sizeof(info) ||
                info.error.size() >
                    initialization_bytes - sizeof(info) - info.name.size()) {
                info = { { }, "GLES initialization metadata exceeds budget" };
                return false;
            }
            return backend->renderer != nullptr;
        });
    if (!result)
        return { };
    state.published = true;
    return worker;
}

void GlesProgramWorker::complete_initialization(Initialization result)
{
    state_->name = std::move(result.name) + " [render-worker]";
    state_->initialized = true;
}

bool GlesProgramWorker::initialized() const { return state_->initialized; }

std::shared_ptr<GlesValueResult<std::optional<GlesProgramWorker::Limits>>>
GlesProgramWorker::enqueue_limits()
{
    return submit_value<std::optional<Limits>>(state_->executor,
        sizeof(std::optional<Limits>),
        [backend = state_->backend](std::optional<Limits>& result) {
            result = backend->renderer->limits();
            return result.has_value();
        });
}

std::shared_ptr<GlesValueResult<GlesProgramWorker::LinkedProgram>>
GlesProgramWorker::enqueue_link(std::uint32_t program, std::string_view vertex,
    std::string_view fragment,
    const std::map<std::string, std::uint32_t, std::less<>>& bindings)
{
    if (vertex.size() > payload_limit ||
        fragment.size() > payload_limit - vertex.size())
        throw std::length_error { "GLES shader sources exceed 64 MiB" };
    auto bytes = vertex.size() + fragment.size();
    for (const auto& [name, location] : bindings) {
        static_cast<void>(location);
        constexpr auto entry =
            sizeof(std::string) + sizeof(std::uint32_t) + 4 * sizeof(void*);
        if (entry > payload_limit - bytes ||
            name.size() > payload_limit - bytes - entry)
            throw std::length_error { "GLES link input exceeds 64 MiB" };
        bytes += entry + name.size();
    }
    // The input snapshot and at most one full-budget metadata result coexist.
    // This conservative credit also bounds captures outside the pixel payload.
    return submit_value<LinkedProgram>(state_->executor, link_result_bytes,
        [backend = state_->backend, program, vertex = std::string { vertex },
            fragment = std::string { fragment },
            bindings](LinkedProgram& value) {
            value.result =
                backend->renderer->link(program, vertex, fragment, bindings);
            value.present = backend->renderer->contains(program);
            if (!bounded_link_result(value.result)) {
                value.result = { };
                value.result.log = "GLES link metadata exceeds 64 MiB";
                return false;
            }
            // A failed guest link is a completed operation with its real log.
            return true;
        });
}

std::shared_ptr<GlesValueResult<GlesProgramWorker::ErasedProgram>>
GlesProgramWorker::enqueue_erase(std::uint32_t program)
{
    return submit_value<ErasedProgram>(state_->executor, sizeof(ErasedProgram),
        [backend = state_->backend, program](ErasedProgram& value) {
            value.erased = backend->renderer->erase(program);
            value.present = backend->renderer->contains(program);
            return true;
        });
}

std::shared_ptr<GlesValueResult<std::optional<std::array<std::int32_t, 3>>>>
GlesProgramWorker::enqueue_precision(
    std::uint32_t shader, std::uint32_t precision)
{
    using Value = std::optional<std::array<std::int32_t, 3>>;
    return submit_value<Value>(state_->executor, sizeof(Value),
        [backend = state_->backend, shader, precision](Value& value) {
            value = backend->renderer->precision(shader, precision);
            return value.has_value();
        });
}

void GlesProgramWorker::record_program(std::uint32_t program, bool present)
{
    if (present)
        state_->linked.insert(program);
    else
        state_->linked.erase(program);
}

GlesProgramWorker::~GlesProgramWorker()
{
    // Discard publisher-owned future references before posting maintenance.
    // Accepted packaged tasks keep their own storage until real execution.
    state_->pending.clear();
    if (state_->published)
        state_->executor->request_retirement(std::move(state_->backend));
}

std::string_view GlesProgramWorker::name() const { return state_->name; }
bool GlesProgramWorker::contains(std::uint32_t program) const
{
    return state_->linked.contains(program);
}
std::shared_ptr<GlesProgramWorker::PreparedDraw>
GlesProgramWorker::prepare_draw(const DisplayFrame& frame,
    GlesRenderTargetKey target, const GlesProgramDraw& draw, std::uint32_t mode,
    const GlesRasterState& raster, std::size_t issuer, std::size_t& bytes,
    bool read_target)
{
    if (!draw.state)
        throw std::invalid_argument { "GLES draw has no uniform state" };
    bytes = payload_bytes(frame, draw, raster, read_target);
    auto credit = state_->executor->reserve_preparation(bytes);
    if (!credit)
        return { };
    auto prepared = std::make_shared<PreparedDraw>();
    prepared->backend = state_->backend;
    prepared->issuer = issuer;
    prepared->epoch = state_->epochs[issuer];
    prepared->bytes = bytes;
    prepared->read_target = read_target;
    prepared->packet =
        snapshot(frame, target, draw, mode, raster, std::move(credit));
    return prepared;
}

std::shared_ptr<GlesWorkResult> GlesProgramWorker::enqueue_target_readback(
    const std::shared_ptr<PreparedDraw>& draw,
    std::shared_ptr<GlesRenderer> renderer,
    std::shared_ptr<HostSurface> surface)
{
    if (draw->target_readback)
        return draw->target_readback;
    const auto epoch = state_->epochs.find(draw->issuer);
    if (!draw->read_target || !renderer ||
        (surface && surface->key() != draw->packet->target) ||
        draw->backend.lock() != state_->backend ||
        epoch == state_->epochs.end() || epoch->second != draw->epoch)
        throw std::invalid_argument { "stale GLES target preparation" };
    auto result = state_->executor->submit_host(
        0,
        [packet = draw->packet, renderer = std::move(renderer),
            surface = std::move(surface)](DisplayFrame&) {
            const auto width = packet->frame.width;
            const auto height = packet->frame.height;
            auto frame = std::move(packet->frame);
            const auto read =
                surface ? renderer->read_surface(*surface, frame)
                        : renderer->synchronize(frame, packet->target);
            if (!read || frame.width != width || frame.height != height ||
                frame.pixels.size() != static_cast<std::size_t>(width) * height)
                return false;
            packet->frame = std::move(frame);
            return true;
        },
        draw->bytes);
    draw->target_readback = result;
    return result;
}

bool GlesProgramWorker::draw_capacity(
    const std::shared_ptr<PreparedDraw>& draw) const
{
    if (draw->accepted)
        return true;
    const auto epoch = state_->epochs.find(draw->issuer);
    // A stale request must be serviced as a failure, not wait for capacity
    // forever. Admission performs the same identity/epoch check before enqueue.
    if (draw->backend.lock() != state_->backend ||
        epoch == state_->epochs.end() || epoch->second != draw->epoch)
        return true;
    return state_->executor->capacity(0, draw->bytes);
}

std::shared_ptr<GlesWorkResult> GlesProgramWorker::enqueue_draw(
    const std::shared_ptr<PreparedDraw>& draw)
{
    if (draw->accepted)
        return draw->accepted;
    const auto epoch = state_->epochs.find(draw->issuer);
    if (draw->backend.lock() != state_->backend ||
        epoch == state_->epochs.end() || epoch->second != draw->epoch)
        throw std::invalid_argument { "stale GLES prepared draw" };
    if (draw->read_target &&
        (!draw->target_readback || !draw->target_readback->ready() ||
            !draw->target_readback->succeeded()))
        throw std::invalid_argument { "GLES target preparation incomplete" };
    state_->reap();
    // Allocate observer storage before publication, including the null-capacity
    // case. Nothing after successful admission allocates or can lose the job.
    state_->pending.emplace_back();
    try {
        auto result = state_->executor->submit_host(
            0,
            [backend = state_->backend, packet = draw->packet](DisplayFrame&) {
                return backend->renderer->draw(packet->frame, packet->target,
                    packet->draw, packet->mode, packet->raster);
            },
            draw->bytes);
        if (!result) {
            state_->pending.pop_back();
            return { };
        }
        state_->pending.back() = { result->sequence, draw->issuer, draw->epoch,
            result };
        draw->accepted = result;
        return result;
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
            entry.epoch == state_->epochs.at(issuer) && !entry.result->ready())
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
