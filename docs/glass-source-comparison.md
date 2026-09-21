# Glass source comparison

Reviewed the source of [Shoji liquid glass](https://github.com/bea4dev/liquid-glass-config-shojiwm/blob/main/src/liquid-glass.frag)
and the [GNOME Liquid Glass extension](https://github.com/ryohsuke1231/liquid-glass).
The GNOME effect is an extension, not a built-in GNOME compositor effect.

Shoji combines rounded-rectangle signed distance, radial contraction, separate
RGB texture samples and tint. Its circular distortion profile and chromatic
mask are useful references, but are not copied here. Noctalia retains independent
optical radius, edge width, strength, falloff, chromatic separation and tint.

The GNOME extension uses a superellipse height, finite-difference normals and
Snell refraction, with blur, RGB separation and edge lighting. The existing
Noctalia Snell implementation retains its MIT attribution in
`src/material/LIQUID-GLASS-LICENSE`.

The shared radial profile previously used `sin(pi/2 * pow(proximity, falloff))`.
For falloff below one its derivative diverges at the flat-face transition,
producing an abrupt inset boundary. A rational bias followed by quintic
smoothing now gives zero first and second derivatives at both endpoints.
A smooth fourth-norm displacement limiter also removes the hard clamp kink.
Neither change adds texture samples or a new render pass. Foreground text and
icons remain separate from backdrop distortion.

The same material-core GLSL is consumed by Noctalia rectangles and Infinite
Desktop terminal backdrops. Updating a resident compositor requires a new
session; a staged build does not change the currently running shader.
