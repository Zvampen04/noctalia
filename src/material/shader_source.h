#pragma once

namespace noctalia::material {

// Insert after the caller's GLSL version/extensions/precision and before main().
// GLSL ES 1.00 compatible; the caller owns geometry, sample textures, coverage and
// alpha compositing. SDF distances and parameter dimensions use logical pixels.
inline constexpr char kMaterialShaderSource[] = R"material_glsl(

float materialPlateauHeight(float distance, float shoulder, float elevation) {
    if (shoulder <= 0.0001) return 0.0;
    float t = clamp(-distance / shoulder, 0.0, 1.0);
    return elevation * t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float materialPlateauDerivative(float distance, float shoulder, float elevation) {
    if (shoulder <= 0.0001) return 0.0;
    float t = clamp(-distance / shoulder, 0.0, 1.0);
    return -elevation * 30.0 * t * t * (t - 1.0) * (t - 1.0) / shoulder;
}

// distanceGradient is the central difference of the caller's exact shape SDF,
// divided by its 2*step. It is not a framebuffer derivative: those would change
// lighting with output scale or output orientation.
vec3 materialPlateauNormal(float distance, vec2 distanceGradient, vec4 plateau) {
    vec2 slope = distanceGradient * materialPlateauDerivative(distance, plateau.y, plateau.x);
    return normalize(vec3(-slope, 1.0));
}

// The flat top retains the original semantic palette color. Shoulder lighting
// compares against that face's diffuse response, so a flat material at any
// elevation remains visually continuous. Input/output are straight RGB.
vec3 materialPlateauShade(vec3 color, float distance, vec2 distanceGradient,
                         vec4 light, vec4 plateau) {
    vec3 normal = materialPlateauNormal(distance, distanceGradient, plateau);
    vec3 lightDirection = normalize(light.xyz);
    float illumination = (max(dot(normal, lightDirection), 0.0) - lightDirection.z) * light.w;
    // Normalize each side by its available angular range. Without this, a
    // light-colored inset loses its lower/right highlight because the face has
    // already consumed most of the diffuse response, leaving an incomplete rim.
    float highlight = max(illumination, 0.0) / max(1.0 - lightDirection.z, 0.05);
    float shadow = max(-illumination, 0.0) / max(lightDirection.z, 0.05);
    vec3 shaded = mix(color, vec3(1.0), clamp(highlight * plateau.z, 0.0, 1.0));
    return mix(shaded, vec3(0.0), clamp(shadow * plateau.w, 0.0, 1.0));
}

// Adam Giebl's generator varies each sRGB channel multiplicatively. These
// formulas accept the current palette color; they never select a theme color.
vec3 materialPlateauFace(vec3 color,vec2 position,vec2 size,vec4 light,vec4 shape,vec4 face) {
    vec2 direction=-light.xy;
    float span=dot(abs(direction),size);
    float t=span>0.0001 ? clamp(dot(position-size*0.5,direction)/span+0.5,0.0,1.0) : 0.5;
    vec3 low=clamp(color*(1.0-face.y),0.0,1.0);
    vec3 high=clamp(color*(1.0+face.x),0.0,1.0);
    vec3 gradient=shape.z>=0.0 ? mix(high,low,t) : mix(low,high,t);
    return mix(color,gradient,abs(shape.z));
}
vec2 materialPlateauShadowOffset(vec4 light,vec4 shape) {
    float extent=max(abs(light.x),abs(light.y));
    return extent>0.0001 ? -light.xy/extent*shape.x : vec2(0.0);
}
// Analytic Gaussian edge convolution (CSS blur sigma = blur/2). Evaluating the
// exact control SDF retains rounded geometry without offscreen blur textures.
float materialShadowCoverage(float distance,float blur,float aa) {
    if(blur<=0.0001)return 1.0-smoothstep(-max(aa,0.001),max(aa,0.001),distance);
    float x=abs(distance)/max(blur*0.70710678118,0.0001);
    float t=1.0/(1.0+0.3275911*x);
    float erf=1.0-(((((1.061405429*t-1.453152027)*t)+1.421413741)*t-0.284496736)*t+0.254829592)*t*exp(-x*x);
    return clamp(0.5-0.5*sign(distance)*erf,0.0,1.0);
}
vec2 materialPlateauShadowWeights(float darkDistance,float lightDistance,float elevation,vec4 contact,float aa) {
    vec2 weights=vec2(materialShadowCoverage(darkDistance,contact.x,aa),materialShadowCoverage(lightDistance,contact.x,aa));
    if(elevation<0.0)weights=1.0-weights;
    return weights*contact.y;
}
vec4 materialPlateauShadowPair(vec3 base,vec2 weights,vec4 shape) {
    vec3 dark=clamp(base*(1.0-shape.y),0.0,1.0);
    vec3 bright=clamp(base*(1.0+shape.y),0.0,1.0);
    // CSS paints the first listed (dark) shadow over the light shadow.
    return vec4(dark*weights.x+bright*weights.y*(1.0-weights.x),weights.x+weights.y*(1.0-weights.x));
}

// Fourth-order shaped glass height and Snell refraction derive from
// ryohsuke1231/liquid-glass (MIT), copyright (c) 2026 Ryosuke Watanabe.
// See src/material/LIQUID-GLASS-LICENSE and README.md for exact provenance.
float materialGlassHeight(float distance, vec4 optical) {
    if (optical.y <= 0.0001) return 0.0;
    float t = clamp(-distance / optical.y, 0.0, 1.0);
    return pow(max(1.0 - pow(1.0 - t, 4.0), 0.0), 0.25) * optical.x;
}

// Pass central differences of materialGlassHeight() as slope. Finite differences
// keep the high-curvature outer edge finite without changing the height profile.
vec2 materialGlassDisplacement(vec2 slope, vec4 optical) {
    vec3 normal = normalize(vec3(-clamp(slope, vec2(-64.0), vec2(64.0)), 1.0));
    vec3 ray = refract(vec3(0.0, 0.0, -1.0), normal, 1.0 / max(optical.z, 1.0));
    vec2 offset = ray.xy / max(-ray.z, 0.15) * optical.x;
    return offset * min(1.0, optical.w / max(length(offset), 0.0001));
}

// Independently implemented radial edge contraction in logical coordinates.
// Lens fields: optical radius, mapping, radial strength, edge falloff. Keep the
// existing Snell branch untouched, including its no-material defaults.
vec2 materialGlassLensDisplacement(vec2 slope, vec2 fromCenter, float distance,
                                   vec4 optical, vec4 lens) {
    if (lens.y < 0.5) return materialGlassDisplacement(slope, optical);
    if (optical.y <= 0.0001 || optical.w <= 0.0 || lens.z <= 0.0) return vec2(0.0);
    float proximity = clamp(1.0 + distance / optical.y, 0.0, 1.0);
    float envelope = sin(1.57079632679 * pow(proximity, lens.w));
    vec2 displacement = -fromCenter * (lens.z * envelope);
    return displacement * min(1.0, optical.w / max(length(displacement), 0.0001));
}

// Applies only to sampled backdrop, before the palette coating and foreground.
vec3 materialGlassBackdropColor(vec3 color, vec4 adjustment) {
    float luminance=dot(color,vec3(0.2126,0.7152,0.0722));
    color=mix(vec3(luminance),color,adjustment.z);
    return clamp((color-vec3(0.5))*adjustment.y+vec3(0.5+adjustment.x),0.0,1.0);
}

vec3 materialGlassShade(vec3 color, vec2 slope, float distance,
                       vec4 optical, vec4 light, vec4 opticalLight, float rimFalloff) {
    vec3 normal = normalize(vec3(-clamp(slope, vec2(-64.0), vec2(64.0)), 1.0));
    vec3 lightDirection = normalize(light.xyz);
    vec3 viewDirection = vec3(0.0, 0.0, 1.0);
    float edge = max(optical.y, 0.001);
    float depth = max(-distance, 0.0);
    float bandWidth=opticalLight.w<0.0 ? edge*0.22 : opticalLight.w;
    float band = bandWidth<=0.0 ? 0.0 : 1.0-smoothstep(0.0,bandWidth,abs(distance));
    float ao = 1.0 - smoothstep(0.0, edge * 0.45, depth);
    color *= 1.0 - ao * opticalLight.x * 0.2 * step(0.0001,bandWidth);
    float fresnel = pow(max(1.0 - dot(normal, viewDirection), 0.0), 2.0);
    float directional = rimFalloff<=0.0 ? 1.0 : pow(abs(dot(normal, lightDirection)), rimFalloff);
    float rim = mix(pow(band, 0.85), fresnel, 0.55) * band * directional * opticalLight.x;
    float specular = pow(max(dot(reflect(-lightDirection, normal), viewDirection), 0.0), 24.0)
                     * opticalLight.y * clamp(band + 0.65, 0.0, 1.0);
    float sheen = pow(max(dot(normal, lightDirection), 0.0), 1.65)
                  * mix(1.0, 0.55, band) * opticalLight.z;
    float highlight = clamp(rim + specular + sheen, 0.0, 1.0);
    return 1.0 - (1.0 - clamp(color, 0.0, 1.0)) * (1.0 - highlight);
}

// Add these offsets to the refracted sample, in logical material coordinates.
// Four bounded taps plus the center are sufficient for the intended low blur.
// The caller must transform them to texture/device coordinates before sampling.
vec2 materialGlassScatterOffset(float tap, float radius) {
    if (tap < 0.5) return vec2(radius, 0.0);
    if (tap < 1.5) return vec2(-radius, 0.0);
    if (tap < 2.5) return vec2(0.0, radius);
    return vec2(0.0, -radius);
}

// Spatial harmonics, never a noise texture or per-frame random signal. The
// position is component-local: moving a window cannot crawl its linework.
float materialIllustratedVariation(vec2 position, vec4 illustration) {
    vec2 p = position * (6.28318530718 / max(illustration.z, 4.0));
    float phase = illustration.w;
    return illustration.y * (0.58 * sin(p.x * 0.79 + p.y * 0.61 + phase)
                            + 0.27 * sin(p.x * -0.43 + p.y * 1.17 + phase * 1.73)
                            + 0.15 * sin(p.x * 1.51 + p.y * -0.91 + phase * 0.47));
}

// Decorate the inward stroke only. Multiply by the caller's original shape
// coverage so the precise outer contour, clipping and all hit targets are kept.
float materialIllustratedStroke(float distance, vec2 position, vec4 illustration, float aa) {
    if (illustration.x <= 0.0) return 0.0;
    float irregular = materialIllustratedVariation(position, illustration);
    float inwardEdge = illustration.x + irregular;
    return 1.0 - smoothstep(inwardEdge - max(aa, 0.001), inwardEdge + max(aa, 0.001), -distance);
}

// A deliberate broad painted glaze near edges, with a clean flat central fill.
// No alpha/transparency or pixel-frequency noise is introduced by this helper.
vec3 materialIllustratedPaint(vec3 color, float distance, vec2 position,
                             vec4 illustration, vec4 paint) {
    if (paint.x <= 0.0 || paint.y <= 0.0) return color;
    float edge = 1.0 - smoothstep(0.0, paint.x, max(-distance, 0.0));
    float glaze = 0.65 + 0.35 * sin(position.x * 0.013 + position.y * 0.019 + illustration.w);
    return mix(color, color * 0.62, edge * glaze * paint.y);
}
)material_glsl";

} // namespace noctalia::material
