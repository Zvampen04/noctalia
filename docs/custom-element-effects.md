# Custom element background effects

Advanced appearance settings support native material presets and imported custom background effects. A custom effect changes only the background plane of its selected target. Noctalia still owns the rounded shape, clipping, border, opacity, hit area, and foreground content.

Custom effects use ABI version 1. The imported file must contain one function:

```glsl
vec4 noctalia_effect(
    vec4 source_pm, vec4 backdrop_pm,
    vec2 local_uv, vec2 local_px, vec2 size_px,
    vec4 p0, vec4 p1, vec4 p2, vec4 p3,
    vec4 p4, vec4 p5, vec4 p6, vec4 p7);
```

The file may call `noctalia_sample_source(offset_local_px)` and `noctalia_sample_backdrop(offset_local_px)`. With Noctalia's native renderer, source sampling reads the canonical fill or gradient and backdrop sampling reads only earlier pixels in the same Noctalia framebuffer. That path cannot read the desktop behind a Wayland surface.

With a version 4 material-protocol compositor, a ready effect and an exact armed scene may instead render the background body from an immutable compositor snapshot. Version 5 adds a continuous lease: after one exact initial arm, geometry, opacity, tint, material fields, sampling radius, and parameters can follow each matching surface commit without another round trip. The stable target and compiled-resource signature cannot change under that lease. A target or resource change atomically restores the native body with a token-zero scene before a new lease is requested. Live zero-opacity custom targets remain eligible so popup entrances can prewarm without repeatedly changing owners.

In the compositor path backdrop sampling can read the desktop snapshot, while source sampling currently receives the plane's resolved solid tint because the scene descriptor does not carry gradient stops. The handoff replaces only the target's background body. Noctalia continues to render its border, contour, icons, text, other child content, and input area. Version 1–3 peers keep the native body; version 4 retains the exact-descriptor arm behavior.

The host clamps sampling to the imported asset's declared radius and to 256 pixels. It also owns the textures and their uniforms. Imported files cannot declare `main`, preprocessor directives, uniforms, samplers, attributes, varyings, loops, discard, texture calls, or `gl_` and host-owned identifiers. Source must be printable ASCII and no larger than 32 KiB. The eight parameter vectors contain 32 finite values exposed by Advanced settings; each effect documents how it uses them.

Import copies a user-owned regular file that is not writable by other users into Noctalia's private content-addressed XDG data directory and records its SHA-256 digest. Loading rechecks the digest and file permissions. Replacing an effect preserves the target's stable effect ID while changing its content digest. If the replacement passes source validation but fails the GPU compiler, the live renderer keeps the previous compiled program and reports both the requested and active digest. Current bounded parameters remain live; the sampling radius is capped by the previous program's declared radius. A renderer context loss, process restart, last-consumer eviction, or changing the stable effect ID clears that in-memory program, so the same compile failure then uses the target's resolved native material preset. Missing, changed, or source-invalid imported bytes retain the asset service's live last-known-good binding when one still exists. Reset removes the custom binding and restores normal material inheritance.

Custom effects are portable as sealed profile resources. The guarded resource request carries the source bytes, size, ABI, digest, and sampling limit; it never carries the original local path. Noctalia validates the complete manifest before installing the bytes into its private digest-addressed store. The bounded parameters travel in the ordinary appearance config. Profiles refer to a stable asset ID and digest. Selecting a Basic theme supplies defaults; target-specific Advanced overrides remain explicit and can be reset independently.

Individual targets currently include bar widget placements, the supported popup background classes, Compact Home Wi-Fi, Bluetooth, display and sound cards, and persistent Home/Compact shortcut tiles. Shortcut IDs are storage-only and survive reorder; Advanced settings show the translated control label instead. Hidden or unavailable controls do not enter a rendered scene and therefore do not retain a compiled effect. Dynamic Audio-tab device and application rows and arbitrary nested plugin controls currently inherit their role, family, and containing surface rather than exposing individual targets.
