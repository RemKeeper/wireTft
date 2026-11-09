# 局域网投送（外部发射）接入文档

go-musicfox 可以把**当前播放歌曲的专辑封面**与一段**歌词滚动窗口**通过局域网推送给其它设备，
由外部设备自行实现渲染。服务端不提供任何前端界面，只提供推送协议与封面图片。

## 1. 启用

在 `config.toml` 中配置并重启应用：

```toml
[main.broadcast]
enable = false         # 是否启用（默认关闭）
port = 8765            # 监听端口
bind = ""              # 监听地址，留空监听所有网卡（0.0.0.0）；可填某个局域网 IP 限制访问
coverSize = 512        # 请求专辑封面时的分辨率（像素）
windowBefore = 4       # 歌词滚动窗口：当前行之前包含的行数
windowAfter = 8        # 歌词滚动窗口：当前行之后包含的行数
```

启动成功后，日志会输出 `broadcast: server started addr=[::]:8765`（监听所有网卡时；绑定具体 IP 则显示该 IP）。
在同一局域网设备上访问 `http://<本机局域网IP>:8765/events` 即可接收推送。

> 该功能默认关闭。它会将当前播放信息暴露到局域网，请仅在可信网络中使用。

## 2. 端点

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/events` | **推送协议**：Server-Sent Events 流，持续推送 `nowplaying` 事件 |
| GET | `/cover` | 专辑封面，统一为不超过 `coverSize` 的基线 JPEG（`image/jpeg`，服务端带缓存与缩放） |

## 3. 事件流协议（`/events`）

- 响应头：`Content-Type: text/event-stream`。
- 每个事件使用 `event: nowplaying` + 单行 `data: <JSON>`（JSON 内部换行已转义）。
- **连接建立后立即推送一次当前快照**，之后在下列情况推送：
  - 歌曲变化；
  - 播放状态变化（播放/暂停/停止）；
  - 歌词滚动窗口变化（当前行推进或回退）；
  - 播放中约每 2 秒的周期性刷新（用于校正漂移，`seq` 会递增）。
- 空闲时每 15 秒发送 `: keep-alive` 注释行（非数据事件，客户端忽略即可）。
- 客户端断开连接后服务端自动停止推送。

### 3.1 事件示例

```
event: nowplaying
data: {"type":"nowplaying","version":1,"seq":12,"playing":true,"state":"playing","positionMs":12345,"durationMs":215000,"updatedAtMs":1690000000000,"song":{"id":123,"name":"歌名","artists":"歌手A/歌手B","album":"专辑","durationMs":215000,"picUrl":"http://...","coverUrl":"/cover"},"lyric":[{"index":8,"startMs":11000,"endMs":14000,"text":"上一行","translation":"…"},{"index":9,"startMs":14000,"endMs":17500,"text":"当前行","translation":"…","words":[{"startMs":14000,"endMs":14700,"text":"当"},{"startMs":14700,"endMs":17500,"text":"前行"}]}]}
```

### 3.2 字段说明

顶层：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `type` | string | 固定为 `"nowplaying"` |
| `version` | int | 协议版本（不兼容变更时递增） |
| `seq` | int | 快照序号，单调递增；可用于判断是否收到新快照 |
| `playing` | bool | 是否正在播放 |
| `state` | string | `playing` / `paused` / `stopped` / `unknown` |
| `positionMs` | int | 采样瞬间的播放位置（毫秒） |
| `durationMs` | int | 歌曲总时长（毫秒） |
| `updatedAtMs` | int | 服务端采样时的 Unix 毫秒时间戳 |
| `song` | object | 歌曲信息（见下） |
| `lyric` | array | 歌词滚动窗口（见下） |

`song`：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | int | 歌曲 ID |
| `name` | string | 歌名 |
| `artists` | string | 歌手，多个以 `/` 分隔 |
| `album` | string | 专辑名 |
| `durationMs` | int | 时长（毫秒） |
| `picUrl` | string | 上游原始封面地址（备用，可能受网络/地域限制） |
| `coverUrl` | string | 封面相对路径（推荐），通常为 `/cover` |

`lyric[]`（**滚动窗口**，仅包含当前位置附近的行；无歌词时为空数组）：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `index` | int | 行在完整歌词中的绝对序号，从 0 开始 |
| `startMs` | int | 该行开始时间（毫秒，相对歌曲开头） |
| `endMs` | int | 该行结束时间（毫秒） |
| `text` | string | 该行文本（YRC 时为各词拼接） |
| `translation` | string | 翻译文本，可能为空 |
| `words` | array | 逐字时间（仅当该歌有 YRC 逐字歌词时存在） |

`words[]`：`{ "startMs": int, "endMs": int, "text": string }`。

> 窗口大小由 `windowBefore` / `windowAfter` 控制，窗口会随播放进度在完整歌词中滚动。

## 4. 时间同步建议

- 收到快照后记录本地时刻 `t0` 与 `positionMs`。当 `playing == true` 时，
  估算位置：`pos = positionMs + (now - t0)`；到达 `durationMs` 后停止增长。
- 播放中服务端会周期性刷新（约 2 秒），可据此纠正漂移（尤其拖动进度条后）。
- 使用歌词行的 `startMs` / `endMs` 定位当前行与高亮；有 `words` 时可按词时间做逐字高亮。

## 5. 专辑封面

- 推荐使用 `song.coverUrl`（相对路径）拼接为 `http://<host>:<port>/cover` 获取。
  仅在 `song.id` 变化时重新拉取一次即可（服务端带缓存）。
- `song.picUrl` 为上游原始地址，仅在无法访问本服务端 `/cover` 时作为备用。

## 6. 客户端示例

Go：

```go
resp, _ := http.Get("http://192.168.1.50:8765/events")
defer resp.Body.Close()
sc := bufio.NewScanner(resp.Body)
for sc.Scan() {
    line := sc.Text()
    if !strings.HasPrefix(line, "data: ") {
        continue // event: / : keep-alive 等
    }
    var np struct {
        Playing    bool  `json:"playing"`
        PositionMs int64 `json:"positionMs"`
        DurationMs int64 `json:"durationMs"`
        Song       struct {
            Name    string `json:"name"`
            Artists string `json:"artists"`
            CoverURL string `json:"coverUrl"`
        } `json:"song"`
        Lyric []struct {
            Index   int    `json:"index"`
            StartMs int64  `json:"startMs"`
            EndMs   int64  `json:"endMs"`
            Text    string `json:"text"`
        } `json:"lyric"`
    }
    if err := json.Unmarshal([]byte(strings.TrimPrefix(line, "data: ")), &np); err != nil {
        continue
    }
    // 交给本地渲染逻辑处理……
}
```

Node.js：

```js
const http = require('http');
http.get('http://192.168.1.50:8765/events', (res) => {
  let buf = '';
  res.setEncoding('utf8');
  res.on('data', (chunk) => {
    buf += chunk;
    let idx;
    while ((idx = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, idx).trim();
      buf = buf.slice(idx + 1);
      if (line.startsWith('data: ')) {
        const np = JSON.parse(line.slice(6));
        // 交给本地渲染逻辑处理……
      }
    }
  });
});
```

Python：

```python
import json, requests
with requests.get("http://192.168.1.50:8765/events", stream=True) as r:
    for raw in r.iter_lines():
        line = raw.decode("utf-8")
        if not line.startswith("data: "):
            continue
        np = json.loads(line[6:])
        # 交给本地渲染逻辑处理……
```

## 7. 版本与安全

- `version` 描述线协议版本；不兼容变更会递增该值。
- 默认关闭；启用后播放信息对局域网可见，建议通过 `bind` 限制监听地址或仅在可信网络使用。
