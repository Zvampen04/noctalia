# Glass source comparison

Material slider previews are numeric updates, not shader compilation. The settings
slider retains the latest value per 16 ms preview interval and flushes the final
value on release. `config/material_preview.cpp` validates scalar material edits
through the existing shell schema and updates the in-memory profile without file
reads or plugin discovery. Save, Cancel, imports and structural changes retain
the normal profile transaction path. Curve editors use the same latest-value
queue and flush the restored curve on Escape. Native animation curves and
registered plugin curve fields also preview in memory, with their existing
schema validation. Plugin settings alone do not rescan the plugin registry;
source or enablement changes still do.

`compositors/hyprland/material_bridge.cpp` sends material changes directly to
Infinite Desktop's existing settings API. Its worker coalesces pending updates
and bounds socket waits; neither the UI thread nor a one-second polling loop
owns delivery. Shader identities remain unchanged while parameters change.

Reviewed the source of [Shoji liquid glass](https://github.com/bea4dev/liquid-glass-config-shojiwm/blob/main/src/liquid-glass.frag)
and the [GNOME Liquid Glass extension](https://github.com/ryohsuke1231/liquid-glass).
The GNOME effect is an extension, not a built-in GNOME compositor effect.

Shoji combines rounded-rectangle signed distance, radial contraction, separate
RGB texture samples and tint. The radial mapping now uses its circular-sag principle and normalized radial
direction scaled by body half extents, implemented independently. Noctalia retains independent
optical radius, edge width, strength, falloff, chromatic separation and tint.

The GNOME extension uses a superellipse height, finite-difference normals and
Snell refraction, with blur, RGB separation and edge lighting. The existing
Noctalia Snell implementation retains its MIT attribution in
`src/material/LIQUID-GLASS-LICENSE`.

The shared radial profile previously used `sin(pi/2 * pow(proximity, falloff))`.
For falloff below one its derivative diverges at the flat-face transition,
producing an abrupt inset boundary. A rational bias and quintic shoulder now feed a circular-sag lens profile. A
small outer-rim regularizer keeps its derivative finite; the shoulder gives zero
first and second derivatives at both endpoints. Separate red and blue samples
follow the radial refraction with configurable separation, fading to zero in
the clear face to avoid a chromatic seam.
A smooth fourth-norm displacement limiter also removes the hard clamp kink.
Neither change adds texture samples or a new render pass. Foreground text and
icons remain separate from backdrop distortion.

The same material-core GLSL is consumed by Noctalia rectangles and Infinite
Desktop terminal backdrops. Updating a resident compositor requires a new
session; a staged build does not change the currently running shader.
