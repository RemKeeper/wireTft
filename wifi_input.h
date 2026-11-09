#ifndef WIRETFT_WIFI_INPUT_H
#define WIRETFT_WIFI_INPUT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void wifi_wipe(void *buffer, size_t size)
{
    volatile unsigned char *p = (volatile unsigned char *)buffer;
    while (size--) *p++ = 0;
}

static inline bool wifi_valid_ssid(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 32) return false;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp; unsigned count;
        if (*p < 0x80) { if (*p < 0x20 || *p == 0x7f) return false; ++p; continue; }
        if (*p >= 0xc2 && *p <= 0xdf) { cp = *p & 31; count = 1; }
        else if (*p >= 0xe0 && *p <= 0xef) { cp = *p & 15; count = 2; }
        else if (*p >= 0xf0 && *p <= 0xf4) { cp = *p & 7; count = 3; }
        else return false;
        unsigned total = count;
        ++p;
        while (count--) { if ((*p & 0xc0) != 0x80) return false; cp = (cp << 6) | (*p++ & 63); }
        if ((total == 2 && cp < 0x800) || (total == 3 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return false;
    }
    return true;
}

static inline bool wifi_valid_password(const char *s)
{
    size_t n = strlen(s);
    if (!n) return true;
    if (n == 64) {
        for (size_t i = 0; i < n; ++i)
            if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
                  (s[i] >= 'A' && s[i] <= 'F'))) return false;
        return true;
    }
    if (n < 8 || n > 63) return false;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return false;
    return true;
}

struct WifiLine {
    char text[96] = {};
    size_t length = 0;
    bool overflow = false;
    bool afterCR = false;
    bool cancelled = false;
    void clear() { wifi_wipe(text, sizeof(text)); length = 0; overflow = false; cancelled = false; }
    bool feed(unsigned char c) {
        if (c == '\n' && afterCR) { afterCR = false; return false; }
        afterCR = c == '\r';
        if (c == 3 || c == 27) { clear(); cancelled = true; return true; }
        if (c == '\r' || c == '\n') { text[length] = 0; return true; }
        if (c == 8 || c == 127) { if (length && !overflow) text[--length] = 0; return false; }
        if (!c) { overflow = true; return false; }
        if (length + 1 < sizeof(text)) text[length++] = char(c);
        else overflow = true;
        return false;
    }
};
#endif