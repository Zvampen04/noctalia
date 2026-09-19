#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::material {
inline constexpr std::uint32_t kCustomEffectAbi = 1;
inline constexpr std::size_t kCustomEffectTransportDigestBytes = 32;
inline constexpr std::size_t kMaxCustomEffectSourceBytes = 32 * 1024;
inline constexpr std::uint32_t kMaxCustomEffectSampleRadiusFixed = 256 * 256;
using CustomEffectTransportDigest = std::array<std::uint8_t, kCustomEffectTransportDigestBytes>;

namespace effect_digest_detail {
inline constexpr std::array<std::uint32_t,64> kRound{
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
inline std::uint32_t rotate(std::uint32_t value, unsigned bits) { return std::rotr(value,bits); }
inline CustomEffectTransportDigest sha256(std::span<const std::uint8_t> input) {
  std::vector<std::uint8_t> bytes(input.begin(),input.end());
  const std::uint64_t bitCount=static_cast<std::uint64_t>(bytes.size())*8;
  bytes.push_back(0x80);while(bytes.size()%64!=56) bytes.push_back(0);
  for(int shift=56;shift>=0;shift-=8) bytes.push_back(static_cast<std::uint8_t>(bitCount>>shift));
  std::array<std::uint32_t,8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  for(std::size_t offset=0;offset<bytes.size();offset+=64) {
    std::array<std::uint32_t,64> w{};
    for(unsigned i=0;i<16;++i) for(unsigned j=0;j<4;++j)
      w[i]|=static_cast<std::uint32_t>(bytes[offset+i*4+j])<<(24-j*8);
    for(unsigned i=16;i<64;++i) {
      const auto s0=rotate(w[i-15],7)^rotate(w[i-15],18)^(w[i-15]>>3);
      const auto s1=rotate(w[i-2],17)^rotate(w[i-2],19)^(w[i-2]>>10);
      w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    auto [a,b,c,d,e,f,g,hh]=h;
    for(unsigned i=0;i<64;++i) {
      const auto s1=rotate(e,6)^rotate(e,11)^rotate(e,25);
      const auto choice=(e&f)^((~e)&g);
      const auto t1=hh+s1+choice+kRound[i]+w[i];
      const auto s0=rotate(a,2)^rotate(a,13)^rotate(a,22);
      const auto majority=(a&b)^(a&c)^(b&c);
      const auto t2=s0+majority;
      hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
  }
  CustomEffectTransportDigest result{};
  for(unsigned i=0;i<h.size();++i) for(unsigned j=0;j<4;++j)
    result[i*4+j]=static_cast<std::uint8_t>(h[i]>>(24-j*8));
  return result;
}
} // namespace effect_digest_detail

// This transport digest is distinct from the shell asset's source-only SHA-256.
// It keys compiled source by ABI and exact bytes; radius and parameter changes
// remain scene data and never force recompilation.
inline std::string_view validateCustomEffectSource(std::string_view source) {
  if(source.empty() || source.size()>kMaxCustomEffectSourceBytes) return "source size is outside the bounded range";
  if(source.find('\0')!=std::string_view::npos) return "source contains a NUL byte";
  std::string code;code.reserve(source.size());
  for(std::size_t index=0;index<source.size();) {
    if(index+1<source.size()&&source[index]=='/'&&source[index+1]=='/') {
      code.append("  ");index+=2;while(index<source.size()&&source[index]!='\n'){code.push_back(' ');++index;}continue;
    }
    if(index+1<source.size()&&source[index]=='/'&&source[index+1]=='*') {
      code.append("  ");index+=2;bool closed=false;
      while(index<source.size()) {
        if(index+1<source.size()&&source[index]=='*'&&source[index+1]=='/') {
          code.append("  ");index+=2;closed=true;break;
        }
        code.push_back(source[index]=='\n'?'\n':' ');++index;
      }
      if(!closed)return "source contains an unterminated block comment";
      continue;
    }
    code.push_back(source[index++]);
  }
  if(code.find("vec4 noctalia_effect(")==std::string_view::npos) return "required noctalia_effect function is missing";
  constexpr std::array<std::string_view,8> forbidden{"#","void main","sampler","uniform","varying","attribute","texture","gl_"};
  for(const auto token:forbidden) if(code.find(token)!=std::string_view::npos) return "source declares a host-owned shader feature";
  return {};
}

inline CustomEffectTransportDigest customEffectTransportDigest(std::uint32_t abi,std::span<const std::uint8_t> source) {
  constexpr std::string_view domain="noctalia-custom-effect-v1";
  std::vector<std::uint8_t> canonical(domain.begin(),domain.end());
  const auto integer=[&](std::uint32_t value) {
    for(unsigned i=0;i<4;++i) canonical.push_back(static_cast<std::uint8_t>(value>>(i*8)));
  };
  integer(abi);canonical.insert(canonical.end(),source.begin(),source.end());
  return effect_digest_detail::sha256(canonical);
}
} // namespace noctalia::material
