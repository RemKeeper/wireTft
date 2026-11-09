// Host test for music_json.h:
//   g++ -std=c++17 -Wall -Wextra -I. tests/music_parse_test.cpp -o /tmp/music_test && /tmp/music_test
#include "music_json.h"
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static MusicSnapshot snap;

static bool parse(const char *json) { return musicParse(json, strlen(json), snap); }

int main()
{
	const char *doc =
		"{\"type\":\"nowplaying\",\"version\":1,\"seq\":12,\"playing\":true,\"state\":\"playing\","
		"\"positionMs\":12345,\"durationMs\":215000,\"updatedAtMs\":1690000000000,"
		"\"song\":{\"id\":123,\"name\":\"\\u6b4c\\u540d\",\"artists\":\"歌手A/歌手B\",\"album\":\"专辑\","
		"\"durationMs\":215000,\"picUrl\":\"http://x\",\"coverUrl\":\"/cover\"},"
		"\"lyric\":[{\"index\":8,\"startMs\":11000,\"endMs\":14000,\"text\":\"上一行\",\"translation\":\"…\"},"
		"{\"index\":9,\"startMs\":14000,\"endMs\":17500,\"text\":\"当前行\",\"translation\":\"\","
		"\"words\":[{\"startMs\":14000,\"endMs\":14700,\"text\":\"当\"},{\"startMs\":14700,\"endMs\":17500,\"text\":\"前行\"}]}]}";
	CHECK(parse(doc));
	CHECK(snap.seq == 12 && snap.playing && !strcmp(snap.state, "playing"));
	CHECK(snap.positionMs == 12345 && snap.durationMs == 215000 && snap.songId == 123);
	CHECK(!strcmp(snap.name, "歌名") && !strcmp(snap.artists, "歌手A/歌手B") && !strcmp(snap.coverUrl, "/cover"));
	CHECK(snap.lineCount == 2 && snap.lines[1].index == 9 && snap.lines[1].start == 14000);
	CHECK(!strcmp(snap.text(snap.lines[0]), "上一行") && !strcmp(snap.translation(snap.lines[0]), "…"));
	CHECK(snap.lines[1].wordCount == 2 && snap.words[snap.lines[1].word0 + 1].off == 3);
	CHECK(musicCurrentLine(snap, 13000) == 0 && musicCurrentLine(snap, 14000) == 1 && musicCurrentLine(snap, 100) == -1);
	CHECK(musicKaraokePx(snap, snap.lines[0], 12000) == -1);
	CHECK(musicKaraokePx(snap, snap.lines[1], 13000) == 0);
	CHECK(musicKaraokePx(snap, snap.lines[1], 14350) == 8);
	CHECK(musicKaraokePx(snap, snap.lines[1], 14700) == 16);
	CHECK(musicKaraokePx(snap, snap.lines[1], 16100) == 32);
	CHECK(musicKaraokePx(snap, snap.lines[1], 20000) == 48);

	// Protocol guards, empty lyric, nulls, unknown nested fields.
	CHECK(!parse("{\"type\":\"nowplaying\",\"version\":2}"));
	CHECK(!parse("{\"type\":\"other\",\"version\":1}"));
	CHECK(!parse("{\"type\":\"nowplaying\",\"version\":1"));
	CHECK(!parse("{\"type\":\"nowplaying\",\"version\":1} x"));
	CHECK(parse("{\"type\":\"nowplaying\",\"version\":1,\"x\":{\"a\":[1,2.5e3,{\"b\":null}]},\"song\":null,\"lyric\":[]}"));
	CHECK(snap.lineCount == 0 && snap.songId == 0);
	CHECK(parse("{\"type\":\"nowplaying\",\"version\":1,\"song\":{\"durationMs\":999}}") && snap.durationMs == 999);

	// Escapes: surrogate pair, control chars, lone surrogate.
	CHECK(parse("{\"type\":\"nowplaying\",\"version\":1,\"song\":{\"name\":\"a\\nb\\ud83c\\udfb5\\udc00\\\"\"}}"));
	CHECK(!strcmp(snap.name, "a b\xF0\x9F\x8E\xB5\xEF\xBF\xBD\""));

	// Truncation never splits a UTF-8 sequence.
	char longName[400] = "{\"type\":\"nowplaying\",\"version\":1,\"song\":{\"name\":\"";
	for (int i = 0; i < 40; ++i) strcat(longName, "汉");
	strcat(longName, "\"}}");
	CHECK(parse(longName));
	CHECK(strlen(snap.name) == 93 && JsonReader::utf8Complete(snap.name, strlen(snap.name)) == strlen(snap.name));

	// Too many lines are skipped, not fatal.
	char many[8192] = "{\"type\":\"nowplaying\",\"version\":1,\"lyric\":[";
	for (int i = 0; i < 30; ++i) {
		char row[96];
		snprintf(row, sizeof(row), "%s{\"index\":%d,\"startMs\":%d,\"text\":\"line %d\"}", i ? "," : "", i, i * 1000, i);
		strcat(many, row);
	}
	strcat(many, "]}");
	CHECK(parse(many) && snap.lineCount == MUSIC_MAX_LINES && !strcmp(snap.text(snap.lines[23]), "line 23"));

	// Width and wrapping.
	CHECK(musicTextWidth("ab汉", strlen("ab汉")) == 32);
	MusicRow rows[3];
	const char *cjk = "一二三四五六七八九十一二三四五六七八九十";
	int n = musicWrap(cjk, strlen(cjk), 232, rows, 3);
	CHECK(n == 2 && rows[0].width == 224 && rows[1].width == 96 && rows[1].startPx == 224 && !rows[1].ellipsis);
	const char *latin = "hello wonderful world of tiny screens";
	n = musicWrap(latin, strlen(latin), 100, rows, 3);
	CHECK(n == 3 && rows[0].len == 5 && rows[1].len == 9 && !memcmp(latin + rows[1].off, "wonderful", 9));
	CHECK(rows[2].ellipsis && rows[2].len == 8 && rows[2].width == 88);
	n = musicWrap("", 0, 232, rows, 3);
	CHECK(n == 0);
	// Short text stays centered. Long text pauses at the left edge, then scrolls.
	MusicScroll parked = musicScrollX(100, 228, 6, 0, false, 0);
	CHECK(parked.x == 6 + (228 - 100) / 2 && parked.repeat == 0x7fffffff);
	MusicScroll start = musicScrollX(400, 228, 6, 100, false, 0);
	CHECK(start.x == 6 && start.repeat == 6 + 400 + 48);
	MusicScroll moving = musicScrollX(400, 228, 6, 800 + 1000, false, 0);
	CHECK(moving.x == 6 - 24 && moving.repeat == moving.x + 448);
	MusicScroll head = musicScrollX(400, 228, 6, 0, true, 0);
	CHECK(head.x == 6 && head.repeat == 0x7fffffff);
	MusicScroll tail = musicScrollX(400, 228, 6, 0, true, 400);
	CHECK(tail.x == 6 + 228 - 400);
	CHECK(musicLinkedX(300, 228, 6, 400, tail.x) == 6 - (6 - tail.x) * (300 - 228) / (400 - 228));
	// Lyric progress scrolls once: hold, slide, hold the end; never past it.
	CHECK(musicProgressX(100, 228, 6, 500, 4000) == 6 + 64);
	CHECK(musicProgressX(400, 228, 6, 500, 4000) == 6);
	CHECK(musicProgressX(400, 228, 6, 2000, 4000) == 6 - 172 * 35 / 70);
	CHECK(musicProgressX(400, 228, 6, 3600, 4000) == 6 - 172);
	CHECK(musicProgressX(400, 228, 6, 99999, 4000) == 6 - 172);
	const char *bad = "\xff\xc3";
	CHECK(musicTextWidth(bad, 2) == 16);

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("music_json: all checks passed\n");
	return 0;
}
