/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/factory.hpp"
#include <string_view>

namespace ilemu {
// Empty selects the existing runtime backend; independent host allocation
// stays in the frontend, outside the emulated CPU and memory implementation.
execution::ExecutorFactory make_executor_factory(std::string_view backend);
}
