#include "render/custom_effect/custom_effect_program.h"

#include "material/shape_source.h"
#include "render/custom_effect/custom_effect_capture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <string>
#include <utility>

namespace {
constexpr char kVertexShader[] = R"glsl(
precision highp float;
attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform mat3 u_transform;
varying vec2 v_local_px;
vec2 to_ndc(vec2 pixel_pos) {
  vec2 normalized = pixel_pos / u_surface_size;
  return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}
void main() {
  v_local_px = a_position * u_quad_size;
  vec3 pixel = u_transform * vec3(v_local_px, 1.0);
  gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)glsl";

std::string fragmentShader(std::string_view userSource) {
  return std::string{R"glsl(
#extension GL_OES_standard_derivatives : require
precision highp float;
uniform vec2 u_rect_size;
uniform vec4 u_fill;
uniform vec4 u_gradient_stops;
uniform vec4 u_gradient_color0;
uniform vec4 u_gradient_color1;
uniform vec4 u_gradient_color2;
uniform vec4 u_gradient_color3;
uniform vec2 u_gradient_direction;
uniform int u_fill_mode;
uniform vec4 u_radii;
uniform vec4 u_corners;
uniform vec4 u_insets;
uniform float u_corner_power;
uniform vec4 u_paint_clip;
uniform float u_paint_clip_radius;
uniform float u_paint_clip_power;
uniform sampler2D u_backdrop;
uniform vec4 u_copy_bounds;
uniform vec2 u_backdrop_size;
uniform vec4 u_displacement_axes;
uniform float u_sample_radius;
uniform vec4 u_parameter0;
uniform vec4 u_parameter1;
uniform vec4 u_parameter2;
uniform vec4 u_parameter3;
uniform vec4 u_parameter4;
uniform vec4 u_parameter5;
uniform vec4 u_parameter6;
uniform vec4 u_parameter7;
varying vec2 v_local_px;
)glsl"} + noctalia::material::kShapeShaderSource + std::string{R"glsl(
float segment_t(float p, float a, float b) { return clamp((p-a)/max(b-a,0.0001),0.0,1.0); }
vec4 source_color_at(vec2 local_px) {
  if (u_fill_mode != 2) return u_fill;
  float p = dot(clamp(local_px,vec2(0.0),u_rect_size)/max(u_rect_size,vec2(0.0001)),u_gradient_direction);
  vec4 s=clamp(u_gradient_stops,0.0,1.0);s.y=max(s.y,s.x);s.z=max(s.z,s.y);s.w=max(s.w,s.z);
  if(p<=s.y)return mix(u_gradient_color0,u_gradient_color1,segment_t(p,s.x,s.y));
  if(p<=s.z)return mix(u_gradient_color1,u_gradient_color2,segment_t(p,s.y,s.z));
  return mix(u_gradient_color2,u_gradient_color3,segment_t(p,s.z,s.w));
}
vec4 premultiply(vec4 c) { return vec4(c.rgb*c.a,c.a); }
vec2 bounded_offset(vec2 offset_px) {
  float n=length(offset_px);
  return n>u_sample_radius && n>0.0 ? offset_px*(u_sample_radius/n) : offset_px;
}
vec4 noctalia_sample_source(vec2 offset_px) {
  return premultiply(source_color_at(v_local_px+bounded_offset(offset_px)));
}
vec4 noctalia_sample_backdrop(vec2 offset_px) {
  vec2 offset=bounded_offset(offset_px);
  vec2 device=vec2(dot(u_displacement_axes.xy,offset),dot(u_displacement_axes.zw,offset));
  vec2 pixel=gl_FragCoord.xy+device;
  pixel=clamp(pixel,u_copy_bounds.xy+vec2(0.5),u_copy_bounds.xy+u_copy_bounds.zw-vec2(0.5));
  return texture2D(u_backdrop,(pixel-u_copy_bounds.xy)/u_backdrop_size);
}
)glsl"} + std::string(userSource) + std::string{R"glsl(
void main() {
  if (u_paint_clip.z==0.0 || u_paint_clip.w==0.0) discard;
  float d=shape_distance(v_local_px,u_rect_size,u_radii,u_corners,u_insets,u_corner_power);
  if (u_paint_clip.z>=0.0 && u_paint_clip.w>=0.0)
    d=max(d,shape_distance(v_local_px-u_paint_clip.xy,u_paint_clip.zw,vec4(u_paint_clip_radius),vec4(0.0),vec4(0.0),u_paint_clip_power));
  float aa=max(length(vec2(dFdx(d),dFdy(d))),0.0001);
  float coverage=clamp(0.5-d/aa,0.0,1.0);
  if(coverage<=0.0)discard;
  vec4 source_pm=noctalia_sample_source(vec2(0.0));
  vec4 backdrop_pm=noctalia_sample_backdrop(vec2(0.0));
  vec4 result=noctalia_effect(source_pm,backdrop_pm,v_local_px/max(u_rect_size,vec2(0.0001)),v_local_px,u_rect_size,
      u_parameter0,u_parameter1,u_parameter2,u_parameter3,u_parameter4,u_parameter5,u_parameter6,u_parameter7);
  result=clamp(result,0.0,1.0);
  float alpha=result.a*source_color_at(v_local_px).a*coverage;
  gl_FragColor=vec4(result.rgb*alpha,alpha);
}
)glsl"};
}

GLint uniform(GLuint program, const char* name) { return glGetUniformLocation(program, name); }
}

bool CustomEffectProgramCache::compile(Entry& entry, const CustomEffectAsset& asset) {
  entry.requestedDigest = asset.sha256Digest;
  entry.requestedAbi = asset.abi;
  std::string validationError;
  if (!validCustomEffectAsset(asset, &validationError)) {
    entry.log = std::move(validationError);
    return false;
  }
  Program candidate;
  try {
    const auto fragment = fragmentShader(asset.source);
    candidate.shader.create(kVertexShader, fragment.c_str());
  } catch (const std::exception& error) {
    entry.log = error.what();
    return false;
  }
  const auto id = candidate.shader.id();
  candidate.activeDigest = asset.sha256Digest;
  candidate.maxSampleRadiusPx = asset.maxSampleRadiusPx;
  candidate.position = glGetAttribLocation(id, "a_position");
  candidate.surfaceSize = uniform(id, "u_surface_size");
  candidate.quadSize = uniform(id, "u_quad_size");
  candidate.rectSize = uniform(id, "u_rect_size");
  candidate.transform = uniform(id, "u_transform");
  candidate.fill = uniform(id, "u_fill");
  candidate.gradientStops = uniform(id, "u_gradient_stops");
  candidate.gradientColors[0] = uniform(id, "u_gradient_color0");
  candidate.gradientColors[1] = uniform(id, "u_gradient_color1");
  candidate.gradientColors[2] = uniform(id, "u_gradient_color2");
  candidate.gradientColors[3] = uniform(id, "u_gradient_color3");
  candidate.gradientDirection = uniform(id, "u_gradient_direction");
  candidate.fillMode = uniform(id, "u_fill_mode");
  candidate.radii = uniform(id, "u_radii");
  candidate.corners = uniform(id, "u_corners");
  candidate.insets = uniform(id, "u_insets");
  candidate.cornerPower = uniform(id, "u_corner_power");
  candidate.paintClip = uniform(id, "u_paint_clip");
  candidate.paintClipRadius = uniform(id, "u_paint_clip_radius");
  candidate.paintClipPower = uniform(id, "u_paint_clip_power");
  candidate.backdrop = uniform(id, "u_backdrop");
  candidate.copyBounds = uniform(id, "u_copy_bounds");
  candidate.backdropSize = uniform(id, "u_backdrop_size");
  candidate.displacementAxes = uniform(id, "u_displacement_axes");
  candidate.sampleRadius = uniform(id, "u_sample_radius");
  for (std::size_t index = 0; index < 8; ++index)
    candidate.parameters[index] = uniform(id, std::format("u_parameter{}", index).c_str());
  if (candidate.position < 0 || candidate.surfaceSize < 0 || candidate.rectSize < 0
      || candidate.transform < 0) {
    entry.log = "custom effect wrapper is missing a required shader location";
    return false;
  }
  candidate.backdropTexture=std::exchange(entry.active.backdropTexture,0);
  candidate.backdropWidth=std::exchange(entry.active.backdropWidth,0);
  candidate.backdropHeight=std::exchange(entry.active.backdropHeight,0);
  entry.active = std::move(candidate);
  entry.log.clear();
  return true;
}

bool CustomEffectProgramCache::draw(
    float surfaceWidth, float surfaceHeight, float width, float height,
    const RoundedRectStyle& style, const Mat3& transform) {
  if (!style.customBackground || !style.customBackground->asset || width <= 0.0F || height <= 0.0F)
    return false;
  if (style.outerShadow || style.invertFill || style.frameContour
      || style.segmentContour.kind != SegmentContourKind::None)
    return false;
  const auto& binding = *style.customBackground;
  const auto& asset = *binding.asset;
  auto& entry = m_entries[asset.stableId];
  entry.owner = binding.asset;
  if (entry.requestedDigest != asset.sha256Digest || entry.requestedAbi != asset.abi)
    (void)compile(entry, asset);
  if (!entry.active.shader.isValid()) return false;
  return drawActive(entry.active, binding, surfaceWidth, surfaceHeight, width, height, style, transform);
}

bool CustomEffectProgramCache::drawActive(
    Program& program, const CustomEffectBinding& binding,
    float surfaceWidth, float surfaceHeight, float width, float height,
    const RoundedRectStyle& style, const Mat3& transform) {
  GLint viewport[4]{};
  glGetIntegerv(GL_VIEWPORT, viewport);
  if (surfaceWidth <= 0.0F || surfaceHeight <= 0.0F || viewport[2] <= 0 || viewport[3] <= 0) return false;
  const float sampleRadius = std::min(binding.asset->maxSampleRadiusPx, program.maxSampleRadiusPx);
  const auto capture=customEffectCapture(viewport[0],viewport[1],viewport[2],viewport[3],
      surfaceWidth,surfaceHeight,width,height,sampleRadius,transform);
  if (!capture) return false;
  const int x=capture->x,y=capture->y,w=capture->width,h=capture->height;

  GLint oldActive=0,oldTexture=0;
  glGetIntegerv(GL_ACTIVE_TEXTURE,&oldActive);glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture);
  if (program.backdropTexture == 0) glGenTextures(1,&program.backdropTexture);
  glBindTexture(GL_TEXTURE_2D,program.backdropTexture);
  if (w > program.backdropWidth || h > program.backdropHeight) {
    program.backdropWidth=std::max(program.backdropWidth,(w+63)/64*64);
    program.backdropHeight=std::max(program.backdropHeight,(h+63)/64*64);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,program.backdropWidth,program.backdropHeight,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  }
  glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,x,y,w,h);
  glUseProgram(program.shader.id());
  glUniform1i(program.backdrop,0);
  glUniform2f(program.surfaceSize,surfaceWidth,surfaceHeight);
  glUniform2f(program.quadSize,width,height);
  glUniform2f(program.rectSize,width,height);
  glUniformMatrix3fv(program.transform,1,GL_FALSE,transform.m.data());
  glUniform4f(program.fill,style.fill.r,style.fill.g,style.fill.b,style.fill.a);
  glUniform4f(program.gradientStops,style.gradientStops[0].position,style.gradientStops[1].position,
      style.gradientStops[2].position,style.gradientStops[3].position);
  for (std::size_t index=0;index<4;++index) {
    const auto& color=style.gradientStops[index].color;
    glUniform4f(program.gradientColors[index],color.r,color.g,color.b,color.a);
  }
  glUniform2f(program.gradientDirection,
      style.gradientDirection==GradientDirection::Horizontal?1.0F:0.0F,
      style.gradientDirection==GradientDirection::Vertical?1.0F:0.0F);
  glUniform1i(program.fillMode,style.fillMode==FillMode::LinearGradient?2:style.fillMode==FillMode::Solid?1:0);
  glUniform4f(program.radii,style.radius.tl,style.radius.tr,style.radius.br,style.radius.bl);
  glUniform4f(program.corners,style.corners.tl==CornerShape::Concave?1.0F:0.0F,
      style.corners.tr==CornerShape::Concave?1.0F:0.0F,
      style.corners.br==CornerShape::Concave?1.0F:0.0F,
      style.corners.bl==CornerShape::Concave?1.0F:0.0F);
  glUniform4f(program.insets,style.logicalInset.left,style.logicalInset.top,style.logicalInset.right,style.logicalInset.bottom);
  glUniform1f(program.cornerPower,style.cornerPower.value_or(2.0F));
  const auto clip=style.paintClip.value_or(RoundedPaintClip{0,0,-1,-1,0});
  glUniform4f(program.paintClip,clip.x,clip.y,clip.width,clip.height);
  glUniform1f(program.paintClipRadius,clip.radius);
  glUniform1f(program.paintClipPower,clip.cornerPower.value_or(style.cornerPower.value_or(2.0F)));
  glUniform4f(program.copyBounds,x,y,w,h);
  glUniform2f(program.backdropSize,program.backdropWidth,program.backdropHeight);
  glUniform4f(program.displacementAxes,
      capture->axisXx,capture->axisYx,capture->axisXy,capture->axisYy);
  glUniform1f(program.sampleRadius,sampleRadius);
  for (std::size_t index=0;index<8;++index) {
    std::array<float,4> bounded{};
    for(std::size_t component=0;component<bounded.size();++component) {
      const float value=binding.parameters[index][component];
      bounded[component]=std::isfinite(value)?std::clamp(value,-16.0F,16.0F):0.0F;
    }
    glUniform4fv(program.parameters[index],1,bounded.data());
  }
  static constexpr std::array<float,12> vertices{0,0,1,0,0,1,0,1,1,0,1,1};
  glVertexAttribPointer(static_cast<GLuint>(program.position),2,GL_FLOAT,GL_FALSE,0,vertices.data());
  glEnableVertexAttribArray(static_cast<GLuint>(program.position));
  glDrawArrays(GL_TRIANGLES,0,6);
  glDisableVertexAttribArray(static_cast<GLuint>(program.position));
  glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(oldTexture));glActiveTexture(static_cast<GLenum>(oldActive));
  return true;
}

std::optional<CustomEffectCompileStatus> CustomEffectProgramCache::status(std::string_view stableId) const {
  const auto found=m_entries.find(std::string(stableId));
  if(found==m_entries.end())return std::nullopt;
  return CustomEffectCompileStatus{.stableId=found->first,.activeDigest=found->second.active.activeDigest,
      .requestedDigest=found->second.requestedDigest,.log=found->second.log,
      .active=found->second.active.shader.isValid()};
}

void CustomEffectProgramCache::destroy() {
  for(auto& item:m_entries) {
    auto& entry=item.second;
    if(entry.active.backdropTexture!=0)glDeleteTextures(1,&entry.active.backdropTexture);
  }
  m_entries.clear();
  m_pendingRelease.clear();
}

void CustomEffectProgramCache::abandon() noexcept {
  for(auto& item:m_entries) {
    auto& entry=item.second;
    entry.active.shader.abandon();entry.active.backdropTexture=0;
    entry.active.backdropWidth=0;entry.active.backdropHeight=0;entry.active.activeDigest.clear();
    entry.active.maxSampleRadiusPx=0.0F;
    entry.requestedDigest.clear();entry.requestedAbi=0;
  }
}

void CustomEffectProgramCache::collectUnused() {
  std::erase_if(m_entries,[&](auto& item) {
    if(!item.second.owner.expired() && !m_pendingRelease.contains(item.first))return false;
    if(item.second.active.backdropTexture!=0)glDeleteTextures(1,&item.second.active.backdropTexture);
    return true;
  });
  m_pendingRelease.clear();
}

void CustomEffectProgramCache::release(std::string_view stableId) {
  m_pendingRelease.emplace(stableId);
}
