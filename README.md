# wireTFT

ESP32-WROOM-32D + ST7789 240×320 桌面屏。用低功耗蓝牙完成配网、换照片、切换方向和音乐摆件设置。控制页是静态 HTTPS 页面，浏览器通过 Web Bluetooth 直接连接设备，电脑上不用跑服务。每台设备有一个 6 位配置码，配置和控制都必须带上它，避免同一环境里的多块屏幕被误操作。

照片以 RGB565 保存在 SPIFFS 双槽中。整张图传输完并且 CRC 通过后才切换活动槽，所以断线、短包、CRC 错误都不会换掉上一张图。

硬件工程在 `hardware/`（KiCad 与嘉立创 EDA）。固件按 GPL-2.0 发布，见 `LICENSE`。

## 硬件连接

经典 ESP32、4 MB Flash、不需要 PSRAM。屏幕为 10 针 FPC 的 ST7789，无 MISO、无 TE。

| 信号 | GPIO |
|---|---|
| SCK | 18 |
| MOSI | 23 |
| CS | 21 |
| DC | 27 |
| RST | 33 |
| 背光 | 32，高电平点亮 |

SPI 时钟 40 MHz。这些引脚走 GPIO 矩阵，40 MHz 已是该路径的上限，不要改成 80 MHz，也超出 ST7789 的时钟规格。

## 控制页面

页面源码在 `web/public/`，用 Cloudflare Workers 的静态资源托管（`web/wrangler.jsonc`，没有 Worker 脚本）。需要桌面版 Chrome / Edge，或 Android Chrome。iOS Safari 没有 Web Bluetooth。

```text
cd web
npx wrangler deploy
```

部署完成后用自己的 `*.workers.dev` 地址打开：

1. 「搜索并连接蓝牙设备」，选择 `wireTFT-xxxx`（后 4 位是设备 ID 的末尾）。
2. 输入串口打印的 6 位配置码。勾选「记住」后，配置码按设备名存在这个浏览器的 localStorage。
3. 「02 Wi-Fi」扫描 2.4 GHz 网络并保存。
4. 「03 屏幕与画面」切换竖屏 / 横屏、发送图片、填写音乐服务地址。

照片和方向只需要蓝牙。音乐摆件需要设备已经连上 Wi-Fi。`_headers` 限制了脚本来源，并只允许本页使用蓝牙。

## 配置码

- NVS 里没有配置码时，用硬件随机数生成 6 位数字，存在键 `code`，之后重启不变。
- 每次启动串口都会打印配置码和蓝牙名称，格式为 `配置码：<六位数字>  蓝牙名称：wireTFT-xxxx`。串口输入 `code` 可再看一次，`code reset` 生成新码，浏览器里记住的旧码立即失效。
- 连续输错 5 次后，60 秒内拒绝所有要配置码的操作。蓝牙返回 `locked`，局域网返回 429。
- 蓝牙不配对、不加密。配置码只防止误操作，挡不住能听空口的人。Wi-Fi 密码也是明文经过空口，在可信环境里配网。

## 串口命令

115200 baud、8N1。打开串口会复位芯片（DTR/RTS）。

```text
wifi setup   # 交互式输入 SSID 与密码（密码不回显）
wifi status  # 连接状态与 IP
wifi forget  # 删除保存的凭据，回退到编译配置
code         # 显示配置码
code reset   # 重新生成配置码
wifi help    # 帮助
```

SSID 为 1–32 字节。密码可以为空（开放网络）、8–63 个可打印字符，或 64 位十六进制 PSK。蓝牙和串口写的是同一条 NVS 记录。没有这条记录时，使用 `idf.py menuconfig → wireTFT Wi-Fi` 里的编译配置。两项都空时不连接，等待蓝牙或串口配网。设备只支持 2.4 GHz。

没有照片时显示等待画面，底部有蓝牙名称和 Wi-Fi 状态（`WiFi OK <IP>` 等）。照片和音乐界面占满全屏。

## 构建与烧录

在 ESP-IDF 5.5 上验证。另外两个依赖要单独准备，本仓库不包含它们：

- [Arduino-ESP32 3.3.12](https://github.com/espressif/arduino-esp32/releases/tag/3.3.12)，用作 IDF 组件。`ARDUINO_COMPONENT_PATH` 指向里面的 `components/arduino`。
- [Arduino_GFX 1.6.3](https://github.com/moononournation/Arduino_GFX/releases/tag/1.6.3) 的源码目录。

这不是 Arduino IDE 工程。根 `CMakeLists.txt` 在两个路径为空时会直接失败。

```text
. $IDF_PATH/export.sh
idf.py -B build set-target esp32 \
  -DARDUINO_COMPONENT_PATH=/path/to/arduino \
  -DARDUINO_GFX_PATH=/path/to/Arduino_GFX-1.6.3
idf.py -B build menuconfig
idf.py -B build build
```

`menuconfig` 里的 `wireTFT Wi-Fi` 可以留空，以后用蓝牙或串口配置。如果在这里填了密码，它会写进构建目录的 `sdkconfig`。`sdkconfig` 已被忽略，不要提交，也不要把它和固件一起外发。

第一次烧录写入 bootloader、分区表和应用，按地址写入，不擦整片：

```text
idf.py -B build -p PORT -b 460800 flash
```

`PORT` 在 Linux 上一般是 `/dev/ttyUSB0`，Windows 上是 `COMx`。用 WSL 时，USB 串口要先接到这个发行版里。之后只改了应用：

```text
idf.py -B build -p PORT -b 460800 app-flash
```

`app-flash` 只覆盖 `0x10000` 的 factory 分区，下面这些会留下来：

| 分区 | 偏移 | 大小 | 内容 |
|---|---|---|---|
| nvs | `0x9000` | 24 KB | Wi-Fi、配置码、方向、音乐地址 |
| phy_init | `0xf000` | 4 KB | 分区表保留；本固件不往里写校准数据 |
| factory | `0x10000` | 2 MB | 应用程序 |
| images | `0x210000` | 1984 KB | 两张照片 |

不要运行 `idf.py erase-flash` 或 `esptool erase_flash`。那会清掉 NVS 和照片。也不要改 `partitions.csv` 之后再整片烧录，除非接受这些数据消失。NVS 初始化失败时固件直接停住，不会自动擦除 NVS。

烧录后用 115200 打开串口，确认打印了蓝牙名称和配置码：

```text
idf.py -B build -p PORT monitor
```

## 二次开发注意事项

### 单核

`sdkconfig.defaults` 打开了 `CONFIG_FREERTOS_UNICORE`。Wi-Fi、蓝牙控制器和 Arduino `loop` 都在 CPU0 上。不要为了“还有一个核”就关掉这个选项：Arduino loop、音乐任务和 NimBLE host 任务的亲和性、`displayLock`（屏幕）和 `musicLock`（快照）都是按单核来的。

同时固定的还有：

- `CONFIG_ESP32_REV_MIN_0`：二进制兼容 rev0 及以后。不要使用只有 rev3 才有的指令。
- `CONFIG_FREERTOS_HZ=1000`。
- Arduino loop 栈 8192 字节。音乐任务栈 8192 字节、优先级 1。NimBLE host 任务栈 6144 字节。

### Wi-Fi

固件不使用 Arduino 的 `WiFi.h`，由 `wifi_bridge.c` 直接调用 ESP-IDF 的 `esp_wifi`，只做 STA。`wifi_bridge_ap_start()` 是未接入主流程的热点辅助函数，正常启动不会开 AP。

- 凭据先用 NVS 里由蓝牙或串口写入的记录，否则用 menuconfig 的 `CONFIG_WIRETFT_SSID` / `CONFIG_WIRETFT_PASSWORD`。
- 断线后最多自动重连 5 次，用尽后要检查路由器并重启。
- HTTP 只绑在 STA 的私网 IPv4 80 端口。根 CMake 把 IDF 5.5 的 `httpd_main.c` 收成仅 IPv4，再用 `lwip_bind` 包装限制来源。不要改成监听所有地址，也不要做公网端口转发。
- `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6`，是为了给音乐模式留内部 RAM。调大就会挤占堆。
- `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE` 关闭：每次启动做完整射频校准，不把结果写入 `phy_init`。启动会稍慢，但不依赖分区里过期的校准数据。这个分区仍要留在分区表里，删掉会改变后面的偏移。

### eFuse

有的模组出厂 MAC 的 CRC 是坏的。`esp_efuse_mac_get_default()` 会得到全 0，或最低位（组播位）为 1 的地址，Wi-Fi 和蓝牙都会拒绝使用。

`wifi_bridge.c` 在协议栈启动前检查。地址无效时不烧写 eFuse，只在 RAM 里放一个本地管理地址：第一字节 `0x02`，第二字节 `0xE5`，其余字节取这次读到的 eFuse 原始值。同一块芯片每次得到同一个地址，设备 ID 保持稳定。蓝牙地址由这个基础 MAC 派生（通常加 2）。串口上的「eFuse MAC 无效，基础 MAC 使用兜底」是这条路径，不是故障。CRC 正常的模组会打印「eFuse MAC 正常」，并使用出厂地址。

不要用 `espefuse.py burn_mac` 或其它 espefuse 写命令去修 MAC。eFuse 位只能从 0 烧成 1，写错不能恢复，严重时模组无法再启动。

### 内存、分区和蓝牙

没有 PSRAM，堆都在内部 RAM。应用分区 2 MB，其中嵌入字库约 606 KB，当前固件大约占 85%。音乐模式另要约 100 KB 堆，字形缓存约 9 KB。音乐和蓝牙同时运行时，空闲堆大约 30 KB 出头，最大连续块约 27 KB。加功能之前先看这块余量。

照片分区放两个槽，每张 153600 字节加文件头。SPIFFS 只在整分区都是 `0xFF` 时格式化。已经写过的分区即使损坏也不会自动擦除。

蓝牙直接使用这份 IDF 自带的 NimBLE host，不使用 Arduino 的 BLE 库。那套库对应的 NimBLE 比 IDF 5.5 新，缺少 `ble_gap_read_local_irk`，编不过。`sdkconfig.defaults` 用选择性编译关掉了 BLE、SimpleBLE 和 BluetoothSerial。`ble_link.h` 里有强符号 `bleInUse()`，否则 Arduino 启动时会把蓝牙控制器内存释放掉。

角色只有外设，最多 1 个连接，无配对、无 GATT 客户端，首选 MTU 517。msys 和 ACL 缓冲已经减小，改回默认值会在音乐模式下把堆用完。

### 屏幕

Arduino_GFX 1.6.3 和 Arduino-ESP32 3.3.12 的 SPI 分频函数签名不一致。`components/panel/CMakeLists.txt` 只在构建目录生成兼容副本，不要改第三方库的源码。

当前没有 DMA：`writePixels()` 每批最多 32 个像素，填进 FIFO 后忙等。若要换成 ESP-IDF `spi_master`（`SPI_DMA_CH_AUTO`）或 `esp_lcd`，缓冲区必须 DMA 对齐，完成回调之后才能复用，并且不能让 Arduino 的寄存器驱动和 IDF 同时占这条总线。全屏 153600 字节在 40 MHz 下的理论下限是 30.72 ms。面板没有引出 TE，歌词区做不到与扫描同步。

## BLE 协议（protocol 2）

GATT 服务 `3f6a0001-8c2e-4d1b-9a77-5e9b1c7d0a10`：

| 特征 | UUID | 属性 |
|---|---|---|
| RX | `3f6a0002-8c2e-4d1b-9a77-5e9b1c7d0a10` | write / write without response |
| TX | `3f6a0003-8c2e-4d1b-9a77-5e9b1c7d0a10` | notify |

RX 帧首字节：`0x01` JSON 片段（未完）、`0x02` JSON 最后一片、`0x10` 图片数据（后跟 u32 小端偏移，再跟数据）。TX 帧首字节：`0x01` 应答片段（未完）、`0x02` 应答最后一片、`0x03` 事件。单条命令最长 768 字节。

每条命令为 `{"id":N,"cmd":"...","code":"123456",...}`，应答为 `{"id":N,"ok":true|false,"error":"..."}`，外加命令相关字段：

- `info`：设备 ID、蓝牙名、Wi-Fi 状态 / SSID / IP、方向、照片状态、音乐设置与状态、空闲堆。
- `wifi_scan`：异步扫描，最长 20 秒，返回去重后最多 16 个网络（`ssid`、`rssi`、`secure`）。
- `wifi_set` `{ssid, password}`、`wifi_forget`。
- `music` `{server, translation}`：`server` 为空串表示关闭。
- `display` `{orientation:"portrait"|"landscape"}`。
- `image_begin` `{orientation}`：返回 `size`（153600）、`chunk`（MTU−8，上限 500）、`window`（16384）。之后按偏移连续发送 `0x10` 帧。设备每收到 4096 字节发送 `{"evt":"image","got":N}`。客户端未确认的数据不超过 `window`。`image_end` `{crc}` 是整段载荷的标准 CRC32，通过后切换活动槽。`image_cancel` 放弃本次传输。断开连接也会放弃未完成的传输。

常见错误码：`bad_code`、`locked`（附 `retry_after` 秒数）、`bad_request`、`unknown_command`、`bad_wifi`、`scan_busy`、`bad_server`、`bad_orientation`、`image_busy`、`transfer_failed`、`crc_or_storage_error`、`save_failed`、`no_memory`。

## 局域网接口

连上 Wi-Fi 后，设备在私网地址的 80 端口提供 HTTP，并拒绝非私网来源。查询不需要配置码。写操作要带 `X-Config-Code: <配置码>`，错误返回 401，锁定返回 429。

- `GET /api/info`
- `POST /api/image`：RGB565 大端，153600 字节，头 `X-Image-CRC32`、`X-Image-Orientation`
- `POST /api/display`：`{"orientation":"portrait"|"landscape"}`
- `POST /api/music`：`{"server":"192.168.1.50:8765","translation":true}`

没有 TLS，只适合可信局域网。

## 屏幕方向

竖屏 240×320，或横屏 320×240（竖屏画面逆时针转 90°）。方向存在 NVS 键 `orient`。竖屏 MADCTL 为 `0x48`，横屏为 `0xE8`。照片槽用 magic 记下方向（`WFT1` 竖屏、`WFTL` 横屏）。换方向后，旧照片仍按原来的方向显示，需要重新发送。

## 音乐摆件

按 `lan_broadcast.md` 接入 go-musicfox：在 `config.toml` 里设置 `[main.broadcast] enable = true`，建议 `coverSize = 320`。控制页填写运行 go-musicfox 的电脑地址（私网 `IPv4:端口`，例如 `192.168.1.50:8765`）。设置存在 NVS 里，重启后自动连接。关闭后恢复照片。

- 竖屏：上方 160×160 封面，背景取封面平均色并调暗；中间一行歌名和歌手；下方歌词区延伸到底边，当前行（含翻译）居中高亮。
- 横屏：顶部整行歌名和歌手；左下 120×120 封面；右侧 182 像素宽的歌词区。
- 歌名超宽时循环滚动。当前歌词行超宽时单程滚动一次；有逐字时间时跟着演唱位置。
- SSE 和封面都是 HTTP/1.1，支持分块传输。断线 2 秒后重连，45 秒没有数据视为断开。封面只请求同一服务的 `song.coverUrl`，由 ROM 里的 TJpgDec 边收边解码，只支持基线 JPEG。
- 字库是 16 像素 GNU Unifont，由 `tools/make_lyric_font.py` 生成并嵌入（约 606 KiB，14381 字），覆盖拉丁、希腊、西里尔、日文假名、GB2312、JIS X 0208、Big5 第一级汉字和 2350 个韩文音节。
- 字库在 Flash 里线性查找一个字大约要 1.6 ms。固件把用过的字收成 16×16 的 1 位缓存（256 项），避免滚动时反复查表。
- 单条推送超过 16 KiB 会被丢掉，并报告 `event_too_large`。

实现在 `music_mode.h`（SSE、封面、渲染）和 `music_json.h`（不动态分配的 JSON 解析）。

主机端解析测试：

```text
g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -I. tests/music_parse_test.cpp -o /tmp/music_test && /tmp/music_test
```
