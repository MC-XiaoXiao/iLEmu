/* SPDX-License-Identifier: MPL-2.0 */
#include "app/executor_factory.hpp"
#include "arm64/executor.hpp"
#include "arm_interpreter/interpreter.hpp"
#include "host_execution/code_memory.hpp"
#include <stdexcept>

namespace ilemu {
execution::ExecutorFactory make_executor_factory(std::string_view backend)
{
    if (backend == "dynarmic")
        return { };
    if (backend == "interpreter")
        return [](std::size_t) {
            return std::make_unique<execution::ArmInterpreter>();
        };
    if (backend == "arm64") {
        if (!execution::Arm64Executor::available())
            throw std::runtime_error("ARM64 executor requires an AArch64 host");
        std::shared_ptr<execution::CodeAllocator> allocator =
            host::make_code_allocator();
        return [allocator = std::move(allocator)](std::size_t bytes) {
            return std::make_unique<execution::Arm64Executor>(
                *allocator, bytes);
        };
    }
    throw std::invalid_argument(
        "--executor must be dynarmic, interpreter or arm64");
}
}
