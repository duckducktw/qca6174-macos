#include <cassert>
#include <cstring>
#include <memory>
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "../src/kext/Ath10kConnection.hpp"
#include "../src/kext/native/Ath10kNativeOps.hpp"
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNotReady=2,kIOReturnUnsupported=3,kIOReturnError=4;
class Ath10kPCIDevice {
public:
    struct {bool serviceReady=false;UInt32 htCapability=0,vhtCapability=0;} _wmi;
    struct {UInt32 requestedChainMask=1;} _wmiInit;
    IOReturn nativePHYCapabilities(AthNativeOp,void *);
};
#include "native_phy_capabilities.inc"
int main(){
    Ath10kPCIDevice d;UInt8 ht[40],vht[40];memset(ht,0xa5,sizeof(ht));memset(vht,0xa5,sizeof(vht));
    assert(d.nativePHYCapabilities(AthNativeOp::getHT_CAPABILITY,nullptr)==kIOReturnBadArgument);
    assert(d.nativePHYCapabilities(AthNativeOp::getHT_CAPABILITY,ht)==kIOReturnNotReady);
    d._wmi.serviceReady=true;
    assert(d.nativePHYCapabilities(AthNativeOp::getHT_CAPABILITY,ht)==kIOReturnUnsupported);
    d._wmi.htCapability=0x803;d._wmi.vhtCapability=0xffffffffu;
    if(!ATH10K_HT_VHT){
        assert(d.nativePHYCapabilities(AthNativeOp::getHT_CAPABILITY,ht)==kIOReturnUnsupported);
        assert(d.nativePHYCapabilities(AthNativeOp::getVHT_CAPABILITY,vht)==kIOReturnUnsupported);
        return 0;
    }
    assert(d.nativePHYCapabilities(AthNativeOp::getHT_CAPABILITY,ht)==kIOReturnSuccess);
    assert(Ath10k::connLE32(ht)==1 && ht[4]==45 && ht[5]==26);
    assert(Ath10k::htcReadLE16(ht+6)==(ATH10K_MAX_WIDTH>=40?0x6e:0x2c));
    assert(ht[8]==27 && ht[9]==0xff && ht[10]==0 && ht[21]==1);
    for(unsigned i=32;i<sizeof(ht);++i)assert(ht[i]==0xa5);
    assert(d.nativePHYCapabilities(AthNativeOp::getVHT_CAPABILITY,vht)==kIOReturnSuccess);
    assert(Ath10k::connLE32(vht)==1 && vht[4]==191 && vht[5]==12);
    // No STBC, beamforming, 160/80+80 or extra RF streams are inherited from FW.
    assert(Ath10k::connLE32(vht+6)==0x01800020);
    const UInt16 map=ATH10K_MAX_WIDTH==20?0xfffd:0xfffe;
    assert(Ath10k::htcReadLE16(vht+10)==map && Ath10k::htcReadLE16(vht+14)==map);
    assert(!vht[12] && !vht[13] && !vht[16] && !vht[17] && !vht[18] && !vht[19]);
    for(unsigned i=20;i<sizeof(vht);++i)assert(vht[i]==0xa5);
    // Query and actual station association must encode the same local IE policy.
    auto s=std::make_unique<Ath10k::ConnectionState>();s->stage=Ath10k::LinkStage::Assoc;
    s->htEnabled=s->vhtEnabled=true;s->ssidLength=1;s->ssid[0]='x';s->ap.rateCount=1;s->ap.rates[0]=12;
    s->localHT=Ath10k::htcReadLE16(ht+6);s->localVHT=Ath10k::connLE32(vht+6);
    s->vhtRxMap=s->vhtTxMap=map;s->negotiatedNSS=1;s->negotiatedWidth=ATH10K_MAX_WIDTH;
    UInt8 frame[1024];UInt32 length=Ath10k::buildStationManagement(frame,sizeof(frame),*s);
    assert(length>28);bool foundHT=false,foundVHT=false;
    for(unsigned at=28;at+2<=length;){unsigned size=unsigned(frame[at+1])+2;assert(at+size<=length);
        if(frame[at]==45){assert(size==28 && !memcmp(frame+at,ht+4,28));foundHT=true;}
        if(frame[at]==191){assert(size==14 && !memcmp(frame+at,vht+4,14));foundVHT=true;}
        at+=size;
    }assert(foundHT && foundVHT);
    // Two-chain QCA6174 must advertise two spatial streams, never fewer or more.
    d._wmiInit.requestedChainMask=3;
    assert(d.nativePHYCapabilities(AthNativeOp::getHT_CAPABILITY,ht)==kIOReturnSuccess);
    assert(ht[9]==0xff && ht[10]==0xff && ht[11]==0);
    assert(d.nativePHYCapabilities(AthNativeOp::getVHT_CAPABILITY,vht)==kIOReturnSuccess);
    {const UInt16 two=ATH10K_MAX_WIDTH==20?0xfff5:0xfffa;assert(Ath10k::htcReadLE16(vht+10)==two && Ath10k::htcReadLE16(vht+14)==two);}
    d._wmiInit.requestedChainMask=1;
    // Missing optional firmware capability bits are not fabricated as SGI support.
    d._wmi.htCapability=1;d._wmi.vhtCapability=0;
    assert(d.nativePHYCapabilities(AthNativeOp::getHT_CAPABILITY,ht)==kIOReturnSuccess);
    assert(Ath10k::htcReadLE16(ht+6)==(ATH10K_MAX_WIDTH>=40?14:12));
    assert(d.nativePHYCapabilities(AthNativeOp::getVHT_CAPABILITY,vht)==kIOReturnSuccess);
    assert(Ath10k::connLE32(vht+6)==0x01800000);
    memset(ht,0xa5,sizeof(ht));d._wmi.htCapability=0;
    assert(d.nativePHYCapabilities(AthNativeOp::getVHT_CAPABILITY,ht)==kIOReturnUnsupported);
    for(auto b:ht)assert(b==0xa5);
    d._wmi.htCapability=1;assert(d.nativePHYCapabilities(AthNativeOp::getRATE,ht)==kIOReturnUnsupported);
    UInt8 out[32];memset(out,0xa5,sizeof(out));
    assert(!Ath10k::writeLocalHTCapabilityIE(out,27,14,1));
    assert(!Ath10k::writeLocalHTCapabilityIE(out,28,14,0));
    assert(!Ath10k::writeLocalHTCapabilityIE(out,28,14,ATH10K_MAX_NSS+1));
    assert(!Ath10k::writeLocalVHTCapabilityIE(out,13,0x01800000,map,map));
    assert(!Ath10k::writeLocalVHTCapabilityIE(out,14,0x01800000,0xffff,map));
    assert(!Ath10k::writeLocalVHTCapabilityIE(out,14,0x01800000,map,0xffff));
    for(auto b:out)assert(b==0xa5);
}
