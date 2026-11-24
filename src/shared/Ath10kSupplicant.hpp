#pragma once
#include "Ath10kSecurity.hpp"
#include "Ath10kSHA256.hpp"
namespace AthSecurity {
// Crypto supplies HMAC-SHA1 and AES-128-ECB decrypt; SHA256 mode additionally
// requires HMAC-SHA256 and AES-128-ECB encrypt.
// RSN/PSK descriptor 2; opt-in PSK-SHA256 descriptor 3; separately opt-in SAE
// descriptor 0 from a verified SAE-derived PMK. Native SAE remains disabled.
// SHA256 mode requires CCMP group/pairwise and an AP not requiring PMF.
// No EAP/TKIP pairwise. SAE stages IGTK delivery; firmware PMF protection/key
// acknowledgement and replay counters remain caller responsibilities.
// Retransmitted M3 returns cached M4; never asks the driver to reinstall a key.
template<class Crypto,bool AllowSHA256=false,bool AllowSAE=false> class Supplicant {
 uint8_t pmk[32]{},ptk[48]{},snonce[32]{},anonce[32]{},local[6]{},ap[6]{};
 uint8_t apRSN[257]{},savedM3[512]{},m2[160]{},m4[128]{};
 size_t rsnLength=0,savedLength=0,m2Length=0,m4Length=0;
 uint64_t replay1=0,replay3=0;
 unsigned state=0;uint8_t selectedAKM=2;bool sha256=false,sae=false,havePMKID=false;
 uint8_t saePMKID[16]{};
 uint8_t apRSNXE[18]{},apRSNXELength=0;bool rsnxeBound=false,saeH2E=false;
 IGTKCMAC128 rememberedIGTK[2]{};bool rememberedIGTKValid[2]{};
 struct ManagementCandidate {
  IGTKCMAC128 key{};bool present=false,changed=false;
  ~ManagementCandidate(){wipe(&key,sizeof(key));}
 };
 uint8_t descriptor() const {return sae?0:(sha256?3:2);}
 bool readManagement(const uint8_t *element,size_t bytes,ManagementCandidate &candidate){
  if(candidate.present)return false;
  candidate.present=true;
  if(!parseIGTKCMAC128(element,bytes,candidate.key))return false;
  candidate.changed=true;
  for(unsigned i=0;i<2;++i)if(rememberedIGTKValid[i] && equal(rememberedIGTK[i].key,candidate.key.key,16)){
   if(i!=candidate.key.keyID-4u)return false;candidate.changed=false;
  }
  return true;
 }
 void commitManagement(const ManagementCandidate &candidate){
  if(!candidate.present)return;
  auto &slot=rememberedIGTK[candidate.key.keyID-4];
  if(candidate.changed){slot=candidate.key;rememberedIGTKValid[candidate.key.keyID-4]=true;managementKey=candidate.key;}
  else if(candidate.key.ipn>slot.ipn)slot.ipn=candidate.key.ipn;
 }
 uint8_t establishedPTK[48]{},establishedANonce[32]{},establishedSNonce[32]{},currentGTK[32]{};
 uint8_t savedGroup[512]{},groupAck[128]{};size_t savedGroupLength=0,groupAckLength=0;
 uint64_t groupReplay=0;uint8_t currentGTKIndex=0,groupCipher=0;bool pairRekey=false;
 bool derive(){
  static constexpr char label[]="Pairwise key expansion";
  uint8_t input[sizeof(label)+76+1]{},digest[20]{};
  copy(input,(const uint8_t *)label,sizeof(label));size_t at=sizeof(label);
  auto less=[](const uint8_t *a,const uint8_t *b,size_t n){for(size_t i=0;i<n;++i){if(a[i]!=b[i])return a[i]<b[i];}return false;};
  bool swap=less(local,ap,6);copy(input+at,swap?local:ap,6);copy(input+at+6,swap?ap:local,6);at+=12;
  swap=less(snonce,anonce,32);copy(input+at,swap?snonce:anonce,32);copy(input+at+32,swap?anonce:snonce,32);
  if constexpr(AllowSHA256 || AllowSAE){if(sha256){
   bool ok=SHA256Personal<Crypto>::derive(pmk,input+sizeof(label),ptk);
   wipe(input,sizeof(input));wipe(digest,sizeof(digest));return ok;
  }}
  for(unsigned block=0;block<3;++block){input[sizeof(input)-1]=uint8_t(block);
   if(!Crypto::hmac(pmk,32,input,sizeof(input),digest)){wipe(input,sizeof(input));wipe(digest,sizeof(digest));return false;}
   copy(ptk+20*block,digest,block==2?8:20);
  }
  wipe(input,sizeof(input));wipe(digest,sizeof(digest));return true;
 }
 bool mic(uint8_t *p,size_t n){
  if constexpr(AllowSHA256 || AllowSAE){if(sha256){uint8_t digest[16]{};bool ok=SHA256Personal<Crypto>::mic(ptk,p,n,digest);if(ok)copy(p+81,digest,16);wipe(digest,16);return ok;}}
  uint8_t digest[20]{};bool ok=Crypto::hmac(ptk,16,p,n,digest);if(ok)copy(p+81,digest,16);wipe(digest,20);return ok;}
 size_t response(uint8_t *frame,const uint8_t *request,bool second,bool group=false){
  uint8_t station[47]{};size_t rsnBytes=0;
  if(second){
   if(!copyAssociationRSN(station,sizeof(station),rsnBytes))return 0;
   if(rsnxeBound){size_t extra=0;if(!copyAssociationRSNXE(station+rsnBytes,sizeof(station)-rsnBytes,extra))return 0;rsnBytes+=extra;}
  }
  size_t n=99+(second?rsnBytes:0);wipe(frame,n+14);copy(frame,ap,6);copy(frame+6,local,6);frame[12]=0x88;frame[13]=0x8e;
  uint8_t *p=frame+14;p[0]=request[0];p[1]=3;put16(p+2,uint16_t(n-4));p[4]=2;
  put16(p+5,uint16_t((second?0x0108:(group?0x0300:0x0308))|descriptor()));copy(p+9,request+9,8);
  if(second){copy(p+17,snonce,32);put16(p+97,uint16_t(rsnBytes));copy(p+99,station,rsnBytes);}
  return mic(p,n)?n+14:0;
 }
 bool unwrap(const uint8_t *in,size_t n,uint8_t *out){
  if(n<24||n>392||n%8)return false;
  uint8_t a[8],b[16],dec[16];copy(a,in,8);copy(out,in+8,n-8);size_t blocks=n/8-1;
  bool ok=true;
  for(int j=5;j>=0 && ok;--j)for(size_t i=blocks;i>0;--i){
   copy(b,a,8);uint64_t t=blocks*unsigned(j)+i;for(unsigned k=0;k<8;++k)b[7-k]^=uint8_t(t>>(8*k));
   copy(b+8,out+8*(i-1),8);if(!Crypto::decrypt(ptk+16,b,dec)){ok=false;break;}
   copy(a,dec,8);copy(out+8*(i-1),dec+8,8);
  }
  for(uint8_t c:a)if(c!=0xa6)ok=false;
  wipe(a,8);wipe(b,16);wipe(dec,16);if(!ok)wipe(out,n-8);return ok;
 }
public:
 enum Result { Ignore, Reply, Keys, Fatal, GroupKeys, PairKeys, ManagementKeys };
 enum Change : uint8_t { PTKChanged=1,GTKChanged=2,IGTKChanged=4 };
 static constexpr size_t keyPayloadLength=56;
 // PTK[16], GTK[32], group cipher[1], GTK index[1], GTK RSC[6].
 uint8_t output[160]{},keys[keyPayloadLength]{};size_t outputLength=0;
 // Last successful key-result payload only. Reply/Ignore never request installs.
 IGTKCMAC128 managementKey{};uint8_t keyChanges=0;
 const char *error="none";
 bool pairwisePending() const {return pairRekey && state==2;}
 // Diagnostic accessor: lets a caller compare this PTK's prefix against an
 // independently computed PRF without exporting key material it does not
 // already hold.
 uint8_t ptkByte(unsigned i) const {return i<sizeof(ptk)?ptk[i]:0;}
 void cancelPairwiseAttempt(){
  if(!pairwisePending())return;
  copy(ptk,establishedPTK,48);copy(anonce,establishedANonce,32);copy(snonce,establishedSNonce,32);
  pairRekey=false;state=3;outputLength=0;
  wipe(output,sizeof(output));wipe(m2,sizeof(m2));m2Length=0;
 }
 ~Supplicant(){reset();}
 void reset(){wipe(this,sizeof(*this));error="none";}
 bool begin(const uint8_t *master,const uint8_t *nonce,const uint8_t *station,const uint8_t *bssid,const uint8_t *rsn,size_t n,uint8_t akm=2,
     const uint8_t *pmkid=nullptr,size_t idBytes=0){
  reset();if(!master || !nonce || !station || !bssid)return false;
  if(akm==8){if(!AllowSAE || !validSAEPersonalRSN(rsn,n) || (idBytes && (idBytes!=16 || !pmkid)))return false;}
  else if(idBytes || (akm==6 && !AllowSHA256) || !supportsPSKAKM(rsn,n,akm))return false;
  selectedAKM=akm;sae=akm==8;sha256=akm==6 || sae;
  if(sae && idBytes){copy(saePMKID,pmkid,16);havePMKID=true;}
  groupCipher=wpa2GroupCipher(rsn,n);
  copy(pmk,master,32);copy(snonce,nonce,32);copy(local,station,6);copy(ap,bssid,6);copy(apRSN,rsn,n);rsnLength=n;state=1;return true;
 }
 // Bind a native caller's AP snapshot once, before M1. Generic software
 // users that do not model RSNXE keep their existing explicit unbound behavior.
 bool bindRSNXE(const uint8_t *ie,size_t bytes,bool h2e){
  if(state!=1 || rsnxeBound || (h2e && !sae) || bytes>sizeof(apRSNXE) ||
     (bytes && !validRSNXE(ie,bytes)) || (h2e && !rsnxeHasH2E(ie,bytes)))return false;
  if(bytes)copy(apRSNXE,ie,bytes);apRSNXELength=uint8_t(bytes);saeH2E=h2e;rsnxeBound=true;return true;
 }
 bool copyAssociationRSNXE(uint8_t *out,size_t capacity,size_t &bytes) const {
  bytes=0;if(!state || !rsnxeBound || !out)return false;
  if(!saeH2E)return true;
  if(capacity<3)return false;out[0]=244;out[1]=1;out[2]=0x20;bytes=3;return true;
 }
 // Public RSNE/PMKID only; no PMK/PTK is exported. The same encoder serves
 // association and EAPOL M2 so profile changes cannot silently diverge them.
 bool copyAssociationRSN(uint8_t *out,size_t capacity,size_t &bytes) const {
  bytes=0;if(!state || !out)return false;
  if constexpr(AllowSAE){if(sae)return buildSAEStationRSN(saePMKID,havePMKID?16:0,out,capacity,bytes);}
  size_t n=0;const auto rsn=stationRSNForGroup(groupCipher,n,selectedAKM);
  if(!rsn || capacity<n)return false;copy(out,rsn,n);bytes=n;return true;
 }
 Result fail(const char *reason){error=reason;return Fatal;}
 Result receive(const uint8_t *frame,size_t length){
  outputLength=0;
  if(!frame||!state||length<113||length>512||!equal(frame,local,6)||!equal(frame+6,ap,6)||frame[12]!=0x88||frame[13]!=0x8e)return Ignore;
  const uint8_t *p=frame+14;size_t n=size_t(be16(p+2))+4;
  if((p[0]!=1 && p[0]!=2)||p[1]!=3||p[4]!=2||n<99||n>length-14||size_t(be16(p+97))+99!=n)return Ignore;
  uint16_t info=be16(p+5);if((info&7)!=descriptor())return Ignore;info=uint16_t((info&~7u)|2u);uint64_t replay=be64(p+9);
  if(info==0x008a){
   if(be16(p+7)!=16)return Ignore;
   uint8_t nz=0;for(unsigned i=0;i<32;++i)nz|=p[17+i];if(!nz)return Ignore;
   if(state>=3){
    if(replay<=replay3 || replay<=groupReplay || equal(p+17,anonce,32))return Ignore;
    // M1 is unauthenticated. Retain the established keys until a valid M3;
    // a timed-out candidate can be discarded without tearing down the link.
    if(!Crypto::random(snonce,32))return fail("Falha ao gerar nonce de renovação.");
    copy(anonce,p+17,32);if(!derive())return fail("Falha na derivação da nova PTK.");
    pairRekey=true;state=2;
   }
   if(state==2 && (replay<replay1 || !equal(p+17,anonce,32)))return Ignore;
   if(state==1){copy(anonce,p+17,32);if(!derive())return fail("Falha na derivação da PTK.");}
   replay1=replay;m2Length=response(m2,p,true);if(!m2Length)return fail("Falha no MIC da mensagem 2.");
   copy(output,m2,m2Length);outputLength=m2Length;state=2;return Reply;
  }
  if(state>=3 && info==0x1382){
   if(replay<groupReplay || replay<=replay3)return Ignore;
   if(replay==groupReplay){
    if(n!=savedGroupLength || !equal(p,savedGroup,n))return Ignore;
    copy(output,groupAck,groupAckLength);outputLength=groupAckLength;return Reply;
   }
   uint8_t check[512]{},plain[384]{},candidate[keyPayloadLength]{};ManagementCandidate management;
   copy(check,p,n);wipe(check+81,16);
   bool verified=mic(check,n) && equal(check+81,p+81,16);wipe(check,sizeof(check));
   if(!verified)return Ignore;
   size_t encrypted=be16(p+97);
   if(!unwrap(p+99,encrypted,plain)){wipe(plain,sizeof(plain));return fail("GTK de renovação inválida.");}
   size_t bytes=encrypted-8,at=0;bool haveGTK=false,haveRSNXE=false,valid=true;
   copy(candidate+50,p+65,6);candidate[48]=groupCipher;
   while(at<bytes){
    if(plain[at]==221 && (at+1==bytes || plain[at+1]==0)){
     for(size_t j=at+1;j<bytes;++j)if(plain[j])valid=false;
     break;
    }
    if(bytes-at<2 || size_t(plain[at+1])+2>bytes-at){valid=false;break;}
    size_t len=size_t(plain[at+1])+2;
    if(plain[at]==48 && (len!=rsnLength || !equal(plain+at,apRSN,len)))valid=false;
    if(plain[at]==244){
     if(rsnxeBound && (haveRSNXE || len!=apRSNXELength || !equal(plain+at,apRSNXE,len)))valid=false;
     haveRSNXE=true;
    }
    if(plain[at]==221 && len>=6 && suite(plain+at+2,1)){
     const size_t gtkBytes=groupCipher==cipherTKIP?32:16;
     if(haveGTK || len!=8+gtkBytes || (plain[at+6]&0xf8) || plain[at+7])valid=false;
     else {candidate[49]=plain[at+6]&3;copy(candidate+16,plain+at+8,gtkBytes);}haveGTK=true;
    }
    if constexpr(AllowSAE){if(sae && plain[at]==221 && len>=6 && suite(plain+at+2,9))
     if(!readManagement(plain+at,len,management))valid=false;
    }
    at+=len;
   }
   if(!valid || (!haveGTK && !(sae && management.present))){wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));return fail("KDE de GTK/IGTK inválida.");}
   const size_t gtkBytes=groupCipher==cipherTKIP?32:16;
   const bool sameKey=!haveGTK || equal(currentGTK,candidate+16,gtkBytes);
   if(haveGTK && sameKey && currentGTKIndex!=candidate[49]){wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));return fail("GTK reutilizada em outro índice.");}
   groupAckLength=response(groupAck,p,false,true);
   if(!groupAckLength){wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));return fail("Falha no MIC de renovação GTK.");}
   copy(savedGroup,p,n);savedGroupLength=n;groupReplay=replay;
   copy(output,groupAck,groupAckLength);outputLength=groupAckLength;
   if(!sameKey){copy(keys,candidate,keyPayloadLength);copy(currentGTK,candidate+16,gtkBytes);currentGTKIndex=candidate[49];}
   const uint8_t changes=uint8_t((sameKey?0:GTKChanged)|(management.changed?IGTKChanged:0));
   commitManagement(management);if(changes)keyChanges=changes;
   wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));
   return !sameKey?GroupKeys:(management.changed?ManagementKeys:Reply);
  }
  if(info!=0x13ca){
   // Unsupported descriptors cannot trigger key replacement.
   return Ignore;
  }
  if(state<2 || be16(p+7)!=16 || !equal(p+17,anonce,32))return Ignore;
  if(state>=3){
   if(replay==replay3 && n==savedLength && equal(p,savedM3,n)){
    copy(output,m4,m4Length);outputLength=m4Length;return Reply;
   }
   // Authenticators may increment replay on a retry. Verify its MIC and
   // identical encrypted key data/RSC; acknowledge without reinstalling keys.
   if(replay>replay3 && n==savedLength && equal(p+65,savedM3+65,8) && equal(p+97,savedM3+97,n-97)){
    uint8_t check[512]{};copy(check,p,n);wipe(check+81,16);
    bool verified=mic(check,n) && equal(check+81,p+81,16);wipe(check,sizeof(check));
    if(verified){m4Length=response(m4,p,false);if(!m4Length)return fail("Falha no MIC da resposta repetida.");
     replay3=replay;copy(savedM3,p,n);copy(output,m4,m4Length);outputLength=m4Length;return Reply;}
   }
   return Ignore;
  }
  if(replay<=replay1)return Ignore;
  uint8_t check[512]{},plain[384]{};copy(check,p,n);wipe(check+81,16);
  if(!mic(check,n)){wipe(check,sizeof(check));return fail("Falha criptográfica.");}
  bool verified=equal(check+81,p+81,16);wipe(check,sizeof(check));
  if(!verified)return pairRekey?Ignore:fail("MIC inválido: confira a senha da rede.");
  size_t encrypted=be16(p+97);
  if(!unwrap(p+99,encrypted,plain)){wipe(plain,sizeof(plain));return fail("Dados de chave inválidos.");}
  size_t bytes=encrypted-8,at=0;bool haveRSN=false,haveGTK=false,haveRSNXE=false,valid=true;
  uint8_t candidate[keyPayloadLength]{};ManagementCandidate management;
  copy(candidate,ptk+32,16);copy(candidate+50,p+65,6);candidate[48]=groupCipher;
  while(at<bytes){
   if(plain[at]==221 && (at+1==bytes || plain[at+1]==0)){
    for(size_t j=at+1;j<bytes;++j)if(plain[j])valid=false;
    break;
   }
   if(bytes-at<2||size_t(plain[at+1])+2>bytes-at){valid=false;break;}
   size_t len=plain[at+1]+2;
   if(plain[at]==48){if(haveRSN||len!=rsnLength||!equal(plain+at,apRSN,len))valid=false;haveRSN=true;}
   if(plain[at]==244){
    if(rsnxeBound && (haveRSNXE || len!=apRSNXELength || !equal(plain+at,apRSNXE,len)))valid=false;
    haveRSNXE=true;
   }
   if(plain[at]==221 && len>=6 && suite(plain+at+2,1)){
    const size_t gtkBytes=groupCipher==cipherTKIP?32:16;
    if(haveGTK||len!=8+gtkBytes||(plain[at+6]&0xf8)||plain[at+7])valid=false;
    else {candidate[49]=plain[at+6]&3;copy(candidate+16,plain+at+8,gtkBytes);}haveGTK=true;
   }
   if constexpr(AllowSAE){if(sae && plain[at]==221 && len>=6 && suite(plain+at+2,9))
    if(!readManagement(plain+at,len,management))valid=false;
   }
   at+=len;
  }
  if(rsnxeBound && bool(apRSNXELength)!=haveRSNXE)valid=false;
  if(sae && pairRekey && !haveGTK){copy(candidate+16,currentGTK,16);candidate[49]=currentGTKIndex;}
  if(!valid || !haveRSN || (!haveGTK && !(sae && pairRekey)) || (sae && !pairRekey && !management.present)){
   wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));return fail("RSN ou GTK/IGTK inválida na mensagem 3.");
  }
  const bool gtkChanged=!pairRekey || !equal(currentGTK,candidate+16,groupCipher==cipherTKIP?32:16);
  if(sae && !gtkChanged && candidate[49]!=currentGTKIndex){wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));return fail("GTK reutilizada em outro índice.");}
  m4Length=response(m4,p,false);
  if(!m4Length){wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));return fail("Falha no MIC da mensagem 4.");}
  // Publish a complete key transaction only after its acknowledgement is ready.
  copy(keys,candidate,sizeof(keys));wipe(candidate,sizeof(candidate));wipe(plain,sizeof(plain));
  keyChanges=uint8_t(PTKChanged|(gtkChanged?GTKChanged:0)|(management.changed?IGTKChanged:0));commitManagement(management);
  copy(savedM3,p,n);savedLength=n;replay3=replay;state=3;
  const bool wasRekey=pairRekey;
  copy(establishedPTK,ptk,48);copy(establishedANonce,anonce,32);copy(establishedSNonce,snonce,32);
  copy(currentGTK,keys+16,groupCipher==cipherTKIP?32:16);currentGTKIndex=keys[49];pairRekey=false;
  copy(output,m4,m4Length);outputLength=m4Length;return wasRekey?PairKeys:Keys;
 }
 // PMK is retained only for the live session to permit pairwise rekey.
 // reset()/destruction wipe every key, nonce and cached handshake frame.
};
}
