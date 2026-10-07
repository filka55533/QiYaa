# `src/skins` — Winamp 2 skin loader

`src/skins` turns a Winamp 2.x `.wsz` skin into a `Skins::Skin` value. A `.wsz` is a zip of BMP
sprite sheets and three text files. The value holds the decoded sheets, the window shapes from
`region.txt`, the playlist colours from `pledit.txt` and the 24 visualizer colours from
`viscolor.txt`, and it draws text with the skin's bitmap fonts (TEXT.BMP, GEN.BMP). `sprites.h`
says where each sprite sits in the sheets and where the main and equalizer windows place things.
The module owns no windows, masks, scaling or input. `src/ui` paints the windows from these sheets
and applies the region masks. `src/vis` decides what each visualizer colour is for. `src/app`
picks, loads and remembers the skin, and turns load errors into messages.

| File | Contains |
|---|---|
| `error.h` | `Skins::Error`, the module's only exception |
| `skin.h/.cpp` | `Skin`: `.wsz` loading (zip, bitmaps, fallbacks, `pledit.txt`, `viscolor.txt`), sheet access, TEXT.BMP and GEN.BMP text |
| `region.h/.cpp` | `ParseRegionTxt` (`region.txt` into polygons) and `RegionFromPolygons` (polygons into a `QRegion`) |
| `sprites.h` | `constexpr` tables of sprite rectangles, grouped by bitmap, and window layout positions |

## Dependencies

`qiyaa_skins` links **PUBLIC** `Qt6::Gui` and **PRIVATE** `qiyaa_miniz` (`contrib/miniz`). It
carries the Qt resource `:/skins`: the 8 bundled `.wsz` files from `resources/skins/`. One of them
is `base-2.91.wsz`, which `Skin::BuiltinBase()` loads. `src/app` offers all 8 in its skin menu.

It is in the first tier of the module order, with `audio` and `yandex`, and includes no other
project module. `vis`, `ui` and `app` use it.

It deliberately does not link:
- `Qt6::Widgets`. The skin draws through a `QPainter` that the caller owns.
- `Qt6::Network`, and every other `qiyaa_*` library.

miniz is private. Only `skin.cpp` includes `<miniz.h>`, so no header of the module exposes it.

```bash
grep -rn --include='*.h' --include='*.cpp' '#include "' src/skins/ | grep -v '#include "skins/'   # must print nothing
grep -rln --include='*.h' --include='*.cpp' 'miniz' src/ tests/ | grep -v '^src/skins/skin.cpp$'  # must print nothing
grep -rn --include='*.h' --include='*.cpp' 'QtWidgets\|<QWidget\|<QApplication' src/skins/        # must print nothing
```

Tests: `tests/skin_test.cpp` loads the base skin and every bundled skin, and pins the error
messages. `tests/region_test.cpp` pins the `region.txt` rules.

## `error.h`

```cpp
namespace Skins {
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
}
```

The messages are listed under [Errors](#errors).

## `skin.h/.cpp` — `Skin`

```cpp
namespace Skins {

class Skin {
public:
    enum class Sheet { Main, CButtons, TitleBar, Numbers, PlayPaus, MonoSter, PosBar, ShufRep,
                       Volume, Balance, Text, EqMain, PlEdit, EqEx, Gen };

    struct PlaylistStyle {
        QColor normal{0x00, 0xFF, 0x00};
        QColor current{0xFF, 0xFF, 0xFF};
        QColor normalBackground{0x00, 0x00, 0x00};
        QColor selectedBackground{0x00, 0x00, 0xC6};
        QString font = QStringLiteral("Arial");
    };

    static Skin LoadWsz(const QByteArray& archive, const Skin* fallback = nullptr);  // throws Error
    static Skin LoadFile(const QString& path, const Skin* fallback = nullptr);       // throws Error
    static Skin BuiltinBase();                                     // :/skins/base-2.91.wsz, throws Error

    bool isValid() const;                    // the Main sheet is present
    const QImage& sheet(Sheet sheet) const;  // null QImage when absent
    bool numbersAreExtended() const;         // Numbers holds nums_ex.bmp, not numbers.bmp

    void draw(QPainter& painter, Sheet bitmap, const QRect& source, const QPoint& target) const;

    int drawGenText(QPainter& painter, const QPoint& at, const QString& text, bool selected) const;
    int genTextWidth(const QString& text) const;

    int drawText(QPainter& painter, const QPoint& at, const QString& text, int maxWidth = -1) const;
    static int TextWidth(const QString& text);

    const TRegionData& region() const;
    const QList<QColor>& visColors() const;
    const PlaylistStyle& playlistStyle() const;

    static PlaylistStyle ParsePlaylistStyle(const QByteArray& text);
};

size_t qHash(Skin::Sheet sheet, size_t seed = 0) noexcept;

}  // namespace Skins
```

**Loading.** `LoadWsz` reads an archive that is already in memory. `LoadFile` opens the file,
refuses it above `kMaxArchiveBytes`, and calls `LoadWsz`. `BuiltinBase()` is
`LoadFile(":/skins/base-2.91.wsz")` without a fallback. Loading is synchronous: the whole archive
is unpacked and decoded inside the call. Each loader returns a valid `Skin` or throws
`Skins::Error`. `isValid()` is false only for a default-constructed `Skin`.

**Fallback.** What the skin lacks is copied from `fallback`, which normally is the built-in base
skin, as Winamp does. `region.txt` is the exception. The loader reads `fallback` only during the
call and does not keep the pointer. A copied sheet is a `QImage`, which is implicitly shared, so
the fallback and the new skin share the pixel data. Where each item comes from:

| Item | From the skin | Otherwise |
|---|---|---|
| `Main`, `CButtons`, `TitleBar`, `PlayPaus`, `MonoSter`, `PosBar`, `ShufRep`, `Text`, `EqMain`, `PlEdit`, `EqEx`, `Gen`, `Volume` | its bitmap (`main.bmp`, ..., `eq_ex.bmp`), if it decodes | the fallback's sheet; absent without a fallback |
| `Balance` | `balance.bmp` | the skin's own `volume.bmp`, then the fallback's `Balance` |
| `Numbers` | `nums_ex.bmp` (`numbersAreExtended()` is true), then `numbers.bmp` (false) | the fallback's sheet and its `numbersAreExtended()` |
| `region()` | `region.txt` | empty, never taken from the fallback, so the windows stay rectangular |
| `playlistStyle()` | `pledit.txt`, whenever the file exists | the fallback's style; the defaults above without a fallback |
| `visColors()` | `viscolor.txt`, if it yields 24 colours | the fallback's whole list; the partial list without a fallback |

Every sheet is converted to `QImage::Format_ARGB32_Premultiplied`. The file formats and the limits
are under [File formats](#file-formats).

**Drawing.** All positions are skin pixels at scale 1. The painter's transform does the scaling, and
`src/ui` sets it. `draw()` copies `source` from the sheet to `target` and does nothing if the sheet
is absent.

**Bitmap text (`drawText`, `TextWidth`).** Each character is resolved in this order:
1. A cell of TEXT.BMP (the map is under [TEXT.BMP](#textbmp)). Advance 5 px (`kCharWidth`).
2. A Cyrillic letter that looks like a Latin letter or a digit reuses that cell: А→a, В→b,
   Е Ё→e, З→3, К→k, М→m, Н→h, О→o, Р→p, С→c, Т→t, У Ў→y, Х→x, І Ї→i.
3. A built-in pixel glyph, 6 rows high, for Б Г Ґ Д Ж И Й Л П Ф Ц Ч Ш Щ Ъ Ы Ь Э Є Ю Я (21 glyphs).
   A glyph is 4 or 5 ink columns wide and advances by its width + 1. The skin's space cell (row 0,
   column 30) is tiled behind the glyph in 5 px steps, and then the ink pixels are painted.
   Together, steps 2 and 3 cover the whole Russian, Ukrainian and Belarusian alphabets. Lookups
   use the upper-case letter, because the font has only one case.
4. Accented Latin loses its accent, like webamp's `deburr()`: the first character of the NFD
   decomposition, if that character has a cell.
5. Anything else is drawn with the system font "Sans Serif" at 7 px, without antialiasing and with
   full hinting. Its advance comes from `QFontMetrics::horizontalAdvance`.

Pixel glyphs and system-font characters are painted in the TEXT.BMP *ink colour*. The background
is the top-right pixel of TEXT.BMP, `(width − 1, 0)`. The ink colour is the most frequent other
colour in the a–z strip (`y < 6`, `x < 130`). It is `#00FF00` if TEXT.BMP is absent or if that
strip has no other colour. It is computed once per `drawText` call, and only when needed.
`drawText` stops before the first character that would end past `at.x() + maxWidth`. A negative
`maxWidth` means no limit. It returns the width it drew. `TextWidth` gives the same advances
without a skin.

**Title letters (`drawGenText`, `genTextWidth`).** These draw only A–Z (case-insensitive) and the
space, which is 5 px wide. Every other character is skipped without advancing. The letter
positions are measured from GEN.BMP once per load, for the two rows separately (layout under
[GEN.BMP](#genbmp-letters)). `drawGenText` returns the width it drew.

**Threads.** `Skin` is a plain value with no thread rules of its own. `src/app` loads skins and
paints them on the GUI thread. The file-local tables in `skin.cpp` (font map, fallback `QFont`,
regular expressions, the empty `QImage` that `sheet()` returns) are function-local statics, created
on first use.

**Traps:**
- With a fallback, every zip that holds at least one file loads. A zip without `main.bmp`, or with
  names the loader does not recognise, silently becomes the fallback skin. "main.bmp is missing"
  is thrown only when no fallback is given.
- Sheet sizes are not checked against the coordinates in `sprites.h`.
- The loader unpacks every entry of the archive, up to the limits: cursors, readmes, nested zips,
  AVS files. It then uses only the files listed under [File formats](#file-formats).
- Entries are matched by base name. `QFileInfo` treats `\` as a path separator only on Windows. An
  archive whose entry names use backslashes (`Skin\main.bmp`) therefore loads on Windows, but on
  Linux and macOS it finds no bitmaps and falls back (see the first trap).
- `LoadWsz` does not apply `kMaxArchiveBytes`. The caller already holds the bytes, and only
  `LoadFile` checks the file size.
- If `pledit.txt` exists but a value in it does not parse, that key keeps the default from
  `PlaylistStyle`, not the fallback's value.
- Without a fallback, `visColors()` can hold fewer than 24 entries. A `viscolor.txt` component
  above 255 gives an invalid `QColor`, and that colour still counts toward the 24.
- `drawText` changes the painter's font and pen when a character reaches the system font (step 5),
  and it does not restore them.
- Step 5 needs a `QGuiApplication` (`QFontMetrics`), and its glyphs and widths depend on the fonts
  installed on the machine.
- Lower-case å, ö, ä are drawn as a, o, a. The lookup lower-cases first and the table holds
  upper-case Å, Ö, Ä, so only the upper-case letters find their cells.
- `genTextWidth` measures with the normal row. `drawGenText(..., selected = true)` draws with the
  selected row, and its widths differ if a skin's two rows differ. The rows are the same in the
  three bundled skins that have a `gen.bmp` (base-2.91, Green-Dimension-V2, Skinner_Atlas).

## `region.h/.cpp`

```cpp
namespace Skins {
using TRegionData = QHash<QString, QList<QPolygon>>;  // lower-case section name -> polygons

TRegionData ParseRegionTxt(const QByteArray& text);
QRegion RegionFromPolygons(const QList<QPolygon>& polygons);  // empty list -> empty region (no mask)
}
```

`ParseRegionTxt` keeps every section that yields at least one polygon, under its lower-case name.
`src/ui` asks for `normal` (main window), `windowshade` (main window, shaded), `equalizer` and
`equalizerws` (equalizer, shaded). Points are skin pixels at scale 1. `src/ui` scales the polygons
before it calls `RegionFromPolygons`. `RegionFromPolygons` unites the polygons, each filled with
`Qt::WindingFill`. It never throws. Bad input is dropped as described under
[region.txt](#regiontxt).

**Traps:**
- A `PointList` spread over several lines keeps only its first line. The format is line-based, and
  a line without `=` is ignored. `region_test` pins this.
- A token that is not an integer is dropped, and that shifts every later coordinate by one.

## `sprites.h`

```cpp
namespace Skins {
struct ButtonSprite { QRect normal; QRect pressed; };
struct ToggleSprite { QRect off; QRect offPressed; QRect on; QRect onPressed; };
inline constexpr QRect DigitSprite(int digit);  // the digit's cell in NUMBERS.BMP / NUMS_EX.BMP
// ... constants and structs below
}
```

A `QRect` sprite is `{x, y, width, height}` in the pixels of its bitmap. The layout entries (a
`QPoint`, or a `QRect` that names a window area) are positions in the window, in skin pixels. The
values come from webamp. Which entries cover which bitmap:

| Bitmap (`Skin::Sheet`) | In `sprites.h` |
|---|---|
| MAIN.BMP (`Main`) | `kMainBackground` |
| TITLEBAR.BMP (`TitleBar`) | `kTitleBar(Selected)`, `kOptionsButton(Down)`, `kMinimizeButton(Down)`, `kShadeButton(Down)`, `kCloseButton(Down)`, `kClutterBar`; the shaded main window: `kShadeBackground(Selected)`, `kShadeButtonShaded(Down)`, `kShadePositionBackground`, `kShadePositionThumb(Left/Right)` |
| CBUTTONS.BMP (`CButtons`) | `ButtonSprite`s `kPrevious`, `kPlay`, `kPause`, `kStop`, `kNext`, `kEject` |
| PLAYPAUS.BMP (`PlayPaus`) | `kPlayingIndicator`, `kPausedIndicator`, `kStoppedIndicator`, `kWorkingIndicator` |
| MONOSTER.BMP (`MonoSter`) | `kStereo(Selected)`, `kMono(Selected)` |
| NUMBERS.BMP / NUMS_EX.BMP (`Numbers`) | `kDigitWidth`, `kDigitHeight`, `DigitSprite(digit)`, `kMinusSign` (numbers.bmp), `kMinusSignEx` (nums_ex.bmp) |
| POSBAR.BMP (`PosBar`) | `kPositionBackground`, `kPositionThumb(Selected)` |
| SHUFREP.BMP (`ShufRep`) | `ToggleSprite`s `kShuffle`, `kRepeat`, `kEqButton`, `kPlaylistButton` |
| VOLUME.BMP / BALANCE.BMP (`Volume`, `Balance`) | `kSliderFrameStep`, `kSliderFrameHeight`, `kVolumeThumb(Selected)`, `kBalanceThumb(Selected)` |
| TEXT.BMP (`Text`) | `kCharWidth`, `kCharHeight` |
| EQMAIN.BMP (`EqMain`) | `EqualizerSprites`, including the equalizer window layout |
| EQ_EX.BMP (`EqEx`) | `EqualizerShadeSprites` (the shaded equalizer window) |
| PLEDIT.BMP (`PlEdit`) | `PlaylistSprites`, including the playlist sizes |
| GEN.BMP (`Gen`) | `GenWindowSprites`, including the letter rows |
| none: main window layout | `MainWindowSprites` |

Layout values whose meaning the name does not give:
- `MainWindowSprites::kTime` is the origin of the time display. `src/ui` draws the four digits at
  x +9, +21, +39 and +51, and the minus at (−1, 0) from nums_ex.bmp or at (−1, +6) from
  numbers.bmp.
- `EqualizerSprites`: `kBandsX` (78) is the x of the first band slider, and `kBandStep` (18) is the
  distance between bands. `kSliderTravel` (62 − 11 = 51) is the thumb's travel in px.
- `EqualizerShadeSprites::kVolume` and `kBalance` are the slider areas of the shaded equalizer
  window, not sprites. `kVolumeThumb[i]` and `kBalanceThumb[i]` are the thumbs for the left,
  centre and right third of the travel (`i` = 0, 1, 2).
- `PlaylistSprites`: the window starts at `kMinSize` (275×116) and grows in steps of
  `kStepWidth` × `kStepHeight` (25×29). List rows are `kRowHeight` (13) px high.

**Traps:**
- `kMinusSign` is valid only in numbers.bmp, and `kMinusSignEx` only in nums_ex.bmp. Choose with
  `Skin::numbersAreExtended()`.
- `kVolumeThumb` and `kBalanceThumb` have the same rectangles. They differ only in the sheet you
  draw them from.
- `EqualizerShadeSprites::kShadeButtonDown` is the pressed button in normal mode (webamp calls it
  "maximize"). `kShadeButtonShadedDown` is the pressed button in shade mode (webamp's "minimize").
- The same names mean different bitmaps in different places. The free `kTitleBar` is in
  TITLEBAR.BMP, and `EqualizerSprites::kTitleBar` is in EQMAIN.BMP. `kClose` is a `QPoint`
  position, while `kCloseButton` is a sprite.

## File formats

### `.wsz` archive

A `.wsz` is a zip, read with miniz. Only the store and deflate methods can be extracted. Each
entry is stored under its base name, in lower case: `Skin Folder/MAIN.BMP` becomes `main.bmp`.
The name is decoded as UTF-8. Directory entries are skipped.

| Limit | Value | Checked against | When exceeded |
|---|---|---|---|
| `kMaxArchiveBytes` | 64 MiB (67'108'864) | file size, in `LoadFile` only | throws |
| `kMaxEntries` | 4096 | entries in the central directory, directories included | throws |
| `kMaxEntryBytes` | 32 MiB | declared uncompressed size of one entry | entry skipped |
| `kMaxTotalBytes` | 64 MiB | running sum of declared sizes converted with `static_cast<qint64>` after the per-entry limit check | entry skipped |

The per-entry checks run before the entry is extracted. miniz then allocates the declared size and
fails the entry if the data does not match that size or its CRC. An entry that fails to extract
(unsupported method, encryption, size or CRC mismatch) is skipped. When two entries have the same
base name, the first one in central-directory order that passes the limits and extracts is kept.

Files the loader uses (everything else in the archive is ignored):
- Bitmaps: `main.bmp`, `cbuttons.bmp`, `titlebar.bmp`, `playpaus.bmp`, `monoster.bmp`,
  `posbar.bmp`, `shufrep.bmp`, `text.bmp`, `eqmain.bmp`, `pledit.bmp`, `eq_ex.bmp`, `gen.bmp`,
  `volume.bmp`, `balance.bmp`, `nums_ex.bmp`, `numbers.bmp`. They are decoded by `QImageReader`,
  which decides the format from the content, so a PNG named `main.bmp` also works. Their pixel
  sizes are bounded only by Qt's `QImageReader::allocationLimit()`, which this module does not
  change. A bitmap that does not decode counts as missing.
- Text: `region.txt`, `pledit.txt` and `viscolor.txt`, decoded as Latin-1.
- Never read: `*.cur`, `avs.bmp`, `mb.bmp`, `genex.bmp`, `video.bmp`, readmes.

### `region.txt`

It is INI-like and line-based. Lines end at any run of CR or LF, and each line is trimmed.

```
; a comment line starts with ';', '#' or '//'
[Normal]         ; section name: the text between '[' and ']', case-insensitive
NumPoints=4,4    ; number of points in each polygon
PointList=0,0 275,0 275,14 0,14  0,14,275,14,275,116,0,116   ; x,y pairs
```

- Keys are case-insensitive. A value ends at the first `;`. Numbers are separated by any mix of
  commas and whitespace. Keys before the first section are ignored. A repeated key keeps the last
  value, and a repeated section merges into the earlier one.
- Polygon *i* takes the next `NumPoints[i]` pairs from `PointList`.

What it refuses, silently:
- A section without `NumPoints`, or with fewer than 2 numbers in `PointList`, is dropped.
- A count below 3 is not a polygon: its points are skipped, and a negative count skips none.
- A count that asks for more points than remain ends the section. The polygons before it are kept.
- An odd trailing number is ignored.
- A section that ends up with no polygon is not stored.

There is no limit of its own on counts or coordinates. The input is bounded by `kMaxEntryBytes`.

### `pledit.txt`

```
[Text]
Normal=#00FF00
Current=#FFFFFF
NormalBG=#000000
SelectedBG=#0000C6
Font=Arial
```

- Every `Key=Value` line counts, in whatever section it appears, because section headers are
  ignored. Keys are letters only and case-insensitive. The keys used are `normal`, `current`,
  `normalbg`, `selectedbg` and `font`. Others (`mbFG`, `mbBG`, ...) are ignored.
- The `#` of a colour is optional. The first 7 characters, after a `#` is added, go to
  `QColor::fromString`, so `#RRGGBB` and `#RGB` work and anything after the sixth hex digit is
  ignored. A value that does not parse leaves that key at its default.
- `Font` accepts any non-empty value, as a family name. `src/ui` chooses the size.

### `viscolor.txt`

```
0,0,0, // color 0 = black
24,33,41, // color 1 = grey for dots
...
```

Each line that starts with `r,g,b` (whitespace around the commas is allowed) gives one colour. The
rest of the line is ignored, and so are lines that do not match. Reading stops at 24 colours. The
values are not range-checked. What each index means is decided in [src/vis](../vis/README.md).

### TEXT.BMP

Cells are 5×6 px (`kCharWidth` × `kCharHeight`). Cell (row *r*, column *c*) is at x = 5*c*,
y = 6*r*.

| Row | Columns |
|---|---|
| 0 | `a`–`z` 0–25, `"` 26, `@` 27, space 30 |
| 1 | `0`–`9` 0–9, `…` 10, `.` 11, `:` 12, `(` 13, `)` 14, `-` 15, `'` 16, `!` 17, `_` 18, `+` 19, `\` 20, `/` 21, `[` 22, `]` 23, `^` 24, `&` 25, `%` 26, `,` 27, `=` 28, `$` 29, `#` 30 |
| 2 | `Å` 0, `Ö` 1, `Ä` 2, `?` 3, `*` 4 |

`<` and `{` use the `[` cell. `>` and `}` use the `]` cell. The lookup is webamp's `FONT_LOOKUP`.

### GEN.BMP letters

There are two strips of A–Z, 7 px high (`kLetterHeight`): the selected (active window) strip at
y = 88 and the normal strip at y = 96. Column x = 0 of each strip has the separator colour. The
letters follow from x = 1, left to right, each followed by one column of the separator colour. A
letter's width is the run of pixels on the strip's top row that differ from the pixel at x = 0.
This is a port of webamp's `genGenTextSprites()`.

### Slider frames and digits

- VOLUME.BMP and BALANCE.BMP: 28 background frames stacked vertically, 15 px apart
  (`kSliderFrameStep`) and 13 px high (`kSliderFrameHeight`). The 14×11 thumbs are at y = 422.
- EQMAIN.BMP: 28 slider frames (low to high value) of 14×63 px from (13, 164), 14 per row, 15 px
  apart, with rows 65 px apart. There is also a 1×19 column of graph-line colours at (115, 294).
- NUMBERS.BMP: digits 0–9 in 9×13 cells, digit *d* at x = 9*d*. The minus is the 5×1 dash at (20,
  6). NUMS_EX.BMP has the same digits plus a minus in its own 9×13 cell at x = 99.

## Errors

```
std::runtime_error
└── Skins::Error        skins/error.h
```

`LoadWsz` throws `Skins::Error`, and so do `LoadFile` and `BuiltinBase`, which call it. There are
no child classes: every failure means "this skin cannot be used", and every caller reacts the same
way. The messages are:

| Message | When |
|---|---|
| `not a zip archive (<n> bytes)` | miniz cannot open the data as a zip |
| `the archive lists <n> files; a skin has at most 4096` | more than `kMaxEntries` entries |
| `the archive holds no files` | nothing was extracted, which includes the case where every entry was skipped by the limits or failed |
| `main.bmp is missing or cannot be decoded` | neither the skin nor the fallback has a `Main` sheet |
| `<path>: <QFile::errorString()>` | `LoadFile` cannot open the file |
| `<path>: <n> bytes, more than the 67108864 a skin may have` | `LoadFile`, above `kMaxArchiveBytes` |

`LoadFile` adds `<path>: ` in front of every `LoadWsz` message. Paths and messages are UTF-8.
Nothing else is translated. `std::bad_alloc`, for example, passes through unchanged. How
`src/app` reports these messages is in [src/app](../app/README.md).

These failures are data, not exceptions:
- A missing or undecodable bitmap takes the fallback's sheet, or leaves the sheet absent:
  `sheet()` returns a null `QImage` and `draw()` does nothing.
- Zip entries that are too large, over the total, duplicated or unextractable are skipped.
- `region.txt` problems drop sections. An empty `TRegionData` means rectangular windows.
- A bad `pledit.txt` value keeps the default. A short `viscolor.txt` takes the fallback's list.
- A character with no glyph falls through to the system font (TEXT.BMP), or is skipped (GEN.BMP).

## Not here

- Painting the windows, docking, input, scaling, and applying `region()` as a window mask:
  [src/ui](../ui/README.md).
- What the 24 visualizer colours mean, the analyzer and the oscilloscope:
  [src/vis](../vis/README.md).
- Choosing a skin, the skin menu, remembering the choice and showing load errors:
  [src/app](../app/README.md).
- Equalizer presets (`.eqf`) and the built-in Winamp presets: [src/audio](../audio/README.md).
- Authors and licences of the bundled skins and of the webamp-derived code:
  [THIRD_PARTY.md](../../THIRD_PARTY.md).
- Cursors (`*.cur`), AVS and the minibrowser (`mb.bmp`) are not supported anywhere.
