#include <cassert>
#include <cstring>
#include "Ath10kPayload.hpp"

int main() {
    const auto &liteon=Ath10kPayload::selectProfile(0x168c,0x003e,0x11ad,0x0807,10);
    const auto &dell=Ath10kPayload::selectProfile(0x168c,0x003e,0x1028,0x0310,10);
    assert(liteon.available && liteon.kBoardSize==8124);
    assert(!std::strcmp(liteon.kBoardSelection,"board-api2-exact"));
    assert(!std::strcmp(liteon.kBoardSHA256,"4c9b92a20a3f11f025a2ea60b605de1344d9b24e575ad5195ab10fdcdf44bf3c"));
    assert(std::memcmp(liteon.kBoard,dell.kBoard,8124));
    assert(liteon.kFirmware==dell.kFirmware && liteon.kOTP==dell.kOTP);
    assert(liteon.matchesTarget(0x05030000,8,12));
    assert(!liteon.matchesTarget(0x05020001,8,12));
    assert(!Ath10kPayload::selectProfile(0x168c,0x003e,0x11ad,0x0808,10).available);
    assert(!Ath10kPayload::selectProfile(0x168c,0x003e,0x11ad,0x0807,9).available);
    return 0;
}
