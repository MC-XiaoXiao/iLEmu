#include "ilemu/big_number.hpp"

#include <limits>
#include <memory>
#include <openssl/bn.h>

namespace ilemu {

bool BigNumberArithmetic::power_modulo(std::span<const std::byte> base,
    std::span<const std::byte> exponent, std::span<const std::byte> modulus,
    std::span<std::byte> result)
{
    if (result.empty() || result.size() > std::numeric_limits<int>::max() ||
        base.size() != result.size() || exponent.size() != result.size() ||
        modulus.size() != result.size()) {
        return false;
    }
    // Each operation owns its workspace; independent guest CPUs do not share
    // OpenSSL scratch state. The secure context clears intermediate values.
    std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> context {
        BN_CTX_secure_new(), BN_CTX_free
    };
    if (!context)
        return false;
    BN_CTX_start(context.get());
    auto* a = BN_CTX_get(context.get());
    auto* e = BN_CTX_get(context.get());
    auto* m = BN_CTX_get(context.get());
    auto* r = BN_CTX_get(context.get());
    if (!r)
        return false;
    const auto decode = [](auto bytes, BIGNUM* number) {
        return BN_lebin2bn(reinterpret_cast<const unsigned char*>(bytes.data()),
                   static_cast<int>(bytes.size()), number) != nullptr;
    };
    if (!decode(base, a) || !decode(exponent, e) || !decode(modulus, m) ||
        BN_is_zero(m) || BN_is_one(m) || !BN_is_odd(m)) {
        return false;
    }
    // Use the constant-time Montgomery implementation for secret exponents.
    if (BN_mod_exp_mont_consttime(r, a, e, m, context.get(), nullptr) != 1)
        return false;
    return BN_bn2lebinpad(r, reinterpret_cast<unsigned char*>(result.data()),
               static_cast<int>(result.size())) ==
           static_cast<int>(result.size());
}

} // namespace ilemu
