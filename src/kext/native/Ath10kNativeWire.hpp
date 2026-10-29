#pragma once
#include "../Ath10kBSS.hpp"
#include <WCL.hpp>
#include <string.h>
// Sequoia's BEACON_INFO is a length followed by the actual received 802.11
// beacon. Keep its TSF intact: rebuilding it from cached IEs would invent
// freshness and hide a dead association from WCL.
inline bool nativeBeaconInfo(const UInt8 *frame,size_t length,void *out,size_t capacity) {
    if(!frame || !out || capacity<0x808 || length<36 || length>0x800 ||
       (frame[0]&0xfc)!=0x80 || !AirportWCL::validIEs(frame+36,length-36))return false;
    auto bytes=static_cast<UInt8 *>(out);bzero(bytes,0x808);
    Ath10k::htcWriteLE32(bytes,static_cast<uint32_t>(length));
    bcopy(frame,bytes+4,length);return true;
}

// Fixed Sequoia 15.2+ ABI. Tahoe uses a different extended-BSS layout.
struct NativeExtendedBSSInput {
    const UInt8 *rates=nullptr,*htRates=nullptr,*rsn=nullptr;
    size_t rateCount=0,htRateCount=0,rsnLength=0;
    UInt16 vhtMap=0xffff;
};
inline bool nativeExtendedBSS(const NativeExtendedBSSInput &in,void *out,size_t capacity) {
    if(!out || capacity<0x1e0 || !in.rates || !in.rateCount || in.rateCount>32 ||
       in.htRateCount>16 || (in.htRateCount && !in.htRates) ||
       in.rsnLength>257 || (in.rsnLength && (!in.rsn ||
       in.rsn[0]!=48 || in.rsnLength<2 || in.rsnLength!=size_t(in.rsn[1])+2 ||
       !AirportWCL::validIEs(in.rsn,in.rsnLength))))return false;
    for(size_t i=0;i<in.htRateCount;++i)if(in.htRates[i]>=96)return false;
    auto bytes=static_cast<UInt8 *>(out);bzero(bytes,0x1e0);
    Ath10k::htcWriteLE32(bytes,1);
    // apple80211_rate_set_data has 15 entries (12 bytes each).
    const size_t count=in.rateCount<15?in.rateCount:15;
    bytes[4]=static_cast<UInt8>(count);
    for(size_t i=0;i<count;++i){
        Ath10k::htcWriteLE32(bytes+8+12*i,1);
        Ath10k::htcWriteLE32(bytes+12+12*i,in.rates[i]);
    }
    Ath10k::htcWriteLE32(bytes+0xbc,1);
    for(size_t i=0;i<in.htRateCount;++i)bytes[0xc0+in.htRates[i]/8]|=1u<<(in.htRates[i]%8);
    Ath10k::htcWriteLE32(bytes+0xcc,1);bytes[0xd0]=in.vhtMap&255;bytes[0xd1]=in.vhtMap>>8;
    // HE is unavailable on QCA9377. Never advertise a fabricated HE MCS map.
    Ath10k::htcWriteLE32(bytes+0xd4,1);bytes[0xd8]=bytes[0xd9]=0xff;
    if(in.rsnLength)bcopy(in.rsn,bytes+0xdd,in.rsnLength);
    return true;
}
// Shared by the real adapter and sanitizer-backed host tests.
inline bool nativeScanMatches(const AirportWCL::ScanRequest &scan,const Ath10k::WMIBSS &b) {
    unsigned channel=b.advertisedChannel?b.advertisedChannel:b.rxChannel;
    if(scan.ssidLength>32 || b.ssidLength>32)return false;
    if(!AirportWCL::scanChannelSelected(scan.channels,scan.channelCount!=0,channel))return false;
    if(scan.ssidLength && (scan.ssidLength!=b.ssidLength || memcmp(scan.ssid,b.ssid,b.ssidLength)))return false;
    UInt8 any=0,notBroadcast=0;
    for(auto byte:scan.bssid){any|=byte;notBroadcast|=UInt8(byte^0xff);}
    return !any || !notBroadcast || memcmp(scan.bssid,b.bssid,6)==0;
}
inline size_t nativeBSSMessage(const Ath10k::WMIBSS &b,UInt8 *buffer,size_t capacity) {
    if(!buffer || b.ssidLength>sizeof(b.ssid) || b.rateCount>sizeof(b.rates) ||
       b.rsnLength>sizeof(b.rsn) || b.countryLength>sizeof(b.country))return 0;
    if(b.rsnLength && !AirportWCL::validIEs(b.rsn,b.rsnLength))return 0;
    if(capacity<sizeof(AirportWCL::BeaconMetadata)+800)return 0;
    auto m=reinterpret_cast<AirportWCL::BeaconMetadata *>(buffer);bzero(m,sizeof(*m));
    unsigned ch=b.advertisedChannel?b.advertisedChannel:b.rxChannel;
    m->channel=ch;m->channelSpec=UInt16(ch|(ch<=14?0x1000:0xd000));
    m->ssidLength=b.ssidLength;bcopy(b.ssid,m->ssid,b.ssidLength);bcopy(b.bssid,m->bssid,6);
    m->rssi=int32_t(b.snr)-95;m->interval=b.beaconInterval;m->capability=b.capabilities;
    m->flags=AirportWCL::SSIDPresent|AirportWCL::RSSIValid;
    UInt8 *out=buffer+sizeof(*m);size_t bytes=0;
    auto ie=[&](UInt8 id,const UInt8 *data,size_t length){out[bytes++]=id;out[bytes++]=length;bcopy(data,out+bytes,length);bytes+=length;};
    ie(0,b.ssid,b.ssidLength);ie(1,b.rates,b.rateCount>8?8:b.rateCount);
    if(b.rateCount>8)ie(50,b.rates+8,b.rateCount-8);
    UInt8 channel=ch;ie(3,&channel,1);
    if(b.rsnLength){bcopy(b.rsn,out+bytes,b.rsnLength);bytes+=b.rsnLength;}
    if(b.htPresent)ie(45,b.ht,26);
    if(b.htOperation)ie(61,b.htOp,22);
    if(b.vhtPresent)ie(191,b.vht,12);
    if(b.vhtOperation)ie(192,b.vhtOp,5);
    if(b.countryPresent)ie(7,b.country,b.countryLength);
    // Keep the original RSN advertisement intact. Unsupported WPA3/PMF joins
    // are explicitly rejected rather than rewriting the AP's security policy.
    m->ieLength=bytes;return sizeof(*m)+bytes;
}
