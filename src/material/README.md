# Shared native material core

This standalone C++23 header library contains palette-independent parameters,
ordered leaf inheritance, bounded material geometry and GLES 2 compatible GLSL
helpers. It has no dependency on Noctalia services, a display server, OpenGL
headers, JSON or a preset registry. No function recognizes a factory preset name.

The current schema version is `4`. The four rendering primitives are `Flat = 0`,
`Plateau = 1`, `Optical = 2` and `Illustrated = 3`. A square, border-only desktop
and a rounded flat desktop both use the flat primitive with different geometry,
layout and motion configuration outside this library.

## C++ contract

Include `material/material.h` and `material/shader_source.h`. Resolve `Patch`
objects in global, semantic role, control family, individual surface and state
order. Optional leaf values distinguish inheritance from an explicit zero. A
patch changing elevation does not reset unrelated optical or lighting values.

`resolve(base, patches)` returns sanitized `Parameters`. Before drawing, call
`fitToBounds(parameters, bodyWidth, bodyHeight)` using the drawable body after
logical insets, then `uniforms(fitted)` for the documented `vec4` upload packs.
Parameters contain no palette values. The caller supplies semantic fill, stroke
and foreground colors and decides which surfaces participate in a material.

`fields.def` is the canonical settings registry for float leaves: member path,
storage key, default, minimum, maximum, step, label and group. A settings adapter
can expand it to TOML/JSON mapping, controls and per-surface override controls.
Primitive selection is an explicit enum; seeds come from persistent component
identity and must not be regenerated on repaint, resize or preset changes.

The library validates numerical ranges and rejects non-finite values at the
rendering boundary. Imported profile validation should report rejected values to
the user before this defensive sanitization. Lighting vectors are normalized;
stored slider values need not be normalized before constructing a profile.

`samplingPadding()` gives conservative logical effect bounds, including optical
displacement, chromatic samples and scattering. Transform those bounds to device
coordinates and round outward before allocating textures or computing damage.
Do not bind unbounded imported dimensions directly as allocation sizes.

## Renderer integration

Insert `kMaterialShaderSource` after the shader's version, extension and precision
declarations and before its main function. The fragment source contains only
functions: the caller declares uniform names and owns the sampler, geometry,
coverage, premultiplication and blend mode. Existing shape SDFs, including concave
corners, remain authoritative for hitboxes and clipping.

All material dimensions are logical pixels. Gradients must use central
differences in those coordinates, with division by twice the sample step. Do not
use `dFdx`/`dFdy` as a substitute for the material slope: that makes the lighting
depend on output scale. Lighting direction uses desktop coordinates (right,
down, outward). Transform it into local coordinates for rotated surfaces.

### Plateau

Use `materialPlateauShade(fill.rgb, distance, distanceGradient, light, plateau)`.
The quintic height ramp reaches a constant central height with zero first and
second derivatives at each seam. `fitToBounds()` reserves the configured flat
face fraction and reduces excessive elevation for tiny controls. A negative
elevation gives an inset plateau; zero elevation is level. Animate elevation
when motion is allowed, keeping layout and input geometry fixed.

Keep the palette fill opaque for opaque material surfaces. The shader preserves
the flat face's palette color, lighting only the shoulder relative to the face.
Backdrop opacity is not needed to create depth. On a background receiving a
paired shadow treatment, use `materialPlateauShadowOffset()`, sample the exact shape SDF at both opposed offsets, then use `materialPlateauShadowWeights()` and `materialPlateauShadowPair()`. Nonnegative elevation selects outer shadows; negative elevation selects the inset pair. `materialPlateauFace()` applies the independent signed face gradient. Native rectangles render the pair themselves, so callers must not add a second material shadow node.

### Optical

Calculate central differences of `materialGlassHeight(distance, optical)`, then
call `materialGlassDisplacement(heightGradient, optical)`. Transform that logical
offset to the captured backdrop's coordinates. The maximum displacement is
enforced before optional chromatic scaling. `opticalStyle` contains low-radius
scattering, tint opacity and chromatic separation. The four offsets returned by
`materialGlassScatterOffset()` can be sampled with the center using weights
`0.125, 0.125, 0.125, 0.125, 0.5`; bypass them at zero scattering radius.

Apply the palette tint with its configured coating opacity, then
`materialGlassShade()` to the resulting straight RGB. Keep text and icons in a
separate foreground pass. Apply the original silhouette coverage exactly once,
using backdrop replacement rather than layering a second distorted backdrop
over the existing one. Transform and clamp samples to the actually captured
texture bounds. The helper does not provide cross-process backdrop transport or
authorize sampling behind a locked session; those remain compositor concerns.

Schema 4 adds an independently implemented radial edge lens. `lens_mapping` is
exactly 0 (existing Snell, the default) or 1 (radial contraction). Radial strength
is dimensionless [0,1], default 0.2; `lens_falloff` is [0.1,16], default 1.
`materialGlassLensDisplacement()` accepts the logical vector from the optical
body center plus the optical signed distance. It preserves the original Snell
helper for mode zero, and caps radial displacement with the existing maximum.
Zero edge width, radial strength or displacement produces no radial offset.
Chromatic/scattering capture padding therefore keeps its existing bound.

`refraction_radius` defaults to -1 (follow the original shape); zero selects a
square optical distance field, and positive values are logical pixels up to 512.
The renderer caps the effective radius to half the smaller optical body extent.
Explicit radius controls sampling independently from painted radii and input
geometry. The optional rounded paint mask still limits both optical normals and
final coverage. The material body center accounts for logical concave insets.
No KWin shader implementation was imported; the source comparison and provenance
are in `docs/kwin-glass-reference.md` in the shell repository.

### Illustrated

Use `materialIllustratedPaint()` for the gentle edge glaze and
`materialIllustratedStroke()` for a palette-colored inward outline. The caller
applies the unchanged outer coverage. Stroke variation is a fixed spatial sum of
smooth harmonics in component-local coordinates. It never adds a noise overlay,
changes hit geometry or uses frame time. Clamp thin-control variation with
`fitToBounds()` so the outline cannot disappear or become ragged.

## Standalone package and tests

Configure this directory directly as a Meson project; it must not be added with
`subdir()` to another Meson project because it has its own `project()` declaration.

```sh
meson setup /tmp/noctalia-material-build /path/to/noctalia/src/material
meson compile -C /tmp/noctalia-material-build -j 1
meson test -C /tmp/noctalia-material-build
```

It installs `material/*.h` and `material/fields.def` under
`include/noctalia`, plus the `noctalia-material-core.pc` pkg-config definition.
The shell can include its source headers directly; external consumers use the
package. No compiled material library or shell build is needed. Tests exercise
geometry, numerical derivatives, inheritance, safe fitting, Snell displacement,
bounds and stable seeds. These tests do not constitute visual acceptance.

`tests/rect_material_gl_test.cpp` is a separate integration harness using the
actual native `RectProgram` and `ShaderProgram` sources. It requires EGL/GLES
headers and libraries and is intentionally excluded from the dependency-free
package target. Run it on a surfaceless GLES context with `LP_NUM_THREADS=1` and
low scheduling priority. It compiles the production shader, renders all four
primitives over light/dark backgrounds, includes raised/inset plateau states,
and saves an unmodified 512 by 320 PPM capture. Its sharp foreground marks are
drawn after the optical pass. Successful compilation/readback is only a smoke
check; the resulting capture requires direct visual review and cannot stand in
for full shell surface/state inspection.

The native semantic adapter exposes `Flex::setMaterialIdentity()` and
`Box::setMaterialIdentity()` for role/family/surface overrides. Direct control
nodes use `SurfaceMaterial::styled()`. `RoundedRectStyle::material` is an explicit
opt-in; decorative rectangles have no material. `MaterialBackdrop::Inherited`
contributes only a thin tint to a parent's optical plane, while `Local` explicitly
enables bounded framebuffer refraction. External desktop backdrop sampling is
still owned by the compositor, not by individual child controls.

Scene owners set `Node::setMaterialSurface("bar")` (or another registered surface
ID) before attaching their content. An empty identity inherits the nearest
ancestor. Semantic rectangle resolver callbacks reapply the scoped parameters
when a subtree is attached, reparented, inserted later or its scope changes.
Explicit nested scopes survive parent scope changes. `Flex`/`Box` also refresh
their receiver shadows when this context changes. This mechanism does not cross
independent Wayland popup trees: popup owners must transfer the opener's scope.

`tests/scene_material_scope_test.cpp` exercises those contracts against the real
Node and RectNode implementation, including stable illustration identity after
reparenting. It is a scene integration test, separate from the dependency-free
material-core test. Compile it with `node.cpp`, the animation/motion sources and
`core/ui_phase.cpp`; release assertions can be disabled without disabling its
explicit test checks.

## Provenance

The optical height, refraction and lighting helpers adapt the MIT-licensed
`ryohsuke1231/liquid-glass` GNOME extension at commit
`af41a303002c69e2f2a3dcd5405fb6903b2430f8`:

<https://github.com/ryohsuke1231/liquid-glass/blob/af41a303002c69e2f2a3dcd5405fb6903b2430f8/liquid-glass%40thinkingcoding1231.gmail.com/shaders/glass.frag>

The initial native port is retained in Infinite Desktop's
`plugin/surface_optics.glsl.hpp`; this module generalizes its fixed parameters,
keeps explicit bounds, and preserves attribution in `LIQUID-GLASS-LICENSE`.
The rest of this module follows the Noctalia repository's MIT license, included
as `LICENSE` in the standalone package.

## Atomic compositor scene transport (v2)

`protocol/noctalia-material-v1.xml` binds one material object to the same client's
`wl_surface`. `scene_descriptor.h` defines the byte codec. The wire header is
little-endian magic `0x4d53434e`, version 5, float32 surface width/height and uint32
plane count. Each plane encodes its group ID, length-prefixed role and surface ID,
float32 local width/height, six affine coefficients, four clipping coordinates,
a uint32 optional-mask flag followed by six local floats (x,y,width,height,radius,power)
when present, plane corner power, four radii, four insets, concave-corner bitmask, straight RGBA tint, opacity,
primitive, the 41 scalar leaves in `fields.def` order, then illustration seed.
A wire layout change requires a protocol/codec version change; never append fields
silently. Payloads are at most 64 KiB and 64 planes, with identifiers at most 64
ASCII characters. NaN, infinities, invalid enums, invalid ranges, degenerate
transforms, duplicate groups, truncated data and trailing bytes are rejected.

Clients send logical surface coordinates after retained scene transforms, before
output scale, output orientation or canvas zoom. The server validates pending
state immediately and latches the last valid pending scene on `wl_surface.commit`.
An empty scene clears optics. Rejection preserves the last valid state. Resource
ownership follows the Wayland surface; no PID, app-title or filesystem identity
is used for transport. Surface destruction clears the scene, and a remaining
material object is inert until destroyed. Protocol removal destroys all resources.
Version 1 protocol bindings use only the inline request, whose payload is limited
to 4,080 bytes. Version 2 bindings upload larger scenes through contiguous chunks
of at most 3,072 bytes, with one bounded 64 KiB assembly per surface. A malformed,
replaced or incomplete upload cannot become current. Protocol version does not
negotiate the descriptor codec: the v1 large-scene empty fallback is understood
only by a compositor that already supports descriptor v5.

Noctalia's `MaterialSceneSender` sends before EGL presentation. Optical plane ownership resolves from the semantic default or explicit `optical_plane`.
Outer planes group ordinary nested controls; contained sibling planes also inherit the
outer group. Explicit independent controls remain separate planes and retain paint order. Separate cards remain separate planes. Native `MaterialBackdrop::Local`
regions render only their own framebuffer (for example, a lock panel over its own
wallpaper) and never ask the compositor for a desktop backdrop.

The compositor samples the framebuffer *before* the client foreground is drawn.
It uses the same `shape_source.h` convex/concave silhouette and
`shader_source.h` optical height, Snell refraction, bounded chromatic separation,
low scattering and directional highlights as the native renderer. Logical effect
padding is transformed outward to device pixels; textures are bounded by the
active viewport. Palette tint is applied by the native foreground exactly once,
so the compositor adds no opaque coating. Layer and window pass paths retain
session-lock and snapshot exclusions. The legacy background-effect region is a
compatibility fallback only for clients without a committed scene descriptor.

The protocol keeps rectangular ancestor scissors in surface coordinates and adds one optional rounded local paint mask per plane. Arbitrary stencil paths remain unsupported.
Per-corner convex/concave geometry shares configurable corner power; compositor window masks also
preserve the window's existing superellipse rounding power. Standalone external
greeter/OSK clients need their own root identity and local-backdrop integration;
this protocol does not change authentication or permission boundaries. Codec and
shader checks are implementation evidence, not acceptance of a running desktop.


## Four configurable Neumorphism shapes

Flat, Concave, Convex, and Pressed are recipes over generic scalar fields. Flat uses face curvature zero and outer paired shadows. Concave uses negative face curvature; Convex uses positive curvature; both retain outer shadows. Pressed uses negative elevation and zero face curvature, which produces an inset pair. The default plateau recipe retains its broad quintic shoulders and a flat central face.

Distance, blur, color intensity, pair opacity, face highlight, face shadow, and face curvature are independent. Radius remains normal scene geometry. Shadow distance denotes the largest x/y offset component, so diagonal light yields `(distance,distance)` rather than scaling it by normalized vector length. Blur may be zero; signed elevation remains meaningful with shoulder width zero. Press interaction cannot invert an already recessed recipe outward.

Color gain and shape semantics follow Adam Giebl's generator at commit `8ab93a6bea6493cc464d45a334b5fb7a90d34308`, specifically [Configuration.js](https://github.com/adamgiebl/neumorphism/blob/8ab93a6bea6493cc464d45a334b5fb7a90d34308/src/Configuration.js) and [utils.js](https://github.com/adamgiebl/neumorphism/blob/8ab93a6bea6493cc464d45a334b5fb7a90d34308/src/utils.js). The BSD-3-Clause license is retained in `NEUMORPHISM-LICENSE`. Gain multiplies sRGB channels and clamps endpoints before interpolation; it does not choose a palette. Pure black remains black under this exact gain model. Our continuously directed face gradient follows the global light; it does not hardcode the generator's four CSS angles. Soft shadows use analytic Gaussian edge convolution over the shape SDF, rather than a pixel-exact browser box-shadow rasterizer.

`tests/neumorphic_shapes_gl_test.cpp` renders the actual native RectProgram in a bounded 512×384 EGL framebuffer and checks gradient direction, a flat central face, and opposed inner/outer shadow pixels. Columns are Flat, Concave, Convex, Pressed; rows are light material without shoulders, light plateau shoulders, and dark material without shoulders. This is primitive evidence; final desktop scene/layout/surface-bleed inspection remains separate.

### Native content clipping

An owned background may bypass its immediate container's paint clip; ancestor clips and hit testing still apply. Scrolled content does not receive that exemption. Notification material bounds reform with reveal geometry while foreground content retains its viewport clip.

RoundedPaintClip is a local logical-pixel mask on an individual native rectangle. It travels with independent optical planes in the v5 descriptor. The native shader and compositor intersect the optional optical-radius field with that mask for lens normals, while final coverage retains the painted silhouette and mask, including fractional scaling. Ordinary Sweep indicators inherit their parent plane; an explicit independent mode keeps their rounded track mask in the compositor as well.

### Optical ownership and appearance

`optical_plane` accepts exactly -1 (semantic default), 0 (inherit), or 1 (independent). Controls default to inheritance, while outer surfaces and overlays own a plane. Global, role, family and surface settings can override that choice. Explicit independent descendants survive containment grouping. The local-only policy on login/lock surface roots makes independent descendants sample only that surface's own framebuffer; those scenes are never sent to the desktop compositor.

Backdrop saturation mixes sampled RGB with Rec.709-weighted luminance, contrast scales about 0.5, and brightness adds an offset, followed by clamping. These adjustments happen before palette tint and foreground. They do not alter theme colors or text and add no texture samples. Rim width -1 retains the edge-relative default; zero disables the rim band, and positive values use logical pixels. Rim directional falloff zero produces uniform directional strength. Specular and sheen remain separate controls.


## Interface corner geometry (payload v5)

`shell.design.corner_power` is a dimensionless geometry token, separate from the
41 material fields and independent of radius. Two retains circular corners;
four selects a superellipse and values up to ten retain more of the corner square.
A zero radius stays square. Intentional circles opt into power two on their node.
Node geometry inherits through the actual scene tree; changing the shared default
invalidates retained paint without changing control hitboxes or layout. Scene-thread
root registration uses identities to tolerate destruction/recreation during invalidation.

Native rectangle fill, borders, shadow exclusions, relief and optical coverage,
image crops, CPU surface regions, and external optical descriptors carry the same
resolved geometry. A rounded paint mask carries independent power. Explicit lens
radius remains a sampling option and cannot change foreground/mask coverage.

Payload v5 adds plane and mask geometry power. The Wayland interface is v2;
matching material-core 5.0.0 and compositor packages are required. Older payloads
are rejected atomically rather than reinterpreted. No mixed-version negotiation
is provided. Publish matching packages into a fresh compositor session.

Powered convex distances use gradient normalization near the contour to preserve
logical bevel widths; concave boundaries retain the existing composed-edge field.
CPU regions remain pixel-strip approximations. Actual GPU, retained-scene and VM
visual checks must pass before appearance acceptance; source tests alone do not
establish a visual match to an external desktop.
