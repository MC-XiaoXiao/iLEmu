// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/output.hpp"
#include "kernel/opengles_hle.hpp"

#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace ilemu {

std::shared_ptr<OpenGlesHle::SyncLease> OpenGlesHle::program_wait(
    UserlandHleCall& call)
{
    if (sync_lease_ && !sync_lease_->consumed) {
        if (sync_lease_->owner != call.cpu().processor_id())
            throw std::logic_error { "graphics wait bypassed its owner gate" };
        return sync_lease_;
    }
    auto lease = std::make_shared<SyncLease>();
    lease->owner = call.cpu().processor_id();
    sync_lease_ = lease;
    return lease;
}

void OpenGlesHle::park_program_job(UserlandHleCall& call,
    std::shared_ptr<SyncLease> lease, UserlandHleRegistry::Handler completion)
{
    const auto result = lease->result;
    call.suspend_for_host([result] { return result->ready(); },
        [this, lease, completion = std::move(completion)](
            UserlandHleCall& resumed) {
            try {
                lease->consume();
            } catch (const std::bad_alloc&) {
                set_gl_error(resumed, gles_abi::out_of_memory);
                return;
            } catch (...) {
                set_gl_error(resumed, gles_abi::invalid_operation);
                return;
            }
            completion(resumed);
        },
        result->sequence());
}

void OpenGlesHle::with_program_renderer(
    UserlandHleCall& call, ProgramCompletion completion)
{
    if (program_renderer_ && program_renderer_->initialized()) {
        completion(call, true);
        return;
    }
    const auto lease = program_wait(call);
    std::shared_ptr<GlesValueResult<GlesProgramWorker::Initialization>> result;
    try {
        program_renderer_ = GlesProgramWorker::begin_create(result);
    } catch (const std::exception& failure) {
        program_renderer_error_ = failure.what();
        lease->consume();
        completion(call, false);
        return;
    }
    if (!result) {
        call.suspend_for_host(
            [] {
                return gles_host_work_capacity(
                    GlesProgramWorker::initialization_bytes);
            },
            [this, completion = std::move(completion)](
                UserlandHleCall& resumed) {
                with_program_renderer(resumed, completion);
            });
        return;
    }
    lease->result = result->completion;
    lease->complete_core = [this, result, owner = renderer_owner_] {
        if (owner != renderer_owner_)
            return;
        if (result->completion->succeeded()) {
            program_renderer_->complete_initialization(
                std::move(result->value));
            program_renderer_error_.clear();
        } else {
            program_renderer_error_ = result->value.error.empty()
                                          ? "cannot initialize GLES executor"
                                          : std::move(result->value.error);
            // Retirement is a worker request, including on failed creation.
            program_renderer_.reset();
        }
    };
    park_program_job(call, lease,
        [this, completion = std::move(completion)](UserlandHleCall& resumed) {
            const auto ready =
                program_renderer_ && program_renderer_->initialized();
            if (ready)
                resumed.output().marker(
                    "[gles] GLSL executor=" +
                    std::string { program_renderer_->name() });
            completion(resumed, ready);
        });
}

void OpenGlesHle::with_program_limits(
    UserlandHleCall& call, ProgramCompletion completion)
{
    if (program_limits_) {
        completion(call, true);
        return;
    }
    with_program_renderer(call, [this, completion = std::move(completion)](
                                    UserlandHleCall& ready, bool available) {
        if (!available) {
            completion(ready, false);
            return;
        }
        const auto lease = program_wait(ready);
        std::shared_ptr<
            GlesValueResult<std::optional<GlesProgramWorker::Limits>>>
            result;
        try {
            result = program_renderer_->enqueue_limits();
        } catch (...) {
            lease->consume();
            completion(ready, false);
            return;
        }
        if (!result) {
            ready.suspend_for_host(
                [] {
                    return gles_host_work_capacity(
                        sizeof(std::optional<GlesProgramWorker::Limits>));
                },
                [this, completion](UserlandHleCall& resumed) {
                    with_program_limits(resumed, completion);
                });
            return;
        }
        lease->result = result->completion;
        lease->complete_core = [this, result, owner = renderer_owner_] {
            if (owner == renderer_owner_)
                program_limits_ = result->completion->succeeded()
                                      ? result->value
                                      : std::nullopt;
        };
        park_program_job(
            ready, lease, [this, completion](UserlandHleCall& resumed) {
                completion(resumed, program_limits_.has_value());
            });
    });
}

void OpenGlesHle::enqueue_program_link(UserlandHleCall& call,
    std::uint32_t name, bool restore, ProgramCompletion completion)
{
    const auto lease = program_wait(call);
    const auto* declaration = programs_.program(name);
    if (!declaration) {
        lease->consume();
        completion(call, false);
        return;
    }
    std::shared_ptr<GlesValueResult<GlesProgramWorker::LinkedProgram>> result;
    try {
        result = program_renderer_->enqueue_link(name,
            programs_.shader_source(name, gles_abi::vertex_shader),
            programs_.shader_source(name, gles_abi::fragment_shader),
            declaration->attributes);
    } catch (const std::exception& failure) {
        program_renderer_error_ = failure.what();
        if (!restore) {
            auto* failed = programs_.program(name);
            failed->linked = false;
            failed->native_execution = false;
            failed->info_log = program_renderer_error_;
        }
        lease->consume();
        completion(call, false);
        return;
    }
    if (!result) {
        call.suspend_for_host(
            [] {
                return gles_host_work_capacity(
                    GlesProgramWorker::link_result_bytes);
            },
            [this, name, restore, completion](UserlandHleCall& resumed) {
                enqueue_program_link(resumed, name, restore, completion);
            });
        return;
    }
    lease->result = result->completion;
    lease->complete_core = [this, result, owner = renderer_owner_, name,
                               restore] {
        if (owner != renderer_owner_)
            return;
        try {
            const auto completed = result->completion->succeeded();
            program_renderer_->record_program(
                name, completed && result->value.present);
            if (restore)
                return; // inherited declarations/uniforms survive host
                        // restoration
            auto* program = programs_.program(name);
            if (!program)
                return;
            auto& linked = result->value.result;
            program->linked = completed && linked.linked;
            program->native_execution = program->linked;
            program->info_log = completed ? std::move(linked.log)
                                          : "GLES link execution failed";
            if (program->linked) {
                program->attributes = std::move(linked.attributes);
                program->active_attributes =
                    std::move(linked.active_attributes);
                program->active_uniforms = std::move(linked.active_uniforms);
                program->uniform_locations.clear();
                program->uniforms.clear();
                programs_.refresh_draw_attributes(name);
            }
        } catch (...) {
            if (!restore)
                if (auto* failed = programs_.program(name)) {
                    failed->linked = false;
                    failed->native_execution = false;
                }
            throw;
        }
    };
    park_program_job(
        call, lease, [result, completion](UserlandHleCall& resumed) {
            completion(resumed,
                result->completion->succeeded() && result->value.result.linked);
        });
}

void OpenGlesHle::link_program(UserlandHleCall& call)
{
    const auto name = call.argument(0);
    if (!programs_.program(name)) {
        set_gl_error(call, gles_abi::invalid_value);
        return;
    }
    static_cast<void>(programs_.link(name));
    auto* prepared = programs_.program(name);
    prepared->native_execution = false;
    prepared->info_log.clear();
    if (!prepared->linked ||
        !prepared->interface_profile.requires_glsl_execution)
        return;
    // Until a host link is actually accepted/completed, an abandoned call
    // must not leave a successful guest executable with no native program.
    prepared->linked = false;
    with_program_renderer(
        call, [this, name](UserlandHleCall& ready, bool available) {
            if (!available) {
                if (auto* failed = programs_.program(name)) {
                    failed->linked = false;
                    failed->info_log = program_renderer_error_;
                }
                ready.output().marker("[gles] GLSL executor unavailable: " +
                                      program_renderer_error_);
                return;
            }
            enqueue_program_link(ready, name, false,
                [this, name](UserlandHleCall& completed, bool linked) {
                    const auto* program = programs_.program(name);
                    if (!linked && program)
                        completed.output().marker(
                            "[gles] GLSL link failed: " + program->info_log);
                });
        });
}

void OpenGlesHle::restore_program_for_draw(UserlandHleCall& call, bool indexed)
{
    const auto* context = current_context(call);
    const auto name = context ? context->current_program : 0U;
    with_program_renderer(
        call, [this, name, indexed](UserlandHleCall& ready, bool available) {
            if (!available) {
                set_gl_error(ready, gles_abi::invalid_operation);
                return;
            }
            const auto finish = [this, indexed](
                                    UserlandHleCall& resumed, bool linked) {
                if (linked)
                    draw(resumed, indexed);
                else
                    set_gl_error(resumed, gles_abi::invalid_operation);
            };
            if (program_renderer_->contains(name))
                finish(ready, true);
            else
                enqueue_program_link(ready, name, true, finish);
        });
}

void OpenGlesHle::delete_program(UserlandHleCall& call, bool prepared)
{
    const auto name = call.argument(0);
    if (!prepared) {
        if (name != 0U && !programs_.program(name)) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        for (auto& [context_name, context] : contexts_) {
            static_cast<void>(context_name);
            if (context.current_program == name)
                context.current_program = 0U;
        }
        programs_.delete_program(name);
    }
    if (!program_renderer_)
        return;
    const auto lease = program_wait(call);
    std::shared_ptr<GlesValueResult<GlesProgramWorker::ErasedProgram>> result;
    try {
        result = program_renderer_->enqueue_erase(name);
    } catch (...) {
        lease->consume();
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    if (!result) {
        call.suspend_for_host(
            [] {
                return gles_host_work_capacity(
                    sizeof(GlesProgramWorker::ErasedProgram));
            },
            [this](
                UserlandHleCall& resumed) { delete_program(resumed, true); });
        return;
    }
    lease->result = result->completion;
    lease->complete_core = [this, result, name, owner = renderer_owner_] {
        if (owner == renderer_owner_)
            program_renderer_->record_program(
                name, result->completion->succeeded() && result->value.present);
    };
    park_program_job(call, lease, [this, result](UserlandHleCall& resumed) {
        if (!result->completion->succeeded() || !result->value.erased)
            set_gl_error(resumed, gles_abi::invalid_operation);
    });
}

void OpenGlesHle::shader_precision(UserlandHleCall& call)
{
    const auto shader = call.argument(0), type = call.argument(1);
    if ((shader != gles_abi::vertex_shader &&
            shader != gles_abi::fragment_shader) ||
        type < 0x8df0U || type > 0x8df5U) {
        set_gl_error(call, gles_abi::invalid_enum);
        return;
    }
    with_program_renderer(call, [this, shader, type](
                                    UserlandHleCall& ready, bool available) {
        if (!available) {
            set_gl_error(ready, gles_abi::invalid_operation);
            return;
        }
        using Value = std::optional<std::array<std::int32_t, 3>>;
        const auto lease = program_wait(ready);
        std::shared_ptr<GlesValueResult<Value>> result;
        try {
            result = program_renderer_->enqueue_precision(shader, type);
        } catch (...) {
            lease->consume();
            set_gl_error(ready, gles_abi::invalid_operation);
            return;
        }
        if (!result) {
            ready.suspend_for_host(
                [] { return gles_host_work_capacity(sizeof(Value)); },
                [this](
                    UserlandHleCall& resumed) { shader_precision(resumed); });
            return;
        }
        lease->result = result->completion;
        park_program_job(
            ready, lease, [this, result](UserlandHleCall& resumed) {
                if (!result->completion->succeeded() || !result->value) {
                    set_gl_error(resumed, gles_abi::invalid_operation);
                    return;
                }
                const auto& value = *result->value;
                const auto output = resumed.argument(2);
                if (!output ||
                    output > std::numeric_limits<std::uint32_t>::max() - 4U ||
                    !resumed.argument(3) ||
                    !resumed.memory().write32(
                        output, static_cast<std::uint32_t>(value[0])) ||
                    !resumed.memory().write32(
                        output + 4U, static_cast<std::uint32_t>(value[1])) ||
                    !resumed.memory().write32(resumed.argument(3),
                        static_cast<std::uint32_t>(value[2])))
                    set_gl_error(resumed, gles_abi::invalid_value);
            });
    });
}

} // namespace ilemu
