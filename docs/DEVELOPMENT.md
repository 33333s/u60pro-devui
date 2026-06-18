# U60Pro 屏幕 UI 开发说明

这份文档记录项目当前最重要的开发事实，方便后续维护、移植，也便于随仓库版本化后公开分享。目标是让关键经验不只停留在本地记忆里，而是和代码一起长期保存。

## 项目目标

`u60pro-devui` 是 ZTE U60Pro 前面板屏幕 UI 的一个 clean-room 开源替代实现。目标形态是一份单独的静态 `aarch64` 二进制，只依赖标准 Linux/OpenWRT 接口，不链接 ZTE 私有库，也不提交任何专有资源。

配套后端是 `u60-datad`：单进程数据采集器，轮询 ubus 并把结果归一化成一份 JSON 快照。UI 只读取 `/tmp/u60-datad/state.json`，从不直接调用 ubus。

核心设计原则有两条：

1. **UI 与后端解耦**：所有 ubus 访问集中在后端，UI 只读一个文件，便于审计、分享、独立重建。
2. **程序与界面解耦**：二进制本身是固定的“框架”，**实际界面是 `/data/ui` 目录里的 HTML/CSS**。用户改界面只需要改 HTML，不必重新编译。开源发布时二进制保持不变，界面完全可由用户自定义。

## 架构总览

```text
ubus 服务 ──▶ u60-datad ──▶ /tmp/u60-datad/state.json ──▶ u60pro-devui ──▶ DRM framebuffer
                                                              │
                                                       渲染 /data/ui/*.html
```

- 渲染引擎：**litehtml**（C++ HTML/CSS 排版）+ **FreeType**（含 CJK 字形）→ 直接画进 RGB565 dumb buffer。
- 没有浏览器、没有 JavaScript、没有网络。一切都是本地静态渲染。
- 全部静态链接（liblitehtml.a + libfreetype.a + musl），单文件可直接拷到设备运行。

### `/data/ui` 界面模型

- 每个 `NN-name.html` 是一页，按文件名排序，可左右滑动切换。`menu.html` 是电源长按弹出的覆盖层，不计入翻页。
- 所有页面用 `<link rel="stylesheet" href="style.css">` 共享一份样式（容器的 `import_css` 从 `/data/ui/` 读取）。
- HTML 里的 `{{TOKEN}}` 在渲染前由 C 宿主替换成实时数据（见“模板令牌”）。
- `href="act:xxx"` 的链接是**动作**：点击后 UI 不跳转，而是执行对应动作（翻页指示、揭示密码、切主题、切 ADB 等）。
- 每次渲染都**重新读取**页面文件 = 改完 HTML 直接 `adb push` 到 `/data/ui` 即可热生效，不必重启进程。

当前六页（左右滑动，底部圆点指示；`menu.html` 为电源长按覆盖层）。**页面按文件名编号排序**——v0.3.5 在信号页后插入短信页，其余顺延，所以编号从 v0.3.4 的 `02-wifi/03-lock/04-charts/05-system` 改成了下面这套（**改名是后面那个「插件更新残留」坑的根因**，见版本机制一节）：

- **01-signal.html — 信号**：状态栏；运营商 + 制式，右上角 QCI/AMBR；按载波分卡片（`频段·频宽 / EARFCN / PCI / RSRP / SINR`，按质量上色、未激活置灰）。表头随组网模式（`5G SA / 5G NSA / EN-DC / 4G`）变化并显示 `L LTE + M NR 载波 · 总X MHz`。
- **02-sms.html — 短信（只读）**：折叠卡片列表，每条号码/时间 + 一行预览；点卡片弹二级页看全文并标记已读。详见下方「短信（只读）」。
- **03-wifi.html — WiFi**：SSID、密码（默认打码）、加密；WiFi 总开关 / 2.4G / 5G / 节能(PSM) / NFC 开关；已连接设备列表（在线过滤）；DHCP 信息（地址池实时由 uci 算）。
- **04-lock.html — 锁频**：选网方式分段控件 + 5G SA / 5G NSA / 4G 三张锁频卡（点开二级弹窗，频段芯片一行四个）+ 恢复默认。
- **05-charts.html — 图表**：CPU 占用+温度 / 内存 / 网速 / 电池(功率+温度) 四张原生折线图。
- **06-system.html — 系统/设置**：CPU/内存占用、温度、充电器/电池电压电流、版本号、IMEI(打码)；屏幕亮度滑条、自动息屏分段控件、锁屏 / 电源直供电(DPS) / USB 模式 / ADB / 速率单位 / 主题 开关。可竖向滚动。
- **lockscreen.html — 锁屏键盘**：特殊页（同 `menu.html`，不计入翻页）。开启锁屏后由宿主在 `g_lock_state` 非 0 时全屏显示，4 位 PIN 数字键盘 + 删除键。状态栏（含未读短信信封图标）一并显示。详见下方「屏幕锁（PIN）」。
- **menu.html — 电源菜单**：电源键长按弹出。三个竖排大圆形按键（关机/重启/取消），圆盘和图标原生绘制，下方文字标签，二次确认。详见下方「电源菜单（原生圆形按键）」。

### 模板令牌 `{{TOKEN}}` 与动作 `act:`

令牌在 `src/htmlmain.c` 的 `build_kv()` 里一次性从 `data_refresh()` 快照填好，`apply_template()` 单遍替换（不递归，所以含子令牌的整段 HTML 必须在 C 里拼好）。`href="act:xxx"` 是动作（不跳转，执行对应操作并重绘）。

> **完整、权威的令牌表与动作表见 [UI-GUIDE.md](UI-GUIDE.md)**（面向"写自己界面"的用户，随代码同步更新）。这里只记内部要点：
> - 复合令牌（C 拼好的整段 HTML）：`STATUSBAR`、`DOTS`、`CARRIERS`（信号页载波区，含表头+卡片）、`NETSEG`（选网分段控件）、`AUTOOFF`（自动息屏分段控件）、`CLIENTLIST`、`TOAST`。
> - 滑条/分段控件（亮度 `#bright-bar`、选网 `#netseg`、自动息屏 `#autoseg`）由宿主按元素 `id` 用 `get_placement` 定位、处理点/拖；这些元素**必须带对应 `id`**。
> - 安全：`KEY`/`CELLID`/`IMEI` 默认打码，需对应 `reveal*` 动作才明文。

### 网络制式徽章逻辑

### 网络制式徽章逻辑

状态栏电池左侧显示 `5GA / 5G+ / 5G / 4G / LTE / 3G`，由 MCC/MNC 判断运营商 + NR 载波聚合情况：

- 纯 NR SA 且（移动/广电 ≥3CC）或（电信/联通 聚合频宽 ≥200MHz）→ **5GA**
- 纯 NR SA 且聚合频宽 >100MHz → **5G+**
- 其余 NR SA / NSA / NR → **5G**
- 中国大陆 LTE → **4G**；境外 LTE → **LTE**；更低 → **3G**

运营商按 MCC=460 + MNC 区分：移动 0/2/4/7/8，联通 1/6/9，电信 3/5/11，广电 15。

### 电池与充电闪电

电池图标用 CSS 画（圆角边框 + 右侧正极小凸点 `.tip` + 绿色填充，低电量转红）。充电时**不再**用流光动画（已移除半透明 `.glow` 扫光与加速刷新），改为充电时由宿主**原生绘制一个闪电**叠在电池上：`draw_batt_bolt()` 用 `html_view_rect("#batt")` 取电池框，按归一化顶点用 `html_view_fill_poly`（扫描线多边形填充）画一个比电池略高的闪电（上下各凸出几像素、仍在 26px 状态栏内），颜色随主题黑/白。litehtml/字体画不出可靠的闪电，所以走原生。

> 翻页时状态栏被钉住（见下「翻页与状态栏」），闪电在 `compose_frame` 合成每帧后补画一次，所以横滑全程都在。

## 数据模型（`u60-datad` 快照）

后端轮询并归一化的字段段：

- `net`：制式、信号条、运营商、频段、NR/LTE 的 RSRP/RSRQ/SNR/RSSI、MCC/MNC、PCI/Cell ID/ARFCN、`nr_bw`，以及载波聚合描述符 `nrca` / `lteca`（直接透传自 `zte_nwinfo_api`），WAN 状态。
  - **`nrca` 格式**（实测）：`;` 分隔载波，`,` 分隔字段，每个副载波 11 个字段：`idx, PCI, ?, band, arfcn, bw, ?, rsrp, rsrq, sinr, rssi`。例：`0,273,1,41,504990,100,0,-140.0,-43.0,-23.0,-120.0;` = n41 / 100MHz / PCI273 / RSRP-140 / SINR-23。所以解析取 `band=f[3]`、`bw=f[5]`、`pci=f[1]`、`rsrp=f[7]`、`sinr=f[9]`。`lteca` 同结构（band 前缀 `B`）。
  - **没有载波聚合时 `nrca`/`lteca` 是空串**——此时信号页只有主载波一张卡片，属正常，不是 bug。CA 激活后副载波卡片自动出现。
  - 另含 `net_select`（选网模式）与 `sa_bands`/`nsa_bands`/`lte_bands`（各制式可用/已锁频段，逗号列表，透传自 netinfo），供锁频页读取/写回。
- `battery`：电量、温度、充电状态、`charger_connect`；以及 `chg_uv`/`chg_ua`（充电器输入电压µV/电流µA，读 `/sys/class/power_supply/usb`）、`bat_uv`/`bat_ua`（电池，读 `.../battery`）——UI 据此显示电压电流并算充/放电功率。
- `clients`：接入设备数 + `list`（DHCP 租约设备名/IP/MAC，解析 `/tmp/dhcp.leases`）。
- `sms`（只读）：`unread`（`zwrt_wms_get_wms_capacity` 的设备+SIM 未读数相加）+ `list`（最近 6 条，每条 `id`/`num`/`date`/`unread`/`text`，读 `zte_libwms_get_sms_data`）。正文是 **UTF-16BE 十六进制**，后端解码成 UTF-8（含 emoji 代理对）；日期 `YY,MM,DD,HH,MM,SS,+TZ` 格式化为 `MM-DD HH:MM`；**每条 `tag` 字段 "1"=未读、"0"=已读（不是反过来）**。`unread` 数每轮刷新，`list` 每 10 轮或未读数变化时重读（标记已读后能马上反映）。
- `wlan`：主 WiFi 的 `ssid` / `key` / `enc`（读 `uci wireless.main_2g.*`）+ `enabled`（`disabled` 取反）。**键名必须是 `wlan` 不能是 `wifi`**——否则消费端按子串找 key 会先命中 `clients.wifi` 计数，导致 SSID/密码读空。
- `nfc`：`switch`（`zwrt_nfc zwrt_nfc_wifi_get`，1=开）。
- `dhcp`：网关 `ip`、地址池 `start`/`limit`、`leasetime`（uci）。
- `traffic`：实时会话上下行速率与字节数（`zwrt_data get_wwandst`，参数须 `cid:1,type:1`）。
- `qos`：`qci`、`ambr_dl`/`ambr_ul`（Mbps）解析自 `/data/logfs/key.log` 的 `[DATA]` 行；外加 `usb_mode`（`zwrt_bsp.usb list`）。**注意**：qci/ambr 是偶发的 PDU 建立日志，会随日志增长滚出窗口——后端读较大尾窗（6000 行）**并缓存最后已知值**。
- `system`：运行时间、CPU 温度、`cpu_usage`（/proc/stat 差值）、`mem_used_pct` + `mem_total`/`mem_avail`、`sw_version`/`imei`（一次性）、型号、固件。

这份 JSON 是后端与 UI 之间的接口契约（完整字段见 `u60-datad/docs/STATE_SCHEMA.md`）。字段增删改名时，后端 schema 和 UI 的 `data.c` / 令牌逻辑必须一起改。

### 选网 / 锁频（厂商 ubus）

从厂商 Web JS（`/usr/zte_web/web`）逆出，已与 `ubus -v list zte_nwinfo_api` 对齐：

- 选网：`nwinfo_set_netselect {net_select}`，值 `WL_AND_5G`(自动) / `Only_5G`(5G SA) / `LTE_AND_5G`(5G NSA) / `Only_LTE`(4G) / `Only_WCDMA`(3G)。
- 锁频（频段逗号列表）：4G 用 `nwinfo_set_lte_ext_band {lte_band}`；5G 用 `nwinfo_set_nrbandlock {nr5g_type, nr5g_band}`（type 0=SA / 1=NSA）；恢复默认 `nwinfo_reset_band_cell_setting`。
- UI 端「可用频段全集」取历来见过的最大集合（modem 锁定后会缩成子集），「已选」未编辑时实时跟随 modem。⚠️ 改选网/锁频会触发 modem 重注册、短暂掉网。

## 硬件事实

- 显示：`/dev/dri/card0`，320x480，RGB565，命令式刷新（DIRTYFB 推帧，`vrefresh=1`），自动枚举 connector/crtc。
- 旋转：面板安装方向相对 framebuffer 扫描顺序等效旋转 180°；`html_view` 的 `put_px` 把逻辑 (x,y) 映射到物理 (W-1-x, H-1-y)。
- 触摸：`/dev/input/event3`（sitronix_ts，多点 ABS_MT），自动探测。
- 电源键：`/dev/input/event0`（`pmic_pwrkey`），键码 `KEY_POWER`(116)。注意触摸屏也会上报 KEY_POWER，所以探测要求“有 KEY_POWER 且无 EV_ABS”以排除触摸屏。
- 背光：`/sys/class/leds/led:lcd/brightness`（0..255，没有 `/sys/class/backlight`）。息屏=写 0，与原厂 `ZTD_SetLcdBrightnessByFile` 一致。

完整设备接口说明见 [HARDWARE.md](HARDWARE.md)。

### 电源键与息屏

- 短按 = 亮屏/息屏；长按 = 切换 `menu.html` 电源菜单；双击（息屏时，可开关）= 唤醒。
- **息屏/亮屏带背光淡入淡出**（`backlight.c`）：息屏从当前亮度平滑降到 1 再写 0；亮屏从 0 渐亮到熄屏前亮度。时长按亮度比例缩放（满亮度各约 0.125s）。
  - 淡变只改实时背光，**不动记忆的用户亮度**——`backlight_get()` 返回 `s_on_level`（用户设定值），所以第五页亮度滑条不会跟着动画跳。
  - 用**真实时钟**控时（写背光 sysfs 较慢，按"步数×sleep"会严重超时）。
  - 息屏后照例 memset 黑 + DIRTYFB（防手电筒下残影）。亮屏时先在背光为 0 时把命令模式面板"热身"几帧，避开退出 idle 的瞬态闪烁（中高亮度已无闪）。
  - **遗留**：极低亮度/偶发息屏的轻微闪，是背光升压电路冷启动/关断的硬件瞬态（"连续按丝滑、单次按才闪"即此）——纯软件难根除，因为"真息屏"就得把背光写 0、boost 断电。

### 屏幕锁（PIN）

4 位数字 PIN 的屏幕锁，状态全在 `htmlmain.c`：

- **状态**：`g_pin`（已设密码，`""`=未启用）、`g_lock_state`（0 正常 / 1 解锁键盘 / 2 设置键盘）、`g_pin_entry`（当前已输入）、`g_lock_err`（显示「密码错误」）。`CUR_PATH` 在 `g_lock_state` 非 0 时优先指向 `lockscreen.html`，覆盖电源菜单与普通页。
- **持久化**：PIN 存 `/data/ui/.lockpin`（点文件，`UI_DIR` 必存在、不被 `adb push *.html` 覆盖）。`load_pin/save_pin/clear_pin`。**开机若已设密码直接进入解锁键盘**。忘记密码 `adb shell rm /data/ui/.lockpin`。
- **触发锁屏**：`screen_off` 的两个入口——电源键短按、自动息屏超时——之后若 `lock_enabled()` 即 `enter_lock(0)`。开机同理。
- **键盘交互**（主循环 `g_lock_state` 分支，仅处理 tap）：`act:pin:<0-9>` 累加，满 4 位**自动提交**（无确认键）；设置键盘存盘并开启，解锁键盘比对——正确 `g_lock_state=0` 回到界面，错误置 `g_lock_err` 并清空。`act:pin:del` 退格，`act:lockcancel` 放弃设置。
- 第五页 `act:locktoggle`：开→关 `clear_pin()`；关→开 `enter_lock(1)` 进设置键盘（输满 4 位才真正开启）。锁定时电源键长按不再开电源菜单。
- **键盘布局**：litehtml 无 grid，用 `.kprow` 居中 + `.kpcell` 行内块（inline-block）等宽，按键也是 inline-block（圆角能被裁切，见下「圆角」教训）；「密码错误」是 `{{LOCKMSG}}` 生成的绝对居中红气泡（屏幕中央）。

### 触摸点击锁存（`touch_input.c`）

`touch_input_read` 会把一次轮询里抽干的事件**坍缩成最后状态**——一次完整的「按下→抬起」若整段落在某次慢渲染期间，按下沿没被看到，整个 tap 就丢了（锁屏键盘连点尤其明显）。修法：触摸层维护**点击边沿锁存 + 8 格队列**，抬起且位移很小即入队；`touch_input_take_tap` 逐个取出。锁屏键盘**一次性把队列全部消费**再渲染一次，所以再快连点也不丢。进入键盘时 `touch_input_clear_taps` 清掉之前残留的 tap（避免开关那一下串进来误触）。

### 翻页与状态栏

横滑翻页用 `compose_frame` 把两页离屏缓冲按位移合成。**顶部 26px 状态栏被钉住**：合成时这一条直接从「滚动到 0 的目标页缓冲」取、不跟着位移，只有下方内容左右滑——否则状态栏会跟着滑、和固定位置的原生闪电割裂。相邻两页状态栏数据相同，所以钉住无缝。

### 渲染器原语（`html_view.cpp`）

litehtml 没有 canvas/SVG/滚动/圆角绘制，这些都由宿主补：

- **原生折线**：HTML 放空的占位框（带 `id`），`get_placement` 拿框坐标，`html_view_polyline` 用 Bresenham 直接画进 framebuffer（可带半透明面积填充）。比"几百个 DOM 点"省太多。
- **圆角填充/描边**：容器的 `draw_solid_fill`/`draw_borders` **原本直接画矩形、忽略 `border-radius`**，所以整个 UI 的圆角在设备上一直没真出现。现 `draw_solid_fill` 在有半径时走 `fill_rounded`（按四角半径跳过圆弧外像素），`draw_borders` 在「四边等宽同色 + 有半径」时走 `stroke_rounded`（内外两个圆角形之差）。卡片/开关/按钮/电池框终于真圆角。
- **多边形填充**：`html_view_fill_poly`（扫描线奇偶规则）画 litehtml/CSS 画不出的形状，如充电闪电。
- **整页高度**：`document::render()` **返回的是内容宽度不是高度**（坑！），用 `root()->get_placement().height` 拿真实高度，否则滚动永远不触发。
- **竖向滚动**：`doc->draw(0,0,-scrollY)` 上移内容；点击命中按 `y+scroll` 修正。拖动时预渲染整页到缓冲、每帧窗口贴图（跟手流畅）。
- **覆盖层（modal）**：`html_view_render_overlay` 不清屏、在已渲染画面上叠加（body 透明只画弹窗框）；先 `fill_rect` 把页面压暗。二级弹窗交互用整屏快照 + 重绘弹窗层（避免每次重排整页）。
- **分段控件滑动高亮**：拖动开始整页快照一次，之后每帧在快照上贴一个半透明蓝框跟手；松手吸附到最近格再应用。
- `html_view_fill_rect`：直接填矩形（压暗背景、画高亮框等）。

### 经验教训（litehtml/CSS）

- `display:table-cell` **不会自动等分**——`table-layout:fixed` 也要给单元格显式 `width:%`，否则按内容宽度参差。
- "调色板"裸类名（如 `.g5p{background}`）会泄漏到任何带该 class 的元素——要带上下文前缀（`.gen.g5p`）。
- 文字与 inline-block 想对齐：让文字也 `display:inline-block; vertical-align:middle`（默认文字按 baseline，会和 inline-block 错位）。
- 绝对定位的底部圆点会盖住长页面最后一张卡——长页面底部留白（`.pgpad`）。
- 不支持图片（`load_image`/`draw_image` 是空实现）：图标用 CSS 画或字体符号；CJK 字体缺 `◔` 这类符号会显示豆腐块，`↑↓` 实测有。
- **圆角靠容器实现**：`border-radius` 只是把半径传给容器，真画矩形还是圆角全看 `draw_solid_fill`/`draw_borders` 怎么写。本项目现已实现圆角填充与等宽圆角描边（见上）；但**复杂边框（四边不等宽/不同色）仍回退直角**。
- **WiFi 开关只能用 `ifconfig`**：本固件厂商把标准 `wifi reload` 禁了（日志 `zte test skip the wifi_load_legacy`），uci `disabled` 不控制实际射频；`ubus zwrt_wlan reload` 会把射频**拆掉不重建**（需重启恢复）。可靠的运行时控制是 `ifconfig wlanN up/down`（wlan0=主2.4G、wlan2=主5G、wlan1/3=访客），开/关状态读 `/sys/class/net/wlanN/operstate`。**注意**：纯 ifconfig 是运行时的，重启后厂商按自身配置恢复；访客接口未启用时不存在，纯 ifconfig 开不出来（故未做访客开关）。
- **节能（PSM）= `iw set power_save` + hotplug 持久化**：`power_save` 只在 ifup 时被应用，所以要持久就得有个 `/etc/hotplug.d/iface/*` 脚本在每次 ifup 重设。本设备上是 `99-disable-powersave`（强制 off）。**开关现在自己把该脚本 echo 到 `/etc/hotplug.d/iface/99-disable-powersave`**（按所选模式写 on/off），功能自包含、不依赖预装脚本；同时清掉旧版遗留的 `psm` 文件（旧版用过那名字，且它排序在 `99-` 之后会反盖）。**实测开机时序**（标记法 + 重启验证）：早期的 `lan` ifup 在 ~15s 触发，那时 wlan0/wlan2 还不存在、`iw set` 静默失败；真正生效靠 **专门的 `ifup INTERFACE=wlan0/wlan2` 事件（~65s，Kiwi 驱动把电台拉起来那一刻）**。所以纯 hotplug 足够、**不需要往 rc.local 加命令**（rc.local 跑得更早，那时 wlanX 不存在，`iw` 一样报 No such device）。
- **UI 设置持久化**：主题/速率单位/双击点亮/自动息屏存 `/data/u60pro/devui.conf`（`load_conf`/`save_conf`，改动即写）；锁屏 PIN 存 `/data/ui/.lockpin`。这些都是内存态，不落盘则重启/重装即丢。
- **DHCP 地址池别信后端前缀**：UI 直接由 uci `network.lan.ipaddr` + `dhcp.lan.start/limit` 现算池范围，跟随实际网段（后端那份可能是写死前缀的）。

## 构建

不需要 root，也不要求宿主机装 `make`。工具链是 Bootlin 的 `aarch64 musl GCC`（`~/aarch64--musl--stable-2025.08-1/`，由 `scripts/_setup_toolchain.sh` 下载）。litehtml 与 FreeType 都编成精简静态库：

```sh
bash scripts/_setup_toolchain.sh      # 一次性
bash scripts/_build_freetype.sh       # → ~/freetype-musl/lib/libfreetype.a
bash scripts/_build_litehtml.sh       # → ~/litehtml-musl/lib/liblitehtml.a
bash scripts/_build_htmlpoc.sh        # → html-poc(.stripped)  即 UI 二进制
```

后端：

```sh
bash u60-datad/scripts/build.sh        # → u60-datad(.stripped)
```

构建要点（踩过的坑）：

- FreeType 不用 configure，直接编模块汇总文件 + 自定义 `ftmodule.h`。litehtml 链接会引用 `FT_Set_Named_Instance`、`FT_Gzip_Uncompress`，所以必须补 `base/ftmm.c`、`gzip/ftgzip.c`；litehtml 头文件要 `-I include/litehtml`（`background.h` 在子目录）。
- litehtml 的 `document_container` 有约 30 个纯虚函数，全部要实现；`create_element` 返回 `nullptr` 也得显式 override，否则是抽象类编译不过。
- litehtml **不支持 CSS grid / JS / CSS 动画**，`var()` 也不可靠——布局用 table/flex/block，主题用 `body.dark`/`body.light` 类切换，动画靠宿主驱动帧。

## 部署与运行

```sh
# 杀掉旧实例再推，否则可能 "Text file busy" 并悄悄保留旧二进制
adb shell "killall -9 u60pro-devui u60-datad"
adb push html-poc.stripped        /data/u60pro/u60pro-devui
adb push u60-datad.stripped       /data/u60pro/u60-datad
adb push ui/*.html ui/*.css       /data/ui/
adb shell "/etc/init.d/zte_topsw_devui stop; sleep 1; \
           cd /data/u60pro; nohup ./u60-datad -i 1000 >/tmp/u60-datad.log 2>&1 & \
           sleep 1; nohup ./u60pro-devui >/tmp/devui.log 2>&1 &"
```

坑位：

- 要完全接管面板，先 `/etc/init.d/zte_topsw_devui stop`；单纯 `killall` 不够，procd 会把它拉起来。
- Busybox 无 `setsid`，后台运行用 `nohup ... &`。
- **本机 shell 注意**：仓库在 Windows 盘，交叉编译在 **WSL**（`wsl -- bash -lc '... /mnt/d/...'`）；而 `adb` 在 **MSYS/Git-Bash** 里会把 `/data`、`/tmp` 这类参数当本地路径“翻译”坏掉（`adb push ui/. /data/ui/` 会卡死）。**adb 命令一律走 PowerShell 工具**，或在路径上加 MSYS 的转义。
- Windows 检出的 `.sh` 可能是 CRLF，WSL 里 `bash` 会报 `set: invalid option` / `$'\r'`；先 `sed -i 's/\r$//'`。

## 开机自启

`/tmp` 是 tmpfs 重启即清空，所以装到持久化的 `/data/u60pro/`：

```text
/data/u60pro/u60pro-devui    # UI
/data/u60pro/u60-datad       # 数据后端
/data/u60pro/start.sh        # 停原厂 UI → nohup 拉起后端 + UI
```

`scripts/install-autostart.sh` 用 awk 在 `/etc/rc.local` 的 `exit 0` 前**幂等**插一行：

```sh
[ -x /data/u60pro/start.sh ] && sh /data/u60pro/start.sh >/tmp/u60pro-boot.log 2>&1 &
```

开机时原厂 UI 可能先起来，rc.local 较晚由 `start.sh` 停掉它再接管，会有短暂切换。

> **别让自启重复。** 上面 `start.sh` 是**手动/开发**装法。而**插件**会在 rc.local 里写它自己的一行（带 `# u60pro_devui` 标记，内联停原厂+拉起 datad/devui，不调用 start.sh）。两者**只能留一个**——都在的话每次开机会重复拉起，出现两个 `u60-datad` 抢着写同一份 `state.json`（与「单一聚合器」设计相悖；第二个 `u60pro-devui` 一般抢不到 DRM 自己退）。插件靠 `grep 'u60pro_devui'` 判断自启状态，所以要统一就保留插件那条、删掉 `start.sh` 行（及文件）。

## 性能说明

这块屏更接近命令式显示：`DRM_IOCTL_MODE_DIRTYFB` 每次固定耗时约 **33ms**且**与脏区大小无关**（在阻塞等待面板 TE/刷新节拍），像素拷贝只占约 30µs。所以整屏刷新率上限 ~30fps 是**面板硬件节拍**，**用户态无法“超频”**（要改内核/DTS 的 DSI/TE）。日常 UI 只在内容变化时才推帧，30fps 只影响整屏动画（滑动翻页、充电流光），实际足够流畅。

滑动翻页是**跟手**的：拖动时把当前页与目标页渲染到两张离屏 logical 位图，按手指位移用 `compose_frame` 实时合成 [左|右] 窗口；松手按位移是否过半决定提交或回弹，再用几帧 settle 动画收尾。

## 本次（2026-06-13）改动与经验

- 修复**纯 SA 误显示 LTE**：SA 下 modem 仍可能残留 `lte_rsrp`，所以 LTE 段只在非纯 SA（有 NSA/LTE 锚点）且 RSRP 在有效区间时才显示。
- 状态栏重排：上下行速率移到状态栏制式徽章左侧；新增 5 段信号阶梯条；电池图标美化 + 充电流光动画。
- 信号页右上角原本的速率位改为 **QCI / AMBR**（来自后端 `qos`）。
- 设置页：ARFCN 默认隐藏点击才显示；删掉原来的“短按电源键…”提示框；新增 **ADB 开关**（读 `usb_mode`，拨动发 `ubus zwrt_bsp.usb set`）；主题切换从右下角文字按钮改为设置页里的开关。
- 底部翻页从“第 N/M 页”文字框改为**居中圆点**指示。
- 后端 `u60-datad` 补回/新增字段：`net.nrca`/`net.lteca`、`qos.usb_mode`，并**修复回 `wlan` 段**（之前本地源缺失会导致 WiFi 页 SSID/密码读空）。
- 路径修正：`data.c` 的快照路径统一为 `/tmp/u60-datad/state.json`（曾残留旧的 `/tmp/zwrt-datad`）。
- 自启脚本名同步为 `u60-datad`（旧名 `zwrt-datad`）。

### 第二轮（状态栏 / 单位 / 运营商 / 排版）

- 状态栏重排为：`时间 … 实时网速 ｜ 制式文字 ｜ 信号阶梯条 ｜ 电池 ｜ 电量`。制式从徽章框改为**纯色文字**（`.gt`）；信号条移到电池左边并缩小。
- 实时网速改为 `↑<上行> ↓<下行> <单位>` 样式（字体确认含 ↑U+2191/↓U+2193 字形，可直接用；CJK 字体缺 `◔` 这类才会豆腐）。单位由设置页新增的「速率单位」开关切换：**Mbps（比特率）↔ MB/s（字节率）**，共享单位取上下行较大值自适应到 M/K/基本单位。
- 运营商对中国大陆四大运营商显示中文：中国移动 / 中国联通 / 中国电信 / 中国广电（按 MCC=460 + MNC 判定，非大陆保留原名）。
- 修复第三页溢出：litehtml 无滚动，页面固定 480px，内容超高时底部开关被裁掉。合并信息卡为一张（6 行），设置卡 3 个开关（ADB / 速率单位 / 主题），并压缩 `.row` 间距，整页控制在 480px 内。
- 信号阶梯条从信号页卡片移入状态栏（页内不再单列）。

### 第三轮（细节修正）

- **未激活副载波**：`nrca` 里 RSRP 为 floor sentinel `-140.0`（配置了但没真正激活）的载波，卡片置灰（`.q-off`）并加「未激活」标签，不再标红，避免误读成“信号极差”。
- **状态栏紫框 bug**：制式徽章的背景色规则原本是裸类名 `.g5p{background}`，会命中任何带该 class 的元素——包括新的纯文字 `.gt` 制式 span，于是文字后面多了个紫色块。已把背景规则收紧到 `.gen.g5p` 等，文字 span 只吃 `.gt.g5p` 的颜色。**教训：litehtml 里这种“调色板类名”一定要带上下文前缀，否则会泄漏到同名 class 的其它元素。**
- **电池正极脱节**：`.tip` 是 `.r` 的直接子元素，吃到了 `.sbar .r>span{margin-left}` 的 6px，于是和电池体分开了。改为把电池体 + 正极包进 `.bw`，外间距只作用于 `.bw`，正极紧贴电池。
- **状态栏对齐**：制式文字 / 信号条 / 电池 / 电量统一 `vertical-align:middle`。
- **信号强度表示**：不再按质量染色，改为**格数表示强度**（由 RSRP 映射 1–5 格），填充格固定白色（浅色主题为深色），空格暗灰。

### 第四轮（状态栏对齐 / 制式白字 / 未激活样式）

- 制式文字（5G+/5G…）统一为**白字**（继承状态栏文字色，主题安全），不再分色。
- **状态栏垂直对齐**：根因是文字 span 默认按 baseline 对齐，而信号格/电池是 inline-block 按 middle 对齐，两者错位（文字“居中”、图标“沉底”）。把 `.sbar .r > span` 统一 `display:inline-block; vertical-align:middle`，全部按中线对齐。**litehtml 里混排文字与 inline-block 想对齐，得让文字也 inline-block。**
- 「未激活」标签：改 `inline-block` + `line-height` 让文字在边框内垂直居中，并加大 `margin-left` 不与频宽挤在一起。
- 未激活载波**只把信号值（RSRP/SINR）置灰**，频段/频宽/PCI 仍按正常格式显示。

### 短信（只读，v0.3.5）

第二页只读短信，不做发送。后端解码见上「数据模型 · `sms`」。UI 端：

- 列表是**折叠卡片**（`act:sms:N`），只显示号码/时间 + 一行预览（`utf8_trunc` 按字符边界截断、不切坏多字节）；未读条目红点 + 蓝色号码。
- 点卡片：`data_refresh` 取该条全文存进 `g_sms_*`，开二级详情弹窗（复用 modal overlay）；若该条未读则 `ubus call zwrt_wms zwrt_wms_modify_tag '{"id":"<id>;","tag":0}'` 标记已读（`tag:0`=已读，分号分隔多 id）。未读数变化后端立即重读列表，红点/状态栏图标随之消失。
- 正文含任意字符，渲染前 `html_esc` 转义 `&<>`。
- **状态栏未读信封图标**：有未读时在时钟右侧原生画一个信封（`draw_sms_icon`，蓝身+白翻盖），占位 span `#smsico` 定位；和电池闪电一样翻页时常驻、锁屏也显示。字体没有信封字形所以走原生。

### 电源菜单（原生圆形按键，v0.3.6）

`menu.html` 三个竖排大圆按键。litehtml 画不了电源符号/圆箭头/X，所以 HTML 只给圆的占位框（`#pmc-off`/`#pmc-reboot`/`#pmc-cancel`）+ 文字标签 + 点击区，圆盘和图标全部由 `draw_power_menu()` 原生绘制（`render()` 里在 `path` 含 `menu.html` 时调用）：`pm_disc` 用 `fill_round_rect`(rad=半径)画实心圆，`pm_arc` 画环带（环形扇区多边形→`fill_poly`），`pm_line` 画粗线段（四边形）。关机=带顶部缺口的环+竖线（电源符号），重启=约 290° 环+箭头三角，取消=两条对角线 X。二次确认沿用 `g_pwr_confirm`：armed 时画白色光环 + 标签变「再按一次」。

### USB-C 数据线模式（v0.3.7）

插线检测走 `zwrt_bsp.typec list`（`cc_attch_state/cc_attach_state`）并兼容 `zwrt_bsp.charger list` 的 `charger_connect`。首次检测到新接入时弹出 USB 模式选择，断开后清空本次选择状态；系统页 `act:usbmode` 可手动再次打开。弹窗不是点选即应用，而是先把 `g_usb_choice` 高亮，点「确认」后才调用 `usb_apply_mode()`。

四个模式按“供电方向 × USB 网络”组合：给 U60 充电时切 `PR_Swap=sink` 并关闭 powerbank；U60 给外设充电时切 `PR_Swap=source` 并打开 `zwrt_bsp.powerbank`。有线网络开启后会按所选协议切 USB gadget composition，拉起 ADB，并把 `rndis0/ecm0/ncm0/usb0` 中存在的接口加入 `br-lan`，让 USB 主机从同一 LAN/DHCP 出网。

协议选择 `g_usb_proto` 持久化到 `/data/u60pro/devui.conf`，可选 `auto/rndis/ecm/ncm`。自动模式目前按场景选择：反向供电给手机/PAD时倾向 ECM，普通电脑侧共享倾向 RNDIS；手动 NCM 会直接重配 configfs 的 `ncm.0 + ffs.adb` 等函数组合。

### 其它版本要点（v0.3.1 – v0.3.7）

- **v0.3.7 锁频频段全集兜底**：已锁定后底层有时只回当前锁定频段，UI 冷启动会误以为其它频段“不存在”。现在 `DEFAULT_NR_BANDS` / `DEFAULT_LTE_BANDS` 作为初始全集，再合并运行时观测值；同时扩大 `data.c` 分段缓冲，减少状态截断。
- **v0.3.4 电源直供电（DPS）**：第六页开关，插电时直供、减电池损耗。走 `ubus zwrt_bsp.charger set '{"direct_power_supply_mode":"enable/disable"}'`，状态读 `list`。同版还做了锁屏改版（锁定先显示首页预览 + 原生锁图标，滑动才进 PIN 键盘）、电源菜单二次确认、设备列表「在线过滤」（WiFi 关联 ∪ ARP 在线，去掉租约残留）、亮度持久化，并**移除双击点亮**（熄屏完全忽略触摸更安全）。
- **v0.3.5 锁频芯片一行四个**：弹窗内宽 ~264px，原 56px 芯片四个顶满会换行，缩到 52px + 边距 3px + 字号 14px 才稳定四列。
- 背光淡入淡出（v0.3.2）见上「电源键与息屏」。

## 版本清单与更新机制（`version.json`）

配套的「U60 DevUI 管理插件」（运行在原厂 Web 后台，不在本仓库）负责在设备上下载/安装/更新这套开源 UI。更新检测不靠 git tag，而靠每个 release 附带的 `version.json`，把整套拆成**三个可独立升级的组件**：

- **datad** — 后端二进制（仓库 [zwrt-datad](https://github.com/33333s/zwrt-datad)，资产 `u60-datad-aarch64`）。
- **devui** — 渲染器二进制（本仓库，资产 `u60pro-devui-aarch64`）。
- **ui** — 界面模板（本仓库，资产 `ui.tar.gz`，装到 `/data/ui`）。

`version.json` **按仓库分散**：本仓库的含 `devui` + `ui` 两项，datad 仓库的只含 `datad`。

```jsonc
// 本仓库 release 的 version.json
{ "schema": 1,
  "devui": { "version": "0.3.3", "asset": "u60pro-devui-aarch64" },
  "ui":    { "version": "0.3.3", "asset": "ui.tar.gz" } }
// zwrt-datad release 的 version.json
{ "schema": 1, "datad": { "version": "0.3.1", "asset": "u60-datad-aarch64" } }
```

- 仓库根目录留了一份 `version.json` 作为**源头**（发版时改它），但插件实际读的是 **release 资产**（`…/releases/latest/download/version.json`），所以**每次发版都要把 `version.json` 连同二进制/`ui.tar.gz` 一起传到 release**。
- 插件把远端各组件版本与本地记录（localStorage）比对，标“可更新”；可单独更新某个组件，也可一键更新全部。二进制更新先下到 `*.new` 再 `mv` 覆盖（避开运行中可执行文件的 `ETXTBSY`），devui 更新后校验失败自动回退原厂。
- **彻底卸载**清 `/data/u60pro`（二进制）、`/data/ui`（界面）、`/etc/rc.local` 自启行，并恢复原厂。

### 更新源：GitHub 直连 / 网盘镜像

插件可切换两种源，文件名必须一致：

- **GitHub 直连**：`https://github.com/<repo>/releases/latest/download/<file>`，基址 + 文件名拼接。
- **网盘镜像**：直接克隆 release 的同名文件。本项目网盘用 AList，**每个文件带独立 `?sign=` 直链**（末尾 `:0` = 永不过期），无法用基址拼接，所以插件里维护一张 `NETDISK_FILES` 全直链表（key 形如 `devui/ui.tar.gz`）。换网盘或重传导致 sign 变化时，只改这张表。两个 `version.json` 不能同目录（撞名），故 devui、datad 各一个目录。

**发版清单**：升对应组件的 `version.json` 版本号（ui-only 改动只升 `ui`、二进制改动只升 `devui`；改了 menu.html/style.css 这类界面文件就也要升 `ui`）→ 传 `version.json` + 资产到 GitHub release →（如启用网盘）跑 `openlist-manual-update.cmd` 把 release 离线下载镜像到网盘并确认。

> **网盘镜像脚本** `openlist-manual-update.cmd`（仓库父目录）：登录 OpenList，把每个 repo 的 `releases/latest/download/version.json` 与网盘上的比对，版本变了才**先删旧文件、再 `add_offline_download` 拉新文件**。双击即可；离线下载是异步的，跑完去网盘列表确认。

> **坑：插件 UI 更新会残留旧页面文件 → 页数翻倍。** 插件的 `installUiTemplates` 早期只 `cp -f` 覆盖、**不删旧文件**。v0.3.5 给页面改名后（`02-wifi`→`02-sms/03-wifi`…），旧名文件不会被同名覆盖，于是新旧并存——「5 页变 10 页」。修复：拷新文件前先 `rm -f /data/ui/*.html /data/ui/*.css`（`.lockpin` 是点文件不受影响），且放在「解压成功」分支内避免下载失败误删。**给页面改名/删页是个跨版本兼容陷阱，更新逻辑必须先清场再铺。** 已中招的用户重跑一次「更新 UI」即自愈。（插件运行在原厂 Web 后台，不在本仓库；改完重新部署插件即可，不走 release。）

## 仓库约定

- 不提交父目录里的 vendor blobs 或分析产物。
- 项目必须能仅靠公开源码和标准接口构建。
- 新增字体或 UI 资源优先选开源许可证资源；仓库**不打包任何 ZTE 字体**（设备上运行时从 `/usr/ui/fonts/ZTEZhengYuan.ttf` 加载，保持 clean-room）。CJK 字体没有 `↑↓◔` 等符号字形，会显示成豆腐块——状态栏等处用纯文字（如“下/上”）而非箭头符号。
- 对后续开发有帮助但不适合只留在本地记忆里的经验，写进这里或 [HARDWARE.md](HARDWARE.md)。
