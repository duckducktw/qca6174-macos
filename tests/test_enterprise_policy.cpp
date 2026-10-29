#include <cassert>
#include <vector>
#include "../src/shared/Ath10kSecurity.hpp"
using namespace AthSecurity;
static std::vector<uint8_t> r(uint8_t akm,uint16_t caps){std::vector<uint8_t> v={48,0,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,akm,uint8_t(caps),uint8_t(caps>>8)};v[1]=uint8_t(v.size()-2);return v;}
int main(){
 auto e=r(1,0);auto s=selectEnterprise(e.data(),e.size(),EnterpriseMode::WPA2);assert(s && s.akm==1 && !s.pmf);
 auto p=r(2,0);assert(selectEnterprise(p.data(),p.size(),EnterpriseMode::WPA2).error==SecurityError::UnsupportedAKM);
 auto w3=r(5,0xc0);assert(selectEnterprise(w3.data(),w3.size(),EnterpriseMode::WPA3).error==SecurityError::PMFUnavailable);
 SecurityBackend b{};b.pmf=true;b.bipCmac128=true;assert(selectEnterprise(w3.data(),w3.size(),EnterpriseMode::WPA3,b).pmf);
 auto tkip=e;tkip[7]=2;assert(selectEnterprise(tkip.data(),tkip.size(),EnterpriseMode::WPA2).error==SecurityError::UnsupportedCipher);
 for(size_t n=0;n<e.size();++n)assert(!selectEnterprise(e.data(),n,EnterpriseMode::WPA2));
}
