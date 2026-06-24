# DevUI 内建外部画面接口（DEVUI-IPC）

`u60pro-devui` 在原有 `/data/ui/*.html` 页面流之外，内建了一条本地 IPC 渲染通道。其他本机进程可以直接把像素帧、图片、绘图命令或纯文字提交给 DevUI，由 DevUI 临时接管屏幕显示；当外部画面结束后，设备会自动回到原本的 DevUI 页面流。

这个接口的定位是“让别的应用借用 DevUI 的屏幕与触摸通道”，而不是替换 `/data/ui` 的 HTML 机制。两者可以并存，互不改写。

## 1. 接口概览

- 协议名称：`DEVUI-IPC`
- 传输方式：本地 Unix Domain Socket
- 接口路径：`/tmp/u60-devui.sock`
- 事件日志：`/tmp/u60-devui-events.log`
- 当前实现：一条连接只处理一条命令
- 当前交互能力：点击（tap）事件回传 + 系统级左边缘右滑返回

## 2. 生命周期与优先级

- DevUI 默认仍显示 `/data/ui/*.html` 页面。
- 当外部进程成功发送 `FRAME`、`IMAGE`、`DRAW` 或 `TEXT` 后，外部画面立即成为当前前台内容区。
- 外部画面激活期间，最上方状态栏仍由 DevUI 原生绘制和刷新，不交给外部应用接管。
- `ttl_ms = 0` 表示该画面持续显示，直到收到 `CLOSE` 或被用户主动退出。
- `ttl_ms > 0` 表示画面在超时后自动关闭，回到原 DevUI。
- 外部画面激活时，DevUI 会清空并重新创建 `/tmp/u60-devui-events.log`，点击序号 `seq` 从 `1` 重新开始。
- 锁屏优先级高于外部画面。设备进入锁屏时，外部画面会被关闭。
- 电源键短按只控制亮灭屏，不会自动销毁仍然有效的外部画面。
- 电源键长按在外部画面前台时会作为“退出外部画面”的兜底手势。
- 用户也可以从内容区左边缘向右滑动，触发系统级返回并关闭外部画面。

## 3. 传输与返回格式

请求规则：

- 客户端连接 `/tmp/u60-devui.sock`
- 先发送一行命令头，以 `\n` 结束
- 需要 payload 的命令把 payload 紧跟在命令头之后
- 服务器处理完成后返回一行文本，再关闭本次连接

返回格式：

```text
OK <message>
ERR <message>
```

当前返回消息：

- `OK pong`
- `OK closed`
- `OK frame`
- `OK image`
- `OK draw`
- `OK text`

常见错误消息：

- `ERR unknown-command`
- `ERR bad-frame-header`
- `ERR short-frame`
- `ERR bad-image-header`
- `ERR bad-text-header`
- `ERR bad-text-body`
- `ERR bad-draw-header`
- `ERR bad-draw-body`

`TEXT` 和 `DRAW` 支持“不带长度”的写法，但这类写法依赖连接结束或短时间空闲来判断 payload 结束。正式接入时建议优先使用显式 `len`，这样边界更稳定，也更适合其他语言封装。

## 4. 坐标系与交互模型

- 当前 U60 Pro 机型上的整屏 DevUI 逻辑坐标为 `320 x 480`。
- 其中顶部 `26 px` 为 DevUI 原生状态栏保留区，外部接口使用其下方内容区。
- 当前 U60 Pro 的外部内容区逻辑尺寸为 `320 x 454`。
- `DRAW` 命令的坐标、`FRAME/IMAGE` 的落屏结果、以及点击事件里的 `x/y`，都以内容区左上角为原点，不再包含顶部状态栏。
- 外部画面激活期间，内容区点击不会再传给原生 HTML UI，而是写入事件日志。
- 状态栏区域的触摸不会转发给外部应用。
- 当前版本只回传点击事件，不回传滑动、长按、多点触控。
- 点击命中规则由外部应用自己负责：DevUI 只负责显示和记录点击坐标。
- 左边缘右滑返回属于系统手势，不会写入事件日志。

事件日志格式为 JSON Lines，每行一个事件，例如：

```json
{"event":"tap","seq":1,"x":123,"y":256,"t":456789}
```

字段说明：

- `event`：当前固定为 `tap`
- `seq`：从本次外部画面激活开始递增的事件序号
- `x` / `y`：点击坐标
- `t`：DevUI 侧的单调时钟毫秒值

## 5. 命令定义

### 5.1 PING

用途：探活，验证 socket 是否可用。

```text
PING
```

返回：

```text
OK pong
```

### 5.2 CLOSE

用途：主动关闭当前外部画面，立即回到原 DevUI。

```text
CLOSE
```

返回：

```text
OK closed
```

### 5.3 FRAME

用途：提交一帧原始像素数据。

格式：

```text
FRAME rgb565 <w> <h> <ttl_ms>\n<raw-bytes>
```

参数说明：

- `rgb565`：当前固定只能写这个格式
- `w` / `h`：源图宽高
- `ttl_ms`：显示持续时间，`0` 表示常驻直到关闭
- `raw-bytes`：小端 RGB565，按“从左到右、从上到下”的顺序排列

行为说明：

- 源尺寸不必等于内容区尺寸
- DevUI 会把源帧最近邻缩放到状态栏下方的内容区

实现限制：

- `w`、`h` 均必须大于 `0`
- `w`、`h` 当前上限为 `1024`
- 像素 payload 当前上限为 `4 MiB`

成功返回：

```text
OK frame
```

### 5.4 IMAGE

用途：让 DevUI 直接读取设备本地图片文件并显示。

格式：

```text
IMAGE <ttl_ms> <fit|stretch|cover> <path>
```

参数说明：

- `ttl_ms`：显示持续时间，`0` 表示常驻
- `fit`：等比完整显示，剩余区域黑底
- `stretch`：拉伸到整个内容区
- `cover`：等比铺满内容区，超出部分裁切
- `path`：设备本地文件路径，例如 `/tmp/demo.png`

支持格式：

- 由 `stb_image` 可解码的常见位图格式承担，常用可按 PNG、JPEG、BMP 理解

成功返回：

```text
OK image
```

### 5.5 DRAW

用途：提交一段轻量绘图脚本，让 DevUI 自己生成画面。

带长度的正式写法：

```text
DRAW <ttl_ms> <len>\n<script>
```

不带长度的兼容写法：

```text
DRAW <ttl_ms>
...
END
```

当前支持的绘图指令：

```text
CLEAR r g b
RECT x y w h r g b [a]
ROUNDRECT x y w h radius r g b [a]
RRECT x y w h radius r g b [a]
LINE x0 y0 x1 y1 r g b [thick]
TEXT x y size r g b text...
CENTER x y w h size r g b text...
```

说明：

- 颜色通道 `r g b` 范围为 `0..255`
- 透明度 `a` 省略时默认为 `255`
- `LINE` 的 `thick` 省略时默认为 `1`
- `TEXT` 以左上起点绘制
- `CENTER` 会在给定矩形内水平居中文字
- 以 `#` 开头的行会被忽略，可用于脚本注释
- 文本与图形都绘制在内容区内；状态栏由 DevUI 继续保留

当前脚本体大小上限约为 `65535` 字节。

成功返回：

```text
OK draw
```

### 5.6 TEXT

用途：提交一段纯文本，由 DevUI 用内建样式自动排版为文字页。

带长度的正式写法：

```text
TEXT <ttl_ms> <len>\n<utf8-text>
```

不带长度的兼容写法：

```text
TEXT <ttl_ms>
这是一段外部应用提交的文字。
```

说明：

- 文本按 UTF-8 处理
- 换行会保留
- DevUI 会使用内建深色文字页样式进行排版

当前文本体大小上限约为 `65535` 字节。

成功返回：

```text
OK text
```

## 6. 接入约束

- 这是本机可信接口，不做 TCP 监听，也不做鉴权。
- 接口默认适合设备本机进程、受控脚本或通过 SSH 落地到设备上的程序调用。
- 外部画面不会改写 `/data/ui` 文件，也不会改变原有页面顺序、主题、菜单状态或设置项。
- `IMAGE` 读取的是设备本地路径，不负责下载远程图片。
- 当前事件回传通道是“文件追加日志”，外部应用需要自己消费该日志并驱动下一次重绘。
- 如果外部应用需要覆盖整块内容区，应按内容区尺寸进行布局，而不是把状态栏区域也算进去。

## 7. 推荐交互模式

一个完整的交互闭环通常是：

1. 外部应用发送 `DRAW`、`IMAGE`、`FRAME` 或 `TEXT`，把画面送上屏幕。
2. 外部应用监听 `/tmp/u60-devui-events.log`。
3. 用户点击后，外部应用按 `x/y` 自己做命中判断。
4. 外部应用根据业务状态再次发送新的 `DRAW` / `FRAME` / `IMAGE`。
5. 结束时发送 `CLOSE`，把控制权交还给原 DevUI。

## 8. 最小示例

探活：

```python
import socket

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/tmp/u60-devui.sock")
s.sendall(b"PING\n")
print(s.recv(128).decode())
s.close()
```

提交一页可点击按钮：

```python
import socket

script = """\
CLEAR 9 13 20
ROUNDRECT 24 96 272 84 18 35 43 56 255
CENTER 24 96 272 84 24 233 238 247 Tap Me
"""

body = script.encode("utf-8")
head = f"DRAW 0 {len(body)}\n".encode("utf-8")

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/tmp/u60-devui.sock")
s.sendall(head + body)
print(s.recv(128).decode())
s.close()
```

监听点击：

```text
tail -F /tmp/u60-devui-events.log
```

## 9. 兼容性说明

本文件描述的是当前仓库内 `u60pro-devui` 已实现的内建接口。后续如果新增事件类型、绘图指令或返回值，应优先在这里更新，再视为对外正式能力的一部分。
