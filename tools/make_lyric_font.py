"""Build the lyric font: a subset of Arduino_GFX's u8g2_font_unifont_h_cjk.

Keeps symbols below U+2E80, CJK punctuation, kana, full-width forms, Hangul
syllables from KS X 1001 and Han characters from GB2312, JIS X 0208 and Big5
level 1 (common traditional). All glyphs come from one font, so the u8g2
bit-stream parameters in the header stay valid.

Every code point of the previously used u8g2_font_unifont_h_chinese4 is kept
too, so upgrading never loses a character.

usage: make_lyric_font.py <unifont_h_cjk.h> <unifont_h_chinese4.h> <out.bin> [--stats]
"""
import re
import sys
from pathlib import Path

BLOCK = 100


def load_c_font(path):
    text = Path(path).read_text(encoding="latin-1")
    body = text[text.index("=") + 1:]
    out = bytearray()
    for part in re.findall(r'"((?:[^"\\]|\\.)*)"', body):
        i = 0
        while i < len(part):
            c = part[i]
            if c != "\\":
                out.append(ord(c))
                i += 1
                continue
            octal = re.match(r"[0-7]{1,3}", part[i + 1:])
            if octal:
                out.append(int(octal.group(0), 8))
                i += 1 + len(octal.group(0))
                continue
            e = part[i + 1]
            out.append({"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11}.get(e, ord(e)))
            i += 2
    out.append(0)  # implicit C string terminator ends the glyph list
    return bytes(out)


def split(font):
    unicode_start = 23 + (font[21] << 8 | font[22])
    p = unicode_start + (font[unicode_start] << 8 | font[unicode_start + 1])
    glyphs = {}
    while True:
        e = font[p] << 8 | font[p + 1]
        if e == 0:
            break
        size = font[p + 2]
        glyphs[e] = font[p:p + size]
        p += size
    return font[:unicode_start], glyphs


def codec_chars(codec, lead, trail):
    chars = set()
    for a in lead:
        for b in trail:
            try:
                chars.update(bytes([a, b]).decode(codec))
            except UnicodeDecodeError:
                pass
    return {ord(c) for c in chars}


def is_han(cp):
    return 0x4E00 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF


def wanted():
    han = set()
    han |= codec_chars("gb2312", range(0xB0, 0xF8), range(0xA1, 0xFF))
    han |= codec_chars("euc_jp", range(0xB0, 0xF5), range(0xA1, 0xFF))
    han |= codec_chars("big5", range(0xA4, 0xC7), list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)))
    han = {c for c in han if is_han(c)}
    hangul = {c for c in codec_chars("euc_kr", range(0xB0, 0xC9), range(0xA1, 0xFF)) if 0xAC00 <= c <= 0xD7A3}

    def keep(cp):
        if cp < 0x2E80:
            return True
        if 0x3000 <= cp <= 0x30FF or 0x31F0 <= cp <= 0x31FF or 0x3130 <= cp <= 0x318F:
            return True
        if 0xFF00 <= cp <= 0xFFEF:
            return True
        return cp in han or cp in hangul
    return keep


def build(prefix, glyphs):
    codes = sorted(glyphs)
    blocks = [codes[i:i + BLOCK] for i in range(0, len(codes), BLOCK)]
    data = [b"".join(glyphs[c] for c in block) for block in blocks]
    table = bytearray()
    for i, block in enumerate(blocks):
        offset = len(blocks) * 4 if i == 0 else len(data[i - 1])
        last = 0xFFFF if i == len(blocks) - 1 else block[-1]
        if offset > 0xFFFF:
            raise SystemExit("u8g2 jump table offset overflow")
        table += bytes([offset >> 8, offset & 255, last >> 8, last & 255])
    return bytes(prefix) + bytes(table) + b"".join(data) + b"\0\0"


def main():
    source, previous, target = sys.argv[1], sys.argv[2], sys.argv[3]
    cjk, old_font = load_c_font(source), load_c_font(previous)
    if cjk[1:17] != old_font[1:17]:
        raise SystemExit("font encodings differ; glyphs cannot be merged")
    prefix, glyphs = split(cjk)
    _, old = split(old_font)
    keep = wanted()
    subset = {cp: g for cp, g in glyphs.items() if keep(cp)}
    # Same Unifont glyphs and encoding parameters: chinese4 fills the few gaps.
    subset.update({cp: g for cp, g in old.items() if cp not in subset})
    font = build(prefix, subset)
    _, check = split(font)
    if check != subset:
        raise SystemExit("subset font round-trip mismatch")
    Path(target).write_bytes(font)
    if "--stats" in sys.argv:
        def count(lo, hi):
            return sum(1 for c in subset if lo <= c <= hi)
        print(f"{len(font)} bytes, {len(subset)} glyphs: kana {count(0x3040, 0x30FF)}, "
              f"han {sum(1 for c in subset if is_han(c))}, hangul {count(0xAC00, 0xD7A3)}, "
              f"other {sum(1 for c in subset if c < 0x2E80)}")


if __name__ == "__main__":
    main()
