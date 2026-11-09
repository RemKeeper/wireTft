# 实现与验证

## 蓝牙配网与控制
- [x] NimBLE GATT 服务（分片 JSON 命令、窗口化图片传输、异步 Wi-Fi 扫描）
- [x] 6 位配置码：首次随机生成、NVS 持久化、启动打印、输错 5 次锁定 60 秒
- [x] 局域网写接口要求 `X-Config-Code`，删除视频投射与 Python 控制台
- [x] 内存裁剪（音乐模式 + 蓝牙连接时空闲堆约 43 KB）
- [x] PC 端 BLE 协议实测（配置码、扫描、方向、150 KiB 照片、坏 CRC）
- [x] Cloudflare Workers 静态资源托管 Web Bluetooth 页面（`web/`，自行 `wrangler deploy`）
- [ ] 在 Chrome / Edge 中用页面实际完成一次配网和换图

## 音乐摆件
- [x] SSE 客户端、快照解析、ROM TJpgDec 封面解码、歌词滚动与逐字高亮
- [x] 设备 `/api/music` + NVS 持久化
- [x] WSL 构建、仅烧录应用分区，连接真实 go-musicfox 与模拟推送实测
- [ ] 目视确认屏幕布局、颜色与滚动观感

## 基础
- [x] 固件识别、流式上传、双槽恢复、桥接修复
- [x] ESP-IDF 真实编译及配置检查
- [x] 检查 SPI/DMA 实现（当前 FIFO 轮询，无 DMA）
