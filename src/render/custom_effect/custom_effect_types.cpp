#include "render/custom_effect/custom_effect_types.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace {
bool tokenBoundary(char value) {
  return !std::isalnum(static_cast<unsigned char>(value)) && value != '_';
}

bool containsToken(std::string_view source, std::string_view token) {
  std::size_t offset = 0;
  while ((offset = source.find(token, offset)) != std::string_view::npos) {
    const bool left = offset == 0 || tokenBoundary(source[offset - 1]);
    const auto end = offset + token.size();
    const bool right = end == source.size() || tokenBoundary(source[end]);
    if (left && right) return true;
    offset = end;
  }
  return false;
}

std::size_t tokenCount(std::string_view source, std::string_view token) {
  std::size_t result = 0;
  std::size_t offset = 0;
  while ((offset = source.find(token, offset)) != std::string_view::npos) {
    const bool left = offset == 0 || tokenBoundary(source[offset - 1]);
    const auto end = offset + token.size();
    const bool right = end == source.size() || tokenBoundary(source[end]);
    if (left && right) ++result;
    offset = end;
  }
  return result;
}

bool identifier(std::string_view value) {
  return !value.empty() && value.size() <= 96
      && std::ranges::all_of(value, [](unsigned char c) {
           return std::isalnum(c) || c == '-' || c == '_' || c == '.';
         });
}

bool digest(std::string_view value) {
  return value.size() == 64 && std::ranges::all_of(value, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
}

bool stripCustomEffectComments(std::string_view source,std::string& stripped,std::string* error) {
  stripped.clear();stripped.reserve(source.size());
  for(std::size_t index=0;index<source.size();) {
    if(index+1<source.size()&&source[index]=='/'&&source[index+1]=='/') {
      stripped.append("  ");index+=2;
      while(index<source.size()&&source[index]!='\n'){stripped.push_back(' ');++index;}
      continue;
    }
    if(index+1<source.size()&&source[index]=='/'&&source[index+1]=='*') {
      stripped.append("  ");index+=2;bool closed=false;
      while(index<source.size()) {
        if(index+1<source.size()&&source[index]=='*'&&source[index+1]=='/') {
          stripped.append("  ");index+=2;closed=true;break;
        }
        stripped.push_back(source[index]=='\n'?'\n':' ');++index;
      }
      if(!closed) {
        if(error)*error="custom effect source contains an unterminated block comment";
        stripped.clear();return false;
      }
      continue;
    }
    stripped.push_back(source[index++]);
  }
  if(error)error->clear();return true;
}

bool validCustomEffectAsset(const CustomEffectAsset& asset, std::string* error) {
  const auto reject = [error](std::string_view message) {
    if (error != nullptr) *error = message;
    return false;
  };
  if (!identifier(asset.stableId)) return reject("invalid stable shader identity");
  if (!digest(asset.sha256Digest)) return reject("digest must be 64 lowercase hexadecimal characters");
  if (asset.abi != kCustomEffectAbiVersion) return reject("unsupported custom effect ABI");
  if (asset.source.empty() || asset.source.size() > kCustomEffectMaxSourceBytes)
    return reject("custom effect source is empty or exceeds 32 KiB");
  if (std::ranges::any_of(asset.source, [](unsigned char value) {
        return value == 0 || (value < 0x20 && value != '\n' && value != '\r' && value != '\t')
            || value >= 0x80;
      })) return reject("custom effect source must use printable ASCII and contain no NUL bytes");
  if (!std::isfinite(asset.maxSampleRadiusPx) || asset.maxSampleRadiusPx < 0.0F
      || asset.maxSampleRadiusPx > kCustomEffectMaxSampleRadiusPx)
    return reject("custom effect sample radius is outside 0..256 pixels");
  std::string code;
  if(!stripCustomEffectComments(asset.source,code,error))return false;
  constexpr std::array forbidden{
      std::string_view{"main"}, std::string_view{"uniform"}, std::string_view{"attribute"},
      std::string_view{"varying"}, std::string_view{"sampler"}, std::string_view{"texture2D"},
      std::string_view{"textureCube"}, std::string_view{"for"}, std::string_view{"while"},
      std::string_view{"do"}, std::string_view{"discard"},
  };
  if (code.contains('#')) return reject("preprocessor directives and includes are not supported");
  if (code.contains("gl_") || code.contains("u_") || code.contains("v_local_px"))
    return reject("host-owned shader identifiers are not accessible");
  for (const auto token : forbidden)
    if (containsToken(code, token)) return reject("custom effect source uses an unsupported shader token");
  constexpr std::string_view signature = "vec4 noctalia_effect";
  const auto signatureOffset = code.find(signature);
  if (signatureOffset == std::string::npos || code.find(signature, signatureOffset + 1) != std::string::npos)
    return reject("source must define exactly one vec4 noctalia_effect function");
  if (tokenCount(code, "noctalia_effect") != 1)
    return reject("recursive custom effects are not supported");
  int braces = 0;
  for (const char value : code) {
    if (value == '{') ++braces;
    if (value == '}' && --braces < 0) return reject("custom effect braces are unbalanced");
  }
  if (braces != 0) return reject("custom effect braces are unbalanced");
  if (error != nullptr) error->clear();
  return true;
}
