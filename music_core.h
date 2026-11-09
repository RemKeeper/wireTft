#ifndef WIRETFT_MUSIC_CORE_H
#define WIRETFT_MUSIC_CORE_H
// Music display logic without ESP-IDF or Arduino dependencies (host-testable).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------- UTF-8 ---------------- */

static inline size_t utf8Encode(uint32_t cp, char out[4])
{
	if (cp < 0x80) { out[0] = char(cp); return 1; }
	if (cp < 0x800) { out[0] = char(0xc0 | cp >> 6); out[1] = char(0x80 | (cp & 63)); return 2; }
	if (cp < 0x10000) {
		out[0] = char(0xe0 | cp >> 12); out[1] = char(0x80 | ((cp >> 6) & 63)); out[2] = char(0x80 | (cp & 63));
		return 3;
	}
	out[0] = char(0xf0 | cp >> 18); out[1] = char(0x80 | ((cp >> 12) & 63));
	out[2] = char(0x80 | ((cp >> 6) & 63)); out[3] = char(0x80 | (cp & 63));
	return 4;
}

// Decodes one code point; invalid bytes yield U+FFFD and advance one byte.
static inline uint32_t utf8Next(const char *s, size_t len, size_t &i)
{
	const unsigned char *p = (const unsigned char *)s + i;
	const size_t left = len - i;
	unsigned count;
	uint32_t cp;
	if (p[0] < 0x80) { ++i; return p[0]; }
	if (p[0] >= 0xc2 && p[0] <= 0xdf) { count = 1; cp = p[0] & 31; }
	else if (p[0] >= 0xe0 && p[0] <= 0xef) { count = 2; cp = p[0] & 15; }
	else if (p[0] >= 0xf0 && p[0] <= 0xf4) { count = 3; cp = p[0] & 7; }
	else { ++i; return 0xfffd; }
	if (left <= count) { ++i; return 0xfffd; }
	for (unsigned k = 1; k <= count; ++k) {
		if ((p[k] & 0xc0) != 0x80) { ++i; return 0xfffd; }
		cp = cp << 6 | (p[k] & 63);
	}
	if ((count == 2 && cp < 0x800) || (count == 3 && (cp < 0x10000 || cp > 0x10ffff)) ||
		(cp >= 0xd800 && cp <= 0xdfff)) { ++i; return 0xfffd; }
	i += count + 1;
	return cp;
}

static inline size_t utf8SeqLen(const char *s, size_t left)
{
	size_t i = 0;
	return utf8Next(s, left, i) == 0xfffd && i == 1 ? 1 : i;
}

/* ---------------- JSON reader (in place, no allocation) ---------------- */

struct JsonReader {
	const char *p;
	const char *end;
	bool ok;
	int depth;
};

static inline JsonReader jsonReader(const char *text, size_t len) { return JsonReader{text, text + len, true, 0}; }

static inline bool jsonFail(JsonReader &j) { j.ok = false; return false; }

static inline void jsonWs(JsonReader &j)
{
	while (j.p < j.end && (*j.p == ' ' || *j.p == '\t' || *j.p == '\n' || *j.p == '\r')) ++j.p;
}

static inline bool jsonEat(JsonReader &j, char c)
{
	jsonWs(j);
	if (!j.ok || j.p >= j.end || *j.p != c) return false;
	++j.p;
	return true;
}

static inline bool jsonLiteral(JsonReader &j, const char *word)
{
	jsonWs(j);
	const size_t n = strlen(word);
	if (size_t(j.end - j.p) < n || memcmp(j.p, word, n)) return jsonFail(j);
	j.p += n;
	return true;
}

static inline bool jsonIsNull(JsonReader &j)
{
	jsonWs(j);
	return j.ok && j.end - j.p >= 4 && !memcmp(j.p, "null", 4) && (j.p += 4);
}

static inline bool jsonHex4(JsonReader &j, uint32_t &value)
{
	if (j.end - j.p < 4) return false;
	value = 0;
	for (int k = 0; k < 4; ++k) {
		const char c = *j.p++;
		value <<= 4;
		if (c >= '0' && c <= '9') value |= uint32_t(c - '0');
		else if (c >= 'a' && c <= 'f') value |= uint32_t(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') value |= uint32_t(c - 'A' + 10);
		else return false;
	}
	return true;
}

// Output (may be null) is NUL-terminated and truncated on a code point boundary.
static inline bool jsonString(JsonReader &j, char *out, size_t cap, size_t *outLen = nullptr, bool *truncated = nullptr)
{
	if (!jsonEat(j, '"')) return jsonFail(j);
	size_t n = 0;
	bool full = false;
	auto put = [&](const char *bytes, size_t k) {
		if (!out || full) { full = full || out; return; }
		if (n + k >= cap) { full = true; return; }
		memcpy(out + n, bytes, k);
		n += k;
	};
	while (j.p < j.end) {
		const unsigned char c = (unsigned char)*j.p;
		if (c == '"') {
			++j.p;
			if (out && cap) out[n] = 0;
			if (outLen) *outLen = n;
			if (truncated) *truncated = full;
			return true;
		}
		if (c < 0x20) return jsonFail(j);
		if (c != '\\') {
			const size_t k = utf8SeqLen(j.p, size_t(j.end - j.p));
			put(j.p, k);
			j.p += k;
			continue;
		}
		if (++j.p >= j.end) break;
		const char e = *j.p++;
		char tmp[4];
		if (e == '"' || e == '\\' || e == '/') put(&e, 1);
		else if (e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') put(" ", 1);
		else if (e == 'u') {
			uint32_t cp;
			if (!jsonHex4(j, cp)) return jsonFail(j);
			if (cp >= 0xd800 && cp < 0xdc00) {
				const char *save = j.p;
				uint32_t low;
				if (j.end - j.p >= 6 && j.p[0] == '\\' && j.p[1] == 'u' && (j.p += 2, jsonHex4(j, low)) &&
					low >= 0xdc00 && low < 0xe000) cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
				else { j.p = save; cp = 0xfffd; }
			} else if (cp >= 0xdc00 && cp < 0xe000) cp = 0xfffd;
			if (cp < 0x20) cp = ' ';
			put(tmp, utf8Encode(cp, tmp));
		} else return jsonFail(j);
	}
	return jsonFail(j);
}

// Integer part of a JSON number; fraction and exponent are accepted and ignored.
static inline bool jsonInt(JsonReader &j, int64_t &value)
{
	jsonWs(j);
	const char *p = j.p;
	const bool negative = p < j.end && *p == '-';
	if (negative) ++p;
	if (p >= j.end || *p < '0' || *p > '9') return jsonFail(j);
	int64_t x = 0;
	for (; p < j.end && *p >= '0' && *p <= '9'; ++p)
		if (x < (INT64_MAX - 9) / 10) x = x * 10 + (*p - '0');
	if (p < j.end && *p == '.') for (++p; p < j.end && *p >= '0' && *p <= '9'; ++p) {}
	if (p < j.end && (*p == 'e' || *p == 'E')) {
		++p;
		if (p < j.end && (*p == '+' || *p == '-')) ++p;
		for (; p < j.end && *p >= '0' && *p <= '9'; ++p) {}
	}
	j.p = p;
	value = negative ? -x : x;
	return true;
}

static inline bool jsonBool(JsonReader &j, bool &value)
{
	jsonWs(j);
	if (j.p < j.end && *j.p == 't') { value = true; return jsonLiteral(j, "true"); }
	value = false;
	if (j.p < j.end && *j.p == 'f') return jsonLiteral(j, "false");
	return jsonLiteral(j, "null");
}

// Usage: bool first = true; while (jsonObjectNext(j, first, key, n)) { read value }
static inline bool jsonObjectNext(JsonReader &j, bool &first, char *key, size_t cap)
{
	if (!j.ok) return false;
	if (first) {
		first = false;
		if (!jsonEat(j, '{')) return jsonFail(j);
		if (jsonEat(j, '}')) return false;
	} else {
		if (jsonEat(j, '}')) return false;
		if (!jsonEat(j, ',')) return jsonFail(j);
	}
	if (!jsonString(j, key, cap)) return false;
	if (!jsonEat(j, ':')) return jsonFail(j);
	return true;
}

static inline bool jsonArrayNext(JsonReader &j, bool &first)
{
	if (!j.ok) return false;
	if (first) {
		first = false;
		if (!jsonEat(j, '[')) return jsonFail(j);
		return !jsonEat(j, ']') && j.ok;
	}
	if (jsonEat(j, ']')) return false;
	if (!jsonEat(j, ',')) return jsonFail(j);
	return true;
}

static inline bool jsonSkip(JsonReader &j)
{
	jsonWs(j);
	if (!j.ok || j.p >= j.end) return jsonFail(j);
	const char c = *j.p;
	if (c == '"') return jsonString(j, nullptr, 0);
	if (c == '{' || c == '[') {
		if (++j.depth > 16) return jsonFail(j);
		bool first = true;
		if (c == '{') { char key[1]; while (jsonObjectNext(j, first, key, 0)) if (!jsonSkip(j)) return false; }
		else while (jsonArrayNext(j, first)) if (!jsonSkip(j)) return false;
		--j.depth;
		return j.ok;
	}
	if (c == 't') return jsonLiteral(j, "true");
	if (c == 'f') return jsonLiteral(j, "false");
	if (c == 'n') return jsonLiteral(j, "null");
	int64_t ignored;
	return jsonInt(j, ignored);
}

/* ---------------- Now-playing snapshot ---------------- */

static constexpr int MUSIC_MAX_LINES = 24;
static constexpr int MUSIC_MAX_WORDS = 320;
static constexpr int MUSIC_POOL = 3072;
static constexpr size_t MUSIC_LINE_TEXT = 192;

enum MusicPlayState : uint8_t { MUSIC_PLAY_UNKNOWN, MUSIC_PLAY_PLAYING, MUSIC_PLAY_PAUSED, MUSIC_PLAY_STOPPED };

struct MusicWord { int32_t start, end; uint16_t off, len; };
// text/trans are offsets into pool; word offsets are byte offsets inside text.
struct MusicLine { int32_t index, start, end; uint16_t text, textLen, trans, transLen, word0, wordCount; };

struct MusicSnapshot {
	uint32_t seq;
	bool playing;
	uint8_t state;
	int64_t positionMs, durationMs, receivedUs;
	int64_t songId;
	char name[96], artists[96], coverUrl[96];
	uint16_t lineCount, wordCount, poolUsed;
	MusicLine lines[MUSIC_MAX_LINES];
	MusicWord words[MUSIC_MAX_WORDS];
	char pool[MUSIC_POOL];
};

enum { MUSIC_PARSE_OK = 0, MUSIC_PARSE_INVALID = -1, MUSIC_PARSE_VERSION = -2 };

static inline int32_t musicClamp32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : int32_t(v); }

static inline uint16_t musicPoolAdd(MusicSnapshot &s, const char *text, size_t len, uint16_t &stored)
{
	stored = 0;
	const size_t room = MUSIC_POOL - s.poolUsed;
	if (!len || room < 2) return 0;
	if (len > room - 1) {
		len = room - 1;
		while (len && (((unsigned char)text[len]) & 0xc0) == 0x80) --len;
	}
	const uint16_t off = s.poolUsed;
	memcpy(s.pool + off, text, len);
	s.pool[off + len] = 0;
	s.poolUsed = uint16_t(off + len + 1);
	stored = uint16_t(len);
	return off;
}

static inline bool musicParseWords(JsonReader &j, MusicSnapshot &s, uint16_t &count, uint32_t &bytes, bool &complete)
{
	bool first = true;
	while (jsonArrayNext(j, first)) {
		if (jsonIsNull(j)) continue;
		MusicWord word{};
		size_t len = 0;
		bool cut = false, fieldFirst = true;
		char key[16];
		while (jsonObjectNext(j, fieldFirst, key, sizeof(key))) {
			int64_t v;
			if (!strcmp(key, "startMs")) { if (!jsonInt(j, v)) return false; word.start = musicClamp32(v); }
			else if (!strcmp(key, "endMs")) { if (!jsonInt(j, v)) return false; word.end = musicClamp32(v); }
			else if (!strcmp(key, "text")) { char text[64]; if (!jsonString(j, text, sizeof(text), &len, &cut)) return false; }
			else if (!jsonSkip(j)) return false;
		}
		if (!j.ok) return false;
		if (word.end < word.start) word.end = word.start;
		if (cut || s.wordCount >= MUSIC_MAX_WORDS || bytes + len > 0xffff) complete = false;
		else {
			word.off = uint16_t(bytes);
			word.len = uint16_t(len);
			s.words[s.wordCount++] = word;
			++count;
		}
		bytes += uint32_t(len);
	}
	return j.ok;
}

static inline bool musicParseLine(JsonReader &j, MusicSnapshot &s)
{
	MusicLine line{};
	line.index = -1;
	char text[MUSIC_LINE_TEXT] = "", trans[MUSIC_LINE_TEXT] = "", key[16];
	size_t textLen = 0, transLen = 0;
	bool textCut = false, wordsComplete = true;
	const uint16_t word0 = s.wordCount;
	uint16_t wordCount = 0;
	uint32_t wordBytes = 0;
	bool first = true;
	while (jsonObjectNext(j, first, key, sizeof(key))) {
		int64_t v;
		if (!strcmp(key, "index")) { if (!jsonInt(j, v)) return false; line.index = musicClamp32(v); }
		else if (!strcmp(key, "startMs")) { if (!jsonInt(j, v)) return false; line.start = musicClamp32(v); }
		else if (!strcmp(key, "endMs")) { if (!jsonInt(j, v)) return false; line.end = musicClamp32(v); }
		else if (!strcmp(key, "text")) { if (!jsonIsNull(j) && !jsonString(j, text, sizeof(text), &textLen, &textCut)) return false; }
		else if (!strcmp(key, "translation")) { if (!jsonIsNull(j) && !jsonString(j, trans, sizeof(trans), &transLen)) return false; }
		else if (!strcmp(key, "words")) { if (!jsonIsNull(j) && !musicParseWords(j, s, wordCount, wordBytes, wordsComplete)) return false; }
		else if (!jsonSkip(j)) return false;
	}
	if (!j.ok) return false;
	if (s.lineCount >= MUSIC_MAX_LINES) { s.wordCount = word0; return true; }
	line.text = musicPoolAdd(s, text, textLen, line.textLen);
	line.trans = musicPoolAdd(s, trans, transLen, line.transLen);
	// Word offsets are only meaningful when the words concatenate to the stored text.
	if (textCut || !wordsComplete || wordBytes != line.textLen) { s.wordCount = word0; wordCount = 0; }
	line.word0 = word0;
	line.wordCount = wordCount;
	s.lines[s.lineCount++] = line;
	return true;
}

static inline bool musicParseSong(JsonReader &j, MusicSnapshot &s, int64_t &songDuration)
{
	bool first = true;
	char key[16];
	while (jsonObjectNext(j, first, key, sizeof(key))) {
		if (!strcmp(key, "id")) { if (!jsonInt(j, s.songId)) return false; }
		else if (!strcmp(key, "durationMs")) { if (!jsonInt(j, songDuration)) return false; }
		else if (!strcmp(key, "name")) { if (!jsonIsNull(j) && !jsonString(j, s.name, sizeof(s.name))) return false; }
		else if (!strcmp(key, "artists")) { if (!jsonIsNull(j) && !jsonString(j, s.artists, sizeof(s.artists))) return false; }
		else if (!strcmp(key, "coverUrl")) { if (!jsonIsNull(j) && !jsonString(j, s.coverUrl, sizeof(s.coverUrl))) return false; }
		else if (!jsonSkip(j)) return false;
	}
	return j.ok;
}

static inline int musicParseSnapshot(const char *json, size_t len, MusicSnapshot &s)
{
	s.seq = 0; s.playing = false; s.state = MUSIC_PLAY_UNKNOWN;
	s.positionMs = s.durationMs = s.receivedUs = 0; s.songId = 0;
	s.name[0] = s.artists[0] = s.coverUrl[0] = 0;
	s.lineCount = s.wordCount = 0;
	s.pool[0] = 0; s.poolUsed = 1;
	JsonReader j = jsonReader(json, len);
	char key[16], type[24] = "", state[16] = "";
	int64_t version = 1, songDuration = 0, v;
	bool first = true;
	while (jsonObjectNext(j, first, key, sizeof(key))) {
		if (!strcmp(key, "type")) { if (!jsonString(j, type, sizeof(type))) break; }
		else if (!strcmp(key, "version")) { if (!jsonInt(j, version)) break; }
		else if (!strcmp(key, "seq")) { if (!jsonInt(j, v)) break; s.seq = uint32_t(v); }
		else if (!strcmp(key, "playing")) { if (!jsonBool(j, s.playing)) break; }
		else if (!strcmp(key, "state")) { if (!jsonString(j, state, sizeof(state))) break; }
		else if (!strcmp(key, "positionMs")) { if (!jsonInt(j, s.positionMs)) break; }
		else if (!strcmp(key, "durationMs")) { if (!jsonInt(j, s.durationMs)) break; }
		else if (!strcmp(key, "song")) { if (!jsonIsNull(j) && !musicParseSong(j, s, songDuration)) break; }
		else if (!strcmp(key, "lyric")) {
			if (jsonIsNull(j)) continue;
			bool lineFirst = true;
			while (jsonArrayNext(j, lineFirst)) if (!jsonIsNull(j) && !musicParseLine(j, s)) break;
		} else if (!jsonSkip(j)) break;
	}
	jsonWs(j);
	if (!j.ok || j.p != j.end) return MUSIC_PARSE_INVALID;
	if (type[0] && strcmp(type, "nowplaying")) return MUSIC_PARSE_INVALID;
	if (version != 1) return MUSIC_PARSE_VERSION;
	if (!strcmp(state, "playing")) s.state = MUSIC_PLAY_PLAYING;
	else if (!strcmp(state, "paused")) s.state = MUSIC_PLAY_PAUSED;
	else if (!strcmp(state, "stopped")) s.state = MUSIC_PLAY_STOPPED;
	if (s.durationMs <= 0) s.durationMs = songDuration;
	return MUSIC_PARSE_OK;
}

/* ---------------- Timing ---------------- */

static inline int64_t musicPosition(const MusicSnapshot &s, int64_t nowUs)
{
	int64_t pos = s.positionMs;
	if (s.playing && nowUs > s.receivedUs) pos += (nowUs - s.receivedUs) / 1000;
	if (s.durationMs > 0 && pos > s.durationMs) pos = s.durationMs;
	return pos < 0 ? 0 : pos;
}

// Index into s.lines of the line being sung, or -1 before the first line.
static inline int musicCurrentLine(const MusicSnapshot &s, int64_t pos)
{
	int current = -1;
	for (int i = 0; i < s.lineCount; ++i)
		if (s.lines[i].start <= pos && (current < 0 || s.lines[i].start >= s.lines[current].start)) current = i;
	return current;
}

/* ---------------- Layout ---------------- */

static constexpr int MUSIC_MAX_ROWS = 64;
static constexpr int MUSIC_TEXT_ROW = 20, MUSIC_TRANS_ROW = 18, MUSIC_LINE_GAP = 4;
static constexpr int MUSIC_TEXT_ROWS_PER_LINE = 3, MUSIC_TRANS_ROWS_PER_LINE = 2;

typedef uint8_t (*MusicAdvance)(uint32_t cp);

// startPx is measured along the unwrapped line so karaoke progress maps onto any row.
struct MusicRow { int16_t y; uint8_t line; uint8_t translation; uint16_t off, len, startPx, width; };

struct MusicLayout {
	uint16_t rowCount;
	int16_t height;
	MusicRow rows[MUSIC_MAX_ROWS];
	int16_t lineTop[MUSIC_MAX_LINES], lineHeight[MUSIC_MAX_LINES];
	uint16_t wordPx[MUSIC_MAX_WORDS], wordWidth[MUSIC_MAX_WORDS];
};

static inline uint32_t musicTextWidth(const char *text, size_t from, size_t to, MusicAdvance advance)
{
	uint32_t width = 0;
	for (size_t i = from; i < to;) width += advance(utf8Next(text, to, i));
	return width;
}

// Greedy wrap that prefers breaking after a space; rows beyond maxRows are dropped.
static inline int musicWrap(const char *text, size_t len, int maxWidth, MusicAdvance advance, MusicRow *rows, int maxRows)
{
	int count = 0;
	size_t i = 0;
	uint32_t px = 0;
	while (i < len && count < maxRows) {
		const size_t rowStart = i;
		size_t j = i, spaceEnd = 0;
		uint32_t width = 0, spaceWidth = 0;
		while (j < len) {
			size_t k = j;
			const uint32_t cp = utf8Next(text, len, k);
			const uint8_t a = advance(cp);
			if (width + a > uint32_t(maxWidth) && j > rowStart) break;
			width += a;
			j = k;
			if (cp == ' ') { spaceEnd = j; spaceWidth = width; }
		}
		size_t next = j;
		if (j < len && spaceEnd > rowStart && spaceEnd < j) { next = spaceEnd; width = spaceWidth; }
		size_t shown = next;
		uint32_t shownWidth = width;
		while (shown > rowStart && text[shown - 1] == ' ') { --shown; shownWidth -= advance(' '); }
		MusicRow &row = rows[count++];
		row = MusicRow{};
		row.off = uint16_t(rowStart);
		row.len = uint16_t(shown - rowStart);
		row.startPx = uint16_t(px);
		row.width = uint16_t(shownWidth);
		px += width;
		i = next;
	}
	return count;
}

static inline void musicLayout(const MusicSnapshot &s, bool translation, int maxWidth, MusicAdvance advance, MusicLayout &out)
{
	out.rowCount = 0;
	int y = 0;
	for (int i = 0; i < s.lineCount; ++i) {
		const MusicLine &line = s.lines[i];
		const char *text = s.pool + line.text;
		out.lineTop[i] = int16_t(y);
		MusicRow rows[MUSIC_TEXT_ROWS_PER_LINE];
		int n = musicWrap(text, line.textLen, maxWidth, advance, rows, MUSIC_TEXT_ROWS_PER_LINE);
		if (!n) { rows[0] = MusicRow{}; n = 1; }
		for (int r = 0; r < n; ++r, y += MUSIC_TEXT_ROW) {
			if (out.rowCount >= MUSIC_MAX_ROWS) continue;
			rows[r].y = int16_t(y);
			rows[r].line = uint8_t(i);
			out.rows[out.rowCount++] = rows[r];
		}
		if (translation && line.transLen) {
			n = musicWrap(s.pool + line.trans, line.transLen, maxWidth, advance, rows, MUSIC_TRANS_ROWS_PER_LINE);
			for (int r = 0; r < n; ++r, y += MUSIC_TRANS_ROW) {
				if (out.rowCount >= MUSIC_MAX_ROWS) continue;
				rows[r].y = int16_t(y);
				rows[r].line = uint8_t(i);
				rows[r].translation = 1;
				out.rows[out.rowCount++] = rows[r];
			}
		}
		out.lineHeight[i] = int16_t(y - out.lineTop[i]);
		y += MUSIC_LINE_GAP;
		for (uint16_t k = 0; k < line.wordCount; ++k) {
			const MusicWord &w = s.words[line.word0 + k];
			out.wordPx[line.word0 + k] = uint16_t(musicTextWidth(text, 0, w.off, advance));
			out.wordWidth[line.word0 + k] = uint16_t(musicTextWidth(text, w.off, w.off + w.len, advance));
		}
	}
	out.height = int16_t(y);
}

// Sung distance in pixels along the unwrapped line; -1 when the line has no word timing.
static inline int32_t musicSungPx(const MusicSnapshot &s, const MusicLayout &l, int lineIndex, int64_t pos)
{
	const MusicLine &line = s.lines[lineIndex];
	if (!line.wordCount) return -1;
	int32_t px = 0;
	for (uint16_t k = 0; k < line.wordCount; ++k) {
		const int w = line.word0 + k;
		const MusicWord &word = s.words[w];
		if (pos < word.start) break;
		if (pos >= word.end || word.end <= word.start) px = l.wordPx[w] + l.wordWidth[w];
		else px = l.wordPx[w] + int32_t((pos - word.start) * l.wordWidth[w] / (word.end - word.start));
	}
	return px;
}

/* ---------------- Server-Sent Events ---------------- */

struct SseParser {
	char *data;
	size_t cap, len;
	char field[8], event[16];
	uint8_t fieldLen, eventLen;
	uint8_t mode;
	bool skipSpace, lineEmpty, lastCR, overflow, hasData, ready;
	uint32_t dropped;
};

enum { SSE_FIELD, SSE_DATA, SSE_EVENT, SSE_SKIP };

static inline void sseClearEvent(SseParser &p)
{
	p.len = 0; p.eventLen = 0; p.event[0] = 0;
	p.hasData = p.overflow = p.ready = false;
}

static inline void sseInit(SseParser &p, char *buffer, size_t cap)
{
	memset(&p, 0, sizeof(p));
	p.data = buffer;
	p.cap = cap;
	p.lineEmpty = true;
}

static inline void sseAppend(SseParser &p, char c)
{
	if (p.len + 1 >= p.cap) p.overflow = true;
	else p.data[p.len++] = c;
}

static inline bool sseEndLine(SseParser &p)
{
	bool dispatch = false;
	if (p.lineEmpty) {
		p.event[p.eventLen] = 0;
		const bool wanted = p.hasData && (!p.eventLen || !strcmp(p.event, "nowplaying"));
		if (wanted && p.overflow) ++p.dropped;
		dispatch = wanted && !p.overflow;
		if (dispatch) { p.data[p.len] = 0; p.ready = true; }
		else sseClearEvent(p);
	} else if (p.mode == SSE_FIELD) {
		p.field[p.fieldLen] = 0;
		if (!strcmp(p.field, "data")) { if (p.hasData) sseAppend(p, '\n'); p.hasData = true; }
	}
	p.mode = SSE_FIELD;
	p.fieldLen = 0;
	p.lineEmpty = true;
	p.skipSpace = false;
	return dispatch;
}

// Returns true when a complete nowplaying event is in p.data (valid until the next feed).
static inline bool sseFeed(SseParser &p, char c)
{
	if (p.ready) sseClearEvent(p);
	if (c == '\n' && p.lastCR) { p.lastCR = false; return false; }
	p.lastCR = c == '\r';
	if (c == '\r' || c == '\n') return sseEndLine(p);
	p.lineEmpty = false;
	if (p.mode == SSE_FIELD) {
		if (c != ':') {
			if (p.fieldLen + 1u < sizeof(p.field)) p.field[p.fieldLen++] = c;
			else p.mode = SSE_SKIP;
			return false;
		}
		p.field[p.fieldLen] = 0;
		p.skipSpace = true;
		if (!strcmp(p.field, "data")) {
			if (p.hasData) sseAppend(p, '\n');
			p.hasData = true;
			p.mode = SSE_DATA;
		} else if (!strcmp(p.field, "event")) {
			p.eventLen = 0;
			p.mode = SSE_EVENT;
		} else p.mode = SSE_SKIP;
		return false;
	}
	const bool skip = p.skipSpace && c == ' ';
	p.skipSpace = false;
	if (skip) return false;
	if (p.mode == SSE_DATA) sseAppend(p, c);
	else if (p.mode == SSE_EVENT && p.eventLen + 1u < sizeof(p.event)) p.event[p.eventLen++] = c;
	return false;
}

/* ---------------- Server address ---------------- */

static inline bool musicPrivate(uint32_t ip)
{
	return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8;
}

// Strict "a.b.c.d:port" with an RFC1918 address; ip is in host byte order.
static inline bool musicParseServer(const char *s, uint32_t &ip, uint16_t &port)
{
	uint32_t value = 0;
	for (int part = 0; part < 4; ++part) {
		if (*s < '0' || *s > '9' || (*s == '0' && s[1] >= '0' && s[1] <= '9')) return false;
		uint32_t octet = 0;
		for (int digits = 0; *s >= '0' && *s <= '9'; ++s)
			if (++digits > 3 || (octet = octet * 10 + uint32_t(*s - '0')) > 255) return false;
		value = value << 8 | octet;
		if (part < 3 && *s++ != '.') return false;
	}
	if (*s++ != ':' || *s < '1' || *s > '9') return false;
	uint32_t p = 0;
	for (int digits = 0; *s >= '0' && *s <= '9'; ++s)
		if (++digits > 5 || (p = p * 10 + uint32_t(*s - '0')) > 65535) return false;
	if (*s || !musicPrivate(value)) return false;
	ip = value;
	port = uint16_t(p);
	return true;
}

#endif
