# 自定义界面教程（写自己的 UI）

`u60pro-devui` 的设计是：**程序固定，界面是数据**。二进制本身不内置任何画面，它在运行时去 `/data/ui` 目录读取你写的 **HTML/CSS** 并渲染到屏幕。所以你想改界面，**完全不用重新编译**——改 HTML、推到设备、即时生效。

这份文档教你从零写一套自己的界面。

---

## 1. 它是怎么跑起来的

```text
后端 u60-datad ──▶ /tmp/u60-datad/state.json ──┐
                                                ├─▶ u60pro-devui ──▶ 屏幕
你写的 /data/ui/*.html + style.css ─────────────┘
```

- 程序把 `/data/ui` 下每个 `*.html` 当作**一页**，左右滑动切换。
- 渲染前，程序会把 HTML 里的 `{{令牌}}` 替换成实时数据（电量、信号、WiFi 密码……）。
- 点击带 `href="act:xxx"` 的链接会触发**动作**（翻页、切主题、显示密码等），而不是跳转网页。
- **每次刷新都重新读文件**，所以 `adb push` 完不用重启，约 1 秒内自动生效。

渲染引擎是 [litehtml](https://github.com/litehtml/litehtml)（一个 C++ 的 HTML/CSS 排版库）+ FreeType 字体。**没有浏览器、没有网络、没有 JavaScript。**

---

## 2. 屏幕和目录

- 屏幕分辨率：**320 × 480**（竖屏）。所有页面都按这个尺寸设计。
- 目录结构：

```text
/data/ui/
├── 01-signal.html    # 第 1 页（按文件名排序）
├── 02-wifi.html      # 第 2 页
├── 03-system.html    # 第 3 页
├── menu.html         # 电源键长按弹出的菜单（不算翻页）
└── style.css         # 所有页面共享的样式
```

- 文件名 `NN-名字.html` 的数字前缀决定**顺序**。想加一页，丢一个 `04-xxx.html` 进去即可，圆点指示会自动变成 4 个。
- `menu.html` 是特殊页：电源键长按时覆盖显示，里面放关机/重启/取消。
- 所有页面用 `<link rel="stylesheet" href="style.css">` 共享同一份样式。

---

## 3. 一个最小页面

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head><meta charset="UTF-8"><link rel="stylesheet" href="style.css"></head>
<body class="{{THEME}}">
  {{STATUSBAR}}                      <!-- 顶部状态栏（程序拼好的整段） -->

  <div class="card">
    <div class="title">你好</div>
    <div class="big">电量 {{BAT}}%</div>
  </div>

  {{DOTS}}                           <!-- 底部翻页圆点（程序拼好的整段） -->
</body>
</html>
```

- `<body class="{{THEME}}">`：`{{THEME}}` 会变成 `dark` 或 `light`，配合 CSS 里的 `body.dark` / `body.light` 实现主题。
- `{{STATUSBAR}}` 和 `{{DOTS}}` 是程序**已经拼成整段 HTML** 的复合令牌，直接放进去就行，建议每页都放，风格统一。

---

## 4. 能用什么、不能用什么（重要）

litehtml 不是浏览器，**限制比较多**，踩坑前先看这里：

| 能用 | 不能用 / 不可靠 |
|------|----------------|
| `block` / `inline-block` / `float` 布局 | ❌ **CSS Grid**（`display:grid`） |
| `table` / `tr` / `td` 表格布局 | ❌ **JavaScript**（完全没有） |
| `flex`（基本可用，复杂对齐不保证） | ❌ **CSS 动画 / transition**（不会动） |
| 颜色、圆角、边框、padding、margin | ❌ **图片**（`<img>`、`background-image` 都画不出来） |
| `position: absolute / relative` | ❌ **`var()` 自定义属性**（不可靠，用 class 切主题代替） |
| 文字、字号、粗细、对齐 | ❌ **滚动**（页面固定 480px，超出部分被裁掉，看不到） |

几条最容易踩的：

- **没有图片**：图标请用 **CSS 画**（圆角块、边框拼形状）或**字体里的符号**。例如电池、信号格、开关都是纯 CSS 画的。
- **不能滚动**：一页内容必须塞进 480px 高度，否则下面的东西看不见。内容多就拆成多页。
- **想动起来只能靠数据刷新**：程序约 1 秒刷新一页（充电时更快）。没有 CSS 动画，"动画"只能通过令牌值每次变化来体现（比如充电时电池的流光就是程序每帧改一个 `left:%`）。
- **字体是设备自带的 CJK 字体**：中文、数字、常见标点都没问题；但很多符号字形缺失，会显示成**豆腐块**（□）。已知可用：`↑`(U+2191) `↓`(U+2193) `▲▼` `·` `℃` `•`；已知不可用：`◔` 等。拿不准的符号先在设备上试，或干脆用中文/CSS 画。

---

## 5. 模板令牌 `{{令牌}}`

程序在渲染前把这些替换成实时值。**替换是单遍的**——复合令牌（已经是整段 HTML 的那些）里不会再二次替换，所以你不能在自己的 HTML 里"拼一个令牌名再让它展开"。

### 复合令牌（整段 HTML，直接放）

| 令牌 | 内容 |
|------|------|
| `{{STATUSBAR}}` | 顶部状态栏：时间 · 实时网速 · 制式 · 信号格 · 电池 · 电量 |
| `{{DOTS}}` | 底部翻页圆点，自动高亮当前页 |
| `{{NR_ROWS}}` | NR 每个载波一张卡片（频段·频宽 / PCI / RSRP / SINR） |
| `{{LTE_ROWS}}` | LTE 每个载波一张卡片 |

### 数据令牌（标量值）

| 令牌 | 含义 | 例 |
|------|------|----|
| `{{TIME}}` | 时间 | `14:30` |
| `{{THEME}}` | 主题类名 | `dark` / `light` |
| `{{OPER}}` | 运营商（大陆四家显示中文） | `中国移动` |
| `{{NETTYPE}}` | 制式 | `SA` |
| `{{GEN}}` / `{{GENCLASS}}` | 网络代际文字 / 对应样式类 | `5G+` / `g5p` |
| `{{BAT}}` / `{{BATCLASS}}` | 电量百分比 / 低电量类 | `100` / `low` |
| `{{QCI}}` / `{{AMBR}}` | QoS QCI / 速率上限 | `6` / `3000/200 Mbps` |
| `{{CA_CC}}` / `{{CA_BW}}` | NR 载波数 / 聚合频宽(MHz) | `2` / `130` |
| `{{LTE_SHOW}}` | 没有 LTE 时为 `display:none` | |
| `{{RXSPEED}}` `{{TXSPEED}}` | 下载/上传速率 | `1.2 MB/s` |
| `{{RXBYTES}}` `{{TXBYTES}}` | 本次会话流量 | `120.5 MB` |
| `{{PCI}}` `{{ARFCN}}` `{{ARFCNBTN}}` | 小区 PCI / ARFCN(默认打码) / 显示按钮文字 | |
| `{{SSID}}` `{{KEY}}` `{{KEYBTN}}` `{{ENC}}` | WiFi 名 / 密码(默认打码) / 显示按钮 / 加密 | |
| `{{CLIENTS}}` | 已连接设备数 | `1` |
| `{{MODEL}}` `{{FW}}` `{{UPTIME}}` | 型号 / 固件 / 运行时间 | |
| `{{CPU}}` `{{MEM}}` | CPU 温度 / 内存占用% | `46` / `52` |
| `{{ADBCLASS}}` `{{ADBSTATE}}` | ADB 开关类/状态文字 | `on` / `已开启` |
| `{{THEMECLASS}}` `{{THEMESTATE}}` | 主题开关类/状态 | `on` / `浅色模式` |
| `{{SPUNITCLASS}}` `{{SPUNITSTATE}}` | 速率单位开关类/状态 | `on` / `比特率 Mbps` |
| `{{PAGE}}` `{{NPAGES}}` | 当前页 / 总页数 | `1` / `3` |

> 安全提示：`{{KEY}}`（WiFi 密码）和 `{{ARFCN}}` **默认是打码的**（显示 `*`），只有点了对应的"显示"动作才明文。请保留这个行为，别把密码默认明文摆屏幕上。

---

## 6. 动作 `href="act:xxx"`

把一个链接的 `href` 写成 `act:动作名`，点它就会触发动作，程序处理完重绘当前页：

| 动作 | 效果 |
|------|------|
| `act:theme` | 深色 / 浅色主题切换 |
| `act:revealkey` | 明文 / 打码显示 WiFi 密码 |
| `act:revealarfcn` | 显示 / 隐藏小区 ARFCN |
| `act:spunit` | 网速单位 Mbps(比特率) / MB/s(字节率) 切换 |
| `act:adb` | 切换 ADB 调试（发 ubus 改 USB 模式；`debug`=开，`user`=关） |
| `act:poweroff` / `act:reboot` / `act:close` / `act:menu` | 关机 / 重启 / 关菜单 / 开菜单（一般只在 `menu.html` 用） |

例：一个开关按钮

```html
<a href="act:theme" class="sw {{THEMECLASS}}"><span class="kn"></span></a>
```

`{{THEMECLASS}}` 是 `on`/`off`，配合 CSS 的 `.sw.on` / `.sw.off` 把圆钮画在右/左，就是一个拨动开关。

---

## 7. 主题（深色 / 浅色）

因为 `var()` 不可靠，主题用 **body 类名 + 两套规则**实现：

```css
body.dark  { background: #15161a; color: #e9ebee; }
body.light { background: #eceef1; color: #1b1d22; }

body.dark  .card { background: #23262c; }
body.light .card { background: #ffffff; }
```

`<body class="{{THEME}}">` 会自动是 `dark` 或 `light`，你只要为两个主题各写一套颜色即可。

---

## 8. 部署与调试

```sh
# 推送你的界面（程序会自动热重载，无需重启）
adb push 你的页面.html /data/ui/
adb push style.css     /data/ui/
```

注意（Windows 用户）：用 Git-Bash/MSYS 跑 `adb push` 时，`/data/ui/` 这种路径可能被错误地翻译成本地路径导致卡住。**建议用 PowerShell 跑 adb**，或一个文件一个文件地推。

调试技巧：

- 改完看不到效果？确认推到的是 `/data/ui/`，并且程序在运行（`pidof u60pro-devui`）。
- 排版乱 / 下面看不到？多半是内容超过了 480px 高度（不能滚动），精简或拆页。
- 出现豆腐块 □？是字体缺这个符号字形，换中文或 CSS 画。
- 想从零做：复制现成的 `01-signal.html` 改，是最快的起点。

---

## 9. 一个完整示例：加一页"关于"

新建 `/data/ui/04-about.html`：

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head><meta charset="UTF-8"><link rel="stylesheet" href="style.css"></head>
<body class="{{THEME}}">
  {{STATUSBAR}}
  <div class="card">
    <div class="title">关于本机</div>
    <table>
      <tr><td class="kv-l">型号</td><td class="val">{{MODEL}}</td></tr>
      <tr><td class="kv-l">固件</td><td class="val">{{FW}}</td></tr>
      <tr><td class="kv-l">运行</td><td class="val">{{UPTIME}}</td></tr>
    </table>
  </div>
  <div class="card">
    <div class="big">{{OPER}} · {{GEN}}</div>
    <div class="sec">第 {{PAGE}} / {{NPAGES}} 页</div>
  </div>
  {{DOTS}}
</body>
</html>
```

```sh
adb push 04-about.html /data/ui/
```

滑到第 4 页就能看到，圆点也自动变 4 个。完成。

---

更深入的实现细节（渲染管线、令牌如何在 C 里生成、硬件接口）见 [DEVELOPMENT.md](DEVELOPMENT.md) 和 [HARDWARE.md](HARDWARE.md)。
