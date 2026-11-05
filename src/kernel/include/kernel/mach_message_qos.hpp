/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "mach/xnu_qos_policy.hpp"

namespace ilemu {

// ipc_kmsg keeps the sender's requested QoS separate from the override that
// protects earlier messages in the same FIFO (XNU ipc_kmsg_enqueue_qos).
struct MachMessageQos {
    XnuQosTier requested { };
    XnuQosTier override_tier { };
};

} // namespace ilemu
