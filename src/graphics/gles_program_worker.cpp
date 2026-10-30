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

    struct Backend {
        std::unique_ptr<GlesProgramRenderer> renderer;
    };

    // One host-wide FIFO also orders different processes' EGL contexts. The
    // worker never acquires kernel, guest memory, SurfaceStore or Vulkan locks.
    class ProgramExecutor {
    public:
        ProgramExecutor()
            : thread_ { [this] { run(); } }
        {
        }
        ~ProgramExecutor()
        {
            {
                std::lock_guard lock { mutex_ };
                stopping_ = true;
            }
            available_.notify_all();
            capacity_.notify_all();
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
            std::unique_lock lock { mutex_ };
            capacity_.wait(lock, [&] {
                return stopping_ || (count_ < command_limit &&
                                        bytes <= payload_limit - bytes_);
            });
            if (stopping_)
                throw std::runtime_error { "GLES render worker stopped" };
            const auto sequence = next_sequence_;
            commands_.push_back({ sequence, bytes, [task] { (*task)(); } });
            ++next_sequence_;
            ++count_;
            bytes_ += bytes;
            lock.unlock();
            available_.notify_one();
            return std::pair { sequence, std::move(result) };
        }

        template <typename F> auto call(F function, std::size_t bytes = 0)
        {
            return submit(bytes, std::move(function)).second.get();
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
        };
        void run()
        {
            for (;;) {
                Command command;
                {
                    std::unique_lock lock { mutex_ };
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
                    std::lock_guard lock { mutex_ };
                    completed_sequence_ = command.sequence;
                    --count_;
                    bytes_ -= command.bytes;
                }
                capacity_.notify_all();
            }
            backends_.clear();
        }

        std::mutex mutex_;
        std::condition_variable available_, capacity_;
        std::deque<Command> commands_;
        std::size_t count_ { }, bytes_ { };
        std::uint64_t next_sequence_ { 1 }, completed_sequence_ { };
        bool stopping_ { };
        std::vector<std::shared_ptr<Backend>> backends_;
        std::thread thread_;
    };

    std::shared_ptr<ProgramExecutor> shared_executor()
    {
        static std::mutex mutex;
        static std::weak_ptr<ProgramExecutor> executor;
        std::lock_guard lock { mutex };
        auto result = executor.lock();
        if (!result) {
            result = std::make_shared<ProgramExecutor>();
            executor = result;
        }
        return result;
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

    void reap(std::uint64_t through = 0)
    {
        while (!pending.empty()) {
            auto& entry = pending.front();
            if (entry.sequence > through &&
                entry.result.wait_for(std::chrono::seconds { 0 }) !=
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
std::vector<std::size_t> GlesProgramWorker::collect_errors(
    std::optional<std::size_t> wait_issuer)
{
    std::uint64_t through = 0;
    if (wait_issuer)
        for (const auto& entry : state_->pending)
            if (entry.issuer == *wait_issuer &&
                entry.epoch == state_->epochs.at(entry.issuer))
                through = entry.sequence;
    state_->reap(through);
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
