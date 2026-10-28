# 自定义界面教程（写自己的 UI）

`u60pro-devui` 的设计是：**程序固定，界面是数据**。二进制本身不内置任何画面，它在运行时去 `/data/plugins/u60pro-devui/ui` 目录读取你写的 **HTML/CSS** 并渲染到屏幕。所以你想改界面，**完全不用重新编译**——改 HTML、推到设备、即时生效。

这份文档教你从零写一套自己的界面。

---

## 1. 它是怎么跑起来的

```text
后端 zwrt-datad ──▶ HTTP /state + SSE /events (127.0.0.1:9460) ──┐
                                                                  ├─▶ u60pro-devui ──▶ 屏幕
你写的 /data/plugins/u60pro-devui/ui/*.html + style.css ───────────┘
```

- 程序把 `/data/plugins/u60pro-devui/ui` 下每个 `*.html` 当作**一页**，左右滑动切换。
- 渲染前，程序会把 HTML 里的 `{{令牌}}` 替换成实时数据（电量、信号、WiFi 密码……）。
- 点击带 `href="act:xxx"` 的链接会触发**动作**（翻页、切主题、显示密码等），而不是跳转网页。
- HTML / CSS 仍然按文件热加载，所以 `adb push` 完不用重启，约 1 秒内自动生效；状态数据则来自本机 HTTP/SSE 缓存。

渲染引擎是 [litehtml](https://github.com/litehtml/litehtml)（一个 C++ 的 HTML/CSS 排版库）+ FreeType 字体。**没有浏览器、没有 JavaScript；状态只读本机 `127.0.0.1` 的 HTTP/SSE。**

---

## 2. 屏幕和目录

- 屏幕分辨率：**320 × 480**（竖屏）。所有页面都按这个尺寸设计。
- 目录结构：

```text
/data/plugins/u60pro-devui/ui/
├── 00-home.html      # 第 1 页（按文件名排序）：时钟、电量环、网络与速率；锁屏预览也显示这一页
├── 01-signal.html    # 第 2 页：RSRP/SINR 表盘 + 信号 / 载波
├── 02-functions.html # 第 2 页：更多功能入口
├── 05-charts.html    # 第 5 页：CPU / 内存 / 网速 图表
├── 06-system.html    # 第 6 页：系统信息 / 屏幕 / 开关（含锁屏开关）
├── subpages/         # 内置二级页面：wifi / sms / cell / lock / speedtest，以及下拉的控制中心 control
├── functions/        # 用户自定义功能页，*.html 会自动出现在更多功能页
├── menu.html         # 电源键长按弹出的菜单（不算翻页）
├── lockscreen.html   # 锁屏 PIN 键盘（不算翻页，开启锁屏后覆盖显示）
└── style.css         # 所有页面共享的样式
```

- 文件名 `NN-名字.html` 的数字前缀决定**顺序**。想加一页，丢一个 `04-xxx.html` 进去即可，圆点指示会自动变成 4 个。
- `menu.html` 是特殊页：电源键长按时覆盖显示，里面放关机/重启/取消。
- 锁屏：启动、熄屏后、自动息屏为“关”时 60 秒无触摸，屏幕都会锁上，显示第 1 页（不响应点按）和底部“长按解锁”胶囊（原生绘制）。没设密码时按住 600 ms 解锁；设了密码时滑动或按住 450 ms 打开 `lockscreen.html` 密码盘。锁定时 `{{DOTS}}` 为空。
- `lockscreen.html` 是特殊页：开启锁屏后，超时/电源键锁屏时全屏显示 PIN 键盘（解锁正确才回到界面）；系统页锁屏开关从关→开时也用它设置新密码。
- `subpages/*.html` 是内置二级页面，通过 `act:sub:<文件名>` 打开，`act:backfunc` 返回顶层页。
- `functions/*.html` 是用户自定义二级页面，通过更多功能页自动扫描，示例见 `examples/custom-functions/`；示例不放进默认 `ui/`，避免安装后默认出现测试页面。
- 特殊页按全屏覆盖层处理，不继承普通页面的竖向滚动位置。
- 所有页面用 `<link rel="stylesheet" href="style.css">` 共享同一份样式。
- 旧版若还残留 `/data/ui`，新版安装脚本会自动把它迁移到这里。

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
| `{{STATUSBAR}}` | 顶部状态栏：时间（有未读短信时右侧由程序原生绘制一个主题第二强调色的信封图标）· 实时网速 · 制式 · 信号格 · 电池 · 电量 |
| `{{DOTS}}` | 底部翻页圆点，自动高亮当前页 |
| `{{SMSLIST}}` | 短信页折叠列表（整段）：最多 32 条，每条号码 / 时间 + 一行预览，是可点的 `act:sms:ID` 卡片（点开弹完整内容的二级页面并标为已读）；详情仅在正文真实超出视口时允许滚动，且只能点底部关闭按钮退出。未读条目红点 + 蓝色号码高亮；无短信显示「暂无短信」 |
| `{{SIGNALCARDS}}` | 默认第一页的完整信号区：运营商/制式/QCI/AMBR、载波卡片、信令解析附加指标、TA/品牌等高级卡片 |
| `{{NEIGHBORCARDS}}` | 首页邻小区入口和展开列表；信令解析关闭时为空，展开后每行显示 `PCI / 频段 / RSRP / RSRQ` |
| `{{CARRIERS}}` | 兼容旧模板的载波区：表头（组网模式 · L LTE + M NR 载波 · 总X MHz）+ 每载波一张卡片（频段·频宽 / EARFCN / PCI / RSRP / SINR，按质量上色，未激活置灰） |
| `{{NETSEG}}` | 选网方式分段控件（自动 / 5G SA / 5G NSA / 4G，当前模式高亮，支持点/滑） |
| `{{TOAST}}` | 居中提示气泡（无提示时为空），如"锁频成功" |
| `{{PINDOTS}}` | 锁屏键盘的 4 个 PIN 圆点（已输入的高亮，整段） |
| `{{LOCKMSG}}` | 解锁错误时的居中「密码错误」红色气泡（无错误为空，整段） |
| `{{LOCKAUX}}` | 锁屏键盘左下角辅助键：设置 PIN 时为「取消」键，解锁时为空 |

> 信号格条已并入 `{{STATUSBAR}}`；锁频的频段芯片由程序在二级弹窗里生成，页面只需用 `{{CURSA}}/{{CURNSA}}/{{CURLTE}}` 显示当前锁定摘要。

### 数据令牌（标量值）

| 令牌 | 含义 | 例 |
|------|------|----|
| `{{TIME}}` `{{DATE}}` | 时间 / 日期和星期 | `14:30` / `10月28日 周二` |
| `{{THEME}}` | body 类名：`dark` + 主题类 | `dark th-ocean` |
| `{{SIGGAUGES}}` | 信号表盘卡片：SA 只有 NR 一排，纯 LTE 只有 LTE 一排，NSA / EN-DC 是 NR 和 LTE 两排 | 一段 HTML |
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
| `{{USBPWRCLASS}}/{{USBPWRSTATE}}` `{{USBNETCLASS}}/{{USBNETSTATE}}` `{{BATPCTCLASS}}/{{BATPCTSTATE}}` `{{WIFICLASS}}/{{WIFISTATE}}` `{{WIFI24CLASS}}/{{WIFI24STATE}}` `{{WIFI5CLASS}}/{{WIFI5STATE}}` `{{PSMCLASS}}/{{PSMSTATE}}` `{{DPSCLASS}}/{{DPSSTATE}}` `{{NFCCLASS}}/{{NFCSTATE}}` `{{THEMESTATE}}`（主题名，如 `海洋`） `{{SPUNITCLASS}}/{{SPUNITSTATE}}` `{{LOCKCLASS}}/{{LOCKSTATE}}` | 各开关的类(`on`/`off`) 与状态文字 | `on` / `已开启` |
| `{{LOCKTITLE}}` | 锁屏键盘标题（设置时「设置锁屏密码」/ 解锁时「请输入锁屏密码」） | |
| `{{PAGE}}` `{{NPAGES}}` | 当前页 / 总页数 | `3` / `5` |

> 安全提示：`{{KEY}}`(WiFi 密码)、`{{CELLID}}`、`{{IMEI}}` **默认打码**(显示 `*`)，点对应"显示"动作才明文。请保留此行为。

---

## 6. 动作 `href="act:xxx"`

把链接 `href` 写成 `act:动作名`，点它触发动作并重绘当前页：

| 动作 | 效果 |
|------|------|
| `act:theme` | 按顺序切换四套主题（海洋 → 极光 → 日落 → 紫罗兰），并弹出主题名 |
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
| `act:sub:<html>` | 打开 `ui/subpages/` 下的内置二级页面 |
| `act:func:<html>` | 打开 `ui/functions/` 下的自定义二级页面 |
| `act:backfunc` | 从二级页面返回顶层页 |
| `act:neighbors` | 首页展开 / 收起邻小区列表 |
| `act:stpage` | 打开测速二级页（只有测速后端存在时更多功能页才显示入口） |

例：一个开关按钮

```html
<a href="act:spunit" class="sw {{SPUNITCLASS}}"><span class="kn"></span></a>
```

`{{SPUNITCLASS}}` 是 `on`/`off`，配合 CSS 的 `.sw.on` / `.sw.off` 把圆钮画在右/左，就是一个拨动开关。

> 滑条/分段控件类（亮度 `id="bright-bar"`、选网 `id="netseg"`）由程序按元素 `id` 定位并处理点击/拖动，所以这些元素**要带对应的 `id`**。

---

## 7. 主题与原生绘制

默认界面照 U30 Pro 屏幕的风格：深蓝底带一圈柔光、半透明卡片、两种强调色。共有四套主题，都是深色，区别只在强调色：

| 主题 | 类名 | 强调色 a / b |
|------|------|------|
| 海洋 | `th-ocean` | `#38D0FF` / `#FF7AC8` |
| 极光 | `th-aurora` | `#4ADE80` / `#FFC857` |
| 日落 | `th-sunset` | `#FF9F43` / `#6C8CFF` |
| 紫罗兰 | `th-violet` | `#A78BFA` / `#2DD4BF` |

`<body class="{{THEME}}">` 会变成 `dark th-ocean` 这样的两个类。因为 `var()` 不可靠，强调色按主题类各写一条规则：

```css
.th-ocean  .seg-on { background: #38d0ff; color: #0b1220; }
.th-aurora .seg-on { background: #4ade80; color: #0b1220; }
```

背景支持 `linear-gradient` 和 `radial-gradient`（默认页面底色就是一个径向渐变）。状态栏由程序原生绘制在 `#070C15` 上，和渐变边缘同色。
旧版保存的“浅色模式”升级后会落到“极光”。

有些图形 CSS 画不出来，程序会在页面渲染完后，按元素 `id` 找到占位框，把图形原生画进去。占位框只需要给出位置和大小：

| `id` | 画什么 |
|------|------|
| `chart-cpu` `chart-mem` `chart-net` `chart-batt` | 折线图（图表页） |
| `gauge-nr-rsrp` `gauge-nr-sinr` `gauge-lte-rsrp` `gauge-lte-sinr` | 270° 圆弧表盘：NR / LTE 那一路的 RSRP 和 SINR，中间是数值，开口处是名称；那一路没有读数时显示 `--`。`{{SIGGAUGES}}` 会按制式生成这些占位框 |
| `gauge-rsrp` `gauge-sinr` | 同上，但只画服务小区（NR 在用时是 NR，否则 LTE），给自定义模板用 |
| `home-ring` | 电量环（绕首页时钟），充电或电量 > 30% 为绿色，≤ 30% 黄色，≤ 15% 红色 |
| `home-bars` `home-wifi` `home-bolt` | 首页的信号格、Wi-Fi 图标、充电闪电（只在充电时画） |
| `fic-wifi` `fic-sms` `fic-cell` `fic-lock` `fic-speed` `fic-custom-N` | 功能列表左侧圆圈里的线条图标（主题色），自定义页面是四宫格 |
| `cc-wifi` `cc-nfc` `cc-dps` `cc-theme` | 控制中心的圆形开关图标，开着时圆盘是主题色、图标深色 |

渐变写进帧缓冲时带 4×4 有序抖动：屏幕是 RGB565，直接量化会出现一条条色带。

### 上下拉面板

和 U30 Pro 一样：在顶层页面上，从状态栏（顶部 30 px）往下拉打开**控制中心** `subpages/control.html`，从屏幕底边（底部 30 px）往上拉打开**短信列表** `subpages/sms.html`。
面板就是普通二级页，点按、滚动、`act:backfunc` 都照常；控制中心从底边上拉收起，短信列表从状态栏下拉收起。打开和收起带一段竖向滑动动画。

想在宿主机上看效果，见 [DEVELOPMENT.md](DEVELOPMENT.md) 的“界面预览”。

---

## 8. 部署与调试

```sh
# 推送你的界面（程序会自动热重载，无需重启）
adb shell 'mkdir -p /data/plugins/u60pro-devui/ui'
adb push 你的页面.html /data/plugins/u60pro-devui/ui/
adb push style.css     /data/plugins/u60pro-devui/ui/
```

注意（Windows 用户）：用 Git-Bash/MSYS 跑 `adb push` 时，`/data/plugins/u60pro-devui/ui/` 这种路径可能被错误地翻译成本地路径导致卡住。**建议用 PowerShell 跑 adb**，或一个文件一个文件地推。

调试技巧：

- 改完看不到效果？确认推到的是 `/data/plugins/u60pro-devui/ui/`，并且程序在运行（`pidof u60pro-devui`）。
- 排版乱 / 下面看不到？先确认页面高度和底部留白是否合理；长页面可以竖向滚动，但内容特别多时仍建议精简或拆页。
- 出现豆腐块 □？是字体缺这个符号字形，换中文或 CSS 画。
- 想从零做：复制现成的 `01-signal.html` 改，是最快的起点。

---

## 9. 一个完整示例：加一页"关于"

新建 `/data/plugins/u60pro-devui/ui/04-about.html`：

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
adb push 04-about.html /data/plugins/u60pro-devui/ui/
```

滑到第 4 页就能看到，圆点也自动变 4 个。完成。

---

更深入的实现细节（渲染管线、令牌如何在 C 里生成、硬件接口）见 [DEVELOPMENT.md](DEVELOPMENT.md) 和 [HARDWARE.md](HARDWARE.md)。

## Optional speedtest page

The "更多功能" page may show a `网络测速` tile when `/data/plugins/better-speedtest/better-speedtest` is installed and executable. The tile opens `ui/subpages/speedtest.html`, which uses DevUI-native drawing for the gauge and line charts. Duration `0` is loop mode: the button turns into a loop start action and the page shows an orange traffic warning until the user taps stop. Locked preview mode does not expose speedtest controls; backend detection still runs for after unlock. See [`SPEEDTEST.md`](SPEEDTEST.md) for paths, actions, rendering notes, and install/uninstall conventions.
