#pragma once

// Native shell and compositor share the exact convex/concave silhouette.
namespace noctalia::material {
inline constexpr char kShapeShaderSource[] = R"shape(
float rounded_rect_distance(vec2 point, vec2 size, vec4 radii) {
    vec2 half_size = size * 0.5;
    vec2 centered = point - half_size;
    float r = centered.x < 0.0
        ? (centered.y < 0.0 ? radii.x : radii.w)
        : (centered.y < 0.0 ? radii.y : radii.z);
    vec2 q = abs(centered) - (half_size - vec2(r));
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Window corner metadata may use a superellipse. Keep the circular path
// bit-for-bit unchanged at power two. For higher powers this is an Lp
// proximity field with the correct contour, not exact Euclidean distance.
float powered_rounded_rect_distance(vec2 point, vec2 size, float radius, float power) {
    float r = clamp(radius, 0.0, max(0.0, min(size.x, size.y) * 0.5));
    if (power <= 2.0) return rounded_rect_distance(point, size, vec4(r));
    vec2 q = abs(point - size * 0.5) - (size * 0.5 - vec2(r));
    vec2 extent = max(q, vec2(0.0));
    float largest = max(extent.x, extent.y);
    // Normalize before exponentiation so large windows/high powers cannot
    // overflow. All powered operands are in [0,1].
    vec2 unit = extent / max(largest, 0.000001);
    float norm = largest * pow(pow(unit.x, power) + pow(unit.y, power), 1.0 / power);
    return norm + min(max(q.x, q.y), 0.0) - r;
}

// First-order Euclidean distance near an Lp corner. The normalization keeps
// logical bevel/border widths stable as power changes; power two is exact.
float corner_norm_distance(vec2 value, float radius, float power) {
    if (power <= 2.0) return length(value) - radius;
    vec2 a = abs(value);
    float largest = max(a.x, a.y);
    if (largest <= 0.000001) return -radius;
    vec2 u = a / largest;
    float n = pow(pow(u.x,power)+pow(u.y,power),1.0/power);
    vec2 gradient = pow(u,vec2(power-1.0)) / pow(n,power-1.0);
    return (largest*n-radius) / max(length(gradient),0.000001);
}
float rounded_rect_distance(vec2 point, vec2 size, vec4 radii, float power) {
    if (power <= 2.0) return rounded_rect_distance(point,size,radii);
    vec2 centered=point-size*0.5;
    float r=centered.x<0.0 ? (centered.y<0.0?radii.x:radii.w) : (centered.y<0.0?radii.y:radii.z);
    if (r <= 0.0) return rounded_rect_distance(point,size,vec4(0.0));
    vec2 q=abs(centered)-(size*0.5-vec2(r));
    return corner_norm_distance(max(q,0.0),r,power)+min(max(q.x,q.y),0.0);
}
float circle_extent(float radius, float delta, float power) {
    if (power <= 2.0) return sqrt(max(0.0,radius*radius-delta*delta));
    if (radius <= 0.0) return 0.0;
    float d=clamp(abs(delta)/radius,0.0,1.0);
    return radius*pow(max(0.0,1.0-pow(d,power)),1.0/power);
}

float circle_extent(float radius, float delta) {
    return sqrt(max(0.0, radius * radius - delta * delta));
}

float shape_distance(vec2 point, vec2 size, vec4 radii, vec4 corner_shapes, vec4 logical_inset, float power) {
    vec4 safe_inset = max(logical_inset, vec4(0.0));
    vec2 body_min = min(safe_inset.xy, size);
    vec2 body_max = max(body_min, size - safe_inset.zw);
    vec2 body_size = max(body_max - body_min, vec2(0.0));
    float max_radius = max(min(body_size.x, body_size.y) * 0.5, 0.0);
    vec4 r = clamp(radii, vec4(0.0), vec4(max_radius));

    bool tl_concave = corner_shapes.x > 0.5;
    bool tr_concave = corner_shapes.y > 0.5;
    bool br_concave = corner_shapes.z > 0.5;
    bool bl_concave = corner_shapes.w > 0.5;
    bool any_concave = tl_concave || tr_concave || br_concave || bl_concave;

    if (!any_concave) {
        return rounded_rect_distance(point - body_min, body_size, r, power);
    }

    float x = point.x;
    float y = point.y;
    float left = body_min.x;
    float right = body_max.x;
    float top = body_min.y;
    float bottom = body_max.y;

    float radius = r.x;
    if (radius > 0.0 && y < body_min.y + radius) {
        float sample_y = clamp(y, body_min.y, body_min.y + radius);
        float dy = sample_y - (body_min.y + radius);
        float extent = circle_extent(radius, dy, power);
        if (tl_concave) {
            left = min(left, body_min.x - radius + extent);
        } else {
            left = max(left, body_min.x + radius - extent);
        }
    }
    if (radius > 0.0 && x < body_min.x + radius) {
        float sample_x = clamp(x, body_min.x, body_min.x + radius);
        float dx = sample_x - (body_min.x + radius);
        float extent = circle_extent(radius, dx, power);
        if (tl_concave) {
            top = min(top, body_min.y - radius + extent);
        } else {
            top = max(top, body_min.y + radius - extent);
        }
    }

    radius = r.y;
    if (radius > 0.0 && y < body_min.y + radius) {
        float sample_y = clamp(y, body_min.y, body_min.y + radius);
        float dy = sample_y - (body_min.y + radius);
        float extent = circle_extent(radius, dy, power);
        if (tr_concave) {
            right = max(right, body_max.x + radius - extent);
        } else {
            right = min(right, body_max.x - radius + extent);
        }
    }
    if (radius > 0.0 && x > body_max.x - radius) {
        float sample_x = clamp(x, body_max.x - radius, body_max.x);
        float dx = sample_x - (body_max.x - radius);
        float extent = circle_extent(radius, dx, power);
        if (tr_concave) {
            top = min(top, body_min.y - radius + extent);
        } else {
            top = max(top, body_min.y + radius - extent);
        }
    }

    radius = r.z;
    if (radius > 0.0 && y > body_max.y - radius) {
        float sample_y = clamp(y, body_max.y - radius, body_max.y);
        float dy = sample_y - (body_max.y - radius);
        float extent = circle_extent(radius, dy, power);
        if (br_concave) {
            right = max(right, body_max.x + radius - extent);
        } else {
            right = min(right, body_max.x - radius + extent);
        }
    }
    if (radius > 0.0 && x > body_max.x - radius) {
        float sample_x = clamp(x, body_max.x - radius, body_max.x);
        float dx = sample_x - (body_max.x - radius);
        float extent = circle_extent(radius, dx, power);
        if (br_concave) {
            bottom = max(bottom, body_max.y + radius - extent);
        } else {
            bottom = min(bottom, body_max.y - radius + extent);
        }
    }

    radius = r.w;
    if (radius > 0.0 && y > body_max.y - radius) {
        float sample_y = clamp(y, body_max.y - radius, body_max.y);
        float dy = sample_y - (body_max.y - radius);
        float extent = circle_extent(radius, dy, power);
        if (bl_concave) {
            left = min(left, body_min.x - radius + extent);
        } else {
            left = max(left, body_min.x + radius - extent);
        }
    }
    if (radius > 0.0 && x < body_min.x + radius) {
        float sample_x = clamp(x, body_min.x, body_min.x + radius);
        float dx = sample_x - (body_min.x + radius);
        float extent = circle_extent(radius, dx, power);
        if (bl_concave) {
            bottom = max(bottom, body_max.y + radius - extent);
        } else {
            bottom = min(bottom, body_max.y - radius + extent);
        }
    }

    float boundary_distance = max(max(left - x, x - right), max(top - y, y - bottom));

    // The carved boundary above measures distance along x/y, so on an arc its
    // gradient is not unit length and it creases where the dominant edge term
    // switches; both distort the coverage ramp. A convex corner has one smooth contour;
    // use its exact circular or first-order powered distance near that contour.
    radius = r.x;
    if (!tl_concave && radius > 0.0 && x < body_min.x + radius && y < body_min.y + radius) {
        boundary_distance = max(
            max(x - body_max.x, y - body_max.y),
            corner_norm_distance(point - vec2(body_min.x + radius, body_min.y + radius), radius, power)
        );
    }

    radius = r.y;
    if (!tr_concave && radius > 0.0 && x > body_max.x - radius && y < body_min.y + radius) {
        boundary_distance = max(
            max(body_min.x - x, y - body_max.y),
            corner_norm_distance(point - vec2(body_max.x - radius, body_min.y + radius), radius, power)
        );
    }

    radius = r.z;
    if (!br_concave && radius > 0.0 && x > body_max.x - radius && y > body_max.y - radius) {
        boundary_distance = max(
            max(body_min.x - x, body_min.y - y),
            corner_norm_distance(point - vec2(body_max.x - radius, body_max.y - radius), radius, power)
        );
    }

    radius = r.w;
    if (!bl_concave && radius > 0.0 && x < body_min.x + radius && y > body_max.y - radius) {
        boundary_distance = max(
            max(x - body_max.x, body_min.y - y),
            corner_norm_distance(point - vec2(body_min.x + radius, body_max.y - radius), radius, power)
        );
    }

    float visual_clip = max(max(-point.x, point.x - size.x), max(-point.y, point.y - size.y));
    return max(boundary_distance, visual_clip);
}

// Preserve source compatibility for consumers which explicitly request circles.
float shape_distance(vec2 point, vec2 size, vec4 radii, vec4 corners, vec4 insets) {
    return shape_distance(point,size,radii,corners,insets,2.0);
}
)shape";
}
