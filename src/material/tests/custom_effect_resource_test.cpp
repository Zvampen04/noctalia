#include "custom_effect_resource.h"
#include <cstdlib>
#include <iostream>
#include <string>
using namespace noctalia::material;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " #x << '\n'; std::exit(1); } } while(false)
namespace {
std::string hex(const CustomEffectTransportDigest& transportDigest) {
  constexpr char alphabet[]="0123456789abcdef";std::string result;
  for(auto byte:transportDigest){result.push_back(alphabet[byte>>4]);result.push_back(alphabet[byte&15]);}return result;
}
}
int main() {
  const std::string source="vec4 noctalia_effect(){return vec4(1.0);}";
  const auto bytes=std::span(reinterpret_cast<const std::uint8_t*>(source.data()),source.size());
  const auto transportDigest=customEffectTransportDigest(1,bytes);
  CHECK(hex(transportDigest)=="e96c1ebe848992faa7184dc57e3d540d23d6e4ffe1bcfcc2c0be0016c87181a0");
  CHECK(customEffectTransportDigest(2,bytes)!=transportDigest);
  const std::string changed=source+" ";
  CHECK(customEffectTransportDigest(1,std::span(reinterpret_cast<const std::uint8_t*>(changed.data()),changed.size()))!=transportDigest);
  CHECK(validateCustomEffectSource(source).empty());
  CHECK(validateCustomEffectSource("// uniform texture\n"+source+"/* gl_ main */").empty());
  CHECK(!validateCustomEffectSource(source+"/* unterminated").empty());
  CHECK(!validateCustomEffectSource("void main(){}").empty());
  std::cout << "custom effect ABI-qualified transport digest and source coverage passed\n";
}
