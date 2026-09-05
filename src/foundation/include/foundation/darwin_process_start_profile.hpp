#pragma once

#include <cstdint>

namespace ilemu {

// Darwin's initial apple-vector path entry changed after the iOS 4-era
// process ABI. Keep the choice in a named profile so ProcessLoader does not
// infer it from a firmware or application name.
enum class DarwinInitialAppleVectorProfile : std::uint8_t {
    KeyedExecutablePath,
    LegacyExecutablePath,
};

} // namespace ilemu
