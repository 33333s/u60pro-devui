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
├── 01-signal.html    # 第 1 页（按文件名排序）：信号 / 载波
├── 02-sms.html       # 第 2 页：短信列表（只读）
├── 03-wifi.html      # 第 3 页：WiFi / 设备 / DHCP
├── 04-lock.html      # 第 4 页：选网 / 锁频
├── 05-charts.html    # 第 5 页：CPU / 内存 / 网速 图表
├── 06-system.html    # 第 6 页：系统信息 / 屏幕 / 开关（含锁屏开关）
├── menu.html         # 电源键长按弹出的菜单（不算翻页）
├── lockscreen.html   # 锁屏 PIN 键盘（不算翻页，开启锁屏后覆盖显示）
└── style.css         # 所有页面共享的样式
```

- 文件名 `NN-名字.html` 的数字前缀决定**顺序**。想加一页，丢一个 `04-xxx.html` 进去即可，圆点指示会自动变成 4 个。
- `menu.html` 是特殊页：电源键长按时覆盖显示，里面放关机/重启/取消。
- `lockscreen.html` 是特殊页：开启锁屏后，超时/电源键锁屏时全屏显示 PIN 键盘（解锁正确才回到界面）；系统页锁屏开关从关→开时也用它设置新密码。
- 特殊页按全屏覆盖层处理，不继承普通页面的竖向滚动位置。
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
| 文字、字号、粗细、对齐 | ❌ 元素级滚动容器（页面整体可竖向滚动，但内部 `overflow:scroll` 不可靠） |

几条最容易踩的：

- **没有图片**：图标请用 **CSS 画**（圆角块、边框拼形状）或**字体里的符号**。例如电池、信号格、开关都是纯 CSS 画的。
- **页面可以整体竖向滚动**：内容超过 480px 时可上下拖动；状态栏会固定在顶部。不要依赖元素内部滚动条，内容特别多时仍建议拆页。
- **想动起来只能靠数据刷新**：程序约 1 秒刷新一页（充电时更快）。没有 CSS 动画，"动画"只能通过令牌值每次变化来体现（比如充电时电池的流光就是程序每帧改一个 `left:%`）。
- **字体是设备自带的 CJK 字体**：中文、数字、常见标点都没问题；但很多符号字形缺失，会显示成**豆腐块**（□）。已知可用：`↑`(U+2191) `↓`(U+2193) `▲▼` `·` `℃` `•`；已知不可用：`◔` 等。拿不准的符号先在设备上试，或干脆用中文/CSS 画。

---

## 5. 模板令牌 `{{令牌}}`

程序在渲染前把这些替换成实时值。**替换是单遍的**——复合令牌（已经是整段 HTML 的那些）里不会再二次替换，所以你不能在自己的 HTML 里"拼一个令牌名再让它展开"。

### 复合令牌（整段 HTML，直接放）

| 令牌 | 内容 |
|------|------|
| `{{STATUSBAR}}` | 顶部状态栏：时间（有未读短信时右侧由程序原生绘制一个蓝色信封图标）· 实时网速 · 制式 · 信号格 · 电池 · 电量 |
| `{{DOTS}}` | 底部翻页圆点，自动高亮当前页 |
| `{{SMSLIST}}` | 短信页折叠列表（整段）：最多 32 条，每条号码 / 时间 + 一行预览，是可点的 `act:sms:ID` 卡片（点开弹完整内容的二级页面并标为已读），未读条目红点 + 蓝色号码高亮；无短信显示「暂无短信」 |
| `{{CARRIERS}}` | 信号页载波区：表头（组网模式 · L LTE + M NR 载波 · 总X MHz）+ 每载波一张卡片（频段·频宽 / EARFCN / PCI / RSRP / SINR，按质量上色，未激活置灰） |
| `{{NETSEG}}` | 选网方式分段控件（自动 / 5G SA / 5G NSA / 4G，当前模式高亮，支持点/滑） |
| `{{TOAST}}` | 居中提示气泡（无提示时为空），如"锁频成功" |
| `{{PINDOTS}}` | 锁屏键盘的 4 个 PIN 圆点（已输入的高亮，整段） |
| `{{LOCKMSG}}` | 解锁错误时的居中「密码错误」红色气泡（无错误为空，整段） |
| `{{LOCKAUX}}` | 锁屏键盘左下角辅助键：设置 PIN 时为「取消」键，解锁时为空 |

> 信号格条已并入 `{{STATUSBAR}}`；锁频的频段芯片由程序在二级弹窗里生成，页面只需用 `{{CURSA}}/{{CURNSA}}/{{CURLTE}}` 显示当前锁定摘要。

### 数据令牌（标量值）

| 令牌 | 含义 | 例 |
|------|------|----|
| `{{TIME}}` `{{THEME}}` | 时间 / 主题类名 | `14:30` / `dark` |
| `{{OPER}}` `{{NETTYPE}}` | 运营商（大陆四家中文）/ 制式 | `中国移动` / `SA` |
| `{{GEN}}` `{{GENCLASS}}` | 网络代际文字 / 样式类 | `5G+` / `g5p` |
| `{{BAT}}` `{{BATCLASS}}` | 电量% / 低电量类 | `100` / `low` |
| `{{QCI}}` `{{AMBR}}` | QoS QCI / 速率上限 | `6` / `3000/200 Mbps` |
| `{{RXSPEED}}` `{{TXSPEED}}` | 下载/上传速率（单位随设置） | `1.2 MB/s` |
| `{{RXBYTES}}` `{{TXBYTES}}` | 本次会话流量 | `120.5 MB` |
| `{{PCI}}` | 小区 PCI | `273` |
| `{{CELLID}}` `{{CELLBTN}}` | NR Cell ID(默认打码) / 显示按钮文字 | `8641413121` / `显示` |
| `{{SSID}}` `{{KEY}}` `{{KEYBTN}}` `{{ENC}}` | WiFi 名 / 密码(默认打码) / 显示按钮 / 加密 | |
| `{{CLIENTS}}` `{{CLIENTLIST}}` | 设备数 / 已连接设备列表(整段) | `2` |
| `{{DHCP_IP}}` `{{DHCP_POOL}}` `{{DHCP_LEASE}}` | 网关 / 地址池 / 租期 | `192.168.0.1` |
| `{{MODEL}}` `{{FW}}` `{{SWVER}}` `{{IMEI}}` `{{IMEIBTN}}` | 型号 / 固件 / 版本号 / IMEI(打码) / 显示按钮 | |
| `{{UPTIME}}` `{{UPSHORT}}` | 运行时间(完整 / 紧凑) | `2d 03:04:05` / `2天3时` |
| `{{CPUUSAGE}}` `{{CPUTEMP}}` `{{BATTEMP}}` | CPU占用% / CPU温度 / 电池温度 | `17` / `46` / `33` |
| `{{CHGV}}` `{{CHGI}}` | 充电器电压(V) / 电流(mA) | `4.79` / `456` |
| `{{BATV}}` `{{BATI}}` | 电池电压(V) / 电流(mA) | `4.50` / `59` |
| `{{PWR}}` `{{PWRLBL}}` | 功率(W) / 标签 | `2.2` / `充电`或`放电` |
| `{{MEM}}` `{{MEMDETAIL}}` | 内存占用% / 已用·总(MB) | `52` / `837/1590 MB` |
| `{{BRIGHT}}` | 屏幕亮度% | `80` |
| `{{AUTOOFF}}` | 自动息屏预设按钮组(整段，当前项高亮) | |
| `{{CURSA}}` `{{CURNSA}}` `{{CURLTE}}` | 各制式当前锁定频段摘要 | `已锁 n41 n78` |
| `{{USBPWRCLASS}}/{{USBPWRSTATE}}` `{{USBNETCLASS}}/{{USBNETSTATE}}` `{{BATPCTCLASS}}/{{BATPCTSTATE}}` `{{WIFICLASS}}/{{WIFISTATE}}` `{{WIFI24CLASS}}/{{WIFI24STATE}}` `{{WIFI5CLASS}}/{{WIFI5STATE}}` `{{PSMCLASS}}/{{PSMSTATE}}` `{{DPSCLASS}}/{{DPSSTATE}}` `{{NFCCLASS}}/{{NFCSTATE}}` `{{THEMECLASS}}/{{THEMESTATE}}` `{{SPUNITCLASS}}/{{SPUNITSTATE}}` `{{LOCKCLASS}}/{{LOCKSTATE}}` | 各开关的类(`on`/`off`) 与状态文字 | `on` / `已开启` |
| `{{LOCKTITLE}}` | 锁屏键盘标题（设置时「设置锁屏密码」/ 解锁时「请输入锁屏密码」） | |
| `{{PAGE}}` `{{NPAGES}}` | 当前页 / 总页数 | `3` / `5` |

> 安全提示：`{{KEY}}`(WiFi 密码)、`{{CELLID}}`、`{{IMEI}}` **默认打码**(显示 `*`)，点对应"显示"动作才明文。请保留此行为。

---

## 6. 动作 `href="act:xxx"`

把链接 `href` 写成 `act:动作名`，点它触发动作并重绘当前页：

| 动作 | 效果 |
|------|------|
| `act:theme` | 深色 / 浅色切换 |
| `act:revealkey` `act:revealcell` `act:revealimei` | 明文/打码 显示 WiFi 密码 / Cell ID / IMEI |
| `act:spunit` | 网速单位 Mbps(比特率) / MB/s(字节率) |
| `act:usbpower` | 切换 USB-C 供电方向：给 U60 充电 / U60 反向供电 |
| `act:usbnet` | 切换 USB 网络共享：开启时先走官方 `9057` RNDIS:ECM（无 ADB），若无 carrier 自动降到纯 ECM `90B1`；关闭时回到无网卡的 `1225 mass_storage` |
| `act:batpct` | 切换状态栏电池框内百分比显示 |
| `act:wifi` `act:nfc` | 切换 WiFi 总开关（两个主频段）/ NFC 碰一碰 |
| `act:wifi24` `act:wifi5` | 切换 2.4G / 5G 主频段（`ifconfig wlan0/wlan2 up/down`） |
| `act:psm` | 切换 WiFi 节能模式（`iw set power_save`，开=省电 / 关=高性能） |
| `act:dps` | 切换电源直供电（`zwrt_bsp.charger`，插电直供、不充放电池） |
| `act:bright` | 在亮度滑条上点/拖设置亮度（按命中位置算） |
| `act:autooff:<毫秒>` | 选自动息屏预设（`act:autooff:30000` 等，`0`=关） |
| `act:net:<值>` | 切选网模式（`WL_AND_5G`/`Only_5G`/`LTE_AND_5G`/`Only_LTE`） |
| `act:openmodal:sa` `:nsa` `:lte` | 打开对应制式的锁频二级弹窗 |
| `act:bsa:<频段>` `act:bnsa:<n>` `act:blte:<n>` | 在弹窗里勾选/取消某频段（仅本地缓存） |
| `act:mall` `act:minv` `act:mapply` | 弹窗：全选/全不选 · 反选 · 应用（应用才提交锁频） |
| `act:resetband` | 解锁所有频段并恢复默认 |
| `act:locktoggle` | 锁屏开关：关→开进入 PIN 设置键盘；开→关清除密码、关闭锁屏 |
| `act:pin:<0-9>` | 锁屏键盘按下一位数字（满 4 位自动校验/保存） |
| `act:pin:del` | 锁屏键盘删除最后一位 |
| `act:lockcancel` | 放弃 PIN 设置（仅设置键盘左下角） |
| `act:poweroff` `act:reboot` `act:close` `act:menu` | 关机 / 重启 / 关菜单 / 开菜单（一般只在 `menu.html`） |

例：一个开关按钮

```html
<a href="act:theme" class="sw {{THEMECLASS}}"><span class="kn"></span></a>
```

`{{THEMECLASS}}` 是 `on`/`off`，配合 CSS 的 `.sw.on` / `.sw.off` 把圆钮画在右/左，就是一个拨动开关。

> 滑条/分段控件类（亮度 `id="bright-bar"`、选网 `id="netseg"`）由程序按元素 `id` 定位并处理点击/拖动，所以这些元素**要带对应的 `id`**。

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
- 排版乱 / 下面看不到？先确认页面高度和底部留白是否合理；长页面可以竖向滚动，但内容特别多时仍建议精简或拆页。
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
