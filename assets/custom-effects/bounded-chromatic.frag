// ABI 1. Refracts only pixels already drawn in this Noctalia surface.
vec4 noctalia_effect(
    vec4 source_pm, vec4 backdrop_pm, vec2 local_uv, vec2 local_px, vec2 size_px,
    vec4 p0, vec4 p1, vec4 p2, vec4 p3, vec4 p4, vec4 p5, vec4 p6, vec4 p7) {
  vec2 centered = local_uv * 2.0 - 1.0;
  float edge = smoothstep(0.25, 1.0, max(abs(centered.x), abs(centered.y)));
  vec2 direction = normalize(centered + vec2(0.0001));
  vec2 offset = direction * edge * p0.x;
  vec4 red = noctalia_sample_backdrop(offset * (1.0 + p0.y));
  vec4 green = noctalia_sample_backdrop(offset);
  vec4 blue = noctalia_sample_backdrop(offset * (1.0 - p0.y));
  float backdrop_alpha = max(max(red.a, green.a), blue.a);
  vec3 backdrop_rgb = vec3(red.r, green.g, blue.b) / max(backdrop_alpha, 0.0001);
  vec3 source_rgb = source_pm.rgb / max(source_pm.a, 0.0001);
  float tint = clamp(p0.z, 0.0, 1.0) * source_pm.a;
  return vec4(mix(backdrop_rgb, source_rgb, tint), 1.0);
}
