# FastJsonDL

A JSON Domain Language to draw on E-Ink displays by receiving a JSON payload
from any endpoint.  Built on top of
[FastEPD](https://github.com/bitbank2/FastEPD) and packaged as an
**ESP-IDF component** targeting the ESP32 family (primary target: ESP32-C5).

---

## Features

- Parses a JSON layout description and issues the corresponding drawing
  commands to a `FASTEPD` display instance.
- Configurable bits-per-pixel mode (`display_bpp` in JSON, or via
  `setDefaultBpp()`).
- Named-font registry — map any string to a FastEPD font data blob.
- In-order item rendering: items in the `"items"` array are drawn
  sequentially, so layering (e.g. black bar behind white text) works
  exactly as written.
- Human-readable error reporting via `getLastError()`.

---

## Top-level fields

| Field         | Type    | Default | Description |
|---------------|---------|---------|-------------|
| `display_bpp` | integer | `1`     | Bits per pixel: `1`, `2`, or `4`. |
| `clear`       | bool    | `false` | When `true`, fills the framebuffer with white before rendering any items. Use this to avoid uninitialised pixel data appearing as vertical stripes on the display. |

## Supported item types

| `"type"`       | Required fields          | Optional |
|----------------|--------------------------|----------|
| `drawString`   | `string`, `x`, `y`       | `font`, `c` |
| `fillRect`     | `x`, `y`, `w`, `h`       | `c`      |
| `drawRect`     | `x`, `y`, `w`, `h`       | `c`      |
| `drawLine`     | `x1`, `y1`, `x2`, `y2`   | `c`      |
| `fillCircle`   | `x`, `y`, `r`            | `c`      |
| `drawCircle`   | `x`, `y`, `r`            | `c`      |

`c` is the colour value and depends on `display_bpp`:
- 1BPP: `0..1`   (`0` = black, `1` = white)
- 2BPP: `0..3`   (`0` = black, `3` = white)
- 4BPP: `0..15`  (`0` = black, `15` = white)

When omitted, `c` defaults to black (`0`).

> **`drawString` — y is the text baseline**
> FastEPD BB-format fonts (produced by `fontconvert`) treat `y` as the **text
> baseline**, not the top-left corner of the glyph.  For a 40 pt font the
> ascender height is approximately 50 px, so the top of the rendered characters
> sits at `y − 50`.  Setting `"y": 10` with such a font places the glyphs
> almost entirely above the top edge of the screen (invisible, no error).
> Always set `y` ≥ the font's ascender height — for Ubuntu40 use `"y": 50` or
> greater.

---

## Example JSON

```json
{
  "display_bpp": 4,
  "clear": true,
  "items": [
    {
      "type": "drawString",
      "font": "Ubuntu40",
      "string": "Hello from FastJsonDL!",
      "x": 10, "y": 70,
      "c": 0
    }
  ]
}
```

---

## C++ API

```cpp
// 1. Initialise the EPD panel (FastEPD)
FASTEPD epd;
epd.initPanel(BB_PANEL_M5PAPERS3);

// 2. Create the renderer
FastJsonDL dl(epd);

// 3. (Optional) Register named fonts
static const FastJsonDLFont fonts[] = {
    { "Ubuntu40", Ubuntu40 },
};
dl.setFontRegistry(fonts, 1);

// 4. (Optional) Override display dimensions or BPP default
dl.setDisplaySize(540, 960);
dl.setDefaultBpp(1);

// 5. Render a layout
if (!dl.renderJsonString(myJson)) {
    printf("Error: %s\n", dl.getLastError());
}

// 6. Push to the physical display
epd.fullUpdate();
```

---

## Adding to your ESP-IDF project

### As a managed component (recommended)

Add to your project's `idf_component.yml`:

```yaml
dependencies:
  martinberlin__FastJsonDL:
    git: "https://github.com/martinberlin/FastJsonDL.git"
```

`FastEPD` is intentionally not declared as a managed dependency by this
component. Add your own FastEPD source (for example a submodule or a custom
branch checkout) in your project and wire it as a normal ESP-IDF component
named `FastEPD`.

### As a local component

Clone into your project's `components/` directory:

```sh
cd components
git clone https://github.com/martinberlin/FastJsonDL.git FastJsonDL
```

Then add your own FastEPD component (for example as a submodule pointing to
your preferred branch) under `components/FastEPD`.

Then add `FastJsonDL` to the `REQUIRES` list in your app component's
`CMakeLists.txt`.

---

## Dependencies

| Dependency | Source |
|------------|--------|
| FastEPD | User-provided ESP-IDF component (e.g. local submodule/custom branch) |
| cJSON | Ships with ESP-IDF (`json` component) |

---

## License

See [LICENSE](LICENSE).
