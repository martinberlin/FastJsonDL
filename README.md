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

## Supported item types

| `"type"`       | Required fields          | Optional |
|----------------|--------------------------|----------|
| `drawString`   | `string`, `x`, `y`       | `font`, `c` |
| `fillRect`     | `x`, `y`, `w`, `h`       | `c`      |
| `drawRect`     | `x`, `y`, `w`, `h`       | `c`      |
| `drawLine`     | `x1`, `y1`, `x2`, `y2`   | `c`      |
| `fillCircle`   | `x`, `y`, `r`            | `c`      |
| `drawCircle`   | `x`, `y`, `r`            | `c`      |

`c` is the colour value (0 = black, 1 = white); it defaults to black when
omitted.

---

## Example JSON

```json
{
  "display_bpp": 1,
  "items": [
    {
      "type": "fillRect",
      "x": 0, "y": 0, "w": 540, "h": 60,
      "c": 0
    },
    {
      "type": "drawString",
      "font": "Ubuntu40",
      "string": "Hello from FastJsonDL!",
      "x": 10, "y": 10,
      "c": 1
    },
    {
      "type": "drawRect",
      "x": 10, "y": 80, "w": 300, "h": 120,
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
  FastEPD:
    git: "https://github.com/bitbank2/FastEPD.git"
```

### As a local component

Clone into your project's `components/` directory:

```sh
cd components
git clone https://github.com/martinberlin/FastJsonDL.git FastJsonDL
git clone https://github.com/bitbank2/FastEPD.git FastEPD
```

Then add `FastJsonDL` to the `REQUIRES` list in your app component's
`CMakeLists.txt`.

---

## Dependencies

| Dependency | Source |
|------------|--------|
| [FastEPD](https://github.com/bitbank2/FastEPD) | External component |
| cJSON | Ships with ESP-IDF (`json` component) |

---

## License

See [LICENSE](LICENSE).
