#pragma once
// Arduino_GFX enables its built-in u8g2 font decoder only when <U8g2lib.h>
// exists. It needs just these macros, not the U8g2 library itself.
#define U8G2_WITH_UNICODE
#define U8G2_FONT_SECTION(name)
