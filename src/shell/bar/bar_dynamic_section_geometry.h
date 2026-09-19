#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

namespace noctalia::bar::dynamic_sections {

  enum class LayoutRole { Free, Start, Center, End };
  enum class EdgePolicy { FollowCenter, Equidistant, Edge };

  struct Request {
    float anchor = 0.0F;
    float size = 0.0F;
    float alignment = 0.0F; // 0=start, 0.5=center, 1=end
    float offset = 0.0F;
    bool allowOverlap = false;
  };

  struct Extent {
    float start = 0.0F;
    float end = 0.0F;
  };

  // Opted-in named sections can use the same three-lane placement semantics
  // as the legacy bar. Free sections retain their explicit request verbatim.
  inline void applyLanePolicy(
      std::vector<Request>& requests, const std::vector<LayoutRole>& roles,
      float spanStart, float spanEnd, float gap, float centerAlignment, EdgePolicy edgePolicy
  ) {
    if (requests.size() != roles.size()) return;
    const float span = std::max(0.0F, spanEnd - spanStart);
    gap = std::max(0.0F, gap);
    const auto groupLength = [&](LayoutRole role) {
      float length = 0.0F;
      std::size_t count = 0;
      for (std::size_t i = 0; i < roles.size(); ++i) if (roles[i] == role) {
        length += std::max(0.0F, requests[i].size);
        ++count;
      }
      return std::min(span, length + (count > 1 ? gap * static_cast<float>(count - 1) : 0.0F));
    };
    const float startLength = groupLength(LayoutRole::Start);
    const float centerLength = groupLength(LayoutRole::Center);
    const float endLength = groupLength(LayoutRole::End);
    const float align = std::clamp(centerAlignment, 0.0F, 1.0F);
    float centerStart = spanStart + (span - centerLength) * align;
    float startStart = spanStart;
    float endStart = spanEnd - endLength;
    if (edgePolicy == EdgePolicy::Equidistant) {
      const float lane = span / 3.0F;
      startStart = spanStart + (lane - startLength) * 0.5F;
      endStart = spanStart + span - lane * 0.5F - endLength * 0.5F;
    } else if (edgePolicy == EdgePolicy::FollowCenter && centerLength > 0.0F) {
      startStart = centerStart - (startLength > 0.0F ? gap + startLength : 0.0F);
      endStart = centerStart + centerLength + (endLength > 0.0F ? gap : 0.0F);
      const float first = startLength > 0.0F ? startStart : centerStart;
      const float last = endLength > 0.0F ? endStart + endLength : centerStart + centerLength;
      const float shift = first < spanStart ? spanStart - first : last > spanEnd ? spanEnd - last : 0.0F;
      startStart += shift; centerStart += shift; endStart += shift;
    }
    const auto place = [&](LayoutRole role, float cursor) {
      for (std::size_t i = 0; i < roles.size(); ++i) if (roles[i] == role) {
        const float offset = requests[i].offset;
        requests[i].anchor = cursor + offset;
        requests[i].alignment = 0.0F;
        requests[i].offset = 0.0F;
        cursor += std::max(0.0F, requests[i].size) + gap;
      }
    };
    place(LayoutRole::Start, startStart);
    place(LayoutRole::Center, centerStart);
    place(LayoutRole::End, endStart);
  }

  // Resolve an arbitrary number of anchored sections. Non-overlapping
  // sections are packed in visual order and kept inside the available span;
  // explicit-overlap sections retain their requested position.
  inline std::vector<Extent> resolve(
      const std::vector<Request>& requests, float spanStart, float spanEnd, float gap
  ) {
    std::vector<Extent> result;
    result.reserve(requests.size());
    for (const auto& request : requests) {
      const float size = std::max(0.0F, request.size);
      float start = request.anchor - size * std::clamp(request.alignment, 0.0F, 1.0F) + request.offset;
      if (!request.allowOverlap && size <= spanEnd - spanStart) {
        start = std::clamp(start, spanStart, spanEnd - size);
      }
      result.push_back({start, start + size});
    }

    std::vector<std::size_t> packed;
    for (std::size_t i = 0; i < requests.size(); ++i) {
      if (!requests[i].allowOverlap) packed.push_back(i);
    }
    std::stable_sort(packed.begin(), packed.end(), [&](std::size_t a, std::size_t b) {
      return result[a].start < result[b].start;
    });
    const float resolvedGap = std::max(0.0F, gap);
    for (std::size_t i = 1; i < packed.size(); ++i) {
      auto& current = result[packed[i]];
      const auto& previous = result[packed[i - 1]];
      const float size = current.end - current.start;
      current.start = std::max(current.start, previous.end + resolvedGap);
      current.end = current.start + size;
    }
    if (!packed.empty() && result[packed.back()].end > spanEnd) {
      const float shift = result[packed.back()].end - spanEnd;
      for (const auto index : packed) {
        result[index].start -= shift;
        result[index].end -= shift;
      }
      if (result[packed.front()].start < spanStart) {
        const float restore = spanStart - result[packed.front()].start;
        for (const auto index : packed) {
          result[index].start += restore;
          result[index].end += restore;
        }
      }
    }
    return result;
  }

} // namespace noctalia::bar::dynamic_sections
