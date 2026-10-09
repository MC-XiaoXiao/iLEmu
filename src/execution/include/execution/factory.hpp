/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/run.hpp"
#include <cstddef>
#include <functional>
#include <memory>

namespace ilemu::execution {
// Runtime slots request bounded backends; the host owns code allocation policy.
using ExecutorFactory =
    std::function<std::unique_ptr<Executor>(std::size_t cache_bytes)>;
}
