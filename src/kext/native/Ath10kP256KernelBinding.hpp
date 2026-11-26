#pragma once
// Isolated diagnostic translation units only: private, version-sensitive ABI.
// Constructing these tables calls no crypto. Export linkage is not ABI approval.
#ifndef ATH10K_P256_DIAGNOSTIC_BINDING
#error "P-256 kernel binding requires explicit diagnostic compilation"
#endif
#include "../../shared/Ath10kP256CoreCrypto.hpp"
namespace Ath10kP256KernelSymbols {
#ifdef ATH10K_P256_SYMBOLS_TEST
#define ATH10K_P256_SYMBOL(result,name,args) static result (*name) args=nullptr
#else
#define ATH10K_P256_SYMBOL(result,name,args) extern "C" result name args
#endif
ATH10K_P256_SYMBOL(const void *,ccec_cp_256,());
ATH10K_P256_SYMBOL(int,ccec_x963_import_pub,(const void *,size_t,const uint8_t *,void *));
ATH10K_P256_SYMBOL(bool,ccec_validate_pub,(void *));
ATH10K_P256_SYMBOL(int,ccec_x963_export,(int,void *,const void *));
ATH10K_P256_SYMBOL(int,ccec_projectify,(const void *,void *,const void *,void *));
ATH10K_P256_SYMBOL(int,ccec_affinify,(const void *,void *,const void *));
ATH10K_P256_SYMBOL(int,ccec_mult_blinded,(const void *,void *,const uint64_t *,const void *,void *));
ATH10K_P256_SYMBOL(int,ccec_full_add,(const void *,void *,const void *,const void *));
ATH10K_P256_SYMBOL(int,ccec_full_sub,(const void *,void *,const void *,const void *));
ATH10K_P256_SYMBOL(int,ccn_read_uint,(size_t,uint64_t *,size_t,const uint8_t *));
ATH10K_P256_SYMBOL(void *,ccrng,(int *));
// `cczp_n` / `cczp_prime` are macros over the cczp struct in corecrypto's own
// headers, not exported symbols (measured: absent from every KC symtab), so a
// binding to them fails the whole kext at link time with an unresolved symbol.
// Read the fields here instead. 13.x layout: {struct cczp_funcs *di; cc_size n;
// cc_unit *prime; cc_unit *mod_minus_2; ...}; cc_size is 32-bit and the pointer
// that follows it is 8-byte aligned, i.e. prime sits at +8.
// ccec_cp_256()'s object does not expose {di, n, prime} the way the corecrypto
// headers describe it (measured on 13.7: the first word is the limb count 4, the
// next words are 0x100 and a kernel pointer - neither is a usable prime array).
// Both values are constants for P-256 anyway, and P256FieldCandidate::run() only
// uses them to check "units==4 and prime==the P-256 modulus" before handing the
// real curve pointer to cczp_mul, so answer with the constants instead of
// decoding a layout that differs per build.
static inline size_t Ath10kCczpUnits(const void *)
{
    return 4;
}
static inline const uint64_t *Ath10kCczpPrime(const void *)
{
    static const uint64_t prime[4] = {0xffffffffffffffffull, 0x00000000ffffffffull,
                                      0ull, 0xffffffff00000001ull};
    return prime;
}
ATH10K_P256_SYMBOL(int,cczp_mul,(const void *,uint64_t *,const uint64_t *,const uint64_t *));
ATH10K_P256_SYMBOL(int,cczp_inv,(const void *,uint64_t *,const uint64_t *));
ATH10K_P256_SYMBOL(int,cczp_add,(const void *,uint64_t *,const uint64_t *,const uint64_t *));
ATH10K_P256_SYMBOL(int,cczp_sub,(const void *,uint64_t *,const uint64_t *,const uint64_t *));
#undef ATH10K_P256_SYMBOL
}
struct Ath10kP256KernelBinding {
    static AthSecurity::P256CoreCryptoAPI points(){
        using namespace Ath10kP256KernelSymbols;
        // ccn_read_uint's real prototype (corecrypto's ccn.h) takes
        // (size_t, cc_unit *, size_t, const uint8_t *), while the shared API
        // struct types the slot as const void *. Adapt instead of redeclaring the
        // symbol: redeclaring it is what produced "conflicting types" once the
        // corecrypto headers were in the same translation unit.
        static int (*const readScalar)(size_t, uint64_t *, size_t, const void *) =
            reinterpret_cast<int (*)(size_t, uint64_t *, size_t, const void *)>(ccn_read_uint);
        return {ccec_cp_256,ccec_x963_import_pub,ccec_validate_pub,ccec_x963_export,
            ccec_projectify,ccec_affinify,ccec_mult_blinded,ccec_full_add,ccec_full_sub,readScalar,ccrng};
    }
    static AthSecurity::P256FieldAPI field(){
        using namespace Ath10kP256KernelSymbols;
        return {ccec_cp_256,Ath10kCczpUnits,Ath10kCczpPrime,cczp_mul,cczp_inv,cczp_add,cczp_sub};
    }
};
