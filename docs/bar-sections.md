# Separate bar section surfaces

`bar.section_backgrounds = true` replaces the continuous background with up to three content-sized surfaces for the existing start, center and end lanes. Each surface uses the ordinary `bar` material family, palette, border, opacity and corner settings. Widget selection and grouping remain configurable; explicit widget capsule overrides are retained.

Named `[[bar.<name>.section]]` entries keep their explicit `anchor`,
`alignment`, and `offset` when `layout_role = "free"` (the default). Set
`layout_role` to `start`, `center`, or `end` to opt that section into the
bar-wide `center_alignment` and `edge_cluster_policy` controls. Several named
sections may participate in one lane; their configured order and
`widget_spacing` gap are retained. This keeps existing free-form layouts
backward compatible while allowing stable section IDs to use the same
start/middle/end and edge/follow/equidistant recipes as legacy lanes. Main-axis
offset remains a per-section adjustment after the recipe; `cross_offset` is
independent and works identically on all four bar edges.

The lane backgrounds and shadows are retained scene nodes. Layout derives their length from visible content clipped to the allocated lane, adds configured bar padding and keeps the configured widget spacing between neighbours. Empty or fully clipped lanes do not paint. Horizontal and vertical bars use the same geometry, so top, bottom, left and right placement retain their existing layer-shell anchoring and shadow bleed.

Blur and pointer regions use the painted rounded lane shapes. Visible gaps pass pointer input through. An auto-hidden bar retains its existing narrow edge trigger so it can still be revealed. Live layout and slide updates refresh these regions; ordinary widget hover/focus and explicit capsules remain available. Material shadow bleed and attached-panel shadow exclusion are preserved for each lane.

`bar_section_geometry_test` covers content sizing, signed clipping at output boundaries, hidden/empty lanes, narrow layouts, zero padding/gap, missing center lanes and horizontal/vertical coordinates. This CPU test and syntax validation do not approve appearance. Native captures must still inspect all four orientations, live widget changes, each material, shadow clipping, hover hit targets, fractional scaling, auto-hide and attached panels.
