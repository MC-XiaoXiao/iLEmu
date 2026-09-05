#include "foundation/jit_artifact.hpp"
#include <sstream>

namespace ilemu {

[[nodiscard]] std::string disk_hit_fingerprint_text(
    const JitArtifactStoreStats& stats)
{
    std::ostringstream text;
    text << std::hex;
    for (std::size_t index = 0; index < stats.disk_hit_key_fingerprint_count &&
                                index < stats.disk_hit_key_fingerprints.size();
        ++index) {
        if (index != 0U)
            text << ',';
        text << stats.disk_hit_key_fingerprints[index];
    }
    return text.str();
}
} // namespace ilemu
