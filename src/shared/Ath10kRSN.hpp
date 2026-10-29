#pragma once
#include <stdint.h>
#include <stddef.h>

namespace AthSecurity {
// RSN element layout and suite selectors: IEEE 802.11 RSNE, OUI 00-0f-ac.
// Parsing an SAE advertisement does not imply that SAE/PMF is implemented.
struct RSNInfo {
    uint32_t pairwise = 0, akms = 0;
    uint8_t group = 0, groupManagement = 6; // Default BIP-CMAC-128.
    uint16_t capabilities = 0, pmkidCount = 0;
    bool mfpc() const { return (capabilities & 0x80) != 0; }
    bool mfpr() const { return (capabilities & 0x40) != 0; }
    bool pairwiseHas(unsigned suite) const { return suite < 32 && (pairwise & (uint32_t(1) << suite)); }
    bool akmHas(unsigned suite) const { return suite < 32 && (akms & (uint32_t(1) << suite)); }
};

inline uint16_t rsnLE16(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
inline uint8_t rsnSuiteType(const uint8_t *p) {
    return p[0] == 0 && p[1] == 15 && p[2] == 172 ? p[3] : uint8_t(255);
}
inline bool parseRSN(const uint8_t *ie, size_t n, RSNInfo &out) {
    out = {};
    if (!ie || n < 20 || n > 257 || ie[0] != 48 || size_t(ie[1]) + 2 != n || rsnLE16(ie + 2) != 1)
        return false;
    RSNInfo info{};
    info.group = rsnSuiteType(ie + 4);
    size_t pos = 8;
    for (unsigned section = 0; section < 2; ++section) {
        if (n - pos < 2) return false;
        const unsigned count = rsnLE16(ie + pos);
        pos += 2;
        if (!count || count > (n - pos) / 4) return false;
        for (unsigned i = 0; i < count; ++i, pos += 4) {
            const uint8_t type = rsnSuiteType(ie + pos);
            if (type < 32) (section ? info.akms : info.pairwise) |= uint32_t(1) << type;
        }
    }
    if (pos < n) {
        if (n - pos < 2) return false;
        info.capabilities = rsnLE16(ie + pos);
        pos += 2;
    }
    // An AP cannot require management-frame protection without supporting it.
    if (info.mfpr() && !info.mfpc()) return false;
    if (pos < n) {
        if (n - pos < 2) return false;
        info.pmkidCount = rsnLE16(ie + pos);
        pos += 2;
        if (info.pmkidCount > (n - pos) / 16) return false;
        pos += size_t(info.pmkidCount) * 16;
    }
    if (pos < n) {
        if (n - pos != 4) return false;
        info.groupManagement = rsnSuiteType(ie + pos);
        pos += 4;
    }
    if (pos != n) return false;
    out = info;
    return true;
}

enum class PersonalMode : uint8_t { WPA2, WPA3 };
enum class SecurityError : uint8_t {
    None, MalformedRSN, UnsupportedCipher, UnsupportedAKM,
    PMFRequired, SAEUnavailable, PMFUnavailable, UnsupportedGroupManagement,
    UnsupportedCapabilities
};
struct SecurityBackend {
    // Leave false until the corresponding runtime path is implemented and tested.
    bool sae = false, pmf = false, bipCmac128 = false;
};
struct PersonalSelection {
    SecurityError error = SecurityError::MalformedRSN;
    uint8_t akm = 0, pairwise = 0, group = 0;
    bool pmf = false;
    explicit operator bool() const { return error == SecurityError::None; }
};
inline PersonalSelection selectPersonal(const uint8_t *ie, size_t n, PersonalMode requested,
                                        SecurityBackend backend = {}) {
    RSNInfo ap{};
    PersonalSelection result{};
    if (!parseRSN(ie, n, ap)) return result;
    auto reject = [&result](SecurityError error) { result.error = error; return result; };
    // This transport currently programs CCMP-128 pairwise keys, not GCMP/256-bit keys.
    if (!ap.pairwiseHas(4) || (ap.group != 4 && (requested == PersonalMode::WPA3 || ap.group != 2)))
        return reject(SecurityError::UnsupportedCipher);
    if (ap.capabilities & ((1u << 9) | (1u << 13)))
        return reject(SecurityError::UnsupportedCapabilities);
    const uint8_t akm = requested == PersonalMode::WPA3 ? 8 : 2;
    // No automatic downgrade from an explicitly requested SAE connection to PSK.
    if (!ap.akmHas(akm)) return reject(SecurityError::UnsupportedAKM);
    const bool protectedManagement = requested == PersonalMode::WPA3 || ap.mfpr();
    if (protectedManagement && !ap.mfpc()) return reject(SecurityError::PMFRequired);
    if (requested == PersonalMode::WPA3 && !backend.sae) return reject(SecurityError::SAEUnavailable);
    if (protectedManagement && !backend.pmf) return reject(SecurityError::PMFUnavailable);
    if (protectedManagement && (ap.groupManagement != 6 || !backend.bipCmac128))
        return reject(SecurityError::UnsupportedGroupManagement);
    result.error = SecurityError::None;
    result.akm = akm; result.pairwise = 4; result.group = ap.group; result.pmf = protectedManagement;
    return result;
}
// Decoded BIP-CMAC-128 IGTK KDE. Caller owns key lifetime/zeroization.
// Parsing alone never installs a key or advances replay state.
struct IGTKCMAC128 {
    uint16_t keyID=0;uint64_t ipn=0;uint8_t key[16]{};
};
inline bool parseIGTKCMAC128(const uint8_t *element,size_t bytes,IGTKCMAC128 &out){
    // Vendor IE: RSN OUI/type 9, LE KeyID, six-byte LE IPN, 16-byte IGTK.
    if(!element || bytes!=30 || element[0]!=221 || element[1]!=28 ||
       element[2]!=0 || element[3]!=15 || element[4]!=172 || element[5]!=9)return false;
    const uint16_t id=rsnLE16(element+6);
    if(id!=4 && id!=5)return false;
    uint64_t ipn=0;for(unsigned i=0;i<6;++i)ipn|=uint64_t(element[8+i])<<(8*i);
    out.keyID=id;out.ipn=ipn;
    for(unsigned i=0;i<16;++i)out.key[i]=element[14+i];
    return true;
}

// Input must already have passed EAPOL MIC and key-unwrapping checks.
// Scan the entire IE sequence before publishing any key material.
inline bool findIGTKCMAC128(const uint8_t *data,size_t bytes,IGTKCMAC128 &out){
    if(!data || !bytes || bytes>512)return false;
    const uint8_t *found=nullptr;size_t foundBytes=0;
    for(size_t at=0;at<bytes;){
        // AES key-wrap padding uses 0xdd followed by zero octets.
        if(data[at]==221 && (at+1==bytes || data[at+1]==0)){
            for(size_t j=at+1;j<bytes;++j)if(data[j])return false;
            break;
        }
        if(bytes-at<2)return false;
        const size_t length=size_t(data[at+1])+2;
        if(length>bytes-at)return false;
        if(data[at]==221 && length>=6 && data[at+2]==0 &&
           data[at+3]==15 && data[at+4]==172 && data[at+5]==9){
            if(found)return false;
            found=data+at;foundBytes=length;
        }
        at+=length;
    }
    return found && parseIGTKCMAC128(found,foundBytes,out);
}


enum class EnterpriseMode : uint8_t { WPA2, WPA3 };
struct EnterpriseSelection {
    SecurityError error = SecurityError::MalformedRSN;
    uint8_t akm = 0, pairwise = 0, group = 0;
    bool pmf = false;
    explicit operator bool() const { return error == SecurityError::None; }
};
// Policy only: macOS eapolclient owns EAP. The driver must still deliver
// EAPOL and install PTK/GTK through setCIPHER_KEY before this is admitted.
inline EnterpriseSelection selectEnterprise(const uint8_t *ie, size_t n, EnterpriseMode requested,
                                             SecurityBackend backend = {}) {
    RSNInfo ap{};
    EnterpriseSelection result{};
    if (!parseRSN(ie, n, ap)) return result;
    auto reject = [&result](SecurityError error) { result.error = error; return result; };
    if (!ap.pairwiseHas(4) || ap.group != 4) return reject(SecurityError::UnsupportedCipher);
    if (ap.capabilities & ((1u << 9) | (1u << 13))) return reject(SecurityError::UnsupportedCapabilities);
    const uint8_t akm = requested == EnterpriseMode::WPA3 ? 5 : (ap.akmHas(5) ? 5 : 1);
    if (!ap.akmHas(akm)) return reject(SecurityError::UnsupportedAKM);
    const bool protectedManagement = requested == EnterpriseMode::WPA3 || ap.mfpr();
    if (protectedManagement && !ap.mfpc()) return reject(SecurityError::PMFRequired);
    if (protectedManagement && !backend.pmf) return reject(SecurityError::PMFUnavailable);
    if (protectedManagement && (ap.groupManagement != 6 || !backend.bipCmac128))
        return reject(SecurityError::UnsupportedGroupManagement);
    result.error = SecurityError::None;
    result.akm = akm; result.pairwise = 4; result.group = 4; result.pmf = protectedManagement;
    return result;
}
}
