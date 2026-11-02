/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "foundation/cpu.hpp"
#include <utility>

namespace ilemu {
// Several backend runs can share one Guest execution turn. Keep accounting
// additive and retain the final stop payload at the outer serial boundary.
inline void accumulate_cpu_run(CpuRunResult& total, CpuRunResult part)
{
    total.ticks_consumed += part.ticks_consumed;
    total.svc_calls += part.svc_calls;
    total.host_execution_ns += part.host_execution_ns;
    total.host_yield_checks += part.host_yield_checks;
    total.translated_code |= part.translated_code;
    total.host_yielded |= part.host_yielded;
    total.reason = total.reason | part.reason;
    total.svc = part.svc;
    total.fault = std::move(part.fault);
    total.exception = std::move(part.exception);
    total.architectural_exception = part.architectural_exception;
    total.debug_breakpoint = part.debug_breakpoint;
}
} // namespace ilemu
