"use strict";
// GATT protocol: see ble_link.h in the firmware.
const SERVICE = "3f6a0001-8c2e-4d1b-9a77-5e9b1c7d0a10";
const RX = "3f6a0002-8c2e-4d1b-9a77-5e9b1c7d0a10";
const TX = "3f6a0003-8c2e-4d1b-9a77-5e9b1c7d0a10";
const RX_JSON_MORE = 0x01, RX_JSON_END = 0x02, RX_IMAGE = 0x10;
const TX_MORE = 0x01, TX_END = 0x02, TX_EVENT = 0x03;
const JSON_FRAGMENT = 180;

const $ = (id) => document.getElementById(id);
const canvas = $("preview");
const ctx = canvas.getContext("2d", {willReadFrequently: true});
const encoder = new TextEncoder(), decoder = new TextDecoder();

let device = null, rx = null, tx = null;
let code = "", info = null, authed = false, busy = false, image = null, imageVersion = 0;
let nextId = 1, pending = null, responseParts = [], chain = Promise.resolve();
let imageGot = 0, imageError = "", ackWaiters = [];
let pollTimer = null;

const orientationNames = {portrait: "竖屏", landscape: "横屏"};
const wifiStates = {GOT_IP: "已连接", CONNECTED: "正在获取 IP", STARTED: "正在连接", FAILED: "连接失败", IDLE: "未连接"};
const musicStates = {connecting: "正在连接", connected: "已连接", error: "连接异常", off: "未启用"};
const musicDetails = {
  connect_failed: "无法连接，请确认地址、端口和 go-musicfox 已开启局域网投送",
  http_status: "服务返回错误状态", no_response: "服务无响应", send_failed: "发送请求失败",
  disconnected: "连接已断开，正在重连", event_too_large: "单条推送过大，请减小 windowBefore / windowAfter",
  bad_event: "推送内容无法解析（协议版本不兼容？）", no_memory: "设备内存不足"
};
const errors = {
  bad_code: "配置码错误。连续输错 5 次，设备会拒绝配置 1 分钟。",
  bad_request: "请求格式错误。", too_large: "请求过大。", unknown_command: "设备固件不支持此操作，请升级固件。",
  scan_busy: "设备正在连接 Wi-Fi 或正在扫描，请稍后再试，也可以直接输入网络名称。",
  scan_failed: "扫描失败，请稍后重试。",
  bad_wifi: "Wi-Fi 名称或密码无效：名称 1–32 字节；密码为空、8–63 个可打印字符，或 64 位十六进制。",
  save_failed: "设备保存设置失败。", connect_failed: "设置已保存，但设备启动连接失败。",
  bad_server: "音乐服务地址必须是私网 IPv4:端口，例如 192.168.1.50:8765。",
  bad_orientation: "屏幕方向无效。", image_busy: "设备正在接收另一张图片，请稍后再试。",
  storage_unavailable: "设备图片存储不可用。", cannot_open_slot: "设备无法写入图片存储。",
  transfer_failed: "图片传输中断，设备保留原图片。", crc_or_storage_error: "图片校验或保存失败，设备保留原图片。",
  no_image: "没有进行中的图片传输。", no_memory: "设备内存不足，请稍后再试。"
};

function message(text, kind = "") {
  $("message").textContent = text;
  $("message").className = `message ${kind}`;
}

function deviceError(reply) {
  if (reply.error === "locked") return new Error(`配置码输错次数过多，请 ${reply.retry_after || 60} 秒后再试。`);
  return new Error(errors[reply.error] || `设备返回错误：${reply.error}`);
}

function codeKey() { return `wiretft-code:${device?.name || ""}`; }

// ---------------- Transport ----------------

function onNotify(event) {
  const value = event.target.value;
  const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  if (!bytes.length) return;
  const kind = bytes[0], body = bytes.slice(1);
  if (kind === TX_EVENT) {
    try { onEvent(JSON.parse(decoder.decode(body))); } catch { /* ignore malformed event */ }
    return;
  }
  responseParts.push(body);
  if (kind !== TX_END) return;
  const total = new Uint8Array(responseParts.reduce((n, p) => n + p.length, 0));
  let offset = 0;
  for (const part of responseParts) { total.set(part, offset); offset += part.length; }
  responseParts = [];
  let reply;
  try { reply = JSON.parse(decoder.decode(total)); } catch { return; }
  if (pending && (reply.id === pending.id || reply.id === 0)) {
    const done = pending;
    pending = null;
    clearTimeout(done.timer);
    done.resolve(reply);
  }
}

function onEvent(event) {
  if (event.evt !== "image") return;
  if (event.error) imageError = event.error;
  if (typeof event.got === "number") imageGot = event.got;
  const waiters = ackWaiters;
  ackWaiters = [];
  waiters.forEach(w => w());
}

function waitAck(timeout = 10000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("设备长时间未确认图片数据，传输已中止。")), timeout);
    ackWaiters.push(() => { clearTimeout(timer); resolve(); });
  });
}

async function send(cmd, fields, timeout) {
  if (!rx) throw new Error("未连接设备。");
  const id = nextId++;
  const body = encoder.encode(JSON.stringify({id, cmd, code, ...fields}));
  const reply = new Promise((resolve, reject) => {
    pending = {id, resolve, reject, timer: setTimeout(() => { pending = null; reject(new Error("设备无响应，请确认设备在附近并重新连接。")); }, timeout)};
  });
  try {
    responseParts = [];
    for (let offset = 0; offset < body.length; offset += JSON_FRAGMENT) {
      const part = body.subarray(offset, offset + JSON_FRAGMENT);
      const frame = new Uint8Array(part.length + 1);
      frame[0] = offset + JSON_FRAGMENT >= body.length ? RX_JSON_END : RX_JSON_MORE;
      frame.set(part, 1);
      await rx.writeValueWithResponse(frame);
    }
  } catch (error) {
    if (pending?.id === id) { clearTimeout(pending.timer); pending = null; }
    throw new Error(`蓝牙写入失败：${error.message}`);
  }
  const result = await reply;
  if (!result.ok) throw deviceError(result);
  return result;
}

// Requests are serialised: the device handles one command at a time.
function request(cmd, fields = {}, timeout = 8000) {
  const run = chain.then(() => send(cmd, fields, timeout));
  chain = run.catch(() => {});
  return run;
}

// ---------------- Connection ----------------

async function connect() {
  try {
    const chosen = await navigator.bluetooth.requestDevice({filters: [{services: [SERVICE]}], optionalServices: [SERVICE]});
    disconnect();
    device = chosen;
    chosen.addEventListener("gattserverdisconnected", () => { if (device === chosen) onDisconnected(); });
    $("deviceName").textContent = device.name || "wireTFT";
    message(`正在连接 ${device.name || "设备"}…`);
    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE);
    rx = await service.getCharacteristic(RX);
    tx = await service.getCharacteristic(TX);
    tx.addEventListener("characteristicvaluechanged", onNotify);
    await tx.startNotifications();
    code = localStorage.getItem(codeKey()) || "";
    $("code").value = code;
    update();
    if (code) await authenticate();
    else { message("已连接。请输入设备的 6 位配置码。"); $("code").focus(); }
  } catch (error) {
    if (error.name === "NotFoundError") message("未选择设备。");
    else message(`连接失败：${error.message}`, "error");
    if (!rx) disconnect();
  }
}

function onDisconnected() {
  rx = tx = null;
  authed = false;
  info = null;
  if (pending) { clearTimeout(pending.timer); pending.reject(new Error("蓝牙连接已断开。")); pending = null; }
  onEvent({evt: "image", error: "transfer_failed"});
  clearInterval(pollTimer);
  pollTimer = null;
  update();
  message("蓝牙连接已断开，可重新连接。", "error");
}

function disconnect() {
  const old = device;
  device = null;
  if (old?.gatt?.connected) old.gatt.disconnect();
  rx = tx = null;
  authed = false;
  info = null;
  clearInterval(pollTimer);
  pollTimer = null;
  update();
}

async function authenticate() {
  code = $("code").value.trim();
  if (!/^\d{6}$/.test(code)) { message("配置码为 6 位数字。", "error"); return; }
  try {
    info = (await request("info")).info;
    authed = true;
    if ($("remember").checked) localStorage.setItem(codeKey(), code);
    else localStorage.removeItem(codeKey());
    fillForms();
    update();
    message(`配置码正确，已连接 ${info.name}。`, "success");
    clearInterval(pollTimer);
    pollTimer = setInterval(() => { if (!busy && authed) refresh(true); }, 5000);
  } catch (error) {
    authed = false;
    if (/配置码/.test(error.message)) localStorage.removeItem(codeKey());
    update();
    message(error.message, "error");
  }
}

async function refresh(quiet = false) {
  try {
    info = (await request("info")).info;
    update();
    if (!quiet) message("状态已刷新。", "success");
  } catch (error) { if (!quiet) message(error.message, "error"); }
}

// Runs one operation with the controls locked.
async function operate(text, task) {
  if (busy || !authed) return;
  busy = true;
  update();
  message(text);
  try { await task(); }
  catch (error) { message(error.message, "error"); }
  finally { busy = false; update(); }
}

// ---------------- UI state ----------------

function screenSize() {
  return info?.orientation === "landscape" ? {w: 320, h: 240} : {w: 240, h: 320};
}

function canvasOrientation() { return canvas.width > canvas.height ? "landscape" : "portrait"; }

function syncCanvas() {
  const {w, h} = screenSize();
  $("screenSize").textContent = `${w} × ${h}`;
  if (canvas.width === w && canvas.height === h) return;
  canvas.width = w; canvas.height = h;
  canvas.classList.toggle("landscape", w > h);
  draw();
}

function fillForms() {
  if (!info) return;
  $("orientation").value = info.orientation;
  $("musicServer").value = info.music_server || "";
  $("musicTranslation").checked = info.music_translation === true;
  if (info.wifi_ssid && !$("ssid").value) $("ssid").value = info.wifi_ssid;
}

function statusRow(label, value) {
  const dt = document.createElement("dt"), dd = document.createElement("dd");
  dt.textContent = label; dd.textContent = value;
  return [dt, dd];
}

function update() {
  const connected = Boolean(rx), ready = connected && authed && !busy;
  $("badge").textContent = authed ? "● 已连接" : connected ? "◐ 待验证" : "○ 未连接";
  $("badge").classList.toggle("on", authed);
  if (!device) $("deviceName").textContent = "未选择";
  $("connect").disabled = busy || !navigator.bluetooth;
  $("connect").textContent = connected ? "连接其它设备" : "搜索并连接蓝牙设备";
  $("code").disabled = $("codeSubmit").disabled = !connected || busy;
  $("refresh").disabled = !ready;
  $("disconnect").disabled = !connected || busy;
  for (const id of ["wifiScan", "ssid", "password", "showPassword", "wifiSave", "wifiForget", "orientation",
    "musicServer", "musicTranslation", "musicSave"]) $(id).disabled = !ready;
  $("musicOff").disabled = !ready || !info?.music_server;
  $("orientationApply").disabled = !ready || $("orientation").value === info?.orientation;
  $("upload").disabled = !ready || !image;
  $("file").disabled = busy;

  const rows = [];
  if (info) {
    const wifi = wifiStates[info.wifi_state] || info.wifi_state;
    rows.push(statusRow("设备", `${info.name} · ${info.id}`));
    rows.push(statusRow("Wi-Fi", info.wifi_ssid ? `${wifi} · ${info.wifi_ssid}${info.wifi_state === "GOT_IP" ? ` · ${info.ip}` : ""}` : "未配置"));
    rows.push(statusRow("屏幕", `${orientationNames[info.orientation]} · ${info.has_image ? "已有照片" : "暂无照片"}`));
    rows.push(statusRow("音乐", info.music_server ? `${musicStates[info.music_state] || info.music_state} · ${info.music_server}` : "未启用"));
    rows.push(statusRow("可用内存", `${Math.round(info.heap_free / 1024)} KiB`));
  }
  $("status").replaceChildren(...rows.flat());
  $("wifiSummary").textContent = info ? (wifiStates[info.wifi_state] || info.wifi_state) : "—";

  let orientation = "连接设备后可切换，设置保存在设备中。";
  if (info) {
    orientation = `当前为${orientationNames[info.orientation]}，设置保存在设备中，重启后保持。`;
    if (info.has_image && info.image_orientation && info.image_orientation !== info.orientation)
      orientation += `已保存的照片是${orientationNames[info.image_orientation]}画面，会按原方向显示，请重新发送。`;
  }
  $("orientationStatus").textContent = orientation;

  let music = "连接设备后可设置。";
  if (info && !info.music_server) music = "音乐模式未启用，屏幕显示照片。";
  else if (info) {
    const detail = musicDetails[info.music_detail] || info.music_detail;
    music = `${musicStates[info.music_state] || info.music_state} ${info.music_server}${detail ? ` · ${detail}` : ""}`;
    if (info.wifi_state !== "GOT_IP") music += " · 设备尚未连上 Wi-Fi";
  }
  $("musicStatus").textContent = music;
  syncCanvas();
}

// ---------------- Wi-Fi ----------------

function renderNetworks(networks) {
  const list = $("networks");
  list.replaceChildren();
  if (!networks.length) {
    const empty = document.createElement("p");
    empty.className = "muted";
    empty.textContent = "没有发现 2.4 GHz 网络。";
    list.append(empty);
    return;
  }
  for (const n of networks.sort((a, b) => b.rssi - a.rssi)) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "network";
    const name = document.createElement("span");
    name.textContent = n.ssid;
    const meta = document.createElement("small");
    meta.textContent = `${n.secure ? "加密" : "开放"} · ${n.rssi} dBm`;
    button.append(name, meta);
    button.onclick = () => { $("ssid").value = n.ssid; $("password").value = ""; $(n.secure ? "password" : "wifiSave").focus(); };
    list.append(button);
  }
}

$("wifiScan").onclick = () => operate("正在扫描附近的 Wi-Fi…", async () => {
  const reply = await request("wifi_scan", {}, 25000);
  renderNetworks(reply.networks);
  message(`发现 ${reply.networks.length} 个网络，点击即可填入名称。`, "success");
});

$("wifiForm").onsubmit = event => {
  event.preventDefault();
  const ssid = $("ssid").value, password = $("password").value;
  operate(`正在把「${ssid}」写入设备…`, async () => {
    await request("wifi_set", {ssid, password}, 15000);
    $("password").value = "";
    message(`已保存「${ssid}」，设备正在连接…`);
    for (let i = 0; i < 20; ++i) {
      await new Promise(r => setTimeout(r, 1000));
      info = (await request("info")).info;
      update();
      if (info.wifi_state === "GOT_IP") { message(`设备已连上「${ssid}」，IP ${info.ip}。`, "success"); return; }
      if (info.wifi_state === "FAILED") throw new Error(`设备无法连上「${ssid}」，请检查密码，或确认是 2.4 GHz 网络。`);
    }
    message("设置已保存，设备仍在连接中，可稍后刷新状态查看。");
  });
};

$("showPassword").onclick = () => {
  const hidden = $("password").type === "password";
  $("password").type = hidden ? "text" : "password";
  $("showPassword").textContent = hidden ? "隐藏" : "显示";
};

$("wifiForget").onclick = () => {
  if (!confirm("清除设备上保存的 Wi-Fi？音乐摆件将无法联网。")) return;
  operate("正在清除 Wi-Fi 设置…", async () => {
    await request("wifi_forget", {}, 15000);
    $("ssid").value = "";
    info = (await request("info")).info;
    message("已清除设备上保存的 Wi-Fi。", "success");
  });
};

// ---------------- Display and music ----------------

$("orientation").onchange = () => update();
$("orientationApply").onclick = () => {
  const orientation = $("orientation").value;
  operate("正在切换屏幕方向…", async () => {
    await request("display", {orientation});
    info = (await request("info")).info;
    message(`已切换为${orientationNames[orientation]}，预览尺寸已同步；照片需按新方向重新发送。`, "success");
  });
};

async function setMusic(server) {
  await operate(server ? "正在启用音乐摆件…" : "正在关闭音乐模式…", async () => {
    await request("music", {server, translation: $("musicTranslation").checked});
    info = (await request("info")).info;
    message(server ? `已启用音乐摆件，连接 ${server}。` : "已关闭音乐模式，恢复照片显示。", "success");
  });
}
$("musicForm").onsubmit = event => {
  event.preventDefault();
  const server = $("musicServer").value.trim();
  if (!server) { message("请填写音乐服务地址，例如 192.168.1.50:8765。", "error"); return; }
  setMusic(server);
};
$("musicOff").onclick = () => setMusic("");

// ---------------- Image ----------------

function draw() {
  const mode = $("fit").value;
  const W = canvas.width, H = canvas.height;
  const hints = {
    stretch: `拉伸至 ${W} × ${H}，不留边、不裁剪，宽高比不同时可能变形。`,
    contain: "保持原始比例，完整显示图片；宽高比不同时会留边。",
    cover: "保持原始比例铺满画面；宽高比不同时会居中裁剪超出部分。"
  };
  $("fitHint").textContent = hints[mode];
  ctx.fillStyle = $("background").value;
  ctx.fillRect(0, 0, W, H);
  if (!image) return;
  let w = W, h = H;
  if (mode !== "stretch") {
    const scale = (mode === "cover" ? Math.max : Math.min)(W / image.width, H / image.height);
    w = image.width * scale; h = image.height * scale;
  }
  ctx.drawImage(image, (W - w) / 2, (H - h) / 2, w, h);
}

async function handleImageFile(file) {
  if (busy || !file) return;
  if (!file.type.startsWith("image/")) {
    $("fileName").textContent = "不是图片文件";
    message("请选择 JPG、PNG、WebP 等图片文件。", "error");
    return;
  }
  const version = ++imageVersion;
  image?.close(); image = null;
  draw(); update();
  try {
    if (file.size > 20 * 1024 * 1024) throw new Error("请选择小于 20 MiB 的图片");
    const loaded = await createImageBitmap(file);
    if (version !== imageVersion) { loaded.close(); return; }
    if (loaded.width * loaded.height > 40000000) { loaded.close(); throw new Error("图片不得超过 4000 万像素"); }
    image = loaded;
    $("fileName").textContent = `${file.name} · ${image.width} × ${image.height}`;
    draw(); update();
    message(authed ? "预览已更新，可以发送到屏幕。" : "预览已更新，连接设备后即可发送。", "success");
  } catch (error) { $("fileName").textContent = "图片读取失败"; message(error.message, "error"); }
}

function rgb565(rgba) {
  const bytes = new Uint8Array(rgba.length / 2);
  for (let i = 0, j = 0; i < rgba.length; i += 4, j += 2) {
    const pixel = ((rgba[i] >> 3) << 11) | ((rgba[i + 1] >> 2) << 5) | (rgba[i + 2] >> 3);
    bytes[j] = pixel >> 8; bytes[j + 1] = pixel & 255;
  }
  return bytes;
}

const CRC_TABLE = Array.from({length: 256}, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function crc32(bytes) {
  let crc = 0xffffffff;
  for (const b of bytes) crc = CRC_TABLE[(crc ^ b) & 255] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

async function writeChunk(frame) {
  for (let attempt = 0; ; ++attempt) {
    try { await rx.writeValueWithoutResponse(frame); return; }
    catch (error) {
      if (!rx || attempt >= 5) throw new Error(`蓝牙写入失败：${error.message}`);
      await new Promise(r => setTimeout(r, 30));
    }
  }
}

// The device acknowledges every few KiB; at most one window stays unacknowledged.
async function sendImage(pixels, orientation) {
  const begin = await request("image_begin", {orientation});
  imageGot = 0; imageError = "";
  const progress = $("uploadProgress");
  progress.hidden = false; progress.max = pixels.length; progress.value = 0;
  const started = performance.now();
  try {
    for (let offset = 0; offset < pixels.length;) {
      while (offset - imageGot >= begin.window && !imageError) await waitAck();
      if (imageError) throw deviceError({error: imageError});
      const n = Math.min(begin.chunk, pixels.length - offset);
      const frame = new Uint8Array(n + 5);
      frame[0] = RX_IMAGE;
      new DataView(frame.buffer).setUint32(1, offset, true);
      frame.set(pixels.subarray(offset, offset + n), 5);
      await writeChunk(frame);
      offset += n;
      progress.value = imageGot;
      $("upload").textContent = `正在发送 ${Math.round(offset * 100 / pixels.length)}%`;
    }
    while (imageGot < pixels.length && !imageError) await waitAck();
    if (imageError) throw deviceError({error: imageError});
    progress.value = pixels.length;
    $("upload").textContent = "正在保存到 Flash…";
    await request("image_end", {crc: crc32(pixels)}, 20000);
    return (performance.now() - started) / 1000;
  } catch (error) {
    if (rx) await request("image_cancel").catch(() => {});
    throw error;
  } finally {
    progress.hidden = true;
    $("upload").textContent = "发送到屏幕";
  }
}

$("upload").onclick = () => {
  if (!image) return;
  const pixels = rgb565(ctx.getImageData(0, 0, canvas.width, canvas.height).data);
  const orientation = canvasOrientation();
  operate("正在通过蓝牙发送图片，请保持页面开启并靠近设备…", async () => {
    const seconds = await sendImage(pixels, orientation);
    info = (await request("info")).info;
    message(info.music_server
      ? `图片已保存（${seconds.toFixed(1)} 秒）；当前为音乐模式，关闭音乐模式后显示。`
      : `图片已保存并显示（${seconds.toFixed(1)} 秒），设备重启后会恢复显示。`, "success");
  });
};

$("file").onchange = () => handleImageFile($("file").files[0]);
const dropzone = $("dropzone");
dropzone.onclick = event => { if (event.target !== $("file")) $("file").click(); };
dropzone.onkeydown = event => {
  if (event.key === "Enter" || event.key === " ") { event.preventDefault(); $("file").click(); }
};
["dragenter", "dragover"].forEach(type => dropzone.addEventListener(type, event => {
  event.preventDefault(); event.stopPropagation();
  if (!busy) dropzone.classList.add("dragging");
}));
dropzone.addEventListener("dragleave", event => {
  if (!dropzone.contains(event.relatedTarget)) dropzone.classList.remove("dragging");
});
dropzone.addEventListener("drop", event => {
  event.preventDefault(); event.stopPropagation(); dropzone.classList.remove("dragging");
  handleImageFile(event.dataTransfer.files[0]);
});
$("fit").onchange = draw;
$("background").oninput = draw;

// ---------------- Start ----------------

$("connect").onclick = connect;
$("disconnect").onclick = () => { disconnect(); message("已断开连接。"); };
$("refresh").onclick = () => refresh();
$("codeForm").onsubmit = event => { event.preventDefault(); authenticate(); };
$("code").oninput = () => { $("code").value = $("code").value.replace(/\D/g, "").slice(0, 6); };

if (!navigator.bluetooth) {
  $("unsupported").hidden = false;
  message("当前浏览器不支持 Web Bluetooth。", "error");
}
window.addEventListener("pagehide", () => { image?.close(); image = null; disconnect(); });
draw();
update();
