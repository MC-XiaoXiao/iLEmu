/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <oaknut/oaknut.hpp>

namespace ilemu::execution::arm64 {
// Invocation-local page resolution, separate from invariant guest addresses.
// The low halves of V8/V9 are ABI-preserved and do not alias guest vectors.
class ReadPageCache {
public:
    ReadPageCache(oaknut::VectorCodeGenerator& code, bool enabled)
        : code_(code), enabled_(enabled)
    {
    }
    void enter(oaknut::VectorCodeGenerator& code) const
    {
        if (!used_)
            return;
        using namespace oaknut::util;
        // Existing sixteen-byte gap in the native frame.
        code.STP(D8, D9, SP, 176);
        code.MOV(W17, 0xffffffffU);
        code.FMOV(S8, W17);
    }
    void leave() const
    {
        if (used_)
            code_.LDP(oaknut::util::D8, oaknut::util::D9,
                oaknut::util::SP, 176);
    }
    void resolve(oaknut::WReg page, oaknut::Label& checked)
    {
        using namespace oaknut;
        using namespace oaknut::util;
        if (!enabled_) {
            code_.LDR(X21, X25, page, IndexExt::UXTW, 3);
            code_.CBZ(X21, checked);
            return;
        }
        used_ = true;
        const auto begin = code_.offset();
        Label cached, ready;
        // EOR/CBZ preserves the live guest NZCV flags.
        code_.FMOV(W16, S8);
        code_.EOR(W16, W16, page);
        code_.CBZ(W16, cached);
        code_.LDR(X21, X25, page, IndexExt::UXTW, 3);
        code_.CBZ(X21, checked);
        code_.FMOV(S8, page);
        code_.FMOV(D9, X21);
        // A miss already has an integer pointer; avoid a dependent trip
        // through the SIMD bank before the actual memory access.
        code_.B(ready);
        code_.l(cached);
        code_.FMOV(X21, D9);
        code_.l(ready);
        extra_words_ += static_cast<unsigned>((code_.offset() - begin) / 4) - 2U;
    }
    unsigned extra_words() const { return extra_words_; }

private:
    oaknut::VectorCodeGenerator& code_;
    bool enabled_, used_ = false;
    unsigned extra_words_ = 0;
};
}
