#pragma once
#include "Ath10kRegulatoryData.hpp"
namespace Ath10k {
inline const RegulatoryCountry *regulatoryCountry(const char *name){
    if(!name || !name[0] || !name[1] || name[2])return nullptr;
    for(const auto &c:kRegulatoryCountries)if(c.alpha2[0]==name[0] && c.alpha2[1]==name[1])return &c;
    return nullptr;
}
inline const RegulatoryPair *regulatoryPair(UInt32 rd){
    for(const auto &p:kRegulatoryPairs)if(p.rd==rd)return &p;
    return nullptr;
}
struct RegulatoryPolicy {
    const RegulatoryCountry *country=regulatoryCountry("00"),*boardCountry=nullptr;
    UInt32 eepromRD=0,domain=0x64,low2=2300,high2=3000,low5=4800,high5=5925;
    UInt8 ceilingHalfDbm=20;   // firmware radio TX power (buildWMIRadio validates <=20, i.e. 10 dBm)
    UInt8 powerCeilingHalfDbm=40;// assoc-time clamp for our own candidate (20 dBm); 2.4 GHz is legal to 30 dBm EIRP
    UInt16 ctl2=0xff,ctl5=0xff;
    bool indoor=false,firmwareCapsValid=false,stationDFS=false;
    const char *status="world-default";
};
inline const char *configureRegulatory(RegulatoryPolicy &p,UInt32 eeprom,
    UInt32 low2,UInt32 high2,UInt32 low5,UInt32 high5){
    if(!p.country || !p.ceilingHalfDbm || p.ceilingHalfDbm>60)return "regulatory-options-invalid";
    if(!low2 && !high2){low2=2300;high2=3000;}
    if(!low5 && !high5){low5=4800;high5=5925;}
    if(low2<2300 || high2>3000 || high2<low2 || low5<4800 || high5>6200 || high5<low5)
        return "regulatory-firmware-band-bounds";
    UInt32 rd=eeprom & ~0x4000u;const RegulatoryAthCountry *board=nullptr;
    if(rd==0x8000)rd=0x64; // Linux ath_regd_sanitize.
    if(rd & 0x8000){
        for(const auto &c:kRegulatoryAthCountries)if(c.code==(rd&0x7fff)){board=&c;break;}
        if(!board)return "regulatory-eeprom-country-unknown";
        rd=board->rd;
    }else if(!rd){
        for(const auto &c:kRegulatoryAthCountries)if(c.code==840){board=&c;rd=c.rd;break;}
    }
    const auto *pair=regulatoryPair(rd);
    if(!pair || !rd)return "regulatory-eeprom-domain-unknown";
    // Fixed EEPROM country is intersected with the host selection. World
    // roaming boards may choose a known ath country; never invent CTL values.
    const bool world=(rd&0xfff0u)==0x60u;
    if(world && !(p.country->alpha2[0]=='0' && p.country->alpha2[1]=='0')){
        const RegulatoryAthCountry *requested=nullptr;
        for(const auto &c:kRegulatoryAthCountries)
            if(c.alpha2[0]==p.country->alpha2[0] && c.alpha2[1]==p.country->alpha2[1]){requested=&c;break;}
        if(!requested)return "regulatory-country-firmware-map-missing";
        pair=regulatoryPair(requested->rd);if(!pair)return "regulatory-country-pair-missing";
        rd=pair->rd;
    }
    p.eepromRD=eeprom;p.domain=rd;p.ctl2=pair->ctl2g;p.ctl5=pair->ctl5g;
    p.boardCountry=board?regulatoryCountry(board->alpha2):nullptr;
    p.low2=low2;p.high2=high2;p.low5=low5;p.high5=high5;
    p.firmwareCapsValid=true;p.status="configured";return nullptr;
}
inline UInt32 regulatoryFrequency(UInt32 channel){return channel<=14?2407+5*channel:5000+5*channel;}
// Passive receive may observe indoor/DFS channels. Transmit requires an
// associated infrastructure AP, indoor permission and non-DFS channel here.
inline UInt8 countryPower(const RegulatoryCountry *c,UInt32 center,UInt32 width,
                         bool indoor,bool transmit,bool stationDFS=false,bool initiate=false){
    if(!c || (width!=20 && width!=40 && width!=80))return 0;
    const UInt32 f=regulatoryFrequency(center)*1000;
    const UInt32 half=width*500;UInt8 best=0;
    for(UInt32 i=0;i<c->count;++i){const auto &r=kRegulatoryRules[c->first+i];
        if(f<half || f-half<r.lowKHz || f+half>r.highKHz || width>r.maxWidth || (r.flags&48u))continue;
        if(transmit && (((r.flags&1u)&&!stationDFS) || (initiate&&(r.flags&3u)) || ((r.flags&4u)&&!indoor) || ((r.flags&8u)&&indoor)))continue;
        if(r.halfDbm>best)best=r.halfDbm;
    }
    return best;
}
inline UInt8 regulatoryPower(const RegulatoryPolicy &p,UInt32 primary,UInt32 center,UInt32 width,bool transmit=true){
    const UInt32 freq=regulatoryFrequency(primary);
    const UInt32 cf=regulatoryFrequency(center),half=width/2;
    const UInt32 lo=primary<=14?p.low2:p.low5,hi=primary<=14?p.high2:p.high5;
    if(cf<half || cf-half<lo-10 || cf+half>hi+10 || freq<lo || freq>hi)return 0;
    UInt8 limit=countryPower(p.country,center,width,p.indoor,transmit,p.stationDFS);
    if(p.boardCountry){UInt8 board=countryPower(p.boardCountry,center,width,p.indoor,transmit,p.stationDFS);if(board<limit)limit=board;}
    if(p.powerCeilingHalfDbm<limit)limit=p.powerCeilingHalfDbm;
    return limit;
}
}
