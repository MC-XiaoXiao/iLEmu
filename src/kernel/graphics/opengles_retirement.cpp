// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "kernel/opengles_hle.hpp"

#include <limits>
#include <new>
#include <utility>

namespace ilemu {

struct OpenGlesHle::TextureDeletion {
    // The host-only snapshot and its credit outlive either observer.
    std::shared_ptr<void> credit;
    std::vector<std::uint32_t> names;
    std::vector<GlesRenderTargetKey> targets;
    std::shared_ptr<GlesWorkResult> result;
    std::uint64_t owner { };
    std::size_t bytes { };
};

void OpenGlesHle::commit_texture_deletion(std::span<const std::uint32_t> names)
{
    for (const auto name : names) {
        resources_.erase_texture(name);
        for (auto& [context_name, context] : contexts_) {
            static_cast<void>(context_name);
            for (auto& unit : context.texture_units) {
                if (unit.bound_texture_2d == name)
                    unit.bound_texture_2d = 0;
                if (unit.bound_texture_rectangle == name)
                    unit.bound_texture_rectangle = 0;
            }
            for (auto& [framebuffer_name, framebuffer] : context.framebuffers) {
                static_cast<void>(framebuffer_name);
                if (framebuffer.color_texture == name)
                    framebuffer.color_texture = 0;
            }
        }
    }
}

void OpenGlesHle::delete_names(UserlandHleCall& call)
{
    if (!flush_program_draws(call))
        return;
    const auto signed_count = static_cast<std::int32_t>(call.argument(0));
    const auto input = call.argument(1);
    if (!current_context(call)) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    if (signed_count < 0 || (signed_count != 0 && input == 0)) {
        set_gl_error(call, gles_abi::invalid_value);
        return;
    }
    const auto count = static_cast<std::uint32_t>(signed_count);
    const auto read_name = [&](std::uint32_t index) {
        const auto address = static_cast<std::uint64_t>(input) +
                             static_cast<std::uint64_t>(index) * 4U;
        return address <= std::numeric_limits<std::uint32_t>::max()
                   ? call.memory().read32(static_cast<std::uint32_t>(address))
                   : std::optional<std::uint32_t> { };
    };
    if (call.symbol() != "_glDeleteTextures") {
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto name = read_name(index);
            if (!name) {
                set_gl_error(call, gles_abi::invalid_value);
                break;
            }
            resources_.erase_buffer(*name);
            for (auto& [context_name, context] : contexts_) {
                static_cast<void>(context_name);
                if (context.bound_array_buffer == *name)
                    context.bound_array_buffer = 0;
                if (context.bound_element_array_buffer == *name)
                    context.bound_element_array_buffer = 0;
            }
        }
        return;
    }

    constexpr std::size_t limit = 64U * 1024U * 1024U;
    const auto name_bytes = static_cast<std::uint64_t>(count) * 4U;
    if (name_bytes > limit) {
        set_gl_error(call, gles_abi::out_of_memory);
        return;
    }
    const auto visit_targets = [&](std::uint32_t name, auto&& visit) {
        if (const auto* texture = resources_.texture(name)) {
            for (const auto& [level_index, level] : texture->levels) {
                static_cast<void>(level_index);
                if (!level.surface_id && level.host_surface &&
                    !level.native_lifetime &&
                    level.host_surface->key().owner == renderer_owner_)
                    visit(level.host_surface->key());
            }
        }
    };
    // This is a serial core boundary: no guest batch or graphics-store
    // mutation runs between sizing and copying. Reserve one credit for both
    // arrays, so many callers cannot each hold half an admission budget.
    std::uint32_t valid_count = 0;
    std::size_t target_count = 0;
    const auto maximum_targets =
        (limit - static_cast<std::size_t>(name_bytes)) /
        sizeof(GlesRenderTargetKey);
    bool oversized = false;
    for (; valid_count < count; ++valid_count) {
        const auto name = read_name(valid_count);
        if (!name)
            break;
        visit_targets(*name, [&](GlesRenderTargetKey) {
            if (target_count == maximum_targets)
                oversized = true;
            else
                ++target_count;
        });
        if (oversized) {
            set_gl_error(call, gles_abi::out_of_memory);
            return;
        }
    }
    if (valid_count != count)
        set_gl_error(call, gles_abi::invalid_value);
    if (valid_count == 0)
        return;
    std::shared_ptr<TextureDeletion> deletion;
    std::shared_ptr<SyncLease> lease;
    try {
        const auto bytes = static_cast<std::size_t>(name_bytes) +
                           target_count * sizeof(GlesRenderTargetKey);
        auto credit = reserve_gles_host_preparation(bytes);
        if (!credit) {
            call.suspend_until(
                [bytes] { return gles_host_preparation_capacity(bytes); },
                [this](UserlandHleCall& resumed) { delete_names(resumed); });
            return;
        }
        deletion = std::make_shared<TextureDeletion>();
        deletion->credit = std::move(credit);
        deletion->bytes = bytes;
        deletion->owner = renderer_owner_;
        deletion->names.reserve(valid_count);
        deletion->targets.reserve(target_count);
        for (std::uint32_t index = 0; index < valid_count; ++index) {
            const auto name = read_name(index);
            if (!name) {
                set_gl_error(call, gles_abi::invalid_value);
                return;
            }
            deletion->names.push_back(*name);
            visit_targets(*name, [&](GlesRenderTargetKey target) {
                deletion->targets.push_back(target);
            });
        }
        if (deletion->targets.empty()) {
            commit_texture_deletion(deletion->names);
            return;
        }
        if (deletion->names.capacity() > count ||
            deletion->targets.capacity() > target_count) {
            set_gl_error(call, gles_abi::out_of_memory);
            return;
        }
        lease = program_wait(call);
        // A canceled issuing thread still commits accepted deletion to the
        // shared store. Process exit/exec discards this core callback instead.
        lease->complete_core = [this, deletion] {
            if (deletion->result && deletion->result->succeeded() &&
                deletion->owner == renderer_owner_)
                commit_texture_deletion(deletion->names);
        };
        await_texture_deletion(call, std::move(deletion), std::move(lease));
    } catch (const std::bad_alloc&) {
        if (lease && !lease->result)
            lease->consume();
        set_gl_error(call, gles_abi::out_of_memory);
    }
}

void OpenGlesHle::await_texture_deletion(UserlandHleCall& call,
    std::shared_ptr<TextureDeletion> deletion, std::shared_ptr<SyncLease> lease)
{
    try {
        // Allocate the observer closures before accepting work. After enqueue
        // succeeds, publishing these functions only moves owned storage.
        std::function<bool()> ready = [deletion] {
            return deletion->result->ready();
        };
        UserlandHleCall::Continuation completed =
            [this, deletion, lease](UserlandHleCall& resumed) {
                const auto result = deletion->result;
                lease->consume();
                if (!result->succeeded() || deletion->owner != renderer_owner_)
                    set_gl_error(resumed, gles_abi::invalid_operation);
            };
        auto result = enqueue_gles_host_work(
            0,
            [renderer = renderer_, deletion](
                DisplayFrame&) { return renderer->release(deletion->targets); },
            deletion->bytes);
        if (!result) {
            call.suspend_until(
                [bytes = deletion->bytes] {
                    return gles_host_work_capacity(0, bytes);
                },
                [this, deletion, lease](UserlandHleCall& resumed) {
                    await_texture_deletion(resumed, deletion, lease);
                });
            return;
        }
        deletion->result = result;
        lease->result = result;
        call.suspend_until(
            std::move(ready), std::move(completed), result->sequence());
    } catch (const std::bad_alloc&) {
        lease->consume();
        set_gl_error(call, gles_abi::out_of_memory);
    } catch (const std::exception&) {
        lease->consume();
        set_gl_error(call, gles_abi::invalid_operation);
    }
}

} // namespace ilemu
