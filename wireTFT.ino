#include <Arduino.h>
#include <databus/Arduino_ESP32SPI.h>
#include <display/Arduino_ST7789.h>
#include "wifi_bridge.h"
#include "wifi_input.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "freertos/semphr.h"
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static constexpr size_t IMAGE_BYTES = 240 * 320 * 2;
static Arduino_ESP32SPI bus(27, 21, 18, 23, GFX_NOT_DEFINED);
static Arduino_ST7789 gfx(&bus, 33, 2, false, 240, 320);
static const char *slots[] = {"/images/slot0", "/images/slot1"};
// Header is written last. An interrupted write never becomes a valid slot.
struct ImageHeader { uint32_t magic, generation, length, crc; };
static constexpr uint32_t IMAGE_MAGIC = 0x57465431;           // 240x320 rows
static constexpr uint32_t IMAGE_MAGIC_LANDSCAPE = 0x5746544c; // 320x240 rows
static uint8_t slotOrientation[2] = {};
static int currentSlot = -1;
static uint32_t generation = 0;
static bool storageReady = false;
static char deviceId[13];
static SemaphoreHandle_t displayLock;
static volatile int pendingDisplaySlot = -1;
static wifi_br_state_t drawnWifiState = WIFI_BR_STATE_IDLE;
static char drawnWifiIp[16] = "";

static constexpr uint32_t WIFI_STORE_MAGIC = 0x57494631;
struct WifiStore { uint32_t magic; uint8_t ssidLen; uint8_t passwordLen; char ssid[33]; char password[65]; };
static char activeSsid[33] = {};
static char activePassword[65] = {};
static char pendingSsid[33] = {};
static bool savedWifi = false;

// 0 = portrait 240x320, 1 = landscape 320x240 (portrait rotated 90 degrees left).
// Changed only by loop() under displayLock; the HTTP task requests via pending.
enum : uint8_t { ORIENT_PORTRAIT = 0, ORIENT_LANDSCAPE = 1, ORIENT_NONE = 0xff };
static volatile uint8_t displayOrientation = ORIENT_PORTRAIT;
static volatile uint8_t pendingOrientation = ORIENT_NONE;
// Photos and music use the whole screen. The Wi-Fi line is drawn only
// on the waiting screen, so it never covers content.
static bool waitingShown = false;
static int screenWidth() { return displayOrientation == ORIENT_LANDSCAPE ? 320 : 240; }
static int screenHeight() { return displayOrientation == ORIENT_LANDSCAPE ? 240 : 320; }
static const char *orientationName(uint8_t o) { return o == ORIENT_LANDSCAPE ? "landscape" : "portrait"; }

// This panel needs MX+BGR (0x48) for upright portrait. Library rotation 3
// (MY|MV) plus the same MX+BGR gives the counter-clockwise landscape view.
static void applyPanelOrientation(uint8_t o)
{
	gfx.setRotation(o == ORIENT_LANDSCAPE ? 3 : 0);
	bus.beginWrite();
	bus.writeC8D8(0x36, o == ORIENT_LANDSCAPE ? 0xE8 : 0x48);
	bus.endWrite();
}

static void loadOrientation()
{
	nvs_handle_t h;
	uint8_t o = ORIENT_PORTRAIT;
	if (nvs_open("wiretft", NVS_READONLY, &h) != ESP_OK) return;
	if (nvs_get_u8(h, "orient", &o) == ESP_OK && o <= ORIENT_LANDSCAPE) displayOrientation = o;
	nvs_close(h);
}

static bool saveOrientation(uint8_t o)
{
	nvs_handle_t h;
	if (nvs_open("wiretft", NVS_READWRITE, &h) != ESP_OK) return false;
	esp_err_t err = nvs_set_u8(h, "orient", o);
	if (err == ESP_OK) err = nvs_commit(h);
	nvs_close(h);
	return err == ESP_OK;
}

static bool privateIP(uint32_t networkOrder);
#include "music_mode.h"
#include "ble_link.h"
static bool wifiStatusDirty = false;

// Six-digit setup code: created once when missing, kept in NVS and printed at
// boot. Every BLE command and every LAN write must carry it.
static char configCode[7] = {};
static volatile uint8_t codeFailures = 0;
static volatile int64_t codeLockedUntil = 0;
enum CodeCheck { CODE_OK, CODE_BAD, CODE_LOCKED };

static bool saveConfigCode(const char *code)
{
	nvs_handle_t h;
	if (nvs_open("wiretft", NVS_READWRITE, &h) != ESP_OK) return false;
	esp_err_t err = nvs_set_str(h, "code", code);
	if (err == ESP_OK) err = nvs_commit(h);
	nvs_close(h);
	return err == ESP_OK;
}

// Needs the radio running so esp_random() is a true random source.
static bool newConfigCode()
{
	char code[7];
	snprintf(code, sizeof(code), "%06u", unsigned(esp_random() % 1000000));
	if (!saveConfigCode(code)) return false;
	memcpy(configCode, code, sizeof(code));
	codeFailures = 0;
	codeLockedUntil = 0;
	return true;
}

static void loadConfigCode()
{
	nvs_handle_t h;
	size_t size = sizeof(configCode);
	bool ok = nvs_open("wiretft", NVS_READONLY, &h) == ESP_OK;
	if (ok) { ok = nvs_get_str(h, "code", configCode, &size) == ESP_OK; nvs_close(h); }
	ok = ok && strlen(configCode) == 6;
	for (int i = 0; ok && i < 6; ++i) ok = isdigit((unsigned char)configCode[i]);
	if (!ok && !newConfigCode()) configCode[0] = 0;
}

// given must be NUL-terminated within at least 7 bytes. Five wrong codes lock
// all configuration for a minute.
static CodeCheck checkCode(const char *given)
{
	const int64_t now = esp_timer_get_time();
	if (now < codeLockedUntil) return CODE_LOCKED;
	uint8_t diff = strlen(given) != 6 || strlen(configCode) != 6;
	for (int i = 0; i < 6; ++i) diff |= uint8_t(given[i] ^ configCode[i]);
	if (!diff) { codeFailures = 0; return CODE_OK; }
	codeFailures = codeFailures + 1;
	if (codeFailures >= 5) { codeFailures = 0; codeLockedUntil = now + 60000000; }
	return CODE_BAD;
}

static int codeLockSeconds()
{
	const int64_t left = codeLockedUntil - esp_timer_get_time();
	return left > 0 ? int(left / 1000000) + 1 : 0;
}

static bool loadWifiStore()
{
	nvs_handle_t h;
	WifiStore store{};
	if (nvs_open("wiretft", NVS_READONLY, &h) != ESP_OK) return false;
	size_t size = sizeof(store);
	esp_err_t err = nvs_get_blob(h, "wifi", &store, &size);
	nvs_close(h);
	if (err != ESP_OK || size != sizeof(store) || store.magic != WIFI_STORE_MAGIC ||
		store.ssidLen == 0 || store.ssidLen > 32 || store.passwordLen > 64 ||
		store.ssid[store.ssidLen] != '\0' || store.password[store.passwordLen] != '\0') return false;
	if (!wifi_valid_ssid(store.ssid) || !wifi_valid_password(store.password)) return false;
	memcpy(activeSsid, store.ssid, sizeof(activeSsid));
	memcpy(activePassword, store.password, sizeof(activePassword));
	savedWifi = true;
	return true;
}

static bool saveWifiStore(const char *ssid, const char *password)
{
	WifiStore store{};
	size_t ssidLen = 0, passwordLen = 0;
	if (!wifi_valid_ssid(ssid) || !wifi_valid_password(password)) return false;
	ssidLen = strlen(ssid);
	passwordLen = strlen(password);
	store.magic = WIFI_STORE_MAGIC;
	store.ssidLen = uint8_t(ssidLen);
	store.passwordLen = uint8_t(passwordLen);
	memcpy(store.ssid, ssid, ssidLen + 1);
	memcpy(store.password, password, passwordLen + 1);
	nvs_handle_t h;
	if (nvs_open("wiretft", NVS_READWRITE, &h) != ESP_OK) return false;
	esp_err_t err = nvs_set_blob(h, "wifi", &store, sizeof(store));
	if (err == ESP_OK) err = nvs_commit(h);
	nvs_close(h);
	if (err != ESP_OK) return false;
	memcpy(activeSsid, ssid, ssidLen + 1);
	memcpy(activePassword, password, passwordLen + 1);
	savedWifi = true;
	return true;
}

static bool forgetWifiStore()
{
	nvs_handle_t h;
	if (nvs_open("wiretft", NVS_READWRITE, &h) != ESP_OK) return false;
	esp_err_t err = nvs_erase_key(h, "wifi");
	if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
	if (err == ESP_OK) err = nvs_commit(h);
	nvs_close(h);
	if (err == ESP_OK) savedWifi = false;
	return err == ESP_OK;
}

// Caller holds displayLock.
static void paintWifiStatus()
{
	wifi_br_state_t state = wifi_bridge_state();
	const char *ip = wifi_bridge_ip_str();
	const int w = screenWidth(), bar = screenHeight() - 20;
	gfx.fillRect(0, bar, w, 20, 0x0000);
	gfx.setTextColor(0xffff); gfx.setTextSize(1); gfx.setCursor(4, bar + 6);
	if (state == WIFI_BR_STATE_GOT_IP) gfx.printf("WiFi OK %s", ip);
	else if (state == WIFI_BR_STATE_FAILED) gfx.print("WiFi failed");
	else if (state == WIFI_BR_STATE_STARTED || state == WIFI_BR_STATE_CONNECTED) gfx.print("WiFi connecting");
	else gfx.print("WiFi offline");
	drawnWifiState = state;
	strlcpy(drawnWifiIp, ip, sizeof(drawnWifiIp));
}

static void drawWifiStatus(bool force = false)
{
	if (!waitingShown) return;
	if (!force && !wifiStatusDirty && wifi_bridge_state() == drawnWifiState && !strcmp(wifi_bridge_ip_str(), drawnWifiIp)) return;
	if (xSemaphoreTake(displayLock, pdMS_TO_TICKS(100)) != pdTRUE) return;
	if (waitingShown) { paintWifiStatus(); wifiStatusDirty = false; }
	xSemaphoreGive(displayLock);
}

enum WifiSetupMode { WIFI_SETUP_IDLE, WIFI_SETUP_SSID, WIFI_SETUP_PASSWORD };
static WifiSetupMode wifiSetupMode = WIFI_SETUP_IDLE;
static WifiLine serialInput;

static bool equalsIgnoreCase(const char *a, const char *b)
{
	while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
	return *a == '\0' && *b == '\0';
}

static void serialPrompt(const char *text) { Serial.println(text); }

static void handleSerialLine()
{
	const char *line = serialInput.text;
	if (serialInput.cancelled) { wifiSetupMode = WIFI_SETUP_IDLE; wifi_wipe(pendingSsid, sizeof(pendingSsid)); Serial.println("已取消。"); }
	else if (serialInput.overflow) { wifiSetupMode = WIFI_SETUP_IDLE; wifi_wipe(pendingSsid, sizeof(pendingSsid)); Serial.println("输入过长，已取消。"); }
	else if (wifiSetupMode == WIFI_SETUP_SSID) {
		if (equalsIgnoreCase(line, "cancel")) { wifiSetupMode = WIFI_SETUP_IDLE; Serial.println("已取消。"); }
		else if (!wifi_valid_ssid(line)) serialPrompt("SSID 无效：须为 1-32 字节的有效名称。");
		else { wifiSetupMode = WIFI_SETUP_PASSWORD; strlcpy(pendingSsid, line, sizeof(pendingSsid)); serialPrompt("请输入密码（开放网络直接回车，输入 cancel 取消）："); }
	} else if (wifiSetupMode == WIFI_SETUP_PASSWORD) {
		if (equalsIgnoreCase(line, "cancel")) { wifiSetupMode = WIFI_SETUP_IDLE; wifi_wipe(pendingSsid, sizeof(pendingSsid)); Serial.println("已取消。"); }
		else if (!wifi_valid_password(line)) serialPrompt("密码无效：为空、8-63 个可打印字符，或 64 位十六进制 PSK。");
		else if (!saveWifiStore(pendingSsid, line)) { wifiSetupMode = WIFI_SETUP_IDLE; wifi_wipe(pendingSsid, sizeof(pendingSsid)); Serial.println("保存失败，未切换 Wi-Fi。"); }
		else if (wifi_bridge_set_sta_credentials(activeSsid, activePassword) != ESP_OK) { wifiSetupMode = WIFI_SETUP_IDLE; wifi_wipe(pendingSsid, sizeof(pendingSsid)); Serial.println("凭据已保存，但启动连接失败。"); }
		else { wifiSetupMode = WIFI_SETUP_IDLE; wifi_wipe(pendingSsid, sizeof(pendingSsid)); Serial.println("凭据已保存，正在连接（密码不会显示）。"); }
	} else if (equalsIgnoreCase(line, "wifi setup")) {
		wifiSetupMode = WIFI_SETUP_SSID; serialPrompt("请输入 SSID（输入 cancel 取消）：");
	} else if (equalsIgnoreCase(line, "wifi status")) {
		Serial.printf("WiFi 状态: %s", wifi_bridge_state_name(wifi_bridge_state()));
		if (wifi_bridge_has_ip()) Serial.printf(" IP=%s", wifi_bridge_ip_str());
		Serial.println(savedWifi ? "（已使用保存凭据）" : "（使用编译配置）");
	} else if (equalsIgnoreCase(line, "wifi forget")) {
		if (!forgetWifiStore()) Serial.println("删除保存凭据失败。");
		else if (wifi_bridge_set_sta_credentials(CONFIG_WIRETFT_SSID, CONFIG_WIRETFT_PASSWORD) != ESP_OK) Serial.println("已删除保存凭据，但回退连接启动失败。");
		else Serial.println("已删除保存凭据，正在回退到编译配置。");
	} else if (equalsIgnoreCase(line, "code")) {
		Serial.printf("配置码：%s  蓝牙名称：%s\n", configCode, bleName);
	} else if (equalsIgnoreCase(line, "code reset")) {
		if (newConfigCode()) Serial.printf("已生成新配置码：%s（旧配置码立即失效）\n", configCode);
		else Serial.println("生成配置码失败，原配置码保持不变。");
	} else if (equalsIgnoreCase(line, "wifi help") || equalsIgnoreCase(line, "help")) {
		Serial.println("命令: wifi setup | wifi status | wifi forget | code | code reset | wifi help");
	} else if (line[0]) Serial.println("未知命令，输入 wifi help 查看帮助。");
	serialInput.clear();
}

static void serviceSerial()
{
	while (Serial.available()) {
		if (serialInput.feed((unsigned char)Serial.read())) handleSerialLine();
	}
}

static uint32_t crcUpdate(uint32_t crc, const uint8_t *p, size_t n)
{
	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; ++i)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return crc;
}

static bool validateSlot(int slot, ImageHeader &h)
{
	FILE *f = fopen(slots[slot], "rb");
	if (!f) return false;
	bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) &&
			  (h.magic == IMAGE_MAGIC || h.magic == IMAGE_MAGIC_LANDSCAPE) && h.length == IMAGE_BYTES;
	uint8_t buf[960];
	uint32_t crc = ~0u;
	size_t left = IMAGE_BYTES;
	while (ok && left) {
		size_t n = left < sizeof(buf) ? left : sizeof(buf);
		ok = fread(buf, 1, n, f) == n;
		crc = crcUpdate(crc, buf, ok ? n : 0);
		left -= n;
	}
	ok = ok && (crc ^ ~0u) == h.crc && fgetc(f) == EOF;
	fclose(f);
	if (ok) slotOrientation[slot] = h.magic == IMAGE_MAGIC_LANDSCAPE ? ORIENT_LANDSCAPE : ORIENT_PORTRAIT;
	return ok;
}

// Streams one photo into the inactive slot, shared by LAN upload and BLE.
// imageLock is a binary semaphore so begin and finish may run on different tasks.
struct ImageWriter { FILE *f; int target; uint8_t orient; size_t got; uint32_t crc; bool ok; };
static SemaphoreHandle_t imageLock;

static const char *imageBegin(ImageWriter &w, uint8_t orient)
{
	if (!storageReady) return "storage_unavailable";
	if (xSemaphoreTake(imageLock, 0) != pdTRUE) return "image_busy";
	w = {nullptr, currentSlot == 0 ? 1 : 0, orient, 0, ~0u, false};
	w.f = fopen(slots[w.target], "wb");
	ImageHeader h{};
	w.ok = w.f && fwrite(&h, 1, sizeof(h), w.f) == sizeof(h);
	if (w.ok) return nullptr;
	if (w.f) { fclose(w.f); unlink(slots[w.target]); w.f = nullptr; }
	xSemaphoreGive(imageLock);
	return "cannot_open_slot";
}

static bool imageWrite(ImageWriter &w, const uint8_t *p, size_t n)
{
	if (!w.f || !w.ok || w.got + n > IMAGE_BYTES) return w.ok = false;
	w.crc = crcUpdate(w.crc, p, n);
	w.ok = fwrite(p, 1, n, w.f) == n;
	w.got += n;
	return w.ok;
}

static void imageAbort(ImageWriter &w)
{
	if (!w.f) return;
	fclose(w.f);
	unlink(slots[w.target]);
	w.f = nullptr;
	xSemaphoreGive(imageLock);
}

// Persists payload before the valid header; the other slot stays intact throughout.
static const char *imageFinish(ImageWriter &w, uint32_t expected)
{
	if (!w.f) return "no_image";
	bool ok = w.ok && w.got == IMAGE_BYTES && (w.crc ^ ~0u) == expected;
	if (ok) ok = fflush(w.f) == 0 && fsync(fileno(w.f)) == 0;
	if (ok) {
		ImageHeader h = {w.orient == ORIENT_LANDSCAPE ? IMAGE_MAGIC_LANDSCAPE : IMAGE_MAGIC, generation + 1, IMAGE_BYTES, expected};
		ok = fseek(w.f, 0, SEEK_SET) == 0 && fwrite(&h, 1, sizeof(h), w.f) == sizeof(h);
		if (ok) ok = fflush(w.f) == 0 && fsync(fileno(w.f)) == 0;
	}
	if (fclose(w.f) != 0) ok = false;
	w.f = nullptr;
	ImageHeader verified{};
	ok = ok && validateSlot(w.target, verified);
	if (ok) {
		currentSlot = w.target;
		generation = verified.generation;
		pendingDisplaySlot = w.target;
	} else unlink(slots[w.target]);
	xSemaphoreGive(imageLock);
	return ok ? nullptr : "crc_or_storage_error";
}

// Draws a photo in the orientation it was made for, so a photo saved before
// an orientation change keeps its own geometry. Caller holds displayLock.
static bool displaySlot(int slot)
{
	FILE *f = fopen(slots[slot], "rb");
	if (!f) return false;
	waitingShown = false;
	fseek(f, sizeof(ImageHeader), SEEK_SET);
	const uint8_t o = slotOrientation[slot];
	const bool other = o != displayOrientation;
	if (other) applyPanelOrientation(o);
	const int w = o == ORIENT_LANDSCAPE ? 320 : 240, h = o == ORIENT_LANDSCAPE ? 240 : 320;
	uint8_t bytes[640];
	uint16_t pixels[320];
	bool ok = true;
	for (int y = 0; ok && y < h; ++y) {
		ok = fread(bytes, 1, w * 2, f) == size_t(w * 2);
		for (int x = 0; ok && x < w; ++x)
			pixels[x] = (uint16_t(bytes[2*x]) << 8) | bytes[2*x+1];
		if (ok) gfx.draw16bitRGBBitmap(0, y, pixels, w, 1);
		if (!(y % 16)) delay(1);
	}
	fclose(f);
	if (other) applyPanelOrientation(displayOrientation);
	return ok;
}

static bool parseOrientation(const char *text, uint8_t &o)
{
	if (!strcmp(text, "portrait")) o = ORIENT_PORTRAIT;
	else if (!strcmp(text, "landscape")) o = ORIENT_LANDSCAPE;
	else return false;
	return true;
}

// Optional request header; absent means portrait, as sent by older consoles.
static bool headerOrientation(httpd_req_t *req, const char *name, uint8_t &o)
{
	char text[16] = {};
	o = ORIENT_PORTRAIT;
	if (!httpd_req_get_hdr_value_len(req, name)) return true;
	return httpd_req_get_hdr_value_str(req, name, text, sizeof(text)) == ESP_OK && parseOrientation(text, o);
}

static bool privateIP(uint32_t networkOrder)
{
	uint32_t ip = ntohl(networkOrder);
	return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8;
}

// IDF 5.5 has no bind-address setting. Scope its HTTP listener at socket bind.
extern "C" int __real_lwip_bind(int, const sockaddr *, socklen_t);
extern "C" int __wrap_lwip_bind(int fd, const sockaddr *address, socklen_t length)
{
	if (address && address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
		sockaddr_in local = *(const sockaddr_in *)address;
		if (ntohs(local.sin_port) == 80 && local.sin_addr.s_addr == htonl(INADDR_ANY)) {
			if (!wifi_bridge_has_ip() || !inet_aton(wifi_bridge_ip_str(), &local.sin_addr) ||
				!privateIP(local.sin_addr.s_addr)) { errno = EACCES; return -1; }
			return __real_lwip_bind(fd, (const sockaddr *)&local, sizeof(local));
		}
	}
	return __real_lwip_bind(fd, address, length);
}

static bool localRequest(httpd_req_t *req)
{
	sockaddr_in peer{}, local{};
	socklen_t n = sizeof(peer);
	int fd = httpd_req_to_sockfd(req);
	bool ok = getpeername(fd, (sockaddr *)&peer, &n) == 0;
	n = sizeof(local);
	ok = ok && getsockname(fd, (sockaddr *)&local, &n) == 0 &&
		 privateIP(peer.sin_addr.s_addr) && privateIP(local.sin_addr.s_addr);
	if (!ok) httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Private LAN only");
	return ok;
}

static void jsonEscape(char *out, size_t size, const char *in)
{
	size_t o = 0;
	for (; *in && o + 7 < size; ++in) {
		const unsigned char c = *in;
		if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
		else if (c < 0x20) o += snprintf(out + o, size - o, "\\u%04x", c);
		else out[o++] = c;
	}
	out[o] = 0;
}

static constexpr size_t INFO_BYTES = 1024;

// Device state shared by GET /api/info and the BLE info command.
static void buildInfo(char *json, size_t size)
{
	char server[24], ssid[200];
	xSemaphoreTake(musicLock, portMAX_DELAY);
	const MusicConfig music = musicConfig;
	xSemaphoreGive(musicLock);
	musicFormatServer(music, server, sizeof(server));
	jsonEscape(ssid, sizeof(ssid), savedWifi ? activeSsid : CONFIG_WIRETFT_SSID);
	// width/height stay the native panel size; orientation tells the logical size.
	const uint8_t o = pendingOrientation != ORIENT_NONE ? pendingOrientation : displayOrientation;
	snprintf(json, size,
		"{\"product\":\"wireTFT\",\"protocol\":2,\"id\":\"%s\",\"name\":\"%s\","
		"\"width\":240,\"height\":320,\"format\":\"rgb565be\",\"has_image\":%s,"
		"\"display_version\":1,\"orientation\":\"%s\",\"orientations\":[\"portrait\",\"landscape\"],"
		"\"image_orientation\":\"%s\","
		"\"wifi_state\":\"%s\",\"wifi_ssid\":\"%s\",\"wifi_saved\":%s,\"ip\":\"%s\","
		"\"music\":true,\"music_version\":1,\"music_server\":\"%s\",\"music_translation\":%s,"
		"\"music_state\":\"%s\",\"music_detail\":\"%s\",\"code_header\":\"X-Config-Code\","
		"\"heap_free\":%u,\"heap_largest\":%u}",
		deviceId, bleName, currentSlot >= 0 ? "true" : "false", orientationName(o),
		currentSlot >= 0 ? orientationName(slotOrientation[currentSlot]) : "",
		wifi_bridge_state_name(wifi_bridge_state()), ssid, savedWifi ? "true" : "false", wifi_bridge_ip_str(),
		server, music.translation ? "true" : "false", musicStateName(), music.ip ? musicDetail : "",
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
		(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// Returns nullptr on success, otherwise a short error code.
static const char *applyOrientation(const char *value)
{
	uint8_t o = ORIENT_PORTRAIT;
	if (!parseOrientation(value, o)) return "bad_orientation";
	if (!saveOrientation(o)) return "save_failed";
	pendingOrientation = o;
	return nullptr;
}

// An empty server turns music mode off.
static const char *applyMusic(const char *server, size_t serverLen, bool translation)
{
	if (serverLen > 21) return "bad_server";
	MusicConfig next = {MUSIC_MAGIC, 0, 0, uint8_t(translation), 0};
	if (server[0] && !musicParseServer(server, next.ip, next.port)) return "bad_server";
	if (!saveMusicConfig(next)) return "save_failed";
	musicApplyConfig(next);
	return nullptr;
}

// Sends the 401/429 response itself; the handler then returns ESP_FAIL so an
// unread body closes the connection.
static bool requireCode(httpd_req_t *req)
{
	char code[16] = {};
	httpd_req_get_hdr_value_str(req, "X-Config-Code", code, sizeof(code));
	const CodeCheck check = checkCode(code);
	if (check == CODE_OK) return true;
	char body[96];
	if (check == CODE_LOCKED) snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"locked\",\"retry_after\":%d}", codeLockSeconds());
	else snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"bad_code\"}");
	httpd_resp_set_status(req, check == CODE_LOCKED ? "429 Too Many Requests" : "401 Unauthorized");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, body);
	return false;
}

static esp_err_t identify(httpd_req_t *req)
{
	if (!localRequest(req)) return ESP_FAIL;
	char *json = (char *)malloc(INFO_BYTES);
	if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
	buildInfo(json, INFO_BYTES);
	httpd_resp_set_type(req, "application/json");
	const esp_err_t err = httpd_resp_sendstr(req, json);
	free(json);
	return err;
}

// Reads a small JSON body into body (NUL-terminated). Sends the error itself.
static bool readJsonBody(httpd_req_t *req, char *body, size_t size)
{
	char type[64] = {};
	httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type));
	if (strcmp(type, "application/json") || !req->content_len || req->content_len >= size) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected JSON body up to 256 bytes");
		return false;
	}
	for (size_t got = 0; got < req->content_len;) {
		int n = httpd_req_recv(req, body + got, req->content_len - got);
		if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete body"); return false; }
		got += n;
	}
	body[req->content_len] = 0;
	return true;
}

static esp_err_t displaySettings(httpd_req_t *req)
{
	if (!localRequest(req) || !requireCode(req)) return ESP_FAIL;
	char body[257] = {}, key[16], value[16] = {};
	if (!readJsonBody(req, body, sizeof(body))) return ESP_FAIL;
	bool has = false, first = true;
	JsonReader j(body, req->content_len);
	if (j.expect('{'))
		while (j.objectNext(first, key, sizeof(key))) {
			if (!strcmp(key, "orientation")) { has = true; j.string(value, sizeof(value)); }
			else j.skip();
			if (!j.ok) break;
		}
	j.ws();
	const char *error = !j.ok || j.p != j.end || !has ? "bad_orientation" : applyOrientation(value);
	if (error && !strcmp(error, "save_failed"))
		return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot save display settings");
	if (error)
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected {\"orientation\":\"portrait\"|\"landscape\"}");
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t musicSettings(httpd_req_t *req)
{
	if (!localRequest(req) || !requireCode(req)) return ESP_FAIL;
	char body[257] = {};
	if (!readJsonBody(req, body, sizeof(body))) return ESP_FAIL;
	xSemaphoreTake(musicLock, portMAX_DELAY);
	bool translation = musicConfig.translation;
	xSemaphoreGive(musicLock);
	char server[32] = {}, key[16];
	size_t serverLen = 0;
	bool hasServer = false, first = true;
	JsonReader j(body, req->content_len);
	if (j.expect('{'))
		while (j.objectNext(first, key, sizeof(key))) {
			if (!strcmp(key, "server")) { hasServer = true; j.string(server, sizeof(server), &serverLen); }
			else if (!strcmp(key, "translation")) j.boolean(translation);
			else j.skip();
			if (!j.ok) break;
		}
	j.ws();
	if (!j.ok || j.p != j.end || !hasServer)
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected {\"server\":\"IPv4:port\",\"translation\":bool}");
	const char *error = applyMusic(server, serverLen, translation);
	if (error && !strcmp(error, "save_failed"))
		return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot save music settings");
	if (error) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Music server must be a private IPv4:port");
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// ---------------- BLE commands (Arduino loop task) ----------------

// Image data arrives on the NimBLE host task; bleImageMutex keeps it from
// racing the loop task finishing or aborting the same transfer.
static ImageWriter bleImage{};
static SemaphoreHandle_t bleImageMutex;
static volatile bool bleImageActive = false, bleImageFailed = false;
static constexpr size_t BLE_IMAGE_WINDOW = 16384, BLE_IMAGE_ACK = 4096;
static size_t bleImageAcked = 0;
static bool bleScanPending = false;
static int64_t bleScanId = 0, bleScanStarted = 0;

static void bleOnImageData(uint32_t offset, const uint8_t *data, size_t len)
{
	if (!bleImageActive || bleImageFailed) return;
	xSemaphoreTake(bleImageMutex, portMAX_DELAY);
	if (bleImageActive && !bleImageFailed && (offset != bleImage.got || !imageWrite(bleImage, data, len)))
		bleImageFailed = true;
	xSemaphoreGive(bleImageMutex);
}

static void bleImageStop()
{
	xSemaphoreTake(bleImageMutex, portMAX_DELAY);
	if (bleImageActive) imageAbort(bleImage);
	bleImageActive = false;
	xSemaphoreGive(bleImageMutex);
}

// extra is appended verbatim and must start with a comma when not empty.
static void bleReply(int64_t id, const char *error, const char *extra = "")
{
	char json[192];
	if (error) snprintf(json, sizeof(json), "{\"id\":%lld,\"ok\":false,\"error\":\"%s\"%s}", (long long)id, error, extra);
	else snprintf(json, sizeof(json), "{\"id\":%lld,\"ok\":true%s}", (long long)id, extra);
	bleRespond(json);
}

static void bleReplyScan()
{
	wifi_bridge_ap_t aps[WIFI_BRIDGE_SCAN_MAX];
	int n = wifi_bridge_scan_take(aps, WIFI_BRIDGE_SCAN_MAX);
	if (n == -2 && esp_timer_get_time() - bleScanStarted < 20000000) return;
	bleScanPending = false;
	if (n < 0) { bleReply(bleScanId, "scan_failed"); return; }
	static constexpr size_t SIZE = 1600;
	char *json = (char *)malloc(SIZE);
	if (!json) { bleReply(bleScanId, "no_memory"); return; }
	size_t o = snprintf(json, SIZE, "{\"id\":%lld,\"ok\":true,\"networks\":[", (long long)bleScanId);
	for (int i = 0; i < n; ++i) {
		char ssid[200];
		jsonEscape(ssid, sizeof(ssid), aps[i].ssid);
		if (o + strlen(ssid) + 56 >= SIZE) break;
		o += snprintf(json + o, SIZE - o, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
			i ? "," : "", ssid, aps[i].rssi, aps[i].secure ? "true" : "false");
	}
	snprintf(json + o, SIZE - o, "]}");
	bleRespond(json);
	free(json);
}

static void bleHandleCommand(const char *text, size_t len)
{
	// SSID and password buffers have one spare byte so truncation is detectable.
	char cmd[16] = {}, code[16] = {}, ssid[34] = {}, password[66] = {}, server[32] = {}, orientation[16] = {}, key[16];
	size_t serverLen = 0, ssidLen = 0, passwordLen = 0;
	int64_t id = 0, crc = -1;
	bool translation = false, hasTranslation = false, hasServer = false, first = true;
	JsonReader j(text, len);
	if (j.expect('{'))
		while (j.objectNext(first, key, sizeof(key))) {
			if (!strcmp(key, "cmd")) j.string(cmd, sizeof(cmd));
			else if (!strcmp(key, "id")) j.number(id);
			else if (!strcmp(key, "code")) j.string(code, sizeof(code));
			else if (!strcmp(key, "ssid")) j.string(ssid, sizeof(ssid), &ssidLen);
			else if (!strcmp(key, "password")) j.string(password, sizeof(password), &passwordLen);
			else if (!strcmp(key, "server")) { hasServer = true; j.string(server, sizeof(server), &serverLen); }
			else if (!strcmp(key, "translation")) { hasTranslation = true; j.boolean(translation); }
			else if (!strcmp(key, "orientation")) j.string(orientation, sizeof(orientation));
			else if (!strcmp(key, "crc")) j.number(crc);
			else j.skip();
			if (!j.ok) break;
		}
	j.ws();
	const char *error = nullptr;
	char extra[64] = "";
	bool replied = false;
	const CodeCheck check = !j.ok || j.p != j.end || !cmd[0] ? CODE_OK : checkCode(code);
	if (!j.ok || j.p != j.end || !cmd[0]) error = "bad_request";
	else if (check == CODE_LOCKED) { error = "locked"; snprintf(extra, sizeof(extra), ",\"retry_after\":%d", codeLockSeconds()); }
	else if (check == CODE_BAD) error = "bad_code";
	else if (!strcmp(cmd, "info")) {
		char *json = (char *)malloc(INFO_BYTES + 48);
		if (!json) error = "no_memory";
		else {
			const int o = snprintf(json, 48, "{\"id\":%lld,\"ok\":true,\"info\":", (long long)id);
			buildInfo(json + o, INFO_BYTES);
			strlcat(json, "}", INFO_BYTES + 48);
			bleRespond(json);
			free(json);
			replied = true;
		}
	} else if (!strcmp(cmd, "wifi_scan")) {
		if (bleScanPending || wifi_bridge_scan_begin() != ESP_OK) error = "scan_busy";
		else { bleScanPending = true; bleScanId = id; bleScanStarted = esp_timer_get_time(); replied = true; }
	} else if (!strcmp(cmd, "wifi_set")) {
		if (ssidLen > 32 || passwordLen > 64 || !wifi_valid_ssid(ssid) || !wifi_valid_password(password)) error = "bad_wifi";
		else if (!saveWifiStore(ssid, password)) error = "save_failed";
		else if (wifi_bridge_set_sta_credentials(activeSsid, activePassword) != ESP_OK) error = "connect_failed";
		bleScanPending = false;
	} else if (!strcmp(cmd, "wifi_forget")) {
		if (!forgetWifiStore()) error = "save_failed";
		else if (wifi_bridge_set_sta_credentials(CONFIG_WIRETFT_SSID, CONFIG_WIRETFT_PASSWORD) != ESP_OK) error = "connect_failed";
		bleScanPending = false;
	} else if (!strcmp(cmd, "music")) {
		if (!hasTranslation) {
			xSemaphoreTake(musicLock, portMAX_DELAY);
			translation = musicConfig.translation;
			xSemaphoreGive(musicLock);
		}
		error = hasServer ? applyMusic(server, serverLen, translation) : "bad_request";
	} else if (!strcmp(cmd, "display")) {
		error = applyOrientation(orientation);
	} else if (!strcmp(cmd, "image_begin")) {
		uint8_t o = ORIENT_PORTRAIT;
		if (!parseOrientation(orientation, o)) error = "bad_orientation";
		else {
			bleImageStop();
			xSemaphoreTake(bleImageMutex, portMAX_DELAY);
			error = imageBegin(bleImage, o);
			if (!error) { bleImageFailed = false; bleImageAcked = 0; bleImageActive = true; }
			xSemaphoreGive(bleImageMutex);
			snprintf(extra, sizeof(extra), ",\"size\":%u,\"chunk\":%u,\"window\":%u",
				unsigned(IMAGE_BYTES), unsigned(bleImageChunk()), unsigned(BLE_IMAGE_WINDOW));
		}
	} else if (!strcmp(cmd, "image_end")) {
		xSemaphoreTake(bleImageMutex, portMAX_DELAY);
		if (!bleImageActive) error = "no_image";
		else if (bleImageFailed) { imageAbort(bleImage); error = "transfer_failed"; }
		else error = imageFinish(bleImage, uint32_t(crc));
		bleImageActive = false;
		xSemaphoreGive(bleImageMutex);
	} else if (!strcmp(cmd, "image_cancel")) {
		bleImageStop();
	} else error = "unknown_command";
	wifi_wipe(password, sizeof(password));
	if (!replied) bleReply(id, error, error && strcmp(error, "locked") ? "" : extra);
}

static void bleService()
{
	if (bleDisconnected) {
		bleDisconnected = false;
		bleImageStop();
		bleScanPending = false;
		bleCmdLen = 0; bleCmdOverflow = false; bleCmdReady = false;
	}
	if (bleScanPending) bleReplyScan();
	if (bleImageActive) {
		// Acknowledgements pace the client: it keeps at most one window unacknowledged.
		const size_t got = bleImage.got;
		char json[48];
		if (bleImageFailed) {
			if (bleImageAcked != SIZE_MAX) bleEvent("{\"evt\":\"image\",\"error\":\"transfer_failed\"}");
			bleImageAcked = SIZE_MAX;
		} else if (got >= bleImageAcked + BLE_IMAGE_ACK || (got == IMAGE_BYTES && bleImageAcked != got)) {
			snprintf(json, sizeof(json), "{\"evt\":\"image\",\"got\":%u}", unsigned(got));
			bleEvent(json);
			bleImageAcked = got;
		}
	}
	if (bleCmdReady) {
		if (bleCmdOverflow) bleReply(0, "too_large");
		else bleHandleCommand(bleCmd, bleCmdLen);
		wifi_wipe(bleCmd, sizeof(bleCmd));
		bleCmdLen = 0; bleCmdOverflow = false; bleCmdReady = false;
	}
}

static esp_err_t upload(httpd_req_t *req)
{
	if (!localRequest(req) || !requireCode(req)) return ESP_FAIL;
	char type[64] = {}, checksum[16] = {};
	httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type));
	httpd_req_get_hdr_value_str(req, "X-Image-CRC32", checksum, sizeof(checksum));
	char *end = nullptr;
	uint32_t expected = strtoul(checksum, &end, 16);
	if (req->content_len != IMAGE_BYTES || strcmp(type, "application/octet-stream") ||
		strlen(checksum) != 8 || !end || *end) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected 153600 RGB565BE bytes and CRC32");
		return ESP_FAIL;
	}
	uint8_t orient = ORIENT_PORTRAIT;
	if (!headerOrientation(req, "X-Image-Orientation", orient)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid X-Image-Orientation");
		return ESP_FAIL;
	}
	ImageWriter w;
	if (const char *error = imageBegin(w, orient)) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, error);
		return ESP_FAIL;
	}
	uint8_t buf[960];
	const int64_t deadline = esp_timer_get_time() + 30000000;
	while (w.ok && w.got < IMAGE_BYTES) {
		const size_t want = IMAGE_BYTES - w.got;
		const int n = esp_timer_get_time() < deadline ? httpd_req_recv(req, (char *)buf, want < sizeof(buf) ? want : sizeof(buf)) : -1;
		if (n <= 0) { w.ok = false; break; }
		imageWrite(w, buf, n);
		delay(1);
	}
	if (imageFinish(w, expected)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload incomplete or storage/CRC error; previous image retained");
		return ESP_FAIL; // Close incomplete request, do not reuse unread body.
	}
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void drawWaiting()
{
	const int w = screenWidth(), h = screenHeight() - 20;
	gfx.fillScreen(0x0000);
	waitingShown = wifiStatusDirty = true;
	gfx.setTextColor(0xffff); gfx.setTextSize(2); gfx.setCursor(w / 2 - 104, h / 2 - 20);
	gfx.println("wireTFT");
	gfx.setTextSize(1); gfx.setCursor(w / 2 - 104, h / 2 - 2);
	gfx.println("Waiting for image");
	gfx.setCursor(w / 2 - 104, h / 2 + 12);
	gfx.printf("Bluetooth: %s", bleName);
}

// Redraw the non-music screen: saved photo or the waiting text. Holds displayLock.
static void drawIdleScreen()
{
	if (xSemaphoreTake(displayLock, pdMS_TO_TICKS(2000)) != pdTRUE) return;
	if (currentSlot < 0 || !displaySlot(currentSlot)) drawWaiting();
	xSemaphoreGive(displayLock);
}

void setup()
{
	Serial.begin(115200);
	displayLock = xSemaphoreCreateMutex();
	musicLock = xSemaphoreCreateMutex();
	bleImageMutex = xSemaphoreCreateMutex();
	imageLock = xSemaphoreCreateBinary();
	xSemaphoreGive(imageLock);
	pinMode(32, OUTPUT);
	digitalWrite(32, LOW);
	if (!gfx.begin(40000000)) { Serial.println("Display init failed"); return; }
	gfx.setRotation(0);
	bus.beginWrite();
	bus.writeC8D8(0x36, 0x48);
	bus.endWrite();
	bus.sendCommand(0x20);
	gfx.fillScreen(0);
	digitalWrite(32, HIGH);
	esp_err_t err = nvs_flash_init();
	// Never erase saved credentials automatically on an NVS error.
	ESP_ERROR_CHECK(err);
	loadMusicConfig();
	loadOrientation();
	if (displayOrientation != ORIENT_PORTRAIT) applyPanelOrientation(displayOrientation);
	esp_vfs_spiffs_conf_t fs{};
	fs.base_path = "/images";
	fs.partition_label = "images";
	fs.max_files = 3;
	fs.format_if_mount_failed = false;
	storageReady = esp_vfs_spiffs_register(&fs) == ESP_OK;
	// Only format a blank partition. Never erase a damaged existing image store.
	if (!storageReady) {
		const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "images");
		uint8_t block[256];
		bool blank = p != nullptr;
		for (size_t off = 0; blank && off < p->size; off += sizeof(block)) {
			blank = esp_partition_read(p, off, block, sizeof(block)) == ESP_OK;
			for (uint8_t b : block) if (b != 0xff) blank = false;
			delay(1);
		}
		if (blank && esp_spiffs_format("images") == ESP_OK)
			storageReady = esp_vfs_spiffs_register(&fs) == ESP_OK;
	}
	if (storageReady) {
		ImageHeader a{}, b{};
		bool va = validateSlot(0, a), vb = validateSlot(1, b);
		if (va) { currentSlot = 0; generation = a.generation; }
		if (vb && (!va || int32_t(b.generation - generation) > 0)) {
			currentSlot = 1; generation = b.generation;
		}
		if (currentSlot >= 0 && !musicEnabled()) displaySlot(currentSlot);
	}
	loadWifiStore();
	const char *bootSsid = savedWifi ? activeSsid : CONFIG_WIRETFT_SSID;
	const char *bootPassword = savedWifi ? activePassword : CONFIG_WIRETFT_PASSWORD;
	ESP_ERROR_CHECK(wifi_bridge_start(bootSsid, bootPassword));
	uint8_t mac[6];
	ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
	snprintf(deviceId, sizeof(deviceId), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	snprintf(bleName, sizeof(bleName), "wireTFT-%s", deviceId + 8);
	if (currentSlot < 0 && !musicEnabled()) drawWaiting();
	loadConfigCode();
	const bool bleOk = bleStart(bleName);
	Serial.println("串口命令已就绪：输入 wifi help");
	Serial.printf("蓝牙名称：%s%s\n", bleName, bleOk ? "" : "（蓝牙启动失败）");
	Serial.printf("配置码：%s（蓝牙配置、控制和局域网写操作需要此码，输入 code reset 可更换）\n",
		configCode[0] ? configCode : "不可用");
	if (!strlen(bootSsid)) Serial.println("未配置 Wi-Fi：请用网页通过蓝牙配置，或输入 wifi setup");
	wifi_bridge_dump_info();
}

void loop()
{
	serviceSerial();
	bleService();
	musicService();
	if (pendingOrientation != ORIENT_NONE && xSemaphoreTake(displayLock, pdMS_TO_TICKS(2000)) == pdTRUE) {
		const uint8_t o = pendingOrientation;
		pendingOrientation = ORIENT_NONE;
		const bool changed = o != displayOrientation;
		if (changed) {
			displayOrientation = o;
			applyPanelOrientation(o);
			gfx.fillScreen(0);
			waitingShown = false;
		}
		xSemaphoreGive(displayLock);
		if (changed) {
			// Music rebuilds its layout and strip canvas for the new size on re-entry.
			if (view.shown) musicLeave();
			if (!musicEnabled()) drawIdleScreen();
			drawWifiStatus(true);
		}
	}
	const bool musicOn = musicEnabled();
	if (musicOn) {
		// Saved photos stay in their slot and reappear when music mode ends.
		pendingDisplaySlot = -1;
		musicRender();
	} else if (view.shown) {
		musicLeave();
		drawIdleScreen();
		drawWifiStatus(true);
	}
	if (pendingDisplaySlot >= 0) {
		if (xSemaphoreTake(displayLock, pdMS_TO_TICKS(100)) == pdTRUE) {
			int slot = pendingDisplaySlot;
			pendingDisplaySlot = -1;
			if (slot >= 0) displaySlot(slot);
			xSemaphoreGive(displayLock);
			drawWifiStatus(true);
		}
	}
	drawWifiStatus();
	static httpd_handle_t server = nullptr;
	static char boundIP[16] = "";
	if (server && (!wifi_bridge_has_ip() || strcmp(boundIP, wifi_bridge_ip_str()))) {
		httpd_stop(server); server = nullptr;
	}
	if (!server && wifi_bridge_has_ip()) {
		in_addr addr{};
		inet_aton(wifi_bridge_ip_str(), &addr);
		if (privateIP(addr.s_addr)) {
			httpd_config_t config = HTTPD_DEFAULT_CONFIG();
			config.stack_size = 8192;
			config.recv_wait_timeout = 5;
			config.send_wait_timeout = 5;
			config.max_open_sockets = 3;
			config.lru_purge_enable = true;
			if (httpd_start(&server, &config) == ESP_OK) {
				snprintf(boundIP, sizeof(boundIP), "%s", wifi_bridge_ip_str());
				httpd_uri_t info{}; info.uri = "/api/info"; info.method = HTTP_GET; info.handler = identify;
				httpd_uri_t image{}; image.uri = "/api/image"; image.method = HTTP_POST; image.handler = upload;
				ESP_ERROR_CHECK(httpd_register_uri_handler(server, &info));
				ESP_ERROR_CHECK(httpd_register_uri_handler(server, &image));
				httpd_uri_t music{}; music.uri = "/api/music"; music.method = HTTP_POST; music.handler = musicSettings;
				ESP_ERROR_CHECK(httpd_register_uri_handler(server, &music));
				httpd_uri_t display{}; display.uri = "/api/display"; display.method = HTTP_POST; display.handler = displaySettings;
				ESP_ERROR_CHECK(httpd_register_uri_handler(server, &display));
				Serial.printf("wireTFT listening on private LAN %s:80\n", wifi_bridge_ip_str());
			}
		}
	}
	if (server && !wifi_bridge_has_ip()) { httpd_stop(server); server = nullptr; }
	delay(bleImageActive ? 5 : musicOn ? 15 : 50);
}
