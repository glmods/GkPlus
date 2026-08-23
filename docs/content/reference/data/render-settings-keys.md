---
title: "Renderer setting keys"
description: "Every core.render.* key that persists to settings.json, with its type and the two keys that behave differently."
weight: 30
audience: ["player", "mod-author"]
---

The renderer knobs that survive a restart, for players and mod authors. One table in
`src/RenderSettings.cpp` defines them; it is walked document to knobs at the engine's first
intercepted file open, and knobs to document once a frame.

## Key form

A key is `core.render.` followed by the name in the tables below, which is the same spelling the
`render` namespace of the `"gk"` module uses:

| JS member | Settings key |
|---|---|
| `render.msaa` | `core.render.msaa` |
| `render.ao.radius` | `core.render.ao.radius` |
| `render.hdr.enabled` | `core.render.hdr.enabled` |
| `render.bloom.layer(0, …).threshold` | `core.render.bloom_layer.0.threshold` |

A family's own switch is `<family>.enabled`. Bloom layers are flattened to
`bloom_layer.<0\|1\|2>.<field>` rather than stored as an array.

## Defaults

Defaults are not held in this table. Each knob's default is the value the renderer initialises it
to, and the JSDoc on the corresponding `render.*` member in `types/gk.d.ts` states it. A key absent
from `settings.json` leaves the knob at that value.

## Counting the table

Every count below is derivable rather than written down:

```bash
sed -n '/Knobs\[\]/,/^};/p' src/RenderSettings.cpp | grep -c '\.name = '        # 79 keys
sed -n '/Knobs\[\]/,/^};/p' src/RenderSettings.cpp | grep -c '\.sync = false'   # 1
sed -n '/Knobs\[\]/,/^};/p' src/RenderSettings.cpp | grep '\.env = ' | grep -v nullptr   # 4
```

## Keys

### Top level

| Key | Type |
|---|---|
| `specular` | bool |
| `per_pixel_lighting` | bool |
| `stencil_shadow` | bool |
| `msaa` | number |

### `tess`

| Key | Type |
|---|---|
| `tess.enabled` | bool |
| `tess.shadows` | bool |
| `tess.seam_fix` | bool |
| `tess.set` | string |
| `tess.edge_pixels` | number |
| `tess.max` | number |
| `tess.min` | number |
| `tess.pn_strength` | number |
| `tess.pn_flat_threshold` | number |
| `tess.pn_max_offset` | number |
| `tess.shadow_factor` | number |

### `hdr`

| Key | Type |
|---|---|
| `hdr.enabled` | bool |
| `hdr.linear_input` | bool |
| `hdr.tonemap` | string |
| `hdr.exposure` | number |
| `hdr.knee` | number |
| `hdr.white` | number |

### `bloom` and `bloom_layer`

| Key | Type |
|---|---|
| `bloom.enabled` | bool |
| `bloom_layer.0.threshold`, `bloom_layer.1.threshold`, `bloom_layer.2.threshold` | number |
| `bloom_layer.0.knee`, `bloom_layer.1.knee`, `bloom_layer.2.knee` | number |
| `bloom_layer.0.radius`, `bloom_layer.1.radius`, `bloom_layer.2.radius` | number |
| `bloom_layer.0.intensity`, `bloom_layer.1.intensity`, `bloom_layer.2.intensity` | number |
| `bloom_layer.0.blend`, `bloom_layer.1.blend`, `bloom_layer.2.blend` | string |

### `sun_shadow`

| Key | Type |
|---|---|
| `sun_shadow.enabled` | bool |
| `sun_shadow.soft_blur` | bool |
| `sun_shadow.cascades` | number |
| `sun_shadow.soft_taps` | number |
| `sun_shadow.bias` | number |
| `sun_shadow.strength` | number |
| `sun_shadow.extent` | number |
| `sun_shadow.softness` | number |
| `sun_shadow.soft_min` | number |
| `sun_shadow.soft_max` | number |

### `map_shadow`

| Key | Type |
|---|---|
| `map_shadow.enabled` | bool |
| `map_shadow.indirect` | bool |
| `map_shadow.rate` | number |
| `map_shadow.bias` | number |

### `dynamic_shadow`

| Key | Type |
|---|---|
| `dynamic_shadow.enabled` | bool |
| `dynamic_shadow.bias` | number |

### `map_light`

| Key | Type |
|---|---|
| `map_light.enabled` | bool |
| `map_light.all` | bool |
| `map_light.cull` | bool |
| `map_light.gain` | number |

### `local_light`

| Key | Type |
|---|---|
| `local_light.enabled` | bool |
| `local_light.shadows` | bool |
| `local_light.shadow_taps` | number |

### `ao`

| Key | Type |
|---|---|
| `ao.enabled` | bool |
| `ao.map_only` | bool |
| `ao.taps` | number |
| `ao.radius` | number |
| `ao.screen_radius` | number |
| `ao.bias` | number |
| `ao.strength` | number |
| `ao.direct` | number |

### `lighting_map`

| Key | Type |
|---|---|
| `lighting_map.enabled` | bool |
| `lighting_map.chrome_texgen` | bool |
| `lighting_map.bump_scale` | number |
| `lighting_map.bump_diffuse` | number |
| `lighting_map.bump_diffuse_limit` | number |
| `lighting_map.specular_scale` | number |
| `lighting_map.specular_from_diffuse` | number |
| `lighting_map.gloss_min` | number |
| `lighting_map.gloss_max` | number |
| `lighting_map.chrome_scale` | number |
| `lighting_map.chrome_blur` | number |

## Values of the string keys

A name outside the set is refused by the setter. When the refusal comes from restoring the file at
startup it is dropped and the knob keeps its default; when it comes from a script or the front end
the write does not take.

| Key | Values |
|---|---|
| `hdr.tonemap` | `clamp`, `rolloff`, `reinhard`, `aces`, `filmic`, `agx` |
| `tess.set` | `off`, `map`, `all` |
| `bloom_layer.<i>.blend` | `off`, `add`, `screen`, `max` |

## Keys with a companion environment variable

While the variable is set to anything at all, `0` included, the key is neither restored from
`settings.json` nor written back to it.

| Key | Variable |
|---|---|
| `msaa` | `GKPLUS_VK_MSAA` |
| `hdr.enabled` | `GKPLUS_VK_HDR` |
| `bloom.enabled` | `GKPLUS_VK_BLOOM` |
| `per_pixel_lighting` | `GKPLUS_VK_PER_PIXEL_LIGHTING` |

## The key that is restored but never written back

`tess.enabled` is flagged `sync = false`. It is applied from `settings.json` at startup and is
never written back, on any device.

## Keys that do not exist

The `render.debug.*` members of the `"gk"` module are a measurement surface and persist nothing.
No `core.render.debug.*` key is read or written.

## When a change reaches the file

A knob is compared against the stored value once a frame and written only if it differs, from any
source: a script, the REPL, or the Advanced Graphics page. The write itself is debounced; see
[settings.json](/reference/data/settings-json/).

## Related

- [settings.json](/reference/data/settings-json/)
- [Environment variables](/reference/data/environment-variables/)
- [One settings file, many owners](/explanation/one-settings-file-many-owners/): why these
  are synchronised once a frame instead of written by their setters, and why two of them
  behave differently from the rest.
- [Why the renderer seam is the device](/explanation/why-the-renderer-seam-is-the-device/): what
  consumes them.
- [How to turn on renderer features](/how-to/modding/turn-on-renderer-features/): setting
  them, from the menu, a script or the launch environment.
