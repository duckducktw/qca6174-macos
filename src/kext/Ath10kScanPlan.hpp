#pragma once
#include "Ath10kHTC.hpp"
#include "Ath10kRegulatory.hpp"
namespace Ath10k {
// Physical channel plan; regulatory policy determines the enabled subset.
struct WMIScanChannel { UInt32 channel, frequency; };
static constexpr WMIScanChannel kWMIScanPlan[]={
{1,2412},{2,2417},{3,2422},{4,2427},{5,2432},{6,2437},{7,2442},{8,2447},{9,2452},{10,2457},{11,2462},{12,2467},{13,2472},{36,5180},{40,5200},{44,5220},{48,5240},{52,5260},{56,5280},{60,5300},{64,5320},{100,5500},{104,5520},{108,5540},{112,5560},{116,5580},{120,5600},{124,5620},{128,5640},{132,5660},{136,5680},{140,5700},{144,5720},{149,5745},{153,5765},{157,5785},{161,5805},{165,5825}
};
static constexpr UInt32 kWMIScanChannelCount=sizeof(kWMIScanPlan)/sizeof(kWMIScanPlan[0]);
static_assert(kWMIScanChannelCount<64,"scan masks use 64 bits");
static constexpr UInt32 kWMIScanDwellMs=500, kWMIScanFirmwareMs=30000, kWMIScanHostMs=35000;
// Ventura's airportd abandons a scan request after ~20 s, so the interactive
// sweep uses one beacon interval per channel instead of the diagnostic 500 ms.
static constexpr UInt32 kWMIScanAirportdDwellMs=110;
// Conservative host descriptor ceiling, not a regulatory database or TX authorization.
static constexpr UInt8 kWMIScanPowerHalfDbm=20;
inline bool wmiScanChannelAllowed(UInt32 channel){
    for(const auto &c:kWMIScanPlan)if(c.channel==channel)return true;
    return false;
}
inline UInt32 wmiScanFrequency(UInt32 index){return index<kWMIScanChannelCount?kWMIScanPlan[index].frequency:0;}
inline UInt32 wmiScanChannel(UInt32 index){return index<kWMIScanChannelCount?kWMIScanPlan[index].channel:0;}
// Invalid frequency has a sentinel outside the array and bitmask range.
inline UInt32 wmiScanFrequencyIndex(UInt32 freq){
    for(UInt32 i=0;i<kWMIScanChannelCount;++i)if(kWMIScanPlan[i].frequency==freq)return i;
    return kWMIScanChannelCount;
}
inline bool wmiScanFrequencyAllowed(UInt32 freq){return wmiScanFrequencyIndex(freq)<kWMIScanChannelCount;}
// Conservative host ceiling (10 dBm), not a country regulatory database.
// Board/firmware calibration limits remain in force. DFS is receive-only.
inline bool channelNeedsDFS(UInt32 c){return c>=52 && c<=144 && wmiScanChannelAllowed(c);}
inline bool channelCanConnect(UInt32 c){return wmiScanChannelAllowed(c) && !channelNeedsDFS(c);}
inline UInt8 channelPowerHalfDbm(UInt32 c){return wmiScanChannelAllowed(c)?kWMIScanPowerHalfDbm:0;}
inline bool channelDefinitionAllowed(UInt32 primary,UInt32 center,UInt32 width,bool stationDFS=false){
    if(!wmiScanChannelAllowed(primary) || (!stationDFS && channelNeedsDFS(primary)))return false;
    if(width==20)return center==primary;
    if(width==40){
        if(primary<=13){
            if(center<3 || center>11)return false;
            return (primary==center-2 || primary==center+2) &&
                channelCanConnect(center-2) && channelCanConnect(center+2);
        }
        // Standard 40 MHz pairs; 40+44 and 161+165 are not valid pairs.
        const UInt32 pairs[]={36,44,52,60,100,108,116,124,132,140,149,157};
        for(UInt32 low:pairs)
            if(center==low+2 && (primary==low || primary==low+4))return stationDFS || !channelNeedsDFS(primary);
        return false;
    }
    if(width==80){
        if(center!=42 && center!=155 && !(stationDFS && (center==58 || center==106 || center==122 || center==138)))return false;
        return primary==center-6 || primary==center-2 || primary==center+2 || primary==center+6;
    }
    return false; // No 160/80+80, no DFS home channel.
}
inline UInt8 channelDefinitionPowerHalfDbm(UInt32 primary,UInt32 center,UInt32 width){
    if(!channelDefinitionAllowed(primary,center,width))return 0;
    UInt8 limit=channelPowerHalfDbm(primary);
    if(width>20){
        const int first=width==40?-2:-6;
        const int last=width==40?2:6;
        for(int offset=first;offset<=last;offset+=4){
            const UInt8 part=channelPowerHalfDbm(static_cast<UInt32>(static_cast<int>(center)+offset));
            if(part<limit)limit=part;
        }
    }
    return limit;
}
inline bool regulatoryScanChannel(const RegulatoryPolicy &p,UInt32 c){
    return wmiScanChannelAllowed(c) && regulatoryPower(p,c,c,20,false)!=0;
}
// Translate a caller's 256-channel bitmap into physical-plan slots. A null
// bitmap means an unrestricted scan; unknown channels never expand the plan.
inline UInt64 wmiRequestedScanMask(const UInt8 *channels,UInt32 bytes){
    if(!channels)return ~UInt64(0);
    if(bytes<32)return 0;
    UInt64 mask=0;
    for(UInt32 i=0;i<kWMIScanChannelCount;++i){
        const UInt32 channel=kWMIScanPlan[i].channel;
        if(channels[channel/8] & (1u<<(channel%8)))mask|=UInt64(1)<<i;
    }
    return mask;
}
inline UInt32 regulatoryScanCount(const RegulatoryPolicy *p,UInt64 requested=~UInt64(0)){
    UInt32 count=0;
    for(UInt32 i=0;i<kWMIScanChannelCount;++i)
        if((requested & (UInt64(1)<<i)) && (!p || regulatoryScanChannel(*p,kWMIScanPlan[i].channel)))++count;
    return count;
}
// WMI channel packed power bytes: min, max, regulatory, class;
// antenna gain, max TX, reserved, reserved. Powers use half-dBm.
inline void writeChannelPower(UInt8 *reg0,UInt8 *reg1,UInt8 halfDbm){
    htcWriteLE32(reg0,(UInt32(halfDbm)<<8)|(UInt32(halfDbm)<<16));
    htcWriteLE32(reg1,UInt32(halfDbm)<<8);
}
} // namespace Ath10k
