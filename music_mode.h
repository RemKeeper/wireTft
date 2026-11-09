#ifndef WIRETFT_MUSIC_MODE_H
#define WIRETFT_MUSIC_MODE_H
// Music desk-display mode: go-musicfox LAN broadcast client (lan_broadcast.md).
// Included from wireTFT.ino after gfx/displayLock and a privateIP declaration.
#include "music_json.h"
#include <canvas/Arduino_Canvas.h>
#include "esp32/rom/tjpgd.h"
#include <fcntl.h>
#include <sys/select.h>
#include <math.h>
#include <new>

extern const uint8_t *const wiretftLyricFont;

static constexpr int COVER = 160, COVER_BAND = 40, STRIP_H = 25;

// Screen regions for one orientation, covering the whole panel. Title and lyrics are drawn through a pane-wide strip canvas, so
// textX is relative to paneX.
// The title band (titleX/titleW) may be wider than the lyric pane; the strip
// canvas is as wide as the wider of the two. coverSize <= COVER is scaled at draw time.
struct MusicGeom {
	int coverX, coverY, coverSize, titleX, titleY, titleW, paneX, paneW, lyricY, lyricH, textX, textW, screenW, screenH;
};
static constexpr MusicGeom MUSIC_PORTRAIT = {40, 12, 160, 0, 180, 240, 0, 240, 204, 116, 6, 228, 240, 320};
// Landscape: title centred across the top, 120 px cover lower left, wide lyric pane right.
static constexpr MusicGeom MUSIC_LANDSCAPE = {10, 78, 120, 0, 8, 320, 138, 182, 36, 204, 4, 174, 320, 240};
static MusicGeom geo = MUSIC_PORTRAIT;
static int musicStripW = 240;
static constexpr size_t SSE_BYTES = 16384;
static constexpr uint32_t MUSIC_MAGIC = 0x4d555331;

struct MusicConfig { uint32_t magic, ip; uint16_t port; uint8_t translation, reserved; };
enum MusicNet : uint8_t { MUSIC_OFF, MUSIC_CONNECTING, MUSIC_CONNECTED, MUSIC_ERROR };

// musicConfig is written by the HTTP task and read elsewhere under musicLock.
static SemaphoreHandle_t musicLock;
static MusicConfig musicConfig{};
static volatile uint32_t musicConfigVersion = 0;
static volatile MusicNet musicNet = MUSIC_OFF;
static const char *volatile musicDetail = "";
static volatile bool musicPaused = false;
static TaskHandle_t musicTaskHandle = nullptr;

// Buffers exist only while music mode runs. Pointers and the fields below are
// guarded by musicLock; the inactive snapshot and cover bands belong to the
// music task until published.
static MusicSnapshot *musicSnaps[2];
static uint16_t *musicCover[COVER / COVER_BAND];
static char *musicSse;
static bool musicReady = false;
static int musicActive = 0;
static bool musicHasSnap = false;
static uint32_t musicSnapGen = 0;
static int64_t musicSnapUs = 0;
static uint32_t musicCoverGen = 0;
static bool musicCoverOk = false;
static uint16_t musicCoverBg = 0;

static bool musicEnabled() { return musicConfig.ip != 0; }

static void musicFormatServer(const MusicConfig &c, char *out, size_t size)
{
	const uint8_t *b = (const uint8_t *)&c.ip;
	if (c.ip) snprintf(out, size, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], c.port);
	else if (size) out[0] = 0;
}

// Strict dotted-quad private IPv4 with port, e.g. 192.168.1.50:8765.
static bool musicParseServer(const char *s, uint32_t &ip, uint16_t &port)
{
	uint8_t parts[4];
	for (int i = 0; i < 4; ++i) {
		if (*s < '0' || *s > '9') return false;
		unsigned v = 0, digits = 0;
		if (s[0] == '0' && s[1] >= '0' && s[1] <= '9') return false;
		while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (++digits > 3) return false; }
		if (v > 255 || *s++ != (i < 3 ? '.' : ':')) return false;
		parts[i] = uint8_t(v);
	}
	unsigned p = 0, digits = 0;
	if (*s == '0') return false;
	while (*s >= '0' && *s <= '9') { p = p * 10 + (*s++ - '0'); if (++digits > 5) return false; }
	if (*s || !p || p > 65535) return false;
	memcpy(&ip, parts, 4);
	port = uint16_t(p);
	return privateIP(ip);
}

static void loadMusicConfig()
{
	nvs_handle_t h;
	MusicConfig c{};
	size_t size = sizeof(c);
	if (nvs_open("wiretft", NVS_READONLY, &h) != ESP_OK) return;
	esp_err_t err = nvs_get_blob(h, "music", &c, &size);
	nvs_close(h);
	if (err == ESP_OK && size == sizeof(c) && c.magic == MUSIC_MAGIC && (!c.ip || (privateIP(c.ip) && c.port)))
		musicConfig = c;
}

static bool saveMusicConfig(const MusicConfig &c)
{
	nvs_handle_t h;
	if (nvs_open("wiretft", NVS_READWRITE, &h) != ESP_OK) return false;
	esp_err_t err = nvs_set_blob(h, "music", &c, sizeof(c));
	if (err == ESP_OK) err = nvs_commit(h);
	nvs_close(h);
	return err == ESP_OK;
}

static void musicApplyConfig(const MusicConfig &c)
{
	xSemaphoreTake(musicLock, portMAX_DELAY);
	if (c.ip != musicConfig.ip || c.port != musicConfig.port) musicConfigVersion = musicConfigVersion + 1;
	musicConfig = c;
	xSemaphoreGive(musicLock);
}

static const char *musicStateName()
{
	if (!musicEnabled()) return "off";
	return musicNet == MUSIC_CONNECTED ? "connected" : musicNet == MUSIC_ERROR ? "error" : "connecting";
}

// ---------------- Network ----------------

static bool musicAborted(uint32_t version) { return musicConfigVersion != version || !wifi_bridge_has_ip(); }

static int musicConnect(uint32_t ip, uint16_t port, int timeoutMs)
{
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) return -1;
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = ip;
	int r = connect(fd, (sockaddr *)&addr, sizeof(addr));
	if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
	if (r < 0) {
		fd_set w;
		FD_ZERO(&w);
		FD_SET(fd, &w);
		timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
		int err = 0;
		socklen_t len = sizeof(err);
		if (select(fd + 1, nullptr, &w, nullptr, &tv) <= 0 ||
			getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err) { close(fd); return -1; }
	}
	fcntl(fd, F_SETFL, flags);
	timeval rx{1, 0}, tx{3, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rx, sizeof(rx));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tx, sizeof(tx));
	return fd;
}

// HTTP/1.1 response body reader with chunked and Content-Length framing.
// Receive polls every second so configuration changes abort promptly.
struct HttpIn {
	int fd = -1;
	uint32_t version = 0;
	int idleLimitMs = 0, idleMs = 0;
	bool chunked = false, done = false, needCRLF = false;
	int64_t left = -1, chunkLeft = 0;
	uint8_t buf[512];
	int pos = 0, len = 0;
	~HttpIn() { if (fd >= 0) close(fd); }
	int raw()
	{
		while (pos >= len) {
			if (musicAborted(version)) return -1;
			int n = recv(fd, buf, sizeof(buf), 0);
			if (n > 0) { pos = 0; len = n; idleMs = 0; break; }
			if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return -1;
			if ((idleMs += 1000) >= idleLimitMs) return -1;
		}
		return buf[pos++];
	}
	bool line(char *out, size_t cap)
	{
		size_t n = 0;
		for (;;) {
			int c = raw();
			if (c < 0) return false;
			if (c == '\n') break;
			if (c != '\r' && n + 1 < cap) out[n++] = char(c);
		}
		out[n] = 0;
		return true;
	}
	int get()
	{
		if (done) return -1;
		if (chunked && !chunkLeft) {
			char size[24];
			if ((needCRLF && !line(size, sizeof(size))) || !line(size, sizeof(size))) { done = true; return -1; }
			char *end = nullptr;
			long v = strtol(size, &end, 16);
			if (end == size || v <= 0) { done = true; return -1; }
			chunkLeft = v;
			needCRLF = true;
		}
		if (!chunked && left == 0) { done = true; return -1; }
		int c = raw();
		if (c < 0) { done = true; return -1; }
		if (chunked) --chunkLeft;
		else if (left > 0) --left;
		return c;
	}
};

// Returns nullptr on a 200 response with headers consumed, else an error code.
static const char *musicOpen(const MusicConfig &cfg, uint32_t version, const char *path, bool events, int idleLimitMs, HttpIn &in)
{
	in.fd = musicConnect(cfg.ip, cfg.port, 3000);
	if (in.fd < 0) return "connect_failed";
	in.version = version;
	in.idleLimitMs = idleLimitMs;
	char host[24], text[256];
	musicFormatServer(cfg, host, sizeof(host));
	int n = snprintf(text, sizeof(text),
		"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: wireTFT\r\nAccept: %s\r\n%sConnection: %s\r\n\r\n",
		path, host, events ? "text/event-stream" : "image/jpeg",
		events ? "Cache-Control: no-cache\r\n" : "", events ? "keep-alive" : "close");
	for (int sent = 0; sent < n;) {
		int k = send(in.fd, text + sent, n - sent, 0);
		if (k <= 0) return "send_failed";
		sent += k;
	}
	if (!in.line(text, sizeof(text))) return "no_response";
	if (strncmp(text, "HTTP/1.", 7) || strncmp(text + 8, " 200", 4)) return "http_status";
	for (;;) {
		if (!in.line(text, sizeof(text))) return "no_response";
		if (!text[0]) break;
		for (char *p = text; *p; ++p) *p = char(tolower((unsigned char)*p));
		if (!strncmp(text, "transfer-encoding:", 18) && strstr(text, "chunked")) in.chunked = true;
		else if (!strncmp(text, "content-length:", 15)) in.left = strtoll(text + 15, nullptr, 10);
	}
	if (in.chunked) in.left = -1;
	return nullptr;
}

// ---------------- Cover decoding (ROM TJpgDec, baseline JPEG) ----------------

struct CoverCtx {
	HttpIn *in;
	uint32_t version;
	uint16_t srcX[COVER], srcY[COVER];
	uint32_t r, g, b, n;
};

static UINT coverInput(JDEC *jd, BYTE *buf, UINT len)
{
	CoverCtx *c = (CoverCtx *)jd->device;
	UINT n = 0;
	while (n < len) {
		int v = c->in->get();
		if (v < 0) break;
		if (buf) buf[n] = BYTE(v);
		++n;
	}
	return n;
}

// Nearest-neighbour sample of a centred square crop into the 160x160 bands.
static UINT coverOutput(JDEC *jd, void *bitmap, JRECT *rect)
{
	CoverCtx *c = (CoverCtx *)jd->device;
	const uint8_t *rgb = (const uint8_t *)bitmap;
	const int w = rect->right - rect->left + 1;
	for (int dy = 0; dy < COVER; ++dy) {
		const int sy = c->srcY[dy];
		if (sy < rect->top) continue;
		if (sy > rect->bottom) break;
		uint16_t *row = musicCover[dy / COVER_BAND] + (dy % COVER_BAND) * COVER;
		for (int dx = 0; dx < COVER; ++dx) {
			const int sx = c->srcX[dx];
			if (sx < rect->left) continue;
			if (sx > rect->right) break;
			const uint8_t *px = rgb + ((sy - rect->top) * w + (sx - rect->left)) * 3;
			row[dx] = RGB565(px[0], px[1], px[2]);
			c->r += px[0]; c->g += px[1]; c->b += px[2]; ++c->n;
		}
	}
	return musicAborted(c->version) ? 0 : 1;
}

static bool musicDecodeCover(HttpIn &in, uint32_t version, uint16_t &background)
{
	static constexpr UINT WORK = 4096;
	void *work = heap_caps_malloc(WORK, MALLOC_CAP_8BIT);
	CoverCtx *ctx = (CoverCtx *)heap_caps_calloc(1, sizeof(CoverCtx), MALLOC_CAP_8BIT);
	JRESULT r = JDR_MEM1;
	if (work && ctx) {
		ctx->in = &in;
		ctx->version = version;
		JDEC jd;
		r = jd_prepare(&jd, coverInput, work, WORK, ctx);
		if (r == JDR_OK) {
			const int full = jd.width < jd.height ? jd.width : jd.height;
			uint8_t scale = 0;
			while (scale < 3 && (full >> (scale + 1)) >= COVER) ++scale;
			const int sw = jd.width >> scale, sh = jd.height >> scale;
			const int side = sw < sh ? sw : sh;
			for (int d = 0; d < COVER; ++d) {
				ctx->srcX[d] = uint16_t((sw - side) / 2 + (d * side + side / 2) / COVER);
				ctx->srcY[d] = uint16_t((sh - side) / 2 + (d * side + side / 2) / COVER);
			}
			r = side > 0 ? jd_decomp(&jd, coverOutput, scale) : JDR_FMT1;
		}
	}
	const bool ok = r == JDR_OK && ctx->n;
	if (ok) background = RGB565(ctx->r / ctx->n * 3 / 10, ctx->g / ctx->n * 3 / 10, ctx->b / ctx->n * 3 / 10);
	else Serial.printf("music: cover decode failed (%d)\n", int(r));
	free(ctx);
	free(work);
	return ok;
}

static void musicFetchCover(const MusicConfig &cfg, uint32_t version, const MusicSnapshot &s)
{
	xSemaphoreTake(musicLock, portMAX_DELAY);
	musicCoverOk = false;
	xSemaphoreGive(musicLock);
	// Only same-server relative paths; the upstream picUrl is never fetched.
	const char *path = "/cover";
	bool safe = s.coverUrl[0] == '/';
	for (const char *p = s.coverUrl; safe && *p; ++p) safe = *p > 0x20 && *p < 0x7f;
	if (safe) path = s.coverUrl;
	bool ok = false;
	uint16_t background = 0;
	if (s.songId) {
		HttpIn in;
		const char *err = musicOpen(cfg, version, path, false, 8000, in);
		ok = !err && musicDecodeCover(in, version, background);
		if (err) Serial.printf("music: cover request failed: %s\n", err);
	}
	xSemaphoreTake(musicLock, portMAX_DELAY);
	musicCoverOk = ok;
	musicCoverBg = background;
	++musicCoverGen;
	xSemaphoreGive(musicLock);
}

// ---------------- Music task ----------------

static int64_t coverSong = -1;
static int coverTries = 0;
static int64_t coverTriedUs = 0;

static void musicFree()
{
	xSemaphoreTake(musicLock, portMAX_DELAY);
	musicReady = false;
	musicHasSnap = false;
	musicCoverOk = false;
	for (auto &p : musicSnaps) { free(p); p = nullptr; }
	for (auto &p : musicCover) { free(p); p = nullptr; }
	free(musicSse); musicSse = nullptr;
	xSemaphoreGive(musicLock);
}

static bool musicAlloc()
{
	MusicSnapshot *snaps[2] = {};
	uint16_t *cover[COVER / COVER_BAND] = {};
	char *sse = (char *)heap_caps_malloc(SSE_BYTES, MALLOC_CAP_8BIT);
	bool ok = sse != nullptr;
	for (auto &p : snaps) ok = ok && (p = (MusicSnapshot *)heap_caps_calloc(1, sizeof(MusicSnapshot), MALLOC_CAP_8BIT));
	for (auto &p : cover) ok = ok && (p = (uint16_t *)heap_caps_malloc(COVER * COVER_BAND * 2, MALLOC_CAP_8BIT));
	if (!ok) {
		free(sse);
		for (auto p : snaps) free(p);
		for (auto p : cover) free(p);
		return false;
	}
	xSemaphoreTake(musicLock, portMAX_DELAY);
	memcpy(musicSnaps, snaps, sizeof(snaps));
	memcpy(musicCover, cover, sizeof(cover));
	musicSse = sse;
	musicReady = true;
	xSemaphoreGive(musicLock);
	return true;
}

static void musicHandleEvent(const MusicConfig &cfg, uint32_t version, size_t length)
{
	MusicSnapshot *next = musicSnaps[musicActive ^ 1];
	if (!musicParse(musicSse, length, *next)) { musicDetail = "bad_event"; return; }
	const bool songChanged = !musicHasSnap || musicSnaps[musicActive]->songId != next->songId;
	xSemaphoreTake(musicLock, portMAX_DELAY);
	musicActive ^= 1;
	musicHasSnap = true;
	musicSnapUs = esp_timer_get_time();
	++musicSnapGen;
	xSemaphoreGive(musicLock);
	musicPaused = !next->playing;
	musicDetail = "";
	const int64_t now = esp_timer_get_time();
	if (songChanged || next->songId != coverSong) { coverSong = next->songId; coverTries = 0; }
	else if (!next->songId || musicCoverOk || coverTries >= 3 || now - coverTriedUs < 10000000) return;
	++coverTries;
	coverTriedUs = now;
	musicFetchCover(cfg, version, *next);
}

// Server-Sent Events: "field: value" lines, blank line dispatches.
static void musicRunEvents(const MusicConfig &cfg, uint32_t version)
{
	HttpIn in;
	if (const char *err = musicOpen(cfg, version, "/events", true, 45000, in)) {
		musicNet = MUSIC_ERROR;
		musicDetail = err;
		return;
	}
	musicNet = MUSIC_CONNECTED;
	musicDetail = "";
	enum { FIELD, VALUE_START, VALUE, IGNORE } state = FIELD;
	char field[12], event[24] = {};
	size_t fieldLen = 0, eventLen = 0, length = 0;
	bool isData = false, overflow = false, lastCR = false, empty = true;
	for (;;) {
		int c = in.get();
		if (c < 0) break;
		if (c == '\n' && lastCR) { lastCR = false; continue; }
		lastCR = c == '\r';
		if (c == '\r' || c == '\n') {
			if (empty) {
				if (overflow) musicDetail = "event_too_large";
				else if (length && (!event[0] || !strcmp(event, "nowplaying"))) {
					musicSse[length] = 0;
					musicHandleEvent(cfg, version, length);
				}
				length = 0; overflow = false; event[0] = 0;
			}
			state = FIELD; fieldLen = 0; empty = true;
			continue;
		}
		empty = false;
		switch (state) {
		case FIELD:
			if (c != ':') {
				if (fieldLen + 1 < sizeof(field)) field[fieldLen++] = char(c);
				else state = IGNORE;
				break;
			}
			field[fieldLen] = 0;
			isData = !strcmp(field, "data");
			if (isData && length) {
				if (length + 2 < SSE_BYTES) musicSse[length++] = '\n';
				else overflow = true;
			}
			if (!strcmp(field, "event")) { eventLen = 0; event[0] = 0; }
			state = isData || !strcmp(field, "event") ? VALUE_START : IGNORE;
			break;
		case VALUE_START:
			state = VALUE;
			if (c == ' ') break;
			[[fallthrough]];
		case VALUE:
			if (isData) {
				if (length + 2 < SSE_BYTES) musicSse[length++] = char(c);
				else overflow = true;
			} else if (eventLen + 1 < sizeof(event)) { event[eventLen++] = char(c); event[eventLen] = 0; }
			break;
		case IGNORE:
			break;
		}
	}
	if (!musicAborted(version)) { musicNet = MUSIC_ERROR; musicDetail = "disconnected"; }
}

static void musicTask(void *)
{
	uint32_t seen = ~0u;
	for (;;) {
		xSemaphoreTake(musicLock, portMAX_DELAY);
		const MusicConfig cfg = musicConfig;
		const uint32_t version = musicConfigVersion;
		xSemaphoreGive(musicLock);
		if (!cfg.ip) {
			musicFree();
			xSemaphoreTake(musicLock, portMAX_DELAY);
			if (!musicConfig.ip) {
				musicTaskHandle = nullptr;
				musicNet = MUSIC_OFF;
				xSemaphoreGive(musicLock);
				vTaskDelete(nullptr);
			}
			xSemaphoreGive(musicLock);
			continue;
		}
		if (version != seen) {
			seen = version;
			xSemaphoreTake(musicLock, portMAX_DELAY);
			musicHasSnap = false;
			musicCoverOk = false;
			++musicSnapGen;
			++musicCoverGen;
			xSemaphoreGive(musicLock);
			coverSong = -1;
		}
		if (!musicReady && !musicAlloc()) {
			musicNet = MUSIC_ERROR;
			musicDetail = "no_memory";
			vTaskDelay(pdMS_TO_TICKS(3000));
			continue;
		}
		if (!wifi_bridge_has_ip()) { musicNet = MUSIC_CONNECTING; vTaskDelay(pdMS_TO_TICKS(500)); continue; }
		musicNet = MUSIC_CONNECTING;
		musicRunEvents(cfg, version);
		for (int i = 0; i < 20 && !musicAborted(version); ++i) vTaskDelay(pdMS_TO_TICKS(100));
	}
}

static void musicService()
{
	if (!musicEnabled()) return;
	xSemaphoreTake(musicLock, portMAX_DELAY);
	if (musicConfig.ip && !musicTaskHandle) {
		musicNet = MUSIC_CONNECTING;
		if (xTaskCreate(musicTask, "music", 8192, nullptr, 1, &musicTaskHandle) != pdPASS) musicTaskHandle = nullptr;
	}
	xSemaphoreGive(musicLock);
}

// ---------------- Rendering (Arduino loop task) ----------------

struct MusicViewRow { int16_t y; uint8_t line, kind; MusicRow row; };
static constexpr int MUSIC_VIEW_ROWS = MUSIC_MAX_LINES * 2;
struct MusicView {
	bool shown, full, title, lyrics, translation, snapScroll;
	uint32_t snapGen, coverGen;
	uint16_t bg;
	int rows;
	MusicViewRow row[MUSIC_VIEW_ROWS];
	int16_t lineTop[MUSIC_MAX_LINES], lineBottom[MUSIC_MAX_LINES];
	float scroll;
	int drawnScroll, current, karaoke, anchorIndex, anchorTop, marquee;
	int64_t lastUs, songId;
	const char *message;
};
static MusicView view;
static Arduino_Canvas *musicStrip;

// Looking a code point up in the 14k-glyph flash font costs about 1.6 ms, so
// each glyph is rasterised once into a 4-way LRU cache of 1-bit rows. The font
// box is 16x16 with its bottom 2 px below the baseline.
static constexpr int GLYPH_ROWS = 16, GLYPH_ASCENT = 14, GLYPH_SETS = 64, GLYPH_WAYS = 4;
struct MusicGlyph { uint32_t cp; uint16_t bits[GLYPH_ROWS]; };
struct MusicGlyphSet { MusicGlyph way[GLYPH_WAYS]; };
static MusicGlyphSet *musicGlyphs;
static Arduino_Canvas *musicGlyphCanvas;

static const MusicGlyph &musicGlyph(uint32_t cp, const char *utf8, size_t len)
{
	MusicGlyph *way = musicGlyphs[(cp * 2654435761u) >> 26].way;
	int w = 0;
	while (w < GLYPH_WAYS - 1 && way[w].cp != cp) ++w;
	// Ways are kept most-recent first; a miss reuses the last one.
	const bool hit = way[w].cp == cp;
	const MusicGlyph found = way[w];
	memmove(way + 1, way, w * sizeof(MusicGlyph));
	MusicGlyph &g = way[0];
	if (hit) { g = found; return g; }
	musicGlyphCanvas->fillScreen(0);
	musicGlyphCanvas->setCursor(0, GLYPH_ASCENT);
	static_cast<Print *>(musicGlyphCanvas)->write((const uint8_t *)utf8, len);
	const uint16_t *fb = musicGlyphCanvas->getFramebuffer();
	for (int r = 0; r < GLYPH_ROWS; ++r) {
		uint16_t bits = 0;
		for (int c = 0; c < 16; ++c)
			if (fb[r * 16 + c]) bits |= 0x8000 >> c;
		g.bits[r] = bits;
	}
	g.cp = cp;
	return g;
}

static void musicResetView()
{
	view.shown = false; view.full = view.title = view.lyrics = view.snapScroll = true;
	view.snapGen = view.coverGen = ~0u;
	view.rows = 0; view.scroll = 0;
	view.drawnScroll = view.current = view.karaoke = view.marquee = INT32_MIN;
	view.anchorIndex = -1; view.songId = -1; view.message = nullptr;
}

static uint16_t blend565(uint16_t a, uint16_t b, float t)
{
	const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
	const int r = ar + int(((b >> 11) - ar) * t), g = ag + int((((b >> 5) & 63) - ag) * t), bl = ab + int(((b & 31) - ab) * t);
	return uint16_t(r << 11 | g << 5 | bl);
}

static void musicLayout(const MusicSnapshot &s, bool translation)
{
	int y = 0;
	view.rows = 0;
	for (int i = 0; i < s.lineCount; ++i) {
		const MusicLine &l = s.lines[i];
		view.lineTop[i] = int16_t(y);
		// One row per lyric and one per translation. Overflow scrolls sideways
		// instead of wrapping, so a translation stays on the line below its lyric.
		int width = musicTextWidth(s.text(l), l.textLen);
		if (width > 32767) width = 32767;
		if (view.rows < MUSIC_VIEW_ROWS)
			view.row[view.rows++] = {int16_t(y), uint8_t(i), 0, {0, l.textLen, int16_t(width), 0, false}};
		y += 20;
		if (translation && l.transLen && view.rows < MUSIC_VIEW_ROWS) {
			width = musicTextWidth(s.translation(l), l.transLen);
			if (width > 32767) width = 32767;
			view.row[view.rows++] = {int16_t(y), uint8_t(i), 1, {0, l.transLen, int16_t(width), 0, false}};
			y += 16;
		}
		view.lineBottom[i] = int16_t(y);
		y += 6;
	}
	// Keep the anchored line visually still when the server shifts its window.
	view.snapScroll = true;
	for (int i = 0; i < s.lineCount; ++i) {
		if (s.lines[i].index != view.anchorIndex) continue;
		view.scroll += view.lineTop[i] - view.anchorTop;
		view.drawnScroll = INT32_MIN;
		view.snapScroll = false;
		break;
	}
}

// Places each glyph by the same widths karaoke uses, so the highlight matches
// the drawn text even when the run starts off the left edge.
static void musicDrawSpan(const char *text, size_t len, int x, int baseline, uint16_t color, int limit)
{
	uint16_t *fb = musicStrip->getFramebuffer();
	const int top = baseline - GLYPH_ASCENT;
	const int r0 = top < 0 ? -top : 0, r1 = top + GLYPH_ROWS > STRIP_H ? STRIP_H - top : GLYPH_ROWS;
	if (r0 >= r1) return;
	for (size_t i = 0; i < len && x < limit;) {
		size_t next = i;
		const uint32_t cp = utf8Next(text, len, next);
		const int cw = musicGlyphWidth(cp);
		// A malformed sequence would leave the font decoder mid-character.
		if (x + cw > 0 && cp > ' ' && cp != 0xfffd) {
			const MusicGlyph &g = musicGlyph(cp, text + i, next - i);
			const int c0 = x < 0 ? -x : 0, c1 = x + 16 > limit ? limit - x : 16;
			for (int r = r0; r < r1; ++r) {
				const uint16_t bits = g.bits[r];
				if (!bits) continue;
				uint16_t *row = fb + (top + r) * musicStripW + x;
				for (int c = c0; c < c1; ++c)
					if (bits & (0x8000 >> c)) row[c] = color;
			}
		}
		x += cw;
		i = next;
	}
}

static void musicDrawRun(const char *text, const MusicRow &row, const MusicScroll &place, int baseline, uint16_t color)
{
	if (!row.len) return;
	musicDrawSpan(text + row.off, row.len, place.x, baseline, color, geo.paneW);
	if (place.repeat != 0x7fffffff && place.repeat < geo.paneW)
		musicDrawSpan(text + row.off, row.len, place.repeat, baseline, color, geo.paneW);
}

// Copies the left w columns of the strip canvas to the screen. The canvas can be
// wider than the lyric pane, so narrower blits first pack the rows in place
// (each strip is fully redrawn before its next blit) and go out as one window.
static void musicBlit(int x, int y, int w, int h)
{
	uint16_t *fb = musicStrip->getFramebuffer();
	// The last lyric strip may extend past the bottom of the panel.
	if (y + h > geo.screenH) h = geo.screenH - y;
	if (h <= 0) return;
	if (w != musicStripW)
		for (int r = 1; r < h; ++r) memmove(fb + r * w, fb + r * musicStripW, w * sizeof(uint16_t));
	gfx.draw16bitRGBBitmap(x, y, fb, w, h);
}

static uint16_t musicCoverPixel(int x, int y) { return musicCover[y / COVER_BAND][(y % COVER_BAND) * COVER + x]; }

static void musicDrawCover()
{
	const int size = geo.coverSize;
	if (musicCoverOk && size == COVER) {
		for (int b = 0; b < COVER / COVER_BAND; ++b)
			gfx.draw16bitRGBBitmap(geo.coverX, geo.coverY + b * COVER_BAND, musicCover[b], COVER, COVER_BAND);
		return;
	}
	if (musicCoverOk) {
		// Box-filter the decoded 160x160 cover down to the layout size.
		uint16_t row[COVER];
		for (int dy = 0; dy < size; ++dy) {
			const int y0 = dy * COVER / size, y1 = (dy + 1) * COVER / size > y0 ? (dy + 1) * COVER / size : y0 + 1;
			for (int dx = 0; dx < size; ++dx) {
				const int x0 = dx * COVER / size, x1 = (dx + 1) * COVER / size > x0 ? (dx + 1) * COVER / size : x0 + 1;
				int r = 0, g = 0, b = 0, n = 0;
				for (int y = y0; y < y1; ++y)
					for (int x = x0; x < x1; ++x) {
						const uint16_t p = musicCoverPixel(x, y);
						r += p >> 11; g += (p >> 5) & 63; b += p & 31; ++n;
					}
				row[dx] = uint16_t((r / n) << 11 | (g / n) << 5 | (b / n));
			}
			gfx.draw16bitRGBBitmap(geo.coverX, geo.coverY + dy, row, size, 1);
		}
		return;
	}
	// Placeholder note, laid out for 160 px and scaled to the cover size.
	auto s = [size](int v) { return v * size / COVER; };
	const uint16_t panel = blend565(view.bg, 0xffff, 0.10f), note = blend565(view.bg, 0xffff, 0.35f);
	gfx.fillRect(geo.coverX, geo.coverY, size, size, panel);
	gfx.fillCircle(geo.coverX + s(66), geo.coverY + s(108), s(16), note);
	gfx.fillRect(geo.coverX + s(78), geo.coverY + s(44), s(6), s(64), note);
	gfx.fillRect(geo.coverX + s(78), geo.coverY + s(44), s(30), s(8), note);
}

static int musicTitleWidth(const char *name, const char *artists, int &nameWidth)
{
	nameWidth = musicTextWidth(name, strlen(name));
	int artistsWidth = *artists ? musicTextWidth(artists, strlen(artists)) : 0;
	return nameWidth + (artistsWidth ? 16 + artistsWidth : 0);
}

static void musicDrawTitle(const MusicSnapshot *s, int64_t now)
{
	musicStrip->fillScreen(view.bg);
	char server[24];
	musicFormatServer(musicConfig, server, sizeof(server));
	const char *name = s && s->songId ? s->name : server;
	const char *artists = s && s->songId ? s->artists : "";
	int nameWidth = 0;
	int total = musicTitleWidth(name, artists, nameWidth);
	// Centred when it fits, otherwise loops across the whole title band.
	MusicScroll place = musicScrollX(total, geo.titleW - 12, 6, now / 1000, false, 0);
	uint16_t nameColor = s && s->songId ? 0xffff : blend565(view.bg, 0xffff, 0.6f);
	uint16_t artistColor = blend565(view.bg, 0xffff, 0.55f);
	auto draw = [&](int x) {
		musicDrawSpan(name, strlen(name), x, 15, nameColor, geo.titleW);
		if (*artists) musicDrawSpan(artists, strlen(artists), x + nameWidth + 16, 15, artistColor, geo.titleW);
	};
	draw(place.x);
	if (place.repeat != 0x7fffffff && place.repeat < geo.titleW) draw(place.repeat);
	musicBlit(geo.titleX, geo.titleY, geo.titleW, 20);
}

static void musicDrawMessage(const char *text)
{
	const int width = musicTextWidth(text, strlen(text));
	const int x = width <= geo.textW ? geo.textX + (geo.textW - width) / 2 : geo.textX;
	// Centred in the lyric area; each strip draws its clipped share of the glyphs.
	const int baseline = geo.lyricH / 2 + 6;
	for (int y0 = 0; y0 < geo.lyricH; y0 += STRIP_H) {
		musicStrip->fillScreen(view.bg);
		musicDrawSpan(text, strlen(text), x, baseline - y0, blend565(view.bg, 0xffff, 0.6f), geo.paneW);
		musicBlit(geo.paneX, geo.lyricY + y0, geo.paneW, STRIP_H);
	}
}

// Lyrics never loop: inactive lines sit left-aligned when too wide; the
// current line scrolls once, following word timing or its time span.
static int musicLineX(int width, const MusicLine &l, int karaoke, int64_t pos)
{
	if (karaoke >= 0) return musicScrollX(width, geo.textW, geo.textX, 0, true, karaoke).x;
	const int64_t duration = l.end > l.start ? l.end - l.start : 4000;
	return musicProgressX(width, geo.textW, geo.textX, pos - l.start, duration);
}

static MusicScroll musicRowPlace(const MusicViewRow &r, const MusicSnapshot &s, int karaoke, int64_t pos, bool active)
{
	const int width = r.row.width;
	MusicScroll place{width <= geo.textW ? geo.textX + (geo.textW - width) / 2 : geo.textX, 0x7fffffff};
	if (!active || width <= geo.textW) return place;
	const MusicLine &l = s.lines[r.line];
	if (!r.kind) { place.x = musicLineX(width, l, karaoke, pos); return place; }
	const int lead = musicTextWidth(s.text(l), l.textLen);
	place.x = lead > geo.textW ? musicLinkedX(width, geo.textW, geo.textX, lead, musicLineX(lead, l, karaoke, pos))
		: musicLineX(width, l, -1, pos);
	return place;
}

static void musicRecolor(uint16_t *fb, int x, int width, int cutPx, int top, int bottom, uint16_t unplayed)
{
	const int stride = musicStripW;
	int cut = x + (cutPx < 0 ? 0 : cutPx > width ? width : cutPx);
	int end = x + width;
	if (cut < 0) cut = 0;
	if (end > geo.paneW) end = geo.paneW;
	for (int py = top; py < bottom; ++py)
		for (int px = cut; px < end; ++px)
			if (fb[py * stride + px] == 0xffff) fb[py * stride + px] = unplayed;
}

static void musicDrawLyrics(const MusicSnapshot &s, int scroll, int current, int karaoke, int64_t pos, bool onlyCurrent)
{
	uint16_t *fb = musicStrip->getFramebuffer();
	const uint16_t unplayed = blend565(view.bg, 0xffff, 0.5f);
	for (int y0 = 0; y0 < geo.lyricH; y0 += STRIP_H) {
		if (onlyCurrent && current >= 0 && (view.lineBottom[current] - scroll <= y0 || view.lineTop[current] - scroll >= y0 + STRIP_H)) continue;
		musicStrip->fillScreen(view.bg);
		for (int i = 0; i < view.rows; ++i) {
			const MusicViewRow &r = view.row[i];
			const int h = r.kind ? 16 : 20, sy = r.y - scroll - y0;
			if (sy + h <= 0 || sy >= STRIP_H || !r.row.len) continue;
			const bool active = r.line == current;
			uint16_t color;
			if (active) color = r.kind ? blend565(view.bg, 0xffff, 0.7f) : 0xffff;
			else {
				float d = fabsf(r.y - scroll + h / 2.0f - geo.lyricH / 2.0f) / geo.lyricH;
				float a = 1.0f - d;
				color = blend565(view.bg, 0xffff, (a < 0.35f ? 0.35f : a) * (r.kind ? 0.45f : 0.62f));
			}
			const MusicScroll place = musicRowPlace(r, s, karaoke, pos, active);
			const int baseline = sy + (r.kind ? 13 : 16);
			musicDrawRun(r.kind ? s.translation(s.lines[r.line]) : s.text(s.lines[r.line]), r.row, place, baseline, color);
			if (!active || r.kind || karaoke < 0) continue;
			const int top = sy < 0 ? 0 : sy, bottom = sy + h > STRIP_H ? STRIP_H : sy + h;
			musicRecolor(fb, place.x, r.row.width, karaoke, top, bottom, unplayed);
			if (place.repeat != 0x7fffffff)
				musicRecolor(fb, place.repeat, r.row.width, karaoke, top, bottom, unplayed);
		}
		musicBlit(geo.paneX, geo.lyricY + y0, geo.paneW, STRIP_H);
	}
}

static void musicLeave();

static bool musicEnter()
{
	geo = displayOrientation == ORIENT_LANDSCAPE ? MUSIC_LANDSCAPE : MUSIC_PORTRAIT;
	musicStripW = geo.titleW > geo.paneW ? geo.titleW : geo.paneW;
	musicStrip = new (std::nothrow) Arduino_Canvas(musicStripW, STRIP_H, &gfx);
	musicGlyphCanvas = new (std::nothrow) Arduino_Canvas(16, GLYPH_ROWS, &gfx);
	musicGlyphs = new (std::nothrow) MusicGlyphSet[GLYPH_SETS];
	if (!musicStrip || !musicStrip->begin(GFX_SKIP_OUTPUT_BEGIN) || !musicGlyphCanvas ||
		!musicGlyphCanvas->begin(GFX_SKIP_OUTPUT_BEGIN) || !musicGlyphs) {
		musicLeave();
		return false;
	}
	for (int i = 0; i < GLYPH_SETS; ++i)
		for (MusicGlyph &g : musicGlyphs[i].way) g.cp = UINT32_MAX;
	musicGlyphCanvas->setFont(wiretftLyricFont);
	musicGlyphCanvas->setUTF8Print(true);
	musicGlyphCanvas->setTextWrap(false);
	musicGlyphCanvas->setTextColor(0xffff);
	musicResetView();
	view.shown = true;
	return true;
}

static void musicLeave()
{
	delete musicStrip;
	musicStrip = nullptr;
	delete musicGlyphCanvas;
	musicGlyphCanvas = nullptr;
	delete[] musicGlyphs;
	musicGlyphs = nullptr;
	musicResetView();
}

// Called from loop(); redraws only what changed.
static void musicRender()
{
	if (!view.shown && !musicEnter()) return;
	if (xSemaphoreTake(displayLock, pdMS_TO_TICKS(20)) != pdTRUE) return;
	xSemaphoreTake(musicLock, portMAX_DELAY);
	const int64_t now = esp_timer_get_time();
	const MusicSnapshot *s = musicReady && musicHasSnap ? musicSnaps[musicActive] : nullptr;
	if (musicCoverGen != view.coverGen) {
		view.coverGen = musicCoverGen;
		const uint16_t bg = musicCoverOk ? musicCoverBg : 0x0841;
		if (bg != view.bg || view.full) { view.bg = bg; view.full = true; }
		else musicDrawCover();
	}
	if (view.full) {
		waitingShown = false;
		gfx.fillRect(0, 0, geo.screenW, geo.screenH, view.bg);
		musicDrawCover();
		view.full = false;
		view.title = view.lyrics = true;
		view.message = nullptr;
	}
	if (musicSnapGen != view.snapGen || bool(musicConfig.translation) != view.translation) {
		view.snapGen = musicSnapGen;
		view.translation = musicConfig.translation;
		if (s) musicLayout(*s, view.translation);
		else view.rows = 0;
		const int64_t song = s ? s->songId : -1;
		if (song != view.songId) { view.songId = song; view.title = true; view.snapScroll = true; }
		view.lyrics = true;
	}
	const int marquee = int((now / 1000) / 40);
	const bool titleScrolls = s && s->songId && musicTextWidth(s->name, strlen(s->name)) + (*s->artists ? 16 + musicTextWidth(s->artists, strlen(s->artists)) : 0) > geo.titleW - 12;
	if (view.title || (titleScrolls && marquee != view.marquee)) { musicDrawTitle(s, now); view.title = false; }
	const char *message = nullptr;
	if (!s) message = musicNet == MUSIC_ERROR ? "无法连接音乐服务" : "正在连接音乐服务…";
	else if (!s->songId) message = "未在播放";
	else if (!s->lineCount) message = "暂无歌词";
	if (message) {
		if (message != view.message || view.lyrics) musicDrawMessage(message);
		view.message = message;
		view.lyrics = false;
	} else {
		view.message = nullptr;
		int64_t pos = s->positionMs + (s->playing ? (now - musicSnapUs) / 1000 : 0);
		if (s->durationMs > 0 && pos > s->durationMs) pos = s->durationMs;
		const int current = musicCurrentLine(*s, pos), focus = current < 0 ? 0 : current;
		const float target = (view.lineTop[focus] + view.lineBottom[focus]) / 2.0f - geo.lyricH / 2.0f;
		const float dt = (now - view.lastUs) / 1000000.0f;
		if (view.snapScroll || dt > 1.0f) view.scroll = target;
		else view.scroll += (target - view.scroll) * (1.0f - expf(-dt / 0.09f));
		if (fabsf(target - view.scroll) < 0.5f) view.scroll = target;
		view.snapScroll = false;
		const int scroll = int(lroundf(view.scroll));
		const int karaoke = current >= 0 ? musicKaraokePx(*s, s->lines[current], pos) : -1;
		const bool moved = view.lyrics || scroll != view.drawnScroll || current != view.current;
		// Only the current line can move sideways, so ticks redraw just its strips.
		const bool scrolling = marquee != view.marquee && current >= 0;
		if (moved || scrolling || karaoke != view.karaoke)
			musicDrawLyrics(*s, scroll, current, karaoke, pos, !moved && current >= 0);
		view.lyrics = false;
		view.drawnScroll = scroll;
		view.current = current;
		view.karaoke = karaoke;
		view.anchorIndex = s->lines[focus].index;
		view.anchorTop = view.lineTop[focus];
	}
	view.marquee = marquee;
	view.lastUs = now;
	xSemaphoreGive(musicLock);
	xSemaphoreGive(displayLock);
}

#endif
