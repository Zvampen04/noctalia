// ABI 1. A static procedural highlight; it does not request continuous frames.
vec4 noctalia_effect(
    vec4 source_pm, vec4 backdrop_pm, vec2 local_uv, vec2 local_px, vec2 size_px,
    vec4 p0, vec4 p1, vec4 p2, vec4 p3, vec4 p4, vec4 p5, vec4 p6, vec4 p7) {
  vec3 base = source_pm.rgb / max(source_pm.a, 0.0001);
  float diagonal = abs(local_uv.x + local_uv.y - 0.65);
  float sheen = (1.0 - smoothstep(0.0, max(0.001, p0.y), diagonal)) * clamp(p0.x, 0.0, 1.0);
  float rim = smoothstep(0.55, 1.0, max(abs(local_uv.x * 2.0 - 1.0), abs(local_uv.y * 2.0 - 1.0)));
  vec3 color = mix(base, vec3(1.0), clamp(sheen + rim * p0.z, 0.0, 1.0));
  return vec4(color, 1.0);
}
