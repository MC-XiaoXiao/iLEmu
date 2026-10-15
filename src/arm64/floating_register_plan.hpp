/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "scalar_register_plan.hpp"

namespace ilemu::execution::arm64 {
// Scalar reservations require an entry cycle; FP controls cover the whole trace.
struct FloatingRegisterPlan {
    ScalarRegisterPlan scalars;
    bool standard_controls = false;
    class Builder;
};
class FloatingRegisterPlan::Builder {
public:
    void observe(const arm::FloatingArithmeticOperands& value)
    {
        scalar_arithmetic_ = true;
        scalars_.observe(value);
    }
    void observe(const arm::VectorFloatingArithmeticOperands&)
    {
        vector_arithmetic_ = true;
        packed_ = true;
    }
    void observe(const arm::VectorTransferOperands& value, bool load)
    {
        scalars_.observe(value, load);
    }
    void use_packed() { packed_ = true; }
    FloatingRegisterPlan finish(bool reserve_scalars = true) const
    {
        // Packed NEON consumers keep all guest banks available. Scalar-only
        // cycles retain their existing reservations and accumulation units.
        return { reserve_scalars && !packed_ ? scalars_.finish() : ScalarRegisterPlan {},
            vector_arithmetic_ && !scalar_arithmetic_ };
    }

private:
    ScalarRegisterPlan::Builder scalars_;
    bool scalar_arithmetic_ = false, vector_arithmetic_ = false, packed_ = false;
};
}
