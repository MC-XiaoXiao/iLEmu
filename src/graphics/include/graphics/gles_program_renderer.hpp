// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "graphics/gles_program_state.hpp"
#include "graphics/gles_renderer.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ilemu {

// Guest memory and object names are decoded before crossing this boundary.
// The host compiler executes GLSL; no host API is exposed to the guest kernel.
struct GlesProgramDraw {
    struct Attribute {
        std::uint32_t location;
        std::uint32_t size { }, type { }, stride { };
        bool normalized { };
        std::array<float, 4> value { };
        // Preparation-only view. The command publisher snapshots buffer storage
        // before returning; host code never dereferences guest memory.
        std::span<const std::byte> buffer;
        std::vector<std::byte> client;
    };
    std::uint32_t program { };
    std::uint32_t vertex_count { };
    std::array<std::uint32_t, gles_abi::programmable_texture_unit_count>
        textures { };
    std::vector<Attribute> attributes;
    std::vector<std::uint16_t> indices;
    const GlesProgramState::Program* state { };
    bool depth_test { };
    bool depth_write { true };
    std::uint32_t depth_function { 0x0201U };
    float depth_clear { 1.0F };
    std::uint64_t depth_generation { };
};

class GlesProgramRenderer {
public:
    struct Limits {
        std::uint32_t vertex_attributes { };
        std::uint32_t vertex_uniform_vectors { };
        std::uint32_t fragment_uniform_vectors { };
        std::uint32_t varying_vectors { };
        std::uint32_t vertex_texture_units { };
        std::uint32_t combined_texture_units { };
    };
    struct LinkResult {
        bool linked { };
        std::map<std::string, std::uint32_t, std::less<>> attributes;
        std::string log;
        std::vector<GlesProgramState::ActiveVariable> active_attributes;
        std::vector<GlesProgramState::ActiveVariable> active_uniforms;
    };
    virtual ~GlesProgramRenderer() = default;
    [[nodiscard]] virtual std::optional<Limits> limits() = 0;
    [[nodiscard]] virtual LinkResult link(std::uint32_t program,
        std::string_view vertex, std::string_view fragment,
        const std::map<std::string, std::uint32_t, std::less<>>& bindings) = 0;
    [[nodiscard]] virtual bool erase(std::uint32_t program) = 0;
    [[nodiscard]] virtual bool contains(std::uint32_t program) const = 0;
    [[nodiscard]] virtual std::optional<std::array<std::int32_t, 3>> precision(
        std::uint32_t shader_type, std::uint32_t precision_type) = 0;
    // An empty input frame continues the target's queued GPU contents.
    [[nodiscard]] virtual bool draw(const DisplayFrame& frame,
        GlesRenderTargetKey target, const GlesProgramDraw& program,
        std::uint32_t mode, const GlesRasterState& state) = 0;
    // Publish queued GPU contents at a guest-visible synchronization boundary.
    [[nodiscard]] virtual bool readback(
        DisplayFrame& frame, GlesRenderTargetKey target) = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
};

// A retained host-owned result is also a queue credit: completed readbacks
// remain accounted until the core consumes or cancels their observer.
struct GlesWorkResult {
    enum class Status { Pending, Completed, Failed };
    std::atomic<Status> status { Status::Pending };
    std::uint64_t sequence { };
    DisplayFrame frame;
    std::function<void()> release_credit;
    ~GlesWorkResult()
    {
        if (release_credit)
            release_credit();
    }
    [[nodiscard]] bool ready() const
    {
        return status.load(std::memory_order_acquire) != Status::Pending;
    }
    [[nodiscard]] bool succeeded() const
    {
        return status.load(std::memory_order_acquire) == Status::Completed;
    }
};

// The worker writes value before publishing completion. Only the serial core
// consumes it afterwards; no guest pointers or callbacks belong in Value.
template <typename Value> struct GlesValueResult {
    std::shared_ptr<GlesWorkResult> completion;
    Value value;
};

// Same FIFO as programmable draws. Returns null on temporary capacity pressure;
// no driver call or wait takes place during admission. Functions own host data.
[[nodiscard]] std::shared_ptr<GlesWorkResult> enqueue_gles_host_work(
    std::size_t result_bytes, std::function<bool(DisplayFrame&)> execute);
[[nodiscard]] bool gles_host_work_capacity(std::size_t result_bytes);
[[nodiscard]] bool gles_draw_preparation_capacity(std::size_t payload_bytes);
[[nodiscard]] std::uint64_t gles_completion_generation();
void set_gles_completion_wake(std::function<void()> wake);
// Only a session's idle wait uses this, never a graphics call continuation.
void wait_gles_host_activity(
    std::uint64_t observed, std::chrono::nanoseconds timeout);
void shutdown_gles_program_worker();

// Core-facing command publisher. The executor above remains a host-only API;
// factory creation, all executor calls and destruction run on one shared render
// thread. No guest state or callbacks may cross this boundary.
class GlesProgramWorker {
public:
    using Limits = GlesProgramRenderer::Limits;
    using LinkResult = GlesProgramRenderer::LinkResult;
    struct Initialization {
        std::string name, error;
    };
    struct LinkedProgram {
        LinkResult result;
        bool present { };
    };
    struct ErasedProgram {
        bool erased { }, present { };
    };
    static constexpr std::size_t initialization_bytes = 64U * 1024U;
    static constexpr std::size_t link_result_bytes = 64U * 1024U * 1024U;
    static std::unique_ptr<GlesProgramWorker> begin_create(
        std::shared_ptr<GlesValueResult<Initialization>>& result);
    void complete_initialization(Initialization result);
    [[nodiscard]] bool initialized() const;
    [[nodiscard]] std::shared_ptr<GlesValueResult<std::optional<Limits>>>
    enqueue_limits();
    [[nodiscard]] std::shared_ptr<GlesValueResult<LinkedProgram>> enqueue_link(
        std::uint32_t program, std::string_view vertex,
        std::string_view fragment,
        const std::map<std::string, std::uint32_t, std::less<>>& bindings);
    [[nodiscard]] std::shared_ptr<GlesValueResult<ErasedProgram>> enqueue_erase(
        std::uint32_t program);
    [[nodiscard]] std::shared_ptr<
        GlesValueResult<std::optional<std::array<std::int32_t, 3>>>>
    enqueue_precision(std::uint32_t shader_type, std::uint32_t precision_type);
    void record_program(std::uint32_t program, bool present);
    // Transitional process teardown barrier, not an API-call return path.
    // Native owner retirement must be split before this can be removed.
    [[nodiscard]] bool drain() noexcept;
    ~GlesProgramWorker();
    [[nodiscard]] bool contains(std::uint32_t program) const;
    class PreparedDraw;
    // Freeze once before a capacity wait. No driver or guest reference is
    // retained; the backend control block and issuer epoch identify admission.
    [[nodiscard]] std::shared_ptr<PreparedDraw> prepare_draw(
        const DisplayFrame& frame, GlesRenderTargetKey target,
        const GlesProgramDraw& program, std::uint32_t mode,
        const GlesRasterState& state, std::size_t issuer,
        std::size_t& payload_bytes);
    [[nodiscard]] bool draw_capacity(
        const std::shared_ptr<PreparedDraw>& draw) const;
    // Null means retry admission; an accepted prepared draw always returns its
    // same ticket on repeated observation, never publishes a second command.
    [[nodiscard]] std::shared_ptr<GlesWorkResult> enqueue_draw(
        const std::shared_ptr<PreparedDraw>& draw);
    // Consume ready execution errors only. Guest-facing dependencies use the
    // retained readiness level and the kernel continuation, never future wait.
    [[nodiscard]] std::vector<std::size_t> collect_errors();
    // Guest thread-slot reuse must not inherit a retired issuer's errors.
    // Accepted commands still execute; only their now-absent observer retires.
    void retire_issuer(std::size_t issuer);
    [[nodiscard]] std::uint64_t issuer_boundary(std::size_t issuer) const;
    [[nodiscard]] bool issuer_ready(
        std::size_t issuer, std::uint64_t through = ~std::uint64_t { 0 }) const;
    [[nodiscard]] std::shared_ptr<GlesWorkResult> enqueue_readback(
        GlesRenderTargetKey target, std::size_t result_bytes,
        std::shared_ptr<GlesRenderer> invalidate_renderer = { });
    [[nodiscard]] bool readback(
        DisplayFrame& frame, GlesRenderTargetKey target);
    [[nodiscard]] std::string_view name() const;

private:
    struct State;
    explicit GlesProgramWorker(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

using GlesProgramRendererFactory = std::unique_ptr<GlesProgramRenderer> (*)(
    std::string*);
void configure_gles_program_renderer_factory(
    GlesProgramRendererFactory factory);
[[nodiscard]] std::unique_ptr<GlesProgramRenderer> create_gles_program_renderer(
    std::string* error);

} // namespace ilemu
