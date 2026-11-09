#ifndef WIRETFT_MUSIC_JSON_H
#define WIRETFT_MUSIC_JSON_H
// go-musicfox LAN broadcast snapshot parsing and lyric text layout.
// Pure C++, no allocation: shared by firmware and host tests.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct JsonReader {
	const char *p, *end;
	bool ok = true;
	JsonReader(const char *text, size_t length) : p(text), end(text + length) {}
	bool fail() { ok = false; return false; }
	void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
	bool expect(char c) { ws(); if (p < end && *p == c) { ++p; return true; } return fail(); }
	bool literal(const char *word)
	{
		ws();
		size_t n = strlen(word);
		if (size_t(end - p) < n || memcmp(p, word, n)) return false;
		p += n;
		return true;
	}
	bool hex4(uint32_t &v)
	{
		if (end - p < 4) return false;
		v = 0;
		for (int i = 0; i < 4; ++i) {
			char c = *p++;
			v <<= 4;
			if (c >= '0' && c <= '9') v |= c - '0';
			else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
			else return false;
		}
		return true;
	}
	// Decodes into out (cap includes NUL), truncating on a code point boundary.
	// Control characters become spaces. out may be null to skip.
	bool string(char *out, size_t cap, size_t *length = nullptr)
	{
		size_t n = 0;
		bool full = !out || cap == 0;
		if (out && cap) out[0] = 0;
		if (literal("null")) { if (length) *length = 0; return true; }
		if (!expect('"')) return false;
		auto put = [&](uint32_t cp) {
			char b[4];
			size_t k;
			if (cp < 0x20) cp = ' ';
			if (cp < 0x80) { b[0] = char(cp); k = 1; }
			else if (cp < 0x800) { b[0] = char(0xc0 | cp >> 6); b[1] = char(0x80 | (cp & 63)); k = 2; }
			else if (cp < 0x10000) { b[0] = char(0xe0 | cp >> 12); b[1] = char(0x80 | ((cp >> 6) & 63)); b[2] = char(0x80 | (cp & 63)); k = 3; }
			else { b[0] = char(0xf0 | cp >> 18); b[1] = char(0x80 | ((cp >> 12) & 63)); b[2] = char(0x80 | ((cp >> 6) & 63)); b[3] = char(0x80 | (cp & 63)); k = 4; }
			if (full) return;
			if (n + k + 1 > cap) { full = true; return; }
			memcpy(out + n, b, k);
			n += k;
		};
		while (p < end) {
			unsigned char c = (unsigned char)*p++;
			if (c == '"') {
				if (out && cap) { n = utf8Complete(out, n); out[n] = 0; }
				if (length) *length = n;
				return true;
			}
			if (c < 0x20) return fail();
			if (c != '\\') {
				if (!full) {
					if (n + 2 > cap) full = true;
					else out[n++] = char(c);
				}
				continue;
			}
			if (p >= end) return fail();
			char e = *p++;
			uint32_t cp;
			switch (e) {
			case '"': case '\\': case '/': put(uint32_t(e)); break;
			case 'b': case 'f': case 'n': case 'r': case 't': put(' '); break;
			case 'u':
				if (!hex4(cp)) return fail();
				if (cp >= 0xd800 && cp <= 0xdbff) {
					uint32_t lo;
					if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
						p += 2;
						if (!hex4(lo)) return fail();
						cp = lo >= 0xdc00 && lo <= 0xdfff ? 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00) : 0xfffd;
					} else cp = 0xfffd;
				} else if (cp >= 0xdc00 && cp <= 0xdfff) cp = 0xfffd;
				put(cp);
				break;
			default: return fail();
			}
		}
		return fail();
	}
	static size_t utf8Complete(const char *s, size_t n)
	{
		size_t i = n;
		while (i > 0 && n - i < 4 && ((unsigned char)s[i - 1] & 0xc0) == 0x80) --i;
		if (i == 0) return n;
		unsigned char lead = (unsigned char)s[i - 1];
		size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
		return (i - 1) + need > n ? i - 1 : n;
	}
	bool number(int64_t &v)
	{
		if (literal("null")) return true;
		ws();
		bool negative = p < end && *p == '-';
		if (negative) ++p;
		if (p >= end || *p < '0' || *p > '9') return fail();
		int64_t x = 0;
		while (p < end && *p >= '0' && *p <= '9') {
			if (x < 922337203685477580LL) x = x * 10 + (*p - '0');
			++p;
		}
		if (p < end && *p == '.') { ++p; while (p < end && *p >= '0' && *p <= '9') ++p; }
		if (p < end && (*p == 'e' || *p == 'E')) {
			++p;
			if (p < end && (*p == '+' || *p == '-')) ++p;
			while (p < end && *p >= '0' && *p <= '9') ++p;
		}
		v = negative ? -x : x;
		return true;
	}
	bool boolean(bool &v)
	{
		if (literal("true")) { v = true; return true; }
		if (literal("false")) { v = false; return true; }
		if (literal("null")) return true;
		return fail();
	}
	bool objectNext(bool &first, char *key, size_t cap)
	{
		ws();
		if (p >= end) return fail();
		if (*p == '}') { ++p; return false; }
		if (!first) { if (*p != ',') return fail(); ++p; }
		first = false;
		ws();
		if (p >= end || *p != '"') return fail();
		return string(key, cap) && expect(':');
	}
	bool arrayNext(bool &first)
	{
		ws();
		if (p >= end) return fail();
		if (*p == ']') { ++p; return false; }
		if (!first) { if (*p != ',') return fail(); ++p; }
		first = false;
		return true;
	}
	bool skip(int depth = 0)
	{
		ws();
		if (p >= end || depth > 16) return fail();
		if (*p == '{') {
			++p;
			bool first = true;
			while (objectNext(first, nullptr, 0)) if (!skip(depth + 1)) return false;
			return ok;
		}
		if (*p == '[') {
			++p;
			bool first = true;
			while (arrayNext(first)) if (!skip(depth + 1)) return false;
			return ok;
		}
		if (*p == '"') return string(nullptr, 0);
		if (literal("true") || literal("false") || literal("null")) return true;
		int64_t ignored;
		return number(ignored);
	}
	// false with ok intact means the value was null.
	bool objectStart() { if (literal("null")) return false; return expect('{'); }
	bool arrayStart() { if (literal("null")) return false; return expect('['); }
};

static constexpr int MUSIC_MAX_LINES = 24;
static constexpr int MUSIC_MAX_WORDS = 320;
static constexpr int MUSIC_POOL = 3072;
static constexpr size_t MUSIC_LINE_TEXT = 192;

struct MusicWord { int32_t start, end; uint16_t off, len; };
struct MusicLine {
	int32_t index, start, end;
	uint16_t text, textLen, trans, transLen, word0, wordCount;
};
struct MusicSnapshot {
	uint32_t seq;
	bool playing;
	char state[12];
	int64_t positionMs, durationMs;
	int64_t songId;
	char name[96], artists[96], coverUrl[96];
	uint16_t lineCount, wordCount, poolUsed;
	MusicLine lines[MUSIC_MAX_LINES];
	MusicWord words[MUSIC_MAX_WORDS];
	char pool[MUSIC_POOL];
	const char *text(const MusicLine &l) const { return pool + l.text; }
	const char *translation(const MusicLine &l) const { return pool + l.trans; }
};

static inline bool musicPoolString(JsonReader &j, MusicSnapshot &s, uint16_t &off, uint16_t &len)
{
	size_t room = MUSIC_POOL - s.poolUsed, n = 0;
	if (room > MUSIC_LINE_TEXT) room = MUSIC_LINE_TEXT;
	if (room < 2) { off = len = 0; return j.string(nullptr, 0); }
	if (!j.string(s.pool + s.poolUsed, room, &n)) return false;
	off = s.poolUsed; len = uint16_t(n);
	s.poolUsed += uint16_t(n + 1);
	return true;
}

static inline bool musicParseWords(JsonReader &j, MusicSnapshot &s, MusicLine &line)
{
	if (!j.arrayStart()) return j.ok;
	line.word0 = s.wordCount;
	uint32_t off = 0;
	bool first = true;
	while (j.arrayNext(first)) {
		if (!j.objectStart()) { if (!j.ok) return false; continue; }
		int64_t start = 0, finish = 0;
		size_t len = 0;
		char key[16], piece[MUSIC_LINE_TEXT];
		bool fieldFirst = true;
		while (j.objectNext(fieldFirst, key, sizeof(key))) {
			if (!strcmp(key, "startMs")) j.number(start);
			else if (!strcmp(key, "endMs")) j.number(finish);
			else if (!strcmp(key, "text")) j.string(piece, sizeof(piece), &len);
			else j.skip();
			if (!j.ok) return false;
		}
		if (!j.ok) return false;
		if (s.wordCount < MUSIC_MAX_WORDS) {
			s.words[s.wordCount++] = {int32_t(start), int32_t(finish), uint16_t(off), uint16_t(len)};
			++line.wordCount;
		}
		off += len;
	}
	return j.ok;
}

static inline bool musicParseLyric(JsonReader &j, MusicSnapshot &s)
{
	if (!j.arrayStart()) return j.ok;
	bool first = true;
	while (j.arrayNext(first)) {
		if (s.lineCount >= MUSIC_MAX_LINES) { if (!j.skip()) return false; continue; }
		if (!j.objectStart()) { if (!j.ok) return false; continue; }
		MusicLine line{};
		int64_t index = 0, start = 0, finish = 0;
		char key[16];
		bool fieldFirst = true;
		while (j.objectNext(fieldFirst, key, sizeof(key))) {
			if (!strcmp(key, "index")) j.number(index);
			else if (!strcmp(key, "startMs")) j.number(start);
			else if (!strcmp(key, "endMs")) j.number(finish);
			else if (!strcmp(key, "text")) musicPoolString(j, s, line.text, line.textLen);
			else if (!strcmp(key, "translation")) musicPoolString(j, s, line.trans, line.transLen);
			else if (!strcmp(key, "words")) musicParseWords(j, s, line);
			else j.skip();
			if (!j.ok) return false;
		}
		if (!j.ok) return false;
		line.index = int32_t(index); line.start = int32_t(start); line.end = int32_t(finish);
		s.lines[s.lineCount++] = line;
	}
	return j.ok;
}

static inline bool musicParseSong(JsonReader &j, MusicSnapshot &s, int64_t &duration)
{
	if (!j.objectStart()) return j.ok;
	char key[16];
	bool first = true;
	while (j.objectNext(first, key, sizeof(key))) {
		if (!strcmp(key, "id")) j.number(s.songId);
		else if (!strcmp(key, "name")) j.string(s.name, sizeof(s.name));
		else if (!strcmp(key, "artists")) j.string(s.artists, sizeof(s.artists));
		else if (!strcmp(key, "coverUrl")) j.string(s.coverUrl, sizeof(s.coverUrl));
		else if (!strcmp(key, "durationMs")) j.number(duration);
		else j.skip();
		if (!j.ok) return false;
	}
	return j.ok;
}

// Accepts only protocol version 1 "nowplaying" snapshots.
static inline bool musicParse(const char *json, size_t length, MusicSnapshot &s)
{
	s.seq = 0; s.playing = false; s.state[0] = 0;
	s.positionMs = s.durationMs = 0; s.songId = 0;
	s.name[0] = s.artists[0] = s.coverUrl[0] = 0;
	s.lineCount = s.wordCount = 0;
	s.pool[0] = 0; s.poolUsed = 1;
	JsonReader j(json, length);
	if (!j.expect('{')) return false;
	char key[16], type[16] = {};
	int64_t version = 0, seq = 0, songDuration = 0;
	bool first = true;
	while (j.objectNext(first, key, sizeof(key))) {
		if (!strcmp(key, "type")) j.string(type, sizeof(type));
		else if (!strcmp(key, "version")) j.number(version);
		else if (!strcmp(key, "seq")) j.number(seq);
		else if (!strcmp(key, "playing")) j.boolean(s.playing);
		else if (!strcmp(key, "state")) j.string(s.state, sizeof(s.state));
		else if (!strcmp(key, "positionMs")) j.number(s.positionMs);
		else if (!strcmp(key, "durationMs")) j.number(s.durationMs);
		else if (!strcmp(key, "song")) musicParseSong(j, s, songDuration);
		else if (!strcmp(key, "lyric")) musicParseLyric(j, s);
		else j.skip();
		if (!j.ok) return false;
	}
	if (!s.durationMs) s.durationMs = songDuration;
	s.seq = uint32_t(seq);
	j.ws();
	return j.ok && j.p == j.end && version == 1 && !strcmp(type, "nowplaying");
}

// ---- Text metrics for GNU Unifont (8 px half-width, 16 px wide glyphs) ----

static inline uint32_t utf8Next(const char *s, size_t len, size_t &i)
{
	unsigned char c = (unsigned char)s[i++];
	if (c < 0x80) return c;
	size_t need;
	uint32_t cp;
	if (c >= 0xf0 && c <= 0xf4) { need = 3; cp = c & 7u; }
	else if (c >= 0xe0 && c < 0xf0) { need = 2; cp = c & 15u; }
	else if (c >= 0xc2 && c < 0xe0) { need = 1; cp = c & 31u; }
	else return 0xfffd;
	for (size_t k = 0; k < need; ++k) {
		if (i >= len || ((unsigned char)s[i] & 0xc0) != 0x80) return 0xfffd;
		cp = (cp << 6) | ((unsigned char)s[i++] & 63u);
	}
	return cp;
}

static inline int musicGlyphWidth(uint32_t cp)
{
	if (cp < 0x1100) return 8;
	bool wide = cp <= 0x115f || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
		(cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) ||
		(cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) || cp >= 0x20000;
	return wide ? 16 : 8;
}

static inline int musicTextWidth(const char *s, size_t len)
{
	int w = 0;
	for (size_t i = 0; i < len;) w += musicGlyphWidth(utf8Next(s, len, i));
	return w;
}

// One display row. width is the full run; startPx is where it begins on the
// unwrapped line (wrapped rows only). ellipsis is only used by musicWrap.
struct MusicRow { uint16_t off, len; int16_t width, startPx; bool ellipsis; };

// Horizontal placement inside [origin, origin+viewW). Short text is centered.
// Long text pauses, scrolls left, pauses, then repeats after `gap` pixels.
// follow keeps focusPx near the left third and does not repeat.
struct MusicScroll { int x, repeat; };

static inline MusicScroll musicScrollX(int width, int viewW, int origin, int64_t elapsedMs, bool follow, int focusPx)
{
	MusicScroll s{origin, 0x7fffffff};
	if (width <= viewW) { s.x = origin + (viewW - width) / 2; return s; }
	if (follow) {
		int x = origin + viewW * 2 / 5 - focusPx;
		int minX = origin + viewW - width;
		if (x < minX) x = minX;
		if (x > origin) x = origin;
		s.x = x;
		return s;
	}
	const int gap = 48, speed = 24, pause = 800;
	const int travel = width + gap;
	const int scrollMs = travel * 1000 / speed;
	const int cycle = pause + scrollMs + pause;
	int t = cycle > 0 ? int(elapsedMs % cycle) : 0;
	if (t < 0) t += cycle;
	int shift = t < pause ? 0 : t < pause + scrollMs ? (t - pause) * speed / 1000 : travel;
	s.x = origin - shift;
	s.repeat = s.x + travel;
	return s;
}

// One pass with no repeat: hold the start, slide so the end is reached by the
// end of the line's time span, then hold the end.
static inline int musicProgressX(int width, int viewW, int origin, int64_t elapsedMs, int64_t durationMs)
{
	if (width <= viewW) return origin + (viewW - width) / 2;
	if (durationMs <= 0) return origin;
	int64_t percent = elapsedMs * 100 / durationMs - 15;
	if (percent < 0) percent = 0;
	if (percent > 70) percent = 70;
	return origin - int((width - viewW) * percent / 70);
}

// Place a second overflowing line so it shows the same fraction as a lead line.
static inline int musicLinkedX(int width, int viewW, int origin, int leadWidth, int leadX)
{
	if (width <= viewW) return origin + (viewW - width) / 2;
	int leadOverflow = leadWidth - viewW;
	if (leadOverflow <= 0) return origin;
	int shift = origin - leadX;
	if (shift < 0) shift = 0;
	if (shift > leadOverflow) shift = leadOverflow;
	return origin - shift * (width - viewW) / leadOverflow;
};

static inline int musicWrap(const char *s, size_t len, int maxWidth, MusicRow *rows, int maxRows)
{
	int count = 0, absolute = 0;
	size_t i = 0;
	while (i < len && count < maxRows) {
		while (i < len && s[i] == ' ') { ++i; absolute += 8; }
		if (i >= len) break;
		size_t start = i, lastSpace = 0;
		int w = 0, widthAtSpace = 0;
		while (i < len) {
			size_t next = i;
			uint32_t cp = utf8Next(s, len, next);
			int cw = musicGlyphWidth(cp);
			if (w + cw > maxWidth && i > start) break;
			if (cp == ' ') { lastSpace = i; widthAtSpace = w; }
			w += cw;
			i = next;
		}
		// Break Latin words at the last space instead of mid-word.
		if (i < len && s[i] != ' ' && (unsigned char)s[i] < 0x80 && i > start &&
			(unsigned char)s[i - 1] < 0x80 && s[i - 1] != ' ' && lastSpace > start) {
			i = lastSpace; w = widthAtSpace;
		}
		size_t end = i;
		int trimmed = w;
		while (end > start && s[end - 1] == ' ') { --end; trimmed -= 8; }
		rows[count++] = {uint16_t(start), uint16_t(end - start), int16_t(trimmed), int16_t(absolute), false};
		absolute += w;
	}
	while (i < len && s[i] == ' ') ++i;
	if (i < len && count) {
		MusicRow &last = rows[count - 1];
		const char *row = s + last.off;
		size_t keep = 0;
		int w = 0;
		while (keep < last.len) {
			size_t next = keep;
			int cw = musicGlyphWidth(utf8Next(row, last.len, next));
			if (w + cw + 24 > maxWidth) break;
			w += cw;
			keep = next;
		}
		last.len = uint16_t(keep);
		last.width = int16_t(w + 24);
		last.ellipsis = true;
	}
	return count;
}

// Highlight progress in pixels along the unwrapped line. -1 means the whole
// line is highlighted (no word timing available).
static inline int musicKaraokePx(const MusicSnapshot &s, const MusicLine &line, int64_t pos)
{
	if (!line.wordCount) return -1;
	const char *text = s.text(line);
	const MusicWord *w = s.words + line.word0;
	if (pos < w[0].start) return 0;
	for (int k = 0; k < line.wordCount; ++k) {
		if (pos >= w[k].end) continue;
		size_t off = w[k].off < line.textLen ? w[k].off : line.textLen;
		size_t len = off + w[k].len <= line.textLen ? w[k].len : line.textLen - off;
		int before = musicTextWidth(text, off);
		if (pos < w[k].start) return before;
		int span = musicTextWidth(text + off, len);
		int64_t dur = w[k].end - w[k].start;
		return before + (dur > 0 ? int(span * (pos - w[k].start) / dur) : span);
	}
	return musicTextWidth(text, line.textLen);
}

// Index of the line containing pos, or -1 before the first line.
static inline int musicCurrentLine(const MusicSnapshot &s, int64_t pos)
{
	int current = -1;
	for (int i = 0; i < s.lineCount; ++i)
		if (s.lines[i].start <= pos) current = i;
	return current;
}

#endif
