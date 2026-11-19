# stb_truetype

Unmodified `stb_truetype.h` v1.26 by Sean Barrett, from
https://github.com/nothings/stb at commit `d14d46f6b44456eb88beac58cba833a4135d4c89`
(sha256 `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab`).
It reads TrueType/OpenType fonts and rasterizes glyphs for libSceFont
(`src/hle/font_raster.c` compiles the implementation).

License: MIT or public domain (Unlicense), at your choice; the text is at the
end of the header and in `dist/licenses/stb_truetype.txt`.

# stb_image

Unmodified `stb_image.h` v2.27 by Sean Barrett (https://github.com/nothings/stb),
taken from KytyPS5's `3rdparty/stb` (line endings made LF; its `stb_truetype.h`
there is byte-identical to ours above), sha256
`91f435e0fc6a620018b878b9859c74dff60d28046f87e649191ad6f35a98c722`.
It decodes JPEG pictures for libSceJpegDec (`src/hle/lib_jpeg_dec.c` compiles
the implementation, JPEG only).

License: MIT or public domain (Unlicense), at your choice; the text is at the
end of the header and in `dist/licenses/stb_image.txt`.
