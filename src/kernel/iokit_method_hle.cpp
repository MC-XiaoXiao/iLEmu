// This Source Code Form is subject to the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/iokit_method_hle.hpp"

#include "foundation/cpu.hpp"
#include "foundation/output.hpp"
#include "foundation/userland_hle.hpp"
#include "kernel/iokit_abi.hpp"

#include <string>

namespace ilemu {
IOKitMethodHle::IOKitMethodHle(UserlandHleRegistry& registry)
{
    registry.register_function("/IOKit", "_IOConnectCallMethod",
        [this](UserlandHleCall& call) {
            const auto count = [&](std::size_t argument) {
                const auto pointer = call.argument(argument);
                return pointer ? call.memory().read32(pointer).value_or(0U) : 0U;
            };
            auto structure_capacity = count(9U);
            // The public API moves large structures out of line; their inline
            // capacity is zero in the generated request.
            if (structure_capacity > iokit_abi::connect_method::maximum_inband_size)
                structure_capacity = 0U;
            const auto processor = call.cpu().processor_id();
            calls_[processor].push_back({ call.argument(0), call.argument(1),
                count(7U), structure_capacity, scalar_first_ });
            call.resume_original_persistently(
                [this, processor](UserlandHleCall& completed) {
                    const auto found = calls_.find(processor);
                    if (found == calls_.end() || found->second.empty())
                        return;
                    const auto learned = found->second.back().scalar_first;
                    if (learned && learned != scalar_first_) {
                        scalar_first_ = learned;
                        completed.output().write(
                            "[loader] IOKit native method output-order=" +
                            std::string { *learned ? "scalar-first" : "structure-first" } +
                            " pid=" + std::to_string(completed.process_id()) + "\n");
                    }
                    found->second.pop_back();
                    if (found->second.empty())
                        calls_.erase(found);
                });
        });
}

void IOKitMethodHle::reset()
{
    calls_.clear();
    scalar_first_.reset();
}

IOKitMethodHle::Call* IOKitMethodHle::current(std::size_t processor)
{
    const auto found = calls_.find(processor);
    return found == calls_.end() || found->second.empty()
               ? nullptr : &found->second.back();
}
} // namespace ilemu
