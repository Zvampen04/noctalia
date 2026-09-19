#include "render/programs/rect_program.h"

#include "render/core/render_styles.h"
#include "material/shader_source.h"
#include "material/shape_source.h"
#include <string>

#include <array>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform vec2 u_rect_origin;
uniform vec2 u_rect_size;
uniform mat3 u_transform;
varying vec2 v_pixel;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_quad_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_pixel = local - u_rect_origin;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  const std::string kFragmentShaderSource = std::string{R"(
#extension GL_OES_standard_derivatives : require
precision highp float;

uniform vec2 u_rect_size;
uniform vec4 u_paint_clip;
uniform float u_paint_clip_radius;
uniform vec4 u_color;
uniform vec4 u_border_color;
uniform int u_fill_mode;
uniform vec2 u_gradient_direction;
uniform vec4 u_gradient_stops;
uniform vec4 u_gradient_color0;
uniform vec4 u_gradient_color1;
uniform vec4 u_gradient_color2;
uniform vec4 u_gradient_color3;
uniform vec4 u_corner_shapes; // tl, tr, br, bl: 0 = convex, 1 = concave
uniform vec4 u_logical_inset; // left, top, right, bottom
uniform float u_corner_power;
uniform float u_paint_clip_power;
uniform float u_shadow_exclusion_power;
uniform vec4 u_radii;  // tl, tr, br, bl
uniform float u_softness;
uniform int u_no_aa;
uniform int u_invert_fill;
uniform int u_frame_enabled;
uniform int u_frame_chamfered;
uniform vec4 u_frame_chamfers;
uniform vec4 u_frame_shelf_rects[12];
uniform vec4 u_frame_shelf_shapes[12];
uniform vec4 u_frame_border_colors[3];
uniform vec4 u_frame_border_shapes[3]; // width, frameward offset, enabled, reserved
uniform int u_segment_kind;
uniform float u_segment_depth;
uniform int u_segment_vertical;
uniform float u_border_width;
uniform int u_glass;
uniform sampler2D u_backdrop;
uniform vec4 u_copy_bounds;
uniform vec2 u_backdrop_size;
uniform vec4 u_displacement_axes;
uniform int u_material;
uniform vec4 u_material_light;
uniform vec4 u_material_plateau;
uniform vec4 u_material_contact;
uniform vec4 u_material_plateau_shape;
uniform vec4 u_material_plateau_face;
uniform vec4 u_material_optical;
uniform vec4 u_material_optical_style;
uniform vec4 u_material_optical_light;
uniform vec4 u_material_optical_color;
uniform vec4 u_material_optical_lens;
uniform vec4 u_material_illustration;
uniform vec4 u_material_paint;
uniform int u_outer_shadow;
uniform vec2 u_shadow_cutout_offset;
uniform int u_shadow_exclusion;
uniform vec2 u_shadow_exclusion_offset;
uniform vec2 u_shadow_exclusion_size;
uniform vec4 u_shadow_exclusion_corner_shapes;
uniform vec4 u_shadow_exclusion_logical_inset;
uniform vec4 u_shadow_exclusion_radii;
varying vec2 v_pixel;

)"} + noctalia::material::kShapeShaderSource + R"(
// Analytic edge coverage: the signed distance is normalized by its screen-space
// rate of change, so the transition is one device pixel wide whatever the
// surface scale, node scale, or how fast the field varies along a curve. The
// linear ramp is the exact box-filter coverage of a straight edge, so an edge
// landing on a pixel boundary still gives 100% on the inside pixel and 0% on the
// outside one. A smoothstep window instead pushes partial coverage toward the
// extremes, which reads as stair-stepping on curves.
float coverage_for(float distance) {
    if (u_no_aa == 1) {
        return 1.0 - step(0.0, distance);
    }
    float pixel_width = max(length(vec2(dFdx(distance), dFdy(distance))), 1e-5);
    return clamp(0.5 - distance / pixel_width, 0.0, 1.0);
}

float gradient_segment_t(float position, float start, float end) {
    return clamp((position - start) / max(end - start, 0.0001), 0.0, 1.0);
}

vec4 sample_backdrop(vec2 pixel) {
    pixel=clamp(pixel,u_copy_bounds.xy+vec2(0.5),u_copy_bounds.xy+u_copy_bounds.zw-vec2(0.5));
    return texture2D(u_backdrop,(pixel-u_copy_bounds.xy)/u_backdrop_size);
}
)" + noctalia::material::kMaterialShaderSource + R"(
vec4 gradient_fill(float position) {
    vec4 stops = clamp(u_gradient_stops, vec4(0.0), vec4(1.0));
    stops.y = max(stops.y, stops.x);
    stops.z = max(stops.z, stops.y);
    stops.w = max(stops.w, stops.z);

    vec4 c0 = u_gradient_color0;
    vec4 c1 = u_gradient_color1;
    vec4 c2 = u_gradient_color2;
    vec4 c3 = u_gradient_color3;

    if (position <= stops.y) {
        return mix(c0, c1, gradient_segment_t(position, stops.x, stops.y));
    }
    if (position <= stops.z) {
        return mix(c1, c2, gradient_segment_t(position, stops.y, stops.z));
    }
    return mix(c2, c3, gradient_segment_t(position, stops.z, stops.w));
}

// Signed Euclidean distance to the convex, independently chamfered aperture.
// Eight edges share one contour so borders and material normals follow the cuts.
float frame_chamfer_distance(vec2 point) {
    vec2 lo=u_logical_inset.xy;
    vec2 hi=u_rect_size-u_logical_inset.zw;
    if (hi.x<=lo.x || hi.y<=lo.y) return length(point-lo)+1.0;
    vec4 cuts=clamp(u_frame_chamfers,0.0,min(hi.x-lo.x,hi.y-lo.y)*0.5);
    vec2 vertices[8];
    vertices[0]=vec2(lo.x+cuts.x,lo.y);
    vertices[1]=vec2(hi.x-cuts.y,lo.y);
    vertices[2]=vec2(hi.x,lo.y+cuts.y);
    vertices[3]=vec2(hi.x,hi.y-cuts.z);
    vertices[4]=vec2(hi.x-cuts.z,hi.y);
    vertices[5]=vec2(lo.x+cuts.w,hi.y);
    vertices[6]=vec2(lo.x,hi.y-cuts.w);
    vertices[7]=vec2(lo.x,lo.y+cuts.x);
    float nearest=1.0e20;
    bool inside=true;
    vec2 previous=vertices[7];
    for (int i=0;i<8;++i) {
        vec2 edge=vertices[i]-previous;
        vec2 relative=point-previous;
        float lengthSquared=dot(edge,edge);
        if (lengthSquared>0.000001) {
            float along=clamp(dot(relative,edge)/lengthSquared,0.0,1.0);
            vec2 delta=relative-along*edge;
            nearest=min(nearest,dot(delta,delta));
            if (edge.x*relative.y-edge.y*relative.x<0.0) inside=false;
        }
        previous=vertices[i];
    }
    return sqrt(nearest)*(inside?-1.0:1.0);
}

float segment_distance(vec2 point) {
    vec2 p = u_segment_vertical == 1 ? point.yx : point;
    vec2 size = u_segment_vertical == 1 ? u_rect_size.yx : u_rect_size;
    float depth = clamp(u_segment_depth, 0.0, min(size.x, size.y) * 0.5);
    bool leading = u_segment_kind == 1 || u_segment_kind == 3;
    bool trailing = u_segment_kind == 1 || u_segment_kind == 2;
    vec2 vertices[4];
    vertices[0]=vec2(0.0,0.0);
    vertices[1]=vec2(trailing ? size.x-depth : size.x,0.0);
    vertices[2]=vec2(size.x,size.y);
    vertices[3]=vec2(leading ? depth : 0.0,size.y);
    float nearest=1.0e20;
    bool inside=true;
    vec2 previous=vertices[3];
    for (int i=0;i<4;++i) {
        vec2 edge=vertices[i]-previous;
        vec2 relative=p-previous;
        float lengthSquared=dot(edge,edge);
        if (lengthSquared>0.000001) {
            float along=clamp(dot(relative,edge)/lengthSquared,0.0,1.0);
            vec2 delta=relative-along*edge;
            nearest=min(nearest,dot(delta,delta));
            if (edge.x*relative.y-edge.y*relative.x<0.0) inside=false;
        }
        previous=vertices[i];
    }
    return sqrt(nearest)*(inside?-1.0:1.0);
}

// Combine the material before lighting it, so joins have one silhouette and
// one normal field instead of overlapping rectangle shadows.
float surface_distance(vec2 point) {
    if (u_segment_kind != 0) return segment_distance(point);
    float d = shape_distance(point,u_rect_size,u_radii,u_corner_shapes,u_logical_inset,u_corner_power);
    if (u_frame_enabled == 0) return d;
    if (u_frame_chamfered==1) d=frame_chamfer_distance(point);
    d = -d;
    for (int i=0; i<12; ++i) {
        vec4 rect = u_frame_shelf_rects[i];
        if (rect.z <= 0.0 || rect.w <= 0.0) continue;
        vec4 shape = u_frame_shelf_shapes[i];
        float shelf = shape_distance(point-rect.xy,rect.zw,vec4(shape.x),vec4(0.0),vec4(0.0),u_corner_power);
        float k = shape.y;
        if (k > 0.0) {
            float h = clamp(0.5+0.5*(shelf-d)/k,0.0,1.0);
            d = mix(shelf,d,h)-k*h*(1.0-h);
        } else d = min(d,shelf);
    }
    return d;
}

vec4 apply_frame_border_layers(vec4 base, float distance, float shape_coverage) {
    if (u_frame_enabled == 0) return base;
    for (int i=0; i<3; ++i) {
        vec4 shape=u_frame_border_shapes[i];
        if (shape.z<0.5 || shape.x<=0.0 || u_frame_border_colors[i].a<=0.0) continue;
        float near_coverage=coverage_for(distance+shape.y);
        float far_coverage=coverage_for(distance+shape.y+shape.x);
        float alpha=u_frame_border_colors[i].a*near_coverage*(1.0-far_coverage)*shape_coverage;
        base=vec4(u_frame_border_colors[i].rgb*alpha,alpha)+base*(1.0-alpha);
    }
    return base;
}

bool has_frame_border_layers() {
    if (u_frame_enabled == 0) return false;
    for (int i=0; i<3; ++i) {
        if (u_frame_border_shapes[i].z>=0.5 && u_frame_border_shapes[i].x>0.0
                && u_frame_border_colors[i].a>0.0) return true;
    }
    return false;
}

float optical_distance(vec2 point) {
    float d=surface_distance(point);
    if (u_material_optical_lens.x>=0.0 && u_frame_enabled==0 && u_segment_kind==0)
        d=shape_distance(point,u_rect_size,vec4(u_material_optical_lens.x),vec4(0.0),u_logical_inset, 2.0);
    if (u_paint_clip.z>=0.0 && u_paint_clip.w>=0.0)
        d=max(d,shape_distance(point-u_paint_clip.xy,u_paint_clip.zw,
            vec4(u_paint_clip_radius),vec4(0.0),vec4(0.0), u_paint_clip_power));
    return d;
}

void main() {
    float aa = max(u_softness, 0.85);
    vec2 local_point = v_pixel;
    float paint_coverage = 1.0;
    if (u_paint_clip.z >= 0.0 && u_paint_clip.w >= 0.0) {
        if (u_paint_clip.z == 0.0 || u_paint_clip.w == 0.0) discard;
        paint_coverage = coverage_for(shape_distance(local_point-u_paint_clip.xy,
            u_paint_clip.zw, vec4(u_paint_clip_radius), vec4(0.0), vec4(0.0), u_paint_clip_power));
        if (paint_coverage <= 0.0) discard;
    }
    vec2 uv = clamp(local_point / u_rect_size, vec2(0.0), vec2(1.0));

    float outer_distance = surface_distance(local_point);
    float outer_coverage = coverage_for(outer_distance);
    if (u_invert_fill == 1) outer_coverage = 1.0 - outer_coverage;

    if (u_outer_shadow == 1) {
        float cutout_aa = 0.85;
        float shadow_distance = surface_distance(local_point);
        float shadow_outer_coverage = 1.0 - smoothstep(-aa, aa, shadow_distance);
        float cutout_distance = shape_distance(local_point + u_shadow_cutout_offset, u_rect_size, u_radii, u_corner_shapes, u_logical_inset, u_corner_power);
        float cutout_mask = 1.0 - smoothstep(-cutout_aa, cutout_aa, cutout_distance);
        float shadow_coverage = shadow_outer_coverage * (1.0 - cutout_mask);
        if (u_shadow_exclusion == 1 && u_shadow_exclusion_size.x > 0.0 && u_shadow_exclusion_size.y > 0.0) {
            float exclusion_distance = shape_distance(local_point + u_shadow_exclusion_offset, u_shadow_exclusion_size, u_shadow_exclusion_radii, u_shadow_exclusion_corner_shapes, u_shadow_exclusion_logical_inset, u_shadow_exclusion_power);
            float exclusion_mask = 1.0 - smoothstep(-cutout_aa, cutout_aa, exclusion_distance);
            shadow_coverage *= 1.0 - exclusion_mask;
        }
        float out_alpha = u_color.a * shadow_coverage;
        if (out_alpha <= 0.0) {
            discard;
        }
        gl_FragColor = (vec4(u_color.rgb * out_alpha, out_alpha)) * paint_coverage;
        return;
    }

    float gradient_pos = clamp(dot(uv, u_gradient_direction), 0.0, 1.0);
    vec4 fill_base;
    if (u_fill_mode == 0) {
        fill_base = vec4(0.0);
    } else if (u_fill_mode == 1) {
        fill_base = u_color;
    } else {
        fill_base = gradient_fill(gradient_pos);
    }

    vec4 materialReceiver=vec4(0.0);
    if (u_material == 1 && fill_base.a > 0.0) {
        vec3 palette=fill_base.rgb;
        vec2 offset=materialPlateauShadowOffset(u_material_light,u_material_plateau_shape);
        float darkDistance=surface_distance(local_point-offset);
        float lightDistance=surface_distance(local_point+offset);
        float materialAa=max(length(vec2(dFdx(outer_distance),dFdy(outer_distance))),0.001);
        vec2 weights=materialPlateauShadowWeights(darkDistance,lightDistance,u_material_plateau.x,u_material_contact,materialAa);
        vec4 pair=materialPlateauShadowPair(palette,weights,u_material_plateau_shape);
        fill_base.rgb=materialPlateauFace(palette,local_point,u_rect_size,u_material_light,u_material_plateau_shape,u_material_plateau_face);
        if(u_material_plateau.x<0.0)fill_base.rgb=pair.rgb+fill_base.rgb*(1.0-pair.a);
        else materialReceiver=pair*fill_base.a;

        float step = 0.5;
        float dx = surface_distance(local_point + vec2(step, 0.0))
                 - surface_distance(local_point - vec2(step, 0.0));
        float dy = surface_distance(local_point + vec2(0.0, step))
                 - surface_distance(local_point - vec2(0.0, step));
        fill_base.rgb = materialPlateauShade(fill_base.rgb, outer_distance, vec2(dx, dy) / (2.0 * step),
                                            u_material_light, u_material_plateau);

    }

    if (u_material == 2 && u_glass == 0) {
        // Optical child controls belong to their parent's material plane. They
        // only contribute the configured thin tint; text/icons remain separate.
        fill_base.a *= u_material_optical_style.y;
    }

    if (u_glass == 1) {
        float e = 0.5;
        float hR = materialGlassHeight(optical_distance(local_point + vec2(e, 0.0)), u_material_optical);
        float hL = materialGlassHeight(optical_distance(local_point - vec2(e, 0.0)), u_material_optical);
        float hB = materialGlassHeight(optical_distance(local_point + vec2(0.0, e)), u_material_optical);
        float hT = materialGlassHeight(optical_distance(local_point - vec2(0.0, e)), u_material_optical);
        vec2 slope = vec2(hR - hL, hB - hT) / (2.0 * e);
        vec2 bodyMin=min(max(u_logical_inset.xy,vec2(0.0)),u_rect_size);
        vec2 bodyMax=max(bodyMin,u_rect_size-max(u_logical_inset.zw,vec2(0.0)));
        vec2 localOffset = materialGlassLensDisplacement(slope,local_point-(bodyMin+bodyMax)*0.5,
            optical_distance(local_point),u_material_optical,u_material_optical_lens);
        vec2 offset = vec2(dot(u_displacement_axes.xy, localOffset), dot(u_displacement_axes.zw, localOffset));
        vec2 pixel = gl_FragCoord.xy;
        vec4 base = sample_backdrop(pixel);
        vec4 refracted = sample_backdrop(pixel + offset);
        if (u_material_optical_style.x > 0.001) {
            refracted *= 0.5;
            for (int tap = 0; tap < 4; ++tap) {
                vec2 localScatter = materialGlassScatterOffset(float(tap), u_material_optical_style.x);
                vec2 scatter = vec2(dot(u_displacement_axes.xy, localScatter), dot(u_displacement_axes.zw, localScatter));
                refracted += sample_backdrop(pixel + offset + scatter) * 0.125;
            }
        }
        float chromatic = u_material_optical_style.z;
        if (chromatic > 0.0001) {
            // Preserve scattering while adding restrained edge separation.
            refracted.r += (sample_backdrop(pixel + offset * (1.0 + chromatic)).r
                            - sample_backdrop(pixel + offset).r) * 0.5;
            refracted.b += (sample_backdrop(pixel + offset * (1.0 - chromatic)).b
                            - sample_backdrop(pixel + offset).b) * 0.5;
        }
        refracted.rgb=materialGlassBackdropColor(refracted.rgb/max(refracted.a,0.001),u_material_optical_color)*refracted.a;
        float coating = fill_base.a * u_material_optical_style.y;
        vec4 coated = vec4(fill_base.rgb * coating, coating) + refracted * (1.0 - coating);
        coated.rgb = materialGlassShade(coated.rgb / max(coated.a, 0.001), slope, optical_distance(local_point),
                                       u_material_optical, u_material_light, u_material_optical_light,u_material_optical_style.w) * coated.a;
        // Backdrop replacement applies coverage once and never refracts foreground.
        gl_FragColor = apply_frame_border_layers(
            mix(base, coated, outer_coverage * paint_coverage), outer_distance, outer_coverage
        );
        return;
    }

    if (u_material == 3 && fill_base.a > 0.0) {
        fill_base.rgb = materialIllustratedPaint(fill_base.rgb, outer_distance, local_point,
                                                u_material_illustration, u_material_paint);
        float stroke = materialIllustratedStroke(outer_distance, local_point, u_material_illustration,
                                                 max(length(vec2(dFdx(outer_distance), dFdy(outer_distance))), 0.001));
        vec3 ink = u_border_color.a > 0.0 ? u_border_color.rgb : fill_base.rgb * 0.45;
        fill_base.rgb = mix(fill_base.rgb, ink, stroke);
    }

    if (u_border_width <= 0.0 || u_border_color.a <= 0.0) {
        float out_alpha = fill_base.a * outer_coverage;
        if (out_alpha <= 0.0 && materialReceiver.a <= 0.0 && !has_frame_border_layers()) discard;
        gl_FragColor = apply_frame_border_layers(
            (vec4(fill_base.rgb * out_alpha, out_alpha)+materialReceiver*(1.0-out_alpha)) * paint_coverage,
            outer_distance, outer_coverage * paint_coverage
        );
        return;
    }

    bool any_concave = u_corner_shapes.x > 0.5 || u_corner_shapes.y > 0.5 || u_corner_shapes.z > 0.5 || u_corner_shapes.w > 0.5;
    float inner_distance;
    if (any_concave || u_frame_enabled == 1 || u_segment_kind != 0) {
        inner_distance = outer_distance + u_border_width;
    } else {
        vec4 inner_radii = max(u_radii - vec4(u_border_width), vec4(0.0));
        vec2 inner_size = max(u_rect_size - vec2(u_border_width * 2.0), vec2(0.0));
        vec2 inner_point = local_point - vec2(u_border_width);
        vec4 inner_inset = max(u_logical_inset - vec4(u_border_width), vec4(0.0));
        inner_distance = shape_distance(inner_point, inner_size, inner_radii, u_corner_shapes, inner_inset, u_corner_power);
    }
    float inner_coverage = coverage_for(inner_distance);

    if (fill_base.a <= 0.0) {
        float ring_coverage = outer_coverage * (1.0 - inner_coverage);
        float out_alpha = u_border_color.a * ring_coverage;
        if (out_alpha <= 0.0 && !has_frame_border_layers()) {
            discard;
        }
        gl_FragColor = apply_frame_border_layers(
            (vec4(u_border_color.rgb * out_alpha, out_alpha)) * paint_coverage,
            outer_distance, outer_coverage * paint_coverage
        );
        return;
    }

    // Fill and border occupy disjoint regions: the fill lives where
    // inner_coverage == 1, the border ring lives where inner_coverage == 0.
    // Mix between them so a translucent fill never sits on top of a
    // full-area border backplane (which would mask its opacity).
    vec3 border_pm = u_border_color.rgb * u_border_color.a;
    vec3 fill_pm = fill_base.rgb * fill_base.a;

    vec3 interior_rgb = mix(border_pm, fill_pm, inner_coverage);
    float interior_a = mix(u_border_color.a, fill_base.a, inner_coverage);

    // Apply outer shape mask
    float out_alpha = interior_a * outer_coverage;
    if (out_alpha <= 0.0 && materialReceiver.a <= 0.0 && !has_frame_border_layers()) discard;

    // Output premultiplied alpha, with the paired receiver behind the body.
    gl_FragColor = apply_frame_border_layers(
        (vec4(interior_rgb * outer_coverage, out_alpha)+materialReceiver*(1.0-out_alpha)) * paint_coverage,
        outer_distance, outer_coverage * paint_coverage
    );
}
)";

} // namespace

void RectProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource.c_str());
  m_glassLocation = glGetUniformLocation(m_program.id(), "u_glass");
  m_backdropLocation = glGetUniformLocation(m_program.id(), "u_backdrop");
  m_copyBoundsLocation = glGetUniformLocation(m_program.id(), "u_copy_bounds");
  m_backdropSizeLocation = glGetUniformLocation(m_program.id(), "u_backdrop_size");
  m_displacementAxesLocation = glGetUniformLocation(m_program.id(), "u_displacement_axes");
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_quadSizeLocation = glGetUniformLocation(m_program.id(), "u_quad_size");
  m_rectOriginLocation = glGetUniformLocation(m_program.id(), "u_rect_origin");
  m_rectSizeLocation = glGetUniformLocation(m_program.id(), "u_rect_size");
  m_paintClipLocation = glGetUniformLocation(m_program.id(), "u_paint_clip");
  m_paintClipRadiusLocation = glGetUniformLocation(m_program.id(), "u_paint_clip_radius");
  m_colorLocation = glGetUniformLocation(m_program.id(), "u_color");
  m_borderColorLocation = glGetUniformLocation(m_program.id(), "u_border_color");
  m_fillModeLocation = glGetUniformLocation(m_program.id(), "u_fill_mode");
  m_gradientDirectionLocation = glGetUniformLocation(m_program.id(), "u_gradient_direction");
  m_gradientStopsLocation = glGetUniformLocation(m_program.id(), "u_gradient_stops");
  m_gradientColor0Location = glGetUniformLocation(m_program.id(), "u_gradient_color0");
  m_gradientColor1Location = glGetUniformLocation(m_program.id(), "u_gradient_color1");
  m_gradientColor2Location = glGetUniformLocation(m_program.id(), "u_gradient_color2");
  m_gradientColor3Location = glGetUniformLocation(m_program.id(), "u_gradient_color3");
  m_cornerShapesLocation = glGetUniformLocation(m_program.id(), "u_corner_shapes");
  m_logicalInsetLocation = glGetUniformLocation(m_program.id(), "u_logical_inset");
  m_cornerPowerLocation = glGetUniformLocation(m_program.id(), "u_corner_power");
  m_paintClipPowerLocation = glGetUniformLocation(m_program.id(), "u_paint_clip_power");
  m_shadowExclusionPowerLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion_power");
  m_radiiLocation = glGetUniformLocation(m_program.id(), "u_radii");
  m_softnessLocation = glGetUniformLocation(m_program.id(), "u_softness");
  m_noAaLocation = glGetUniformLocation(m_program.id(), "u_no_aa");
  m_invertFillLocation = glGetUniformLocation(m_program.id(), "u_invert_fill");
  m_frameChamferedLocation = glGetUniformLocation(m_program.id(), "u_frame_chamfered");
  m_frameChamfersLocation = glGetUniformLocation(m_program.id(), "u_frame_chamfers");
  m_frameEnabledLocation = glGetUniformLocation(m_program.id(), "u_frame_enabled");
  m_frameShelfRectsLocation = glGetUniformLocation(m_program.id(), "u_frame_shelf_rects[0]");
  m_frameShelfShapesLocation = glGetUniformLocation(m_program.id(), "u_frame_shelf_shapes[0]");
  m_frameBorderColorsLocation = glGetUniformLocation(m_program.id(), "u_frame_border_colors[0]");
  m_frameBorderShapesLocation = glGetUniformLocation(m_program.id(), "u_frame_border_shapes[0]");
  m_segmentKindLocation = glGetUniformLocation(m_program.id(), "u_segment_kind");
  m_segmentDepthLocation = glGetUniformLocation(m_program.id(), "u_segment_depth");
  m_segmentVerticalLocation = glGetUniformLocation(m_program.id(), "u_segment_vertical");
  m_borderWidthLocation = glGetUniformLocation(m_program.id(), "u_border_width");
  m_materialLocation = glGetUniformLocation(m_program.id(), "u_material");
  m_materialLightLocation = glGetUniformLocation(m_program.id(), "u_material_light");
  m_materialPlateauLocation = glGetUniformLocation(m_program.id(), "u_material_plateau");
  m_materialContactLocation = glGetUniformLocation(m_program.id(), "u_material_contact");
  m_materialPlateauShapeLocation = glGetUniformLocation(m_program.id(), "u_material_plateau_shape");
  m_materialPlateauFaceLocation = glGetUniformLocation(m_program.id(), "u_material_plateau_face");
  m_materialOpticalLocation = glGetUniformLocation(m_program.id(), "u_material_optical");
  m_materialOpticalStyleLocation = glGetUniformLocation(m_program.id(), "u_material_optical_style");
  m_materialOpticalLightLocation = glGetUniformLocation(m_program.id(), "u_material_optical_light");
  m_materialOpticalColorLocation = glGetUniformLocation(m_program.id(), "u_material_optical_color");
  m_materialOpticalLensLocation = glGetUniformLocation(m_program.id(), "u_material_optical_lens");
  m_materialIllustrationLocation = glGetUniformLocation(m_program.id(), "u_material_illustration");
  m_materialPaintLocation = glGetUniformLocation(m_program.id(), "u_material_paint");
  m_outerShadowLocation = glGetUniformLocation(m_program.id(), "u_outer_shadow");
  m_shadowCutoutOffsetLocation = glGetUniformLocation(m_program.id(), "u_shadow_cutout_offset");
  m_shadowExclusionLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion");
  m_shadowExclusionOffsetLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion_offset");
  m_shadowExclusionSizeLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion_size");
  m_shadowExclusionCornerShapesLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion_corner_shapes");
  m_shadowExclusionLogicalInsetLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion_logical_inset");
  m_shadowExclusionRadiiLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion_radii");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");

  if (m_glassLocation < 0 || m_backdropLocation < 0 || m_copyBoundsLocation < 0
      || m_backdropSizeLocation < 0 || m_displacementAxesLocation < 0 || m_positionLocation < 0
      || m_surfaceSizeLocation < 0
      || m_quadSizeLocation < 0
      || m_rectOriginLocation < 0
      || m_rectSizeLocation < 0
      || m_paintClipLocation < 0 || m_paintClipRadiusLocation < 0
      || m_colorLocation < 0
      || m_borderColorLocation < 0
      || m_fillModeLocation < 0
      || m_gradientDirectionLocation < 0
      || m_radiiLocation < 0 || m_cornerPowerLocation < 0 || m_paintClipPowerLocation < 0 || m_shadowExclusionPowerLocation < 0
      || m_softnessLocation < 0
      || m_gradientStopsLocation < 0
      || m_gradientColor0Location < 0
      || m_gradientColor1Location < 0
      || m_gradientColor2Location < 0
      || m_gradientColor3Location < 0
      || m_invertFillLocation < 0
      || m_frameChamferedLocation < 0 || m_frameChamfersLocation < 0
      || m_frameEnabledLocation < 0 || m_frameShelfRectsLocation < 0 || m_frameShelfShapesLocation < 0
      || m_frameBorderColorsLocation < 0 || m_frameBorderShapesLocation < 0
      || m_segmentKindLocation < 0 || m_segmentDepthLocation < 0 || m_segmentVerticalLocation < 0
      || m_noAaLocation < 0
      || m_cornerShapesLocation < 0
      || m_logicalInsetLocation < 0
      || m_borderWidthLocation < 0
      || m_materialLocation < 0 || m_materialLightLocation < 0 || m_materialPlateauLocation < 0
      || m_materialContactLocation < 0 || m_materialPlateauShapeLocation < 0 || m_materialPlateauFaceLocation < 0
      || m_materialOpticalLocation < 0 || m_materialOpticalStyleLocation < 0 || m_materialOpticalLightLocation < 0 || m_materialOpticalColorLocation < 0 || m_materialOpticalLensLocation < 0
      || m_materialIllustrationLocation < 0 || m_materialPaintLocation < 0
      || m_outerShadowLocation < 0
      || m_shadowCutoutOffsetLocation < 0
      || m_shadowExclusionLocation < 0
      || m_shadowExclusionOffsetLocation < 0
      || m_shadowExclusionSizeLocation < 0
      || m_shadowExclusionCornerShapesLocation < 0
      || m_shadowExclusionLogicalInsetLocation < 0
      || m_shadowExclusionRadiiLocation < 0
      || m_transformLocation < 0) {
    throw std::runtime_error("failed to query rounded-rect shader locations");
  }
}

void RectProgram::destroy() {
  for (auto& backdrop : m_backdrops) {
    if (backdrop.texture) glDeleteTextures(1,&backdrop.texture);
    backdrop={};
  }
  m_program.destroy();
  m_positionLocation = -1;
  m_surfaceSizeLocation = -1;
  m_quadSizeLocation = -1;
  m_rectOriginLocation = -1;
  m_rectSizeLocation = -1;
  m_paintClipLocation = m_paintClipRadiusLocation = -1;
  m_colorLocation = -1;
  m_borderColorLocation = -1;
  m_fillModeLocation = -1;
  m_gradientDirectionLocation = -1;
  m_gradientStopsLocation = -1;
  m_gradientColor0Location = -1;
  m_gradientColor1Location = -1;
  m_gradientColor2Location = -1;
  m_gradientColor3Location = -1;
  m_cornerShapesLocation = -1;
  m_logicalInsetLocation = -1;
  m_radiiLocation = -1;
  m_cornerPowerLocation = m_paintClipPowerLocation = m_shadowExclusionPowerLocation = -1;
  m_softnessLocation = -1;
  m_noAaLocation = -1;
  m_invertFillLocation = -1;
  m_frameChamferedLocation = m_frameChamfersLocation = -1;
  m_frameEnabledLocation = m_frameShelfRectsLocation = m_frameShelfShapesLocation = -1;
  m_frameBorderColorsLocation = m_frameBorderShapesLocation = -1;
  m_segmentKindLocation = m_segmentDepthLocation = m_segmentVerticalLocation = -1;
  m_borderWidthLocation = -1;
  m_materialLocation = m_materialLightLocation = m_materialPlateauLocation = -1;
  m_materialContactLocation = m_materialPlateauShapeLocation = m_materialPlateauFaceLocation = -1;
  m_materialOpticalLocation = m_materialOpticalStyleLocation = m_materialOpticalLightLocation = m_materialOpticalColorLocation = -1;
  m_materialOpticalLensLocation = -1;
  m_materialIllustrationLocation = m_materialPaintLocation = -1;
  m_outerShadowLocation = -1;
  m_shadowCutoutOffsetLocation = -1;
  m_shadowExclusionLocation = -1;
  m_shadowExclusionOffsetLocation = -1;
  m_shadowExclusionSizeLocation = -1;
  m_shadowExclusionCornerShapesLocation = -1;
  m_shadowExclusionLogicalInsetLocation = -1;
  m_shadowExclusionRadiiLocation = -1;
  m_transformLocation = -1;
}

void RectProgram::abandon() noexcept { m_program.abandon(); m_backdrops={}; }

void RectProgram::draw(
    float surfaceWidth, float surfaceHeight, float width, float height, const RoundedRectStyle& style,
    const Mat3& transform
) const {
  if (!m_program.isValid() || width <= 0.0F || height <= 0.0F) {
    return;
  }

  const std::array<GLfloat, 12> vertices = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  const float materialPadding=style.material && style.material->primitive==noctalia::material::Primitive::Plateau
      ? noctalia::material::samplingPadding(*style.material) : 0.0F;
  const float padding = std::max({style.borderWidth + style.softness + 2.0F, 2.0F, materialPadding});
  const float quadWidth = width + padding * 2.0F;
  const float quadHeight = height + padding * 2.0F;
  const float rectOrigin = padding;
  const Mat3 quadTransform = transform * Mat3::translation(-padding, -padding);

  auto material = noctalia::material::fitToBounds(
      style.material.value_or(noctalia::material::Parameters{}),
      width - style.logicalInset.left - style.logicalInset.right,
      height - style.logicalInset.top - style.logicalInset.bottom);
  // Rotate the global light into local coordinates without letting node scale
  // change the physical slope. Sampling offsets retain the full transform below.
  const float scaleX = std::hypot(transform.m[0], transform.m[1]);
  const float scaleY = std::hypot(transform.m[3], transform.m[4]);
  if (scaleX > 0.0001F && scaleY > 0.0001F) {
    const auto direction = material.lighting.direction;
    material.lighting.direction.x = (direction.x * transform.m[0] + direction.y * transform.m[1]) / scaleX;
    material.lighting.direction.y = (direction.x * transform.m[3] + direction.y * transform.m[4]) / scaleY;
  }
  const auto materialUniforms = noctalia::material::uniforms(material);
  bool glass = style.material && material.primitive == noctalia::material::Primitive::Optical
      && style.materialBackdrop == MaterialBackdrop::Local
      && noctalia::material::ownsOpticalPlane(material,true) && !style.outerShadow && style.fill.a > 0.0F;
  GLint activeTexture=0, previousTexture=0;
  const GLboolean blending=glIsEnabled(GL_BLEND);
  glUseProgram(m_program.id());
  if (glass) {
    const std::array<Vec2,4> points={transform.transformPoint(0,0),transform.transformPoint(width,0),
        transform.transformPoint(0,height),transform.transformPoint(width,height)};
    float left=points[0].x,right=left,top=points[0].y,bottom=top;
    for (const auto& point:points) { left=std::min(left,point.x);right=std::max(right,point.x);top=std::min(top,point.y);bottom=std::max(bottom,point.y); }
    GLint viewport[4];glGetIntegerv(GL_VIEWPORT,viewport);
    const float deviceScaleX = static_cast<float>(viewport[2]) / surfaceWidth;
    const float deviceScaleY = static_cast<float>(viewport[3]) / surfaceHeight;
    left *= deviceScaleX; right *= deviceScaleX;
    top *= deviceScaleY; bottom *= deviceScaleY;
    const int samplePadding = static_cast<int>(std::ceil(noctalia::material::samplingPadding(material)
        * std::max(scaleX * deviceScaleX, scaleY * deviceScaleY)));
    const int x=std::max(0,static_cast<int>(std::floor(left))-samplePadding);
    const int y=std::max(0,viewport[3]-static_cast<int>(std::ceil(bottom))-samplePadding);
    const int w=std::min(viewport[2],static_cast<int>(std::ceil(right))+samplePadding)-x;
    const int h=std::min(viewport[3],viewport[3]-static_cast<int>(std::floor(top))+samplePadding)-y;
    glass=w>0 && h>0;
    if (glass) {
      auto& backdrop=m_backdrops[w>h*3?0:h>w*3?1:2];
      glGetIntegerv(GL_ACTIVE_TEXTURE,&activeTexture);glActiveTexture(GL_TEXTURE0);
      glGetIntegerv(GL_TEXTURE_BINDING_2D,&previousTexture);
      if (!backdrop.texture) glGenTextures(1,&backdrop.texture);
      glBindTexture(GL_TEXTURE_2D,backdrop.texture);
      if (w>backdrop.width || h>backdrop.height) {
        backdrop.width=std::max(backdrop.width,(w+63)/64*64);backdrop.height=std::max(backdrop.height,(h+63)/64*64);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,backdrop.width,backdrop.height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
      }
      glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,x,y,w,h);
      glUniform1i(m_backdropLocation,0);
      glUniform4f(m_copyBoundsLocation,x,y,w,h);glUniform2f(m_backdropSizeLocation,backdrop.width,backdrop.height);
      glUniform4f(m_displacementAxesLocation,transform.m[0] * deviceScaleX,transform.m[3] * deviceScaleX,
                  -transform.m[1] * deviceScaleY,-transform.m[4] * deviceScaleY);
      glDisable(GL_BLEND);
    }
  }
  glUniform1i(m_glassLocation,glass?1:0);
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_quadSizeLocation, quadWidth, quadHeight);
  glUniform2f(m_rectOriginLocation, rectOrigin, rectOrigin);
  glUniform2f(m_rectSizeLocation, width, height);
  const auto paintClip = style.paintClip.value_or(RoundedPaintClip{0,0,-1,-1,0});
  glUniform4f(m_paintClipLocation, paintClip.x, paintClip.y, paintClip.width, paintClip.height);
  glUniform1f(m_paintClipRadiusLocation, std::max(0.0F, std::min(paintClip.radius, std::min(paintClip.width, paintClip.height)*0.5F)));
  glUniform4f(m_colorLocation, style.fill.r, style.fill.g, style.fill.b, style.fill.a);
  glUniform4f(m_borderColorLocation, style.border.r, style.border.g, style.border.b, style.border.a);
  int fillMode = 0;
  if (style.fillMode == FillMode::Solid) {
    fillMode = 1;
  } else if (style.fillMode == FillMode::LinearGradient) {
    fillMode = 2;
  }
  glUniform1i(m_fillModeLocation, fillMode);
  glUniform2f(
      m_gradientDirectionLocation, style.gradientDirection == GradientDirection::Horizontal ? 1.0F : 0.0F,
      style.gradientDirection == GradientDirection::Vertical ? 1.0F : 0.0F
  );
  const auto& stop0 = style.gradientStops[0];
  const auto& stop1 = style.gradientStops[1];
  const auto& stop2 = style.gradientStops[2];
  const auto& stop3 = style.gradientStops[3];
  glUniform4f(m_gradientStopsLocation, stop0.position, stop1.position, stop2.position, stop3.position);
  glUniform4f(m_gradientColor0Location, stop0.color.r, stop0.color.g, stop0.color.b, stop0.color.a);
  glUniform4f(m_gradientColor1Location, stop1.color.r, stop1.color.g, stop1.color.b, stop1.color.a);
  glUniform4f(m_gradientColor2Location, stop2.color.r, stop2.color.g, stop2.color.b, stop2.color.a);
  glUniform4f(m_gradientColor3Location, stop3.color.r, stop3.color.g, stop3.color.b, stop3.color.a);
  const auto cornerShapeValue = [](CornerShape shape) { return shape == CornerShape::Concave ? 1.0F : 0.0F; };
  glUniform4f(
      m_cornerShapesLocation, cornerShapeValue(style.corners.tl), cornerShapeValue(style.corners.tr),
      cornerShapeValue(style.corners.br), cornerShapeValue(style.corners.bl)
  );
  glUniform4f(
      m_logicalInsetLocation, style.logicalInset.left, style.logicalInset.top, style.logicalInset.right,
      style.logicalInset.bottom
  );
  const float power = std::clamp(style.cornerPower.value_or(2.0F),2.0F,10.0F);
  glUniform1f(m_cornerPowerLocation,power);
  glUniform1f(m_paintClipPowerLocation,std::clamp(paintClip.cornerPower.value_or(power),2.0F,10.0F));
  glUniform1f(m_shadowExclusionPowerLocation,std::clamp(style.shadowExclusionPower.value_or(power),2.0F,10.0F));
  glUniform4f(m_radiiLocation, style.radius.tl, style.radius.tr, style.radius.br, style.radius.bl);
  glUniform1f(m_softnessLocation, style.softness);
  glUniform1i(m_noAaLocation, style.noAa ? 1 : 0);
  glUniform1i(m_invertFillLocation, style.invertFill ? 1 : 0);
  glUniform1i(m_frameEnabledLocation, style.frameContour ? 1 : 0);
  glUniform1i(m_segmentKindLocation, static_cast<GLint>(style.segmentContour.kind));
  glUniform1f(m_segmentDepthLocation, segment_contour::clampedDepth(
      style.segmentContour.vertical ? height : width, style.segmentContour.vertical ? width : height,
      style.segmentContour.depth));
  glUniform1i(m_segmentVerticalLocation, style.segmentContour.vertical ? 1 : 0);
  glUniform1i(m_frameChamferedLocation, style.frameContour && style.frameContour->chamfered ? 1 : 0);
  if (style.frameContour) {
    glUniform4fv(m_frameChamfersLocation,1,style.frameContour->chamfers.data());
    std::array<float,48> rects{}, shapes{};
    for (std::size_t i=0; i<12; ++i) {
      const auto& shelf=style.frameContour->shelves[i];
      rects[i*4]=shelf.x; rects[i*4+1]=shelf.y;
      rects[i*4+2]=shelf.width; rects[i*4+3]=shelf.height;
      shapes[i*4]=shelf.radius; shapes[i*4+1]=shelf.shoulder;
    }
    glUniform4fv(m_frameShelfRectsLocation,12,rects.data());
    glUniform4fv(m_frameShelfShapesLocation,12,shapes.data());
  }
  std::array<float,12> borderColors{}, borderShapes{};
  for (std::size_t i=0;i<style.contourBorderLayers.size();++i) {
    const auto& layer=style.contourBorderLayers[i];
    borderColors[i*4]=layer.color.r; borderColors[i*4+1]=layer.color.g;
    borderColors[i*4+2]=layer.color.b; borderColors[i*4+3]=layer.color.a;
    borderShapes[i*4]=std::max(0.0F,layer.width);
    borderShapes[i*4+1]=std::max(0.0F,layer.offset);
    borderShapes[i*4+2]=layer.width>0.0F ? 1.0F : 0.0F;
  }
  glUniform4fv(m_frameBorderColorsLocation,3,borderColors.data());
  glUniform4fv(m_frameBorderShapesLocation,3,borderShapes.data());
  glUniform1f(m_borderWidthLocation, style.borderWidth);
  glUniform1i(m_materialLocation, style.outerShadow ? 0 : materialUniforms.primitive);
  glUniform4fv(m_materialLightLocation, 1, materialUniforms.light.data());
  glUniform4fv(m_materialPlateauLocation, 1, materialUniforms.plateau.data());
  glUniform4fv(m_materialContactLocation, 1, materialUniforms.contact.data());
  glUniform4fv(m_materialPlateauShapeLocation, 1, materialUniforms.plateauShape.data());
  glUniform4fv(m_materialPlateauFaceLocation, 1, materialUniforms.plateauFace.data());
  glUniform4fv(m_materialOpticalLocation, 1, materialUniforms.optical.data());
  glUniform4fv(m_materialOpticalStyleLocation, 1, materialUniforms.opticalStyle.data());
  glUniform4fv(m_materialOpticalLightLocation, 1, materialUniforms.opticalLight.data());
  glUniform4fv(m_materialOpticalColorLocation, 1, materialUniforms.opticalColor.data());
  glUniform4fv(m_materialOpticalLensLocation, 1, materialUniforms.opticalLens.data());
  glUniform4fv(m_materialIllustrationLocation, 1, materialUniforms.illustration.data());
  glUniform4fv(m_materialPaintLocation, 1, materialUniforms.paint.data());
  glUniform1i(m_outerShadowLocation, style.outerShadow ? 1 : 0);
  glUniform2f(m_shadowCutoutOffsetLocation, style.shadowCutoutOffsetX, style.shadowCutoutOffsetY);
  glUniform1i(m_shadowExclusionLocation, style.shadowExclusion ? 1 : 0);
  glUniform2f(m_shadowExclusionOffsetLocation, style.shadowExclusionOffsetX, style.shadowExclusionOffsetY);
  glUniform2f(m_shadowExclusionSizeLocation, style.shadowExclusionWidth, style.shadowExclusionHeight);
  glUniform4f(
      m_shadowExclusionCornerShapesLocation, cornerShapeValue(style.shadowExclusionCorners.tl),
      cornerShapeValue(style.shadowExclusionCorners.tr), cornerShapeValue(style.shadowExclusionCorners.br),
      cornerShapeValue(style.shadowExclusionCorners.bl)
  );
  glUniform4f(
      m_shadowExclusionLogicalInsetLocation, style.shadowExclusionLogicalInset.left,
      style.shadowExclusionLogicalInset.top, style.shadowExclusionLogicalInset.right,
      style.shadowExclusionLogicalInset.bottom
  );
  glUniform4f(
      m_shadowExclusionRadiiLocation, style.shadowExclusionRadius.tl, style.shadowExclusionRadius.tr,
      style.shadowExclusionRadius.br, style.shadowExclusionRadius.bl
  );
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, quadTransform.m.data());
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
  if (glass) {
    glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(previousTexture));glActiveTexture(static_cast<GLenum>(activeTexture));
    if (blending) glEnable(GL_BLEND);
  }
}
