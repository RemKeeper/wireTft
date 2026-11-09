#include <stdint.h>
// u8g2 font built by tools/make_lyric_font.py and embedded by main/CMakeLists.txt.
extern const uint8_t lyricFontStart[] asm("_binary_lyric_font_bin_start");

extern const uint8_t *const wiretftLyricFont = lyricFontStart;
