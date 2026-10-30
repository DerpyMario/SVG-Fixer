# SVG-Fixer

A drag-and-drop Windows tool (C++, single `.exe`, no install) that **repairs and
validates SVG files exported from [vectorizer.ai](https://vectorizer.ai) by a
Tampermonkey/userscript**, one file at a time or a whole `.7z` archive at once.

Drop any of these onto `SVG-Fixer.exe`:

| You drop            | Results go to                       |
|---------------------|-------------------------------------|
| `photos.7z`         | `photos_fixed\` next to the archive |
| a folder            | `<folder>_fixed\` next to it        |
| one or more `.svg`  | `fixed\` in the same folder         |

Each output folder also gets **`SVG-Fixer-Report.html`** (thumbnails of every
repaired file plus everything that was found) and `SVG-Fixer-Report.txt`.
Your original files are never modified. When the tool was started by a drop, it
waits at the end: press **R** to open the report.

## What it fixes

Userscript exports record what the vectorizer.ai viewer draws on its canvas.
That gives SVGs with these problems, all repaired automatically:

| Problem | Effect | Fix |
|---|---|---|
| No `width`, `height` or `viewBox` | Viewers fall back to a 300 x 150 px box: the picture is cropped or tiny | Canvas set to the image's native size, measured from the artwork |
| The viewer's zoom, `transform="matrix(0.32 0 0 0.32 0 0)"`, repeated on every path | Wrong size, bloated file | Removed; coordinates are native image pixels |
| float32 noise: `228.92999267578125` instead of `228.93` | Files 2-3x larger than needed | Rounded to 2 decimals (vectorizer.ai's own grid; at most 0.005 px change) |
| Zero-length segments, `fill-opacity="1"` | Bloat | Removed |
| `NaN`, `Infinity`, `undefined` in path data | Browsers stop drawing the shape at that point | Bad segments removed, the rest of the shape kept |
| File cut off (interrupted download) | Browsers show an XML error instead of the image | Everything before the cut is kept and the document closed |

The stroke-only paths in these exports are vectorizer.ai's **gap fillers**: they
hide the hairline seams anti-aliasing leaves between neighbouring shapes, so they
are kept (use `--strip-gap-fillers` if you need pure filled shapes, e.g. for a
cutting machine).

It also repairs problems found in SVGs from any other source: missing
`xmlns`, undeclared prefixes such as `xlink:`, XML syntax errors (unescaped `&`,
HTML entities like `&nbsp;`, unquoted or duplicate attributes, unclosed tags,
junk before `<?xml`), NUL bytes, Windows-1252 or UTF-16 text, and `<script>` /
`on*` handlers. Files that are already valid are copied byte for byte.

Every file it writes is parsed and validated again before it is saved; the
report marks it "Output re-validated".

### What it cannot fix

Sometimes the userscript saves the **vectorizer.ai result web page** (HTML) under
an `.svg` name instead of the drawing. The page loads the artwork at runtime, so
the file contains no vector data at all. SVG-Fixer detects this, writes nothing
for that file, and lists the original image name, its size and the
vectorizer.ai page to export it from again. (If a saved page *does* contain the
SVG inline, the SVG is extracted.)

Other files saved with an `.svg` name are identified too: PNG/JPEG/GIF/WebP
images are saved with their real extension, empty or zero-filled files are
reported.

## Command line

```
SVG-Fixer [options] <file.svg | folder | archive.7z> ...

  -o, --output DIR        write the results into DIR
  -n, --validate-only     only check the files, write nothing
  -p, --precision N       decimals for vectorizer coordinates (default: automatic, 2 for normal sizes)
      --strip-gap-fillers remove the stroke-only seam fillers
  -q, --quiet             one line per file
      --no-pause          do not wait for a key press at the end
```

Exit code: `0` when everything is valid or was repaired, `1` when a file could
not be repaired or an input could not be read, `2` for usage errors.

`.7z` archives may use LZMA, LZMA2, PPMd or BCJ filters; password-protected
archives are not supported. Only `.svg` files inside archives and folders are
processed.

## Building

Any C++17 compiler and CMake 3.16+. No dependencies: the 7z decoder is the
public-domain [LZMA SDK](third_party/lzma/README.md), included.

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

With Visual Studio this produces a statically linked `build\Release\SVG-Fixer.exe`.
MinGW (including cross-compiling from Linux with `x86_64-w64-mingw32-g++`) works
too and also links statically. The GitHub Actions workflow builds and tests on
Windows and Linux and uploads `SVG-Fixer.exe` as an artifact; pushing a tag such
as `v1.0.0` attaches it to a GitHub release.

## Layout

| Path | Contents |
|---|---|
| `src/main.cpp` | drag and drop / command line, inputs, output folders |
| `src/svgfix.cpp` | file classification, validation and all repairs |
| `src/xml.cpp` | forgiving XML parser that records and repairs syntax errors |
| `src/pathdata.cpp` | SVG path data parser, rounding serializer, exact bounds, transforms |
| `src/archive7z.cpp` | `.7z` reading on top of the LZMA SDK |
| `src/report.cpp` | HTML and text reports |
| `tests/` | unit and integration tests with small fixtures |
