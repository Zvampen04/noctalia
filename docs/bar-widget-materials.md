# Native material targets for bar elements

Bar element effects use Noctalia's native material primitives and the paired material-scene protocol. They do not execute arbitrary user GLSL. A whole-surface compositor shader cannot address sibling widgets after they have been drawn into the same Wayland surface.

Material resolution is ordered from broad to specific:

1. semantic role and family;
2. the global `bar` surface;
3. `bar.instance.<bar-name>`;
4. `bar.section.<bar-name>.<section-id>`;
5. `bar.widget.<persistent-placement-id>`.

Every level inherits the previous result. An Advanced override changes only the fields it owns, and deleting that target restores inheritance. Each lane placement owns a persistent ID separate from its widget config/type. Lanes store an `@widget:<placement-id>` token whose placement record names the widget config. Reordering or moving the token retains the ID; duplicating it creates a new UUID, so two siblings of the same type can resolve different materials. The token is a storage detail: editors, logs, hover summaries, action routing, widget registries, service capability checks, media deduplication, and activity targeting resolve it back to the actual module name.

Token-looking text is interpreted as a placement only when a matching placement record exists. A pre-existing legal module named `@widget:...` remains a literal name. Legacy string-only lane entries receive deterministic, collision-checked IDs in an in-memory migration plan, but reading a config never rewrites it. The IDs and placement records materialize only during an authorized settings save. Repeated load/save is idempotent, and array position is never the stored identity after that first save.

The supported effects are the existing native primitives: flat, neumorphic plateau, optical liquid-glass material, and illustrated material, with the validated material parameter set. Optical naming is reserved for the real refraction/scattering path. A tint-only surface must remain described as tint or rim.

Advanced bar editing selects or resets the primitive for each stable placement. Once selected, the same `bar.widget.<id>` target appears under Appearance → Material overrides, where the existing bounded controls adjust refraction strength, chromatic separation, scattering, tint, rim, specular, sheen, relief, and illustration parameters independently. Each row's Reset action removes that field and reveals the inherited section/bar/theme value; choosing Inherit on the placement removes its primitive override.

Each placement also has optional text and icon foreground overrides. They inherit the widget and bar colors by default, so duplicates sharing one widget configuration can retain readable contrast when their independent materials differ.

Element backgrounds remain decorative scene nodes behind content. Their input area stays on the existing widget wrapper, foreground content paints after the material plane, and the material uses the same rounded capsule geometry through hover and layout animation. Disabling or resetting an element effect removes its independent material plane so it stops contributing backdrop work.
