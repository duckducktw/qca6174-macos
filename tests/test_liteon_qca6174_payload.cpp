#include <cassert>
#include <cstring>
#include "Ath10kPayload.hpp"
int main(){
 const auto &l=Ath10kPayload::selectProfile(0x168c,0x003e,0x11ad,0x0807,10);
 const auto &d=Ath10kPayload::selectProfile(0x168c,0x003e,0x1028,0x0310,10);
 assert(l.available && d.available);
 assert(!std::strcmp(l.kBoardSHA256,"4c9b92a20a3f11f025a2ea60b605de1344d9b24e575ad5195ab10fdcdf44bf3c"));
 assert(std::strcmp(l.kBoardSHA256,d.kBoardSHA256));
 assert(l.kFirmware==d.kFirmware && l.kOTP==d.kOTP);
 assert(!Ath10kPayload::selectProfile(0x168c,0x003e,0x11ad,0x0807,9).available);
 assert(!Ath10kPayload::selectProfile(0x168c,0x003e,0x11ad,0x0808,10).available);
}
