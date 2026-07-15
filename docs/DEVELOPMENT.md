# U60Pro 屏幕 UI 开发说明

这份文档记录项目当前最重要的开发事实，方便后续维护、移植，也便于随仓库版本化后公开分享。目标是让关键经验不只停留在本地记忆里，而是和代码一起长期保存。

> 当前正式命名与安装路径已经统一为：`zwrt-datad`、`/data/plugins/zwrt-datad/zwrt-datad`、`/data/plugins/u60pro-devui/`。下文历史章节里如果出现 `u60-datad` 或 `/data/u60pro`，表示当时版本记录，不再是当前安装规范。

## 维护范围

- 本文只保留公开可分享的架构、构建、硬件接口和 UI 行为说明。
- 本地设备连接、SSH、编译机地址、实机日志和临时部署记录不进入仓库，边界规则见 [`REPO_BOUNDARY.md`](REPO_BOUNDARY.md)。
- 第二页、基站信息卡、`Ports/SSB/TA/Grant/RB` 等 modem/信令页面行为统一整理到 [`modem.md`](modem.md)。

## 项目目标

`u60pro-devui` 是 ZTE U60Pro 前面板屏幕 UI 的一个 clean-room 开源替代实现。目标形态是一份单独的静态 `aarch64` 二进制，只依赖标准 Linux/OpenWRT 接口，不链接 ZTE 私有库，也不提交任何专有资源。

配套后端是 `zwrt-datad`：单进程数据采集器，轮询 ubus 并把结果归一化成一份 JSON 快照，通过本机 `HTTP /state + SSE /events` 暴露。UI 只读这个本机接口，从不直接调用 ubus。

核心设计原则有两条：

1. **UI 与后端解耦**：所有 ubus 访问集中在后端，UI 只读一个本机 HTTP/SSE 接口，便于审计、分享、独立重建。
2. **程序与界面解耦**：二进制本身是固定的“框架”，**实际界面是 `/data/plugins/u60pro-devui/ui` 目录里的 HTML/CSS**。用户改界面只需要改 HTML，不必重新编译。开源发布时二进制保持不变，界面完全可由用户自定义。

## 架构总览

```text
ubus 服务 ──▶ zwrt-datad ──▶ HTTP /state + SSE /events ──▶ u60pro-devui ──▶ DRM framebuffer
                                                                   │
                                              渲染 /data/plugins/u60pro-devui/ui/*.html
```

- 渲染引擎：**litehtml**（C++ HTML/CSS 排版）+ **FreeType**（含 CJK 字形）→ 直接画进 RGB565 dumb buffer。
- 没有浏览器、没有 JavaScript；状态只通过本机 `127.0.0.1:9460` 的 HTTP/SSE 读取，界面本身不直接访问外网。
- 全部静态链接（liblitehtml.a + libfreetype.a + musl），单文件可直接拷到设备运行。

### `/data/plugins/u60pro-devui/ui` 界面模型

- 每个 `NN-name.html` 是一页，按文件名排序，可左右滑动切换。`menu.html` 是电源长按弹出的覆盖层，不计入翻页。
- 所有页面用 `<link rel="stylesheet" href="style.css">` 共享一份样式（容器的 `import_css` 从 `/data/plugins/u60pro-devui/ui/` 读取）。
- HTML 里的 `{{TOKEN}}` 在渲染前由 C 宿主替换成实时数据（见“模板令牌”）。
- `href="act:xxx"` 的链接是**动作**：点击后 UI 不跳转，而是执行对应动作（翻页指示、揭示密码、切主题、切 USB-C 模式等）。
- 每次渲染都**重新读取**页面文件 = 改完 HTML 直接 `adb push` 到 `/data/plugins/u60pro-devui/ui` 即可热生效，不必重启进程。

当前顶层页（左右滑动，底部圆点指示；`menu.html` 为电源长按覆盖层）。**页面按文件名编号排序**；v1.2.9 起 WiFi、短信、信令读取、锁频和测速收进“更多功能”二级页，减少顶层横滑页数。给页面改名/删页仍然会触发「插件更新残留」坑，见版本机制一节。

- **01-signal.html — 信号**：状态栏；运营商 + 制式，右上角 QCI/AMBR；按载波分卡片（`频段·频宽 / EARFCN / PCI / RSRP / SINR`，按质量上色、未激活置灰）。当 datad 输出 `net.HSR=true` 时，主信号卡片会切换为紫色高铁模式并显示“高铁模式”；`mcc == 460` 且载波 `EARFCN/ARFCN` 命中高铁专网白名单 `507150, 527070, 531390, 153370, 505230, 627744, 634464, 423630` 时，单个 LTE/NR 载波卡片会染紫并标注“高铁专网”。真正 `HSR` 仍以 datad 信令确认为准。表头随组网模式（`5G SA / 5G NSA / EN-DC / 4G`）变化并显示 `L LTE + M NR 载波 · 总X MHz`。
- **02-functions.html — 更多功能**：进入 WiFi、短信、信令读取、锁频和可选测速二级页；同时扫描 `ui/functions/*.html` 作为用户自定义入口。
- **ui/subpages/*.html — 二级页**：`wifi.html`、`sms.html`、`cell.html`、`lock.html`、`speedtest.html` 通过 `act:sub:<文件名>` 打开，统一用 `act:backfunc` 返回。
- **05-charts.html — 图表**：CPU 占用+温度 / 内存 / 网速 / 电池(功率+温度) 四张原生折线图。
- **06-system.html — 系统/设置**：CPU/内存占用、温度、充电器/电池电压电流、版本号、IMEI(打码)、QCI/AMBR 缓存；屏幕亮度滑条、自动息屏分段控件、锁屏 / 电源直供电(DPS) / USB-C 供电方向 / USB 网络共享 / 速率单位 / 主题 开关，并提供“刷新 AMBR 缓存”按钮。可竖向滚动。
- **lockscreen.html — 锁屏键盘**：特殊页（同 `menu.html`，不计入翻页）。开启锁屏后由宿主在 `g_lock_state` 非 0 时全屏显示，4 位 PIN 数字键盘 + 删除键。状态栏（含未读短信信封图标）一并显示。详见下方「屏幕锁（PIN）」。
- **menu.html — 电源菜单**：电源键长按弹出。三个竖排大圆形按键（关机/重启/取消），圆盘和图标原生绘制，下方文字标签，二次确认。详见下方「电源菜单（原生圆形按键）」。

### `DEVUI-IPC` 外部内容区（内建接口）

这套项目现在除了 `/data/plugins/u60pro-devui/ui/*.html` 页面流，还内建了一条“别的本机进程借用屏幕”的渲染通道，代码在 `src/devui_ext.c` / `include/devui_ext.h`，对外正式文档见 [DEVUI-IPC.md](DEVUI-IPC.md)。

- 通信方式：本地 Unix Domain Socket `/tmp/u60-devui.sock`
- 事件回传：JSON Lines 文件 `/tmp/u60-devui-events.log`
- 当前命令：`PING`、`CLOSE`、`FRAME`、`IMAGE`、`DRAW`、`TEXT`
- 典型用途：让别的本机程序把像素帧、图片、轻量绘图脚本或纯文字页直接送进 DevUI，而不用接管 DRM，也不用改原来的 `/data/plugins/u60pro-devui/ui`

当前集成方式不是“整屏覆盖”，而是**保留 DevUI 自己的状态栏，把外部内容放到下面的内容区**：

- U60 Pro 逻辑整屏仍是 `320 x 480`
- 顶部 `26px` 状态栏保留给 DevUI 原生绘制
- 外部接口当前使用下方 `320 x 454` 内容区
- 时间、信号、电池、短信图标仍由 DevUI 自己刷新

这样做的目的，是把“系统公共信息”和“外部应用内容”分开：外部接入方只需要管自己的业务画面，设备自己的状态栏、锁屏、亮灭屏和兜底返回逻辑仍由 DevUI 统一掌控。

### 模板令牌 `{{TOKEN}}` 与动作 `act:`

令牌在 `src/htmlmain.c` 的 `build_kv()` 里一次性从 `data_refresh()` 快照填好，`apply_template()` 单遍替换（不递归，所以含子令牌的整段 HTML 必须在 C 里拼好）。`href="act:xxx"` 是动作（不跳转，执行对应操作并重绘）。

> **完整、权威的令牌表与动作表见 [UI-GUIDE.md](UI-GUIDE.md)**（面向"写自己界面"的用户，随代码同步更新）。这里只记内部要点：
> - 复合令牌（C 拼好的整段 HTML）：`STATUSBAR`、`DOTS`、`CARRIERS`（信号页载波区，含表头+卡片）、`NETSEG`（选网分段控件）、`AUTOOFF`（自动息屏分段控件）、`CLIENTLIST`、`TOAST`。
> - 滑条/分段控件（亮度 `#bright-bar`、选网 `#netseg`、自动息屏 `#autoseg`）由宿主按元素 `id` 用 `get_placement` 定位、处理点/拖；这些元素**必须带对应 `id`**。
> - 安全：`KEY`/`CELLID`/`IMEI` 默认打码，需对应 `reveal*` 动作才明文。

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

## 数据模型（`zwrt-datad` 快照）

后端轮询并归一化的字段段：

- `net`：制式、信号条、运营商、频段、NR/LTE 的 RSRP/RSRQ/SNR/RSSI、MCC/MNC、PCI/Cell ID/ARFCN、`nr_band`、`nr_bw`，以及载波聚合描述符 `nrca` / `lteca` / `ltecasig`（直接透传自 `zte_nwinfo_api`），WAN 状态。
  - **`nrca` 格式**（实测）：`;` 分隔载波，`,` 分隔字段，每个副载波 11 个字段：`idx, PCI, ?, band, arfcn, bw, ?, rsrp, rsrq, sinr, rssi`。例：`0,273,1,41,504990,100,0,-140.0,-43.0,-23.0,-120.0;` = n41 / 100MHz / PCI273 / RSRP-140 / SINR-23。所以解析取 `band=f[3]`、`bw=f[5]`、`pci=f[1]`、`rsrp=f[7]`、`sinr=f[9]`。
  - **`lteca` 格式**有两种：新格式与 `nrca` 同样是 11 字段；旧格式是 5 字段 `pci, band, is_scc, earfcn, bw`。旧格式本身不带 `RSRP/SINR`，这些值会单独出现在 `ltecasig`。
  - **`ltecasig` 格式**（实测）通常为每组 `rsrp, rsrq, sinr, rssi, ?, ?`；并且组数可能比 `lteca` 少 1，因为它只包含 `SCC`，不包含 `PCell`。UI 规则是：第一张 LTE 卡片使用主小区 `lte_*` 字段，后续 LTE SCC 按顺序消费 `ltecasig`。
  - **没有载波聚合时 `nrca`/`lteca`/`ltecasig` 是空串**——此时信号页只有主载波一张卡片，属正常，不是 bug。CA 激活后副载波卡片自动出现。
  - **高铁模式**：`net.HSR=true` 由 datad 根据信令确认，UI 会把第一页主信号卡片切换为紫色并显示“高铁模式”。实机预览可临时用 `DEVUI_FORCE_HSR=1` 或 `/tmp/u60-force-hsr` 强制模拟；正常运行没有这些开关时仍只以 datad 为准。载波卡片上的紫色“高铁专网”提示改为白名单制：仅当 `mcc == 460` 且 `EARFCN/ARFCN` 命中 `507150, 527070, 531390, 153370, 505230, 627744, 634464, 423630` 时才染紫，这不是 `net.HSR` 真值来源。
  - 另含 `net_select`（选网模式）与 `sa_bands`/`nsa_bands`/`lte_bands`（各制式可用/已锁频段，逗号列表，透传自 netinfo），供锁频页读取/写回。
- `battery`：电量、温度、充电状态、`charger_connect`；以及 `chg_uv`/`chg_ua`（充电器输入电压µV/电流µA，读 `/sys/class/power_supply/usb`）、`bat_uv`/`bat_ua`（电池，读 `.../battery`）——UI 据此显示电压电流并算充/放电功率。
- `clients`：接入设备数 + `list`（DHCP 租约设备名/IP/MAC，解析 `/tmp/dhcp.leases`）。
- `sms`（只读）：`unread`（`zwrt_wms_get_wms_capacity` 的设备+SIM 未读数相加）+ `list`（最多 32 条，每条 `id`/`num`/`date`/`unread`/`text`，读 `zte_libwms_get_sms_data`）。正文是 **UTF-16BE 十六进制**，后端解码成 UTF-8（含 emoji 代理对）；日期 `YY,MM,DD,HH,MM,SS,+TZ` 格式化为 `MM-DD HH:MM`；**每条 `tag` 字段 "1"=未读、"0"=已读（不是反过来）**。`unread` 数每轮刷新，`list` 每 10 轮或未读数变化时重读（标记已读后能马上反映）。
- `wlan`：主 WiFi 的 `ssid` / `key` / `enc`（读 `uci wireless.main_2g.*`）+ `enabled`（`disabled` 取反）。**键名必须是 `wlan` 不能是 `wifi`**——否则消费端按子串找 key 会先命中 `clients.wifi` 计数，导致 SSID/密码读空。
- `nfc`：`switch`（`zwrt_nfc zwrt_nfc_wifi_get`，1=开）。
- `dhcp`：网关 `ip`、地址池 `start`/`limit`、`leasetime`（uci）。
- `traffic`：实时会话上下行速率与字节数（`zwrt_data get_wwandst`，参数须 `cid:1,type:1`）。
- `qos`：`qci`、`ambr_dl`/`ambr_ul`（Mbps）解析自 `/data/logfs/key.log` 的 `[DATA]` 行；外加 `usb_mode`（`zwrt_bsp.usb list`）。**注意**：qci/ambr 是偶发的 PDU 建立日志；其中 `ambr` 往往比 `qci` 更稀疏。后端会分别读取**最后一条 `qci`** 和 **最后一条 `apn_ambr_*`** 相关日志，并缓存最后已知值，避免 `ambr` 被统一尾窗里的其它日志挤掉。
- `system`：运行时间、CPU 温度、`cpu_usage`（/proc/stat 差值）、`mem_used_pct` + `mem_total`/`mem_avail`、`sw_version`/`imei`（一次性）、型号、固件。

这份 JSON 是后端与 UI 之间的接口契约（完整字段见 `zwrt-datad/docs/STATE_SCHEMA.md`）。字段增删改名时，后端 schema 和 UI 的 `data.c` / 令牌逻辑必须一起改。

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

- **状态**：`g_pin`（已设密码，`""`=未启用）、`g_lock_state`（0 正常 / 1 锁屏首页预览 / 2 设置 PIN 键盘 / 3 解锁 PIN 键盘）、`g_pin_entry`（当前已输入）、`g_lock_err`（显示「密码错误」）。`CUR_PATH` 在 `g_lock_state == 1` 时复用 `g_pages[0]` 作为只读预览，在 `g_lock_state == 2/3` 时指向 `lockscreen.html`，并覆盖电源菜单与普通页。
- **持久化**：PIN 存 `/data/plugins/u60pro-devui/ui/.lockpin`（点文件，`UI_DIR` 必存在、不被 `adb push *.html` 覆盖）。`load_pin/save_pin/clear_pin`。**开机若已设密码直接进入解锁键盘**。忘记密码 `adb shell rm /data/plugins/u60pro-devui/ui/.lockpin`。
- **触发锁屏**：`screen_off` 的两个入口——电源键短按、自动息屏超时——之后若 `lock_enabled()` 即 `enter_lock(0)` 进入首页预览。开机同理。预览态显示实时第一页和原生锁图标，滑动任意方向才进入解锁 PIN 键盘。
- **键盘交互**（主循环 `g_lock_state` 分支，仅处理 tap）：`act:pin:<0-9>` 累加，满 4 位**自动提交**（无确认键）；设置键盘存盘并开启，解锁键盘比对——正确 `g_lock_state=0` 回到界面，错误置 `g_lock_err` 并清空。`act:pin:del` 退格，`act:lockcancel` 放弃设置。
- 第五页 `act:locktoggle`：开→关 `clear_pin()`；关→开 `enter_lock(1)` 进设置键盘（输满 4 位才真正开启）。锁定时电源键长按不再开电源菜单。首页预览态会隐藏 `网络测速` 入口、内嵌面板和测速原生控件，但后台测速检测继续轮询，解锁后仍能立刻显示正确状态。
- **键盘布局**：litehtml 无 grid，用 `.kprow` 居中 + `.kpcell` 行内块（inline-block）等宽，按键也是 inline-block（圆角能被裁切，见下「圆角」教训）；「密码错误」是 `{{LOCKMSG}}` 生成的绝对居中红气泡（屏幕中央）。

### 触摸事件锁存（`touch_input.c`）

`touch_input_read` 会把一次轮询里抽干的事件**坍缩成最后状态**——一次完整的「按下→抬起」若整段落在某次慢渲染期间，按下沿没被看到，整个 tap 就丢了（锁屏键盘连点尤其明显）。修法：触摸层维护**点击边沿锁存 + 8 格队列**，抬起且位移很小即入队；`touch_input_take_tap` 逐个取出。锁屏键盘**一次性把队列全部消费**再渲染一次，所以再快连点也不丢。进入键盘时 `touch_input_clear_taps` 清掉之前残留的 tap（避免开关那一下串进来误触）。

覆盖层必须明确消费属于自己的触摸尾巴。弹层内抬起会同时生成普通 release 和锁存 tap；若只关闭覆盖层，下一帧 tap 会命中下方同坐标的入口。短信详情关闭统一走 `sms_close()`：清除全部已锁存 tap/stroke 与待重放 release，再撤销覆盖层；不能只丢弃其中一个队列项。否则较早的 release 仍可能在下一轮命中下方短信卡片。短信详情只在 `html_view_click()` 命中 `act:smsclose` 时退出，正文或空白区点击保持打开；慢帧期间未观察到按下沿时，弹层会折叠并处理最新完整手势，而不是让它落到下层页面。

为了给 `DEVUI-IPC` 提供系统级返回手势，触摸层现在还会额外维护一条**短笔画队列**：每次完整按下到抬起，都会把起点和终点缓存下来，宿主可通过 `touch_input_take_stroke` 取出最近一笔。外部画面前台时，`htmlmain.c` 只在内容区里消费这些笔画，并用它识别“左边缘起手、明显向右、纵向偏移不大”的返回手势。

- 当前返回手势阈值：起点落在内容区左边缘约 `24px` 内，水平位移至少约 `56px`，纵向偏移不超过约 `44px`
- 手势命中后立即关闭外部画面，回到原 DevUI
- 这类系统手势不会写入 `/tmp/u60-devui-events.log`
- 外部画面刚激活时会先清空残留 tap/stroke，避免上一层页面的触摸尾巴串进外部应用

换句话说，`touch_input.c` 现在不只是“保 tap 不丢”，也承担了“把一次手势完整交给上层判断”的职责。

### 翻页与状态栏

横滑翻页用 `compose_frame` 把两页离屏缓冲按位移合成。**顶部 26px 状态栏被钉住**：合成时这一条直接从「滚动到 0 的目标页缓冲」取、不跟着位移，只有下方内容左右滑——否则状态栏会跟着滑、和固定位置的原生闪电割裂。相邻两页状态栏数据相同，所以钉住无缝。

`DEVUI-IPC` 走的是同一条设计思路：外部画面前台时，不接管整屏，而是先把外部内容渲染到一块“内容区画布”，再由 `render_ext_view()` 把这块画布贴到状态栏下方，最后补画原生状态栏和短信信封图标。这样不论是普通 HTML 页面横滑，还是外部应用临时接管内容区，顶部系统信息的视觉和刷新机制都保持一致。

### 渲染器原语（`html_view.cpp`）

litehtml 没有 canvas/SVG/滚动/圆角绘制，这些都由宿主补：

- **原生折线**：HTML 放空的占位框（带 `id`），`get_placement` 拿框坐标，`html_view_polyline` 用 Bresenham 直接画进 framebuffer（可带半透明面积填充）。比"几百个 DOM 点"省太多。
- **圆角填充/描边**：容器的 `draw_solid_fill`/`draw_borders` **原本直接画矩形、忽略 `border-radius`**，所以整个 UI 的圆角在设备上一直没真出现。现 `draw_solid_fill` 在有半径时走 `fill_rounded`（按四角半径跳过圆弧外像素），`draw_borders` 在「四边等宽同色 + 有半径」时走 `stroke_rounded`（内外两个圆角形之差）。卡片/开关/按钮/电池框终于真圆角。
- **多边形填充**：`html_view_fill_poly`（扫描线奇偶规则）画 litehtml/CSS 画不出的形状，如充电闪电。
- **整页高度**：`document::render()` **返回的是内容宽度不是高度**（坑！），用 `root()->get_placement().height` 拿真实高度，否则滚动永远不触发。
- **竖向滚动**：`doc->draw(0,0,-scrollY)` 上移内容；点击命中按 `y+scroll` 修正。拖动时预渲染整页到缓冲、每帧窗口贴图。滚动帧只搬动 26px 状态栏下方的内容区，固定状态栏沿用上一帧，不要在每帧滚动中重复画状态栏原生图标。`menu.html`、`lockscreen.html` 是全屏特殊页，渲染时必须强制 `scroll=0`，否则会继承当前长页面滚动偏移，出现电源菜单被卷到屏幕外的问题。
- **整页重建与内存水位**：实机排查过一次“手动刷新 AMBR 后内存持续上涨”的问题，结论是 **AMBR 读取本身不是根因**。真正触发上涨的是前端在页面 HTML 发生变化后反复整页重建 `litehtml::document`；如果先创建新文档、再覆盖全局 `g_doc`，旧 DOM 会在新 DOM 分配完成前继续占住内存，musl 下容易把堆高水位一步步抬上去。现已在 `src/html_view.cpp` 的 `html_view_render_html()` / `html_view_render_overlay()` 中先 `g_doc.reset()`，再 `document::createFromString(...)`，实机现象已从“每次刷新继续爬升”收敛为“首次大页面重建有一次性抬升，随后稳定”。后续如果再遇到“某字段一刷新就涨内存”，优先先查是否又走到了未释放旧文档的整页重建路径，而不要先怀疑 `qos/ambr` 解析逻辑。
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
- **UI 设置持久化**：主题/速率单位/双击点亮/自动息屏存 `/data/plugins/u60pro-devui/devui.conf`（`load_conf`/`save_conf`，改动即写）；锁屏 PIN 存 `/data/plugins/u60pro-devui/ui/.lockpin`。这些都是内存态，不落盘则重启/重装即丢。
- **DHCP 地址池别信后端前缀**：UI 直接由 uci `network.lan.ipaddr` + `dhcp.lan.start/limit` 现算池范围，跟随实际网段（后端那份可能是写死前缀的）。

## 构建

不需要 root，也不要求宿主机装 `make`。工具链是 Bootlin 的 `aarch64 musl GCC`（`~/aarch64--musl--stable-2025.08-1/`，由 `scripts/_setup_toolchain.sh` 下载）。litehtml 与 FreeType 都编成精简静态库：

```sh
bash scripts/_setup_toolchain.sh      # 一次性
bash scripts/_build_freetype.sh       # → ~/freetype-musl/lib/libfreetype.a
bash scripts/_build_litehtml.sh       # → ~/litehtml-musl/lib/liblitehtml.a
bash scripts/build.sh                 # → u60pro-devui(.stripped)  即正式 UI 二进制
```

后端：

```sh
bash zwrt-datad/scripts/build.sh       # → zwrt-datad(.stripped)
```

构建要点（踩过的坑）：

- FreeType 不用 configure，直接编模块汇总文件 + 自定义 `ftmodule.h`。litehtml 链接会引用 `FT_Set_Named_Instance`、`FT_Gzip_Uncompress`，所以必须补 `base/ftmm.c`、`gzip/ftgzip.c`；litehtml 头文件要 `-I include/litehtml`（`background.h` 在子目录）。
- litehtml 的 `document_container` 有约 30 个纯虚函数，全部要实现；`create_element` 返回 `nullptr` 也得显式 override，否则是抽象类编译不过。
- litehtml **不支持 CSS grid / JS / CSS 动画**，`var()` 也不可靠——布局用 table/flex/block，主题用 `body.dark`/`body.light` 类切换，动画靠宿主驱动帧。

## 部署与运行

```sh
# 杀掉旧实例再推，否则可能 "Text file busy" 并悄悄保留旧二进制
adb shell "killall -9 u60pro-devui zwrt-datad u60-datad"
adb shell "mkdir -p /data/plugins/u60pro-devui/ui /data/plugins/zwrt-datad"
adb push u60pro-devui.stripped    /data/plugins/u60pro-devui/u60pro-devui
adb push zwrt-datad.stripped      /data/plugins/zwrt-datad/zwrt-datad
adb push ui/*.html ui/*.css       /data/plugins/u60pro-devui/ui/
adb shell "/etc/init.d/zte_topsw_devui stop; sleep 1; \
           nohup /data/plugins/zwrt-datad/zwrt-datad -i 1000 >/tmp/zwrt-datad.log 2>&1 & \
           sleep 1; nohup /data/plugins/u60pro-devui/u60pro-devui >/tmp/devui.log 2>&1 &"
```

坑位：

- 要完全接管面板，先 `/etc/init.d/zte_topsw_devui stop`；单纯 `killall` 不够，procd 会把它拉起来。
- Busybox 无 `setsid`，后台运行用 `nohup ... &`。
- **本机 shell 注意**：仓库在 Windows 盘，交叉编译在 **WSL**（`wsl -- bash -lc '... /mnt/d/...'`）；而 `adb` 在 **MSYS/Git-Bash** 里会把 `/data`、`/tmp` 这类参数当本地路径“翻译”坏掉（`adb push ui/. /data/plugins/u60pro-devui/ui/` 会卡死）。**adb 命令一律走 PowerShell 工具**，或在路径上加 MSYS 的转义。
- Windows 检出的 `.sh` 可能是 CRLF，WSL 里 `bash` 会报 `set: invalid option` / `$'\r'`；先 `sed -i 's/\r$//'`。

## 开机自启

`/tmp` 是 tmpfs 重启即清空，所以装到持久化的 `/data/plugins/`：

```text
/data/plugins/u60pro-devui/u60pro-devui   # UI
/data/plugins/u60pro-devui/start.sh       # 正常开机才停原厂 UI → 拉起后端 + UI
/data/plugins/u60pro-devui/devui.conf     # UI 设置持久化
/data/plugins/zwrt-datad/zwrt-datad       # 数据后端
```

**当前稳定方案（2026-06-25 实机回归后确认）** 仍然是 **vendor 早期 bring-up + `rc.local` 晚接管**：

```sh
[ -x /data/plugins/u60pro-devui/start.sh ] && sh /data/plugins/u60pro-devui/start.sh >/tmp/u60pro-boot.log 2>&1 & # u60pro_devui
```

`scripts/install-autostart.sh` 当前做的是：

- 保留并启用原厂 `/etc/init.d/zte_topsw_devui`
- 清掉旧的 `/etc/init.d/u60pro-devui` / `/etc/init.d/zwrt-datad` / `/etc/init.d/u60-datad` rc.d 软链接（如果之前试过 `procd` 方案）
- 在 `/etc/rc.local` 的 `exit 0` 前写入上面的 `start.sh` 钩子
- 顺手清理旧的 `/data/u60pro/u60pro-devui` / `/data/u60pro/u60-datad` / `/data/u60pro/start.sh` 残留
- 安装完成后立即执行一次 `start.sh`，把当前前台切到自定义 DevUI

`start.sh` 在这条稳定路径下的职责是：

- 记录 `mode_main_state` / `reboot_reason_code` / `/proc/cmdline` / 电源与输入设备枚举到 `/data/plugins/u60pro-devui/boot-trace.log`
- 正常开机时停掉原厂 `zte_topsw_devui`
- 正常开机时拉起单个 `zwrt-datad`
- 拉起 `u60pro-devui`

所以当前真实启动顺序是：

```text
procd -> zte_topsw_devui (早期屏幕/触摸 bring-up) -> rc.local -> /data/plugins/u60pro-devui/start.sh -> stop vendor -> start datad + devui
```

会有一个**短暂的原厂界面闪屏**，但这是目前验证过最稳的方案。

**离线充电现在改为 DevUI 自己接管。** 在这版固件上，“关机插电”并不总会稳定暴露成 `mode_power_off_*` 或缺失 `silent_boot.mode=nonsilent`；实机日志里出现过**明明是关机充电，却同时表现为 `mode_power_on` + `silent_boot.mode=nonsilent`** 的情况，导致单靠启动脚本 guard 无法可靠把屏幕留给原厂，而且落进去的普通 DevUI 还是无触控的。

当前策略改为：

- `start.sh legacy` 是**默认且稳定**的开机路径，会沿用 `nohup` 后台拉起 `zwrt-datad` + `u60pro-devui`。
- `start.sh procd` 仍保留在脚本里，但仅供手动实验；**不要**把它当默认自启方案。
- `u60pro-devui` 内部仍保留“外部供电 + 触控初始化失败 -> 强制切到全屏充电页”的兜底，避免误入普通页面。
- `start.sh` 会把 `mode_main_state`、`reboot_reason_code`、`/proc/cmdline`、电源状态和输入设备枚举写进 `/data/plugins/u60pro-devui/boot-trace.log`，用于继续比对不同开机路径。

已实测：关机状态插电直接进入 DevUI 的全屏充电页；正常开机仍进入普通 DevUI。

```sh
mode_main_state="$(awk -F\"'\" '/option mode_main_state/ { print $2; exit }' /etc/config/zwrt_zte_mc_tmp 2>/dev/null)"
case "$mode_main_state" in
  mode_power_off_*) echo "charge boot: start devui only" ;;
  mode_power_on|mode_power_on_charger) echo "normal boot: start datad + devui" ;;
  *) echo "fallback to cmdline/touch+power heuristics" ;;
esac
```

### `procd` 接管实验记录（2026-06-25）

这天实机尝试过一版“彻底抛弃原厂早期启动”的方案：

- 禁用 `/etc/init.d/zte_topsw_devui`
- 安装 `/etc/init.d/u60-datad`
- 安装 `/etc/init.d/u60pro-devui`
- 让 `u60pro-devui` 直接由 `procd` 在开机阶段接管 DRM

目标是消掉开机先闪原厂界面的过程，但最终**没有通过实机验证**。在干净重启后的真实现象里，出现过：

- `u60pro-devui` / `u60-datad` 服务在 boot 后直接 `inactive`
- 即使稍后手动拉起 `u60pro-devui`，也会遇到 `drm: SETCRTC failed: Permission denied`
- 更关键的是 `/dev/input` 有时只剩 `event0` / `event1` / `event2`，**触摸用的 `event3` 不出现**
- 触摸缺失时，`u60pro-devui` 会按现有兜底逻辑误判成“外部供电 + 无触摸”的充电模式，只进全屏充电页

目前判断：这版固件上，原厂 `zte_topsw_devui` 在早期启动阶段很可能还隐含了屏幕/触摸相关的 bring-up 副作用。单纯把启动顺序前移，不能稳定替代它。

> **别让自启重复。** 当前稳定方案只该保留一条 `rc.local` 钩子。若旧的 `procd` 自启软链接、旧插件写入的重复 `# u60pro_devui` 行，或手工 `nohup` 调试进程同时存在，最常见后果就是两个 `zwrt-datad` 争抢同一个 `9460` 端口，以及第二个 `u60pro-devui` 因 DRM 已被占用而退出。

## 性能说明

这块屏更接近命令式显示：`DRM_IOCTL_MODE_DIRTYFB` 每次固定耗时约 **33ms**且**与脏区大小无关**（在阻塞等待面板 TE/刷新节拍），像素拷贝只占约 30µs。所以整屏刷新率上限 ~30fps 是**面板硬件节拍**，**用户态无法“超频”**（要改内核/DTS 的 DSI/TE）。日常 UI 只在内容变化时才推帧，30fps 只影响整屏动画（滑动翻页、充电流光），实际足够流畅。

滑动翻页是**跟手**的：拖动时把当前页与目标页渲染到两张离屏 logical 位图，按手指位移用 `compose_frame` 实时合成 [左|右] 窗口；松手按位移是否过半决定提交或回弹，再用几帧 settle 动画收尾。

触摸延迟主要受主循环等待和拖动起始阈值影响：屏幕亮着时空闲等待保持在约 8ms，熄屏时才回到较长等待；普通页面拖动起始阈值为 10px，点击判定仍保留独立的 14px 容错。

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

- 列表是**折叠卡片**（`act:sms:ID`），只显示号码/时间 + 一行预览（`utf8_trunc` 按字符边界截断、不切坏多字节）；未读条目红点 + 蓝色号码。
- 点卡片：`data_refresh` 取该条全文存进 `g_sms_*`，开二级详情弹窗（复用 modal overlay）；正文位于固定高度的 `#smsview` 裁剪区，只有 `#smsbody` 的实测高度超过视口且本次手势能改变滚动偏移时才启用拖动，短短信不进入滚动状态。只有底部 `act:smsclose` 才关闭。弹层必须消费自己处理过的 tap/stroke，不能让关闭动作穿透到下方短信卡片。若该条未读则 `ubus call zwrt_wms zwrt_wms_modify_tag '{"id":"<id>;","tag":0}'` 标记已读（`tag:0`=已读，分号分隔多 id）。未读数变化后端立即重读列表，红点/状态栏图标随之消失。
- 关闭详情只能点击底部关闭按钮；正文和弹窗空白区域的 tap 由弹层消费但不关闭，也不能穿透并打开列表中下方的另一条短信。
- 正文含任意字符，渲染前 `html_esc_breaks` 转义 `&<>` 并把原始换行转换成 `<br>`。
- **状态栏未读信封图标**：有未读时在时钟右侧原生画一个信封（`draw_sms_icon`，蓝身+白翻盖），位置按固定状态栏坐标和当前时间文字宽度计算，不依赖页面里的占位元素，所以不会跟着长页面滚动；和电池闪电一样翻页时常驻、锁屏也显示。字体没有信封字形所以走原生。

### 电源菜单（原生圆形按键，v0.3.6）

`menu.html` 三个竖排大圆按键。litehtml 画不了电源符号/圆箭头/X，所以 HTML 只给圆的占位框（`#pmc-off`/`#pmc-reboot`/`#pmc-cancel`）+ 文字标签 + 点击区，圆盘和图标全部由 `draw_power_menu()` 原生绘制（`render()` 里在 `path` 含 `menu.html` 时调用）：`pm_disc` 用 `fill_round_rect`(rad=半径)画实心圆，`pm_arc` 画环带（环形扇区多边形→`fill_poly`），`pm_line` 画粗线段（四边形）。关机=带顶部缺口的环+竖线（电源符号），重启=约 290° 环+箭头三角，取消=两条对角线 X。二次确认沿用 `g_pwr_confirm`：armed 时画白色光环 + 标签变「再按一次」。

`menu.html` 是覆盖式特殊页，不属于普通长页面滚动。`render()` 对 `menu.html` 和 `lockscreen.html` 使用 `html_view_set_scroll(0)`，普通页面继续使用 `g_scroll`。不要把这行改回全局 `g_scroll`，否则在设置页滑到底后长按电源，菜单会被当前滚动偏移卷走。

### USB-C 数据线模式调试记录（v0.3.7 撤回版）

这轮 USB-C 菜单尝试过“插线弹窗，四个模式：给 U60 充电+共享网络、U60 给设备充电+共享网络、仅给 U60 充电、仅给设备充电”。结论是：功能还不能发布，下面是已经确认的事实和坑，后续继续做时先看这里。

- 原厂前屏 UI 的 Type-C 菜单不是直接手搓 USB gadget。静态分析和运行时 trace 显示它主要调用 `ZTD_SetTypeCDeviceRole()`、`ZTD_SetTypeCSinkRole()`、`ZTD_SetPowerBankEnableState()`、`ZTD_GetTypeCRole()`；这些最终走 `zwrt_bsp.typec set` / `zwrt_bsp.powerbank set`。SDK 字符串里能看到 `DR_Swap`、`PR_Swap`、`source`、`sink`、`host`，但不要仅凭函数名把“DeviceRole”想当然映射成 `DR_Swap=device`；这个值需要继续用原厂 UI 实机 trace 确认。
- 原厂开机/ADB 共存的稳定 USB composition 来自 `/tmp/usb.log`：先 `0x19d2:0x1225 mass_storage`，再 `0x19d2:0x1403 rndis_gsi`，最后 `0x19d2:0x1404 rndis_gsi,diag,serial,modem,mass_storage,ffs,dpl,qdss`。这是 ZTE legacy `/sbin/usb/compositions/usb_switch` 路径，最终把 `rndis_gsi`、`ffs.adb`、`diag/serial/modem/mass_storage/dpl/qdss` 挂到同一个 config。系统自带 Qualcomm configfs composition 另有 `9059`，描述是 `DIAG+ADB+RNDIS : ECM`，第一 config 挂 `gsi.rndis + ffs.diag + ffs.adb`，第二 config 挂 `gsi.ecm`。ADB 开关后续优先沿用这两条系统路径，不要手搓 ADB+ECM config。
- `/sys/devices/virtual/android_usb/android0/usbnet_type` 在设备上是只读状态，不要指望直接写它切 `rndis/ecm/ncm`。`usb_switch` 会根据函数组合创建 `rndis0` 或 `ecm0`，但底层 `zte_ubus_bsp_usb` 仍会周期性报告当前 composition/usbnet 状态。
- 设备内核有 `gsi.ecm`、`gsi.rndis`、`ncm.0` 等函数目录，说明 ECM/NCM 相关函数存在；但 `usb_switch` 脚本只内置了 `rndis_gsi`、`rndis_lagecy`、`ecm` 等 case，没有通用 NCM case。NCM 不能只凭目录存在就宣称可用，必须单独验证。
- 已试过的危险/不稳定路径：手搓双 config、复制完整原厂 config 到第二 config、强行组合 ECM+ADB，都可能导致 Windows 不认、ADB 掉线、USB 服务反复重启，严重时设备重启。不要把这些实验方案直接发布。
- 实测一次“ADB 开不起”根因是前面实验残留了空的 `/sys/kernel/config/usb_gadget/g1/configs/c.2`：`idProduct` 已是 `0x1404`、`ffs.adb` 也挂着，但 `UDC` 为空，手动 `echo a600000.dwc3 > UDC` 报 `Invalid argument`，`dmesg` 明确有 `Config c/2 of g1 needs at least one function` / `failed to start g1: -22`。恢复方式是只删除这个空配置再绑定：`rm -f .../configs/c.2/f*; rmdir .../configs/c.2/strings/*; rmdir .../configs/c.2; echo a600000.dwc3 > .../UDC`。恢复后 `zwrt_bsp.usb list` 变 `connect:1`，`android0/state=CONFIGURED`，Windows ADB 重新出现。**这个坑再次说明不要发布双 config 方案。**
- 原厂“ECM/RNDIS 自适应”不要自己复刻 configfs，优先用系统自带 composition。电脑/ADB 调试可用 `/sbin/usb_composition 9059 n n y y`（`RNDIS + DIAG + ADB : ECM`），Windows 选 RNDIS 且 ADB 仍在。发布给前屏的“USB网络共享”不用 ADB，默认先切 Type-C `device/sink`，必要时先 `echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode`，再 `/sbin/usb_composition 9057 n n y n`（`RNDIS : ECM`）。实测在电脑上这是 ADB 关闭状态的 RNDIS 共享，`idProduct=0x9057` 且 `rndis0 carrier=1`；在手机上 `9057` 可能 `CONFIGURED` 但 `rndis0/ecm0` 都 `NO-CARRIER`，所以前屏命令会等待几秒检查 carrier，若 RNDIS/ECM 都没起来则自动降到纯 ECM：`/sbin/usb_composition 90B1 n n y n`。纯 ECM 实测 `ecm0` 变 `UP,LOWER_UP`、桥进 `br-lan`，并出现手机 DHCP 租约。注意 `90B1` 只是 fallback：如果换回电脑，前屏后台看到 `90B1` 无 carrier 会重新探测 `9057`，避免从手机切到电脑后粘在 ECM；如果还残留 `/tmp/u60-typec-source`，watchdog 会先试 `90B1`，但只有 `idProduct=0x90B1/0x90b1` 且 `ecm0 carrier=1` 才允许继续反向供电，不能把切换前残留的 `rndis0 carrier=1` 当成功。若 `90B1` 没有 `ecm0 carrier`，自动清掉 source 标记并回到 `9057/RNDIS`。若用户同时打开反向供电和网络共享，实测可用顺序是：暂停 USB 网络 watchdog，先在 `device/sink` 下用 `90B1` 等到 `ecm0 carrier=1`，再 `DR_Swap=device + PR_Swap=source`，再 `powerbank state=1`，再补一次 `DR_Swap=device`，最后切 `90B1`；成功状态为 `power_role=source`、`data_role=device`、`cc_attch_state=1`、`otg_powerbank_state=1`、`ecm0 carrier=1`。一旦底层 `/sys/bus/platform/devices/a600000.ssusb/mode` 卡成 `none`，直接切 composition 只会停在 `DISCONNECTED`，要先拉回 `peripheral`。
- 系统自带 Qualcomm composition 里有 `9057`（RNDIS:ECM）、`9059`（DIAG+ADB+RNDIS:ECM）、`90B1`（ECM）。`9059` 能在设备侧创建 `rndis0 + ecm0`，但实测手机连接时仍可能 `NO-CARRIER`，且如果 Type-C 角色处在 `no_cc`/host 状态，手机不会识别 U60 网卡。后续如继续用 `9059`，必须从干净拔插开始验证 carrier、DHCP、手机侧网卡，而不是只看设备侧接口存在。
- 反向供电（充电宝模式）和 USB device 网络是两件事：`powerbank state=1` / `PR_Swap=source` 只能证明 U60 在给对端供电，不代表 U60 已经作为 USB 网卡设备被对端枚举。调试时必须同时看 `zwrt_bsp.typec list`、`zwrt_bsp.usb list`、gadget `UDC/idVendor/idProduct/configs`、`ip -br link`、`brctl show br-lan`、`dmesg`。
- 插充电宝也弹菜单是错误方向：不能把“充电连接”当成“数据线连接”。后续弹窗触发应优先看 Type-C/USB 数据连接状态（例如 `zwrt_bsp.usb connect/typec_cc`、`typec cc_attch_state` 和数据角色），不要仅靠 charger 事件；纯充电宝/纯电源不应该弹 USB 数据模式菜单。
- 设置页手动打开 USB 菜单时，确认按钮不能被 USB 后台切换阻塞。UI 上应该先关闭弹窗/显示 toast，再后台执行 Type-C/USB 切换；弹窗打开期间暂停周期数据刷新和 USB 轮询，避免 litehtml 重排导致点击慢。
- 弹窗交互要求：点击选项只高亮，不立即应用；点“确认”才执行，点“取消/稍后”只关闭。litehtml 触摸点击建议走 tap queue，不要只依赖 release 坐标；按钮不要使用贴底 absolute 布局，嵌入式点击命中容易失灵。
- 320x480 屏幕上 USB 弹窗内容很容易被裁切。四个选项如果带长说明会自动换行撑爆高度；更稳的布局是四个较大的标题卡片 + 简短状态行 + 确认/取消，或去掉每项说明。不要用桌面浏览器盒模型估高度，必须实机看屏幕。

### 其它版本要点（v0.3.1 – v0.3.6）

- **v0.3.4 电源直供电（DPS）**：第六页开关，插电时直供、减电池损耗。走 `ubus zwrt_bsp.charger set '{"direct_power_supply_mode":"enable/disable"}'`，状态读 `list`。同版还做了锁屏改版（锁定先显示首页预览 + 原生锁图标，滑动才进 PIN 键盘）、电源菜单二次确认、设备列表「在线过滤」（WiFi 关联 ∪ ARP 在线，去掉租约残留）、亮度持久化，并**移除双击点亮**（熄屏完全忽略触摸更安全）。
- **v0.3.5 锁频芯片一行四个**：弹窗内宽 ~264px，原 56px 芯片四个顶满会换行，缩到 52px + 边距 3px + 字号 14px 才稳定四列。
- 背光淡入淡出（v0.3.2）见上「电源键与息屏」。

## 版本清单与更新机制（`version.json`）

配套的「U60 DevUI 管理插件」（运行在原厂 Web 后台，不在本仓库）负责在设备上下载/安装/更新这套开源 UI。更新检测不靠 git tag，而靠每个 release 附带的 `version.json`，把整套拆成**三个可独立升级的组件**：

- **datad** — 后端二进制（仓库 [zwrt-datad](https://github.com/33333s/zwrt-datad)，资产 `zwrt-datad-aarch64`）。
- **devui** — 渲染器二进制（本仓库，资产 `u60pro-devui-aarch64`）。
- **ui** — 界面模板（本仓库，资产 `ui.tar.gz`，装到 `/data/plugins/u60pro-devui/ui`）。

`version.json` **按仓库分散**：本仓库的含 `devui` + `ui` 两项，datad 仓库的只含 `datad`。

```jsonc
// 本仓库 release 的 version.json
{ "schema": 1,
  "devui": { "version": "1.2.11", "asset": "u60pro-devui-aarch64" },
  "ui":    { "version": "0.4.10", "asset": "ui.tar.gz" } }
// zwrt-datad release 的 version.json
{ "schema": 1, "datad": { "version": "0.6.2", "asset": "zwrt-datad-aarch64" } }
```

- 仓库根目录留了一份 `version.json` 作为**源头**（发版时改它），但插件实际读的是 **release 资产**（`…/releases/latest/download/version.json`），所以**每次发版都要把 `version.json` 连同二进制/`ui.tar.gz` 一起传到 release**。
- 插件把远端各组件版本与本地记录（localStorage）比对，标“可更新”；可单独更新某个组件，也可一键更新全部。二进制更新先下到 `*.new` 再 `mv` 覆盖（避开运行中可执行文件的 `ETXTBSY`），devui 更新后校验失败自动回退原厂。
- **彻底卸载**清 `/data/plugins/u60pro-devui`、`/data/plugins/zwrt-datad`、历史残留 `/data/u60pro` 与旧 `/data/ui`，删掉 `/etc/rc.local` 里的 `u60pro_devui` 自启行；如果设备上还残留过实验版 `/etc/init.d/u60pro-devui` / `/etc/init.d/zwrt-datad` / `/etc/init.d/u60-datad` 或对应 rc.d 软链接，也一并删除；最后重新启用 `/etc/init.d/zte_topsw_devui`。

### 更新源：GitHub release

当前发布流程统一以 GitHub release 为准，文件名保持固定：

- `https://github.com/<repo>/releases/latest/download/<file>`
- devui 仓库资产：`u60pro-devui-aarch64`、`ui.tar.gz`、`version.json`
- datad 仓库资产：`zwrt-datad-aarch64`、`version.json`

**发版清单**：升对应组件的 `version.json` 版本号（ui-only 改动只升 `ui`、二进制改动只升 `devui`；改了 `ui/*.html` / `style.css` 这类界面文件就也要升 `ui`；两边都改就两项都升）→ `bash scripts/build.sh` 生成 `u60pro-devui.stripped` → 发布前把它重命名成 `u60pro-devui-aarch64` → 打包顶层平铺的 `ui.tar.gz` → 把 `version.json` + 资产传到 GitHub release。若你在仓库外另做镜像，直接镜像 GitHub release 的同名文件即可；本仓库不再维护手动网盘同步脚本。
```sh
bash scripts/build.sh
cp u60pro-devui.stripped u60pro-devui-aarch64
(cd ui && COPYFILE_DISABLE=1 tar -czf ../ui.tar.gz -- *.html *.css)
```
> **坑：插件 UI 更新会残留旧页面文件 → 页数翻倍。** 插件的 `installUiTemplates` 早期只 `cp -f` 覆盖、**不删旧文件**。v0.3.5 给页面改名后（`02-wifi`→`02-sms/03-wifi`…），旧名文件不会被同名覆盖，于是新旧并存——「5 页变 10 页」。修复：先把 `ui.tar.gz` 解压到临时目录并确认新页面数量，再清空 `/data/plugins/u60pro-devui/ui` 顶层旧 `*.html` / `*.css`（`.lockpin` 是点文件不受影响），最后复制新模板并核对安装后的页面数量；如果设备还遗留旧 `/data/ui`，则先迁移再删除。**给页面改名/删页是个跨版本兼容陷阱，更新逻辑必须先校验、再清场、再铺新文件。** 已中招的用户点「重装UI」即可自愈；如果远端版本变了，正常「更新 UI」也会走同一套清理逻辑。（插件运行在原厂 Web 后台，不在本仓库；改完重新部署插件即可，不走 release。）
> **坑：`ui.tar.gz` 的打包结构必须和旧版保持一致。** 设备侧更新逻辑默认期望的是“**顶层平铺页面文件**”的 tar 包：顶层直接是 `01-signal.html`、`02-functions.html`、`style.css`、`subpages/...`，**没有** `ui/` 目录、**没有** `./` 前缀、**没有** macOS 产生的 `._*` AppleDouble 文件。用 macOS 自带打包工具直接打整个目录时，容易把这些额外条目也塞进去，导致 UI 更新失败。打包时应显式关闭资源叉（如 `COPYFILE_DISABLE=1`），并直接列出页面文件名生成 tar；`examples/custom-functions/` 不属于默认 UI 包。

## 仓库约定

- 不提交父目录里的 vendor blobs 或分析产物。
- 项目必须能仅靠公开源码和标准接口构建。
- 新增字体或 UI 资源优先选开源许可证资源；仓库**不打包任何 ZTE 字体**（设备上运行时从 `/usr/ui/fonts/ZTEZhengYuan.ttf` 加载，保持 clean-room）。CJK 字体没有 `↑↓◔` 等符号字形，会显示成豆腐块——状态栏等处用纯文字（如“下/上”）而非箭头符号。
- 对后续开发有帮助但不适合只留在本地记忆里的经验，写进这里或 [HARDWARE.md](HARDWARE.md)。

## 2026-06-25 设备 Smoke Test 补记（临时推送 / RSS 观察 / 清理完成）

本轮不是替换正式文件，而是把测试二进制先推到设备 `/tmp` 做最小闭环验证，避免污染正式安装目录 `/data/plugins/...`。

- 本地构建：
  - `zwrt-datad/zwrt-datad.zigtest`
  - `u60pro-devui/html-poc.zigtest`
- 推送方式：设备侧没有 `scp`，改用 `ssh` stdin 直接写入 `/tmp/u60-datad.zigtest` 和 `/tmp/html-poc.zigtest`
- 测试步骤：
  - 停掉正式 `/data/plugins/zwrt-datad/zwrt-datad -i 1000` 与 `/data/plugins/u60pro-devui/u60pro-devui`
  - 启动 `/tmp/u60-datad.zigtest -i 1000`
  - 启动 `/tmp/html-poc.zigtest`
  - 连续 `touch /data/plugins/u60pro-devui/ui/style.css` 6 轮，观察 UI 进程在重复 CSS reload 下的 RSS / VSZ
- 设备侧观测：
  - UI 启动日志正常：DRM / touch / power key 均成功初始化
  - UI RSS：`5272 KB -> 5296 KB -> 5296 KB -> 5452 KB -> 5912 KB -> 5912 KB -> 5968 KB`
  - UI VSZ：`17696 KB -> 17760 KB -> 17760 KB -> 17760 KB -> 18272 KB -> 18272 KB -> 18336 KB`
  - 这轮 smoke test 下没有复现“几十 MB 很快飙到 200 MB”式增长，CSS 重载后 RSS 只有小幅爬升并保持在约 `5.8 MB`
- 限制说明：
  - 这次是 SSH-only 验证，没有直接做屏幕肉眼回读，所以“灭屏后首帧是否会黑屏”没有做人工可视确认
  - 但至少验证了新二进制能在设备正常起进程，并且在连续 CSS 触发重排时没有出现明显内存失控
- 清理 / 恢复：
  - 已杀掉 `/tmp` 测试进程
  - 已删除 `/tmp/u60-datad.zigtest`、`/tmp/html-poc.zigtest`、`/tmp/codex-*`
  - 已恢复正式 `/data/u60pro/u60-datad -i 1000` 与 `/data/u60pro/u60pro-devui`

## 2026-06-25 设置页新增“数据刷新间隔”补记

- 系统页新增一张“数据刷新”卡片，放在开关区域下方。
- 交互复用自动熄屏的 segmented control 风格，选项为：`停止 / 1s / 2s / 5s`。
- 配置项持久化到 `/data/plugins/u60pro-devui/devui.conf` 的 `refresh_ms=`。
- 语义约定：
  - `refresh_ms > 0`：持续接收 SSE 最新快照，但只按该间隔把最新 live 状态提交到页面并触发重绘
  - `refresh_ms = 0`：暂停把 live 状态刷进页面，但分钟级时钟仍继续刷新
- 同步约定：DevUI 会把该档位同步下发到 datad `/modem/control?poll_ms=...`；在支持 modem 信令 side-channel 的 datad 版本中，这个值也会同步控制信令 pump / 解析节奏。
- 这样用户可以在“更实时”和“更省资源/更稳”之间自己取舍。

## 2026-06-25 锁频页短距离左滑回弹后混入图表页残影

- 现象：在 `04-lock.html` 上向左小幅拖动，触发“预览下一页（05-charts）但未达到翻页阈值”，随后页面自动回弹到锁频页；回弹后，监控图表页的原生图表绘制会残留在锁频页上。
- 根因：这条路径里 framebuffer 在拖动/回弹动画阶段已经被“当前页 + 下一页预览”改写过，但回弹结束后当前页路径并没有变化，`render()` 仍可能命中 HTML 复用缓存，导致没有强制整页重绘。图表页的 native draw 内容因此被带回了锁频页。
- 修正：在 page swipe 的“提交翻页 / 回弹归位”结束后统一调用 `invalidate_render_html_cache()`，强制下一帧完整重绘当前页，避免邻页的 native 内容残留。

### 2026-06-25 补充：刷新间隔卡片样式 / 运营商中文显示
- 刷新间隔卡片现已回到 `seg4` 四档布局，并继续保留 `.segc` 的 `white-space: nowrap`，避免分段标签被挤压换行。
- 中国大陆四家运营商中文显示之前没有走全链路统一：部分页面直接使用后端快照里的英文 `operator`，只在后续某些 HTML 片段里额外覆盖。现改为在 `data_refresh()` 解析 `net` 段时统一正规化 `operator_name`，优先按 `MCC=460 + MNC` 映射；若 PLMN 信息缺失，再兼容 `China Mobile / Unicom / Telecom / Broadnet` 等英文名回退为中文。
- 载波汇总显示补充：信号页“已连接载波 / 总 MHz”统计现在按**展示出来的载波卡片**计算，未激活但已配置的载波也会计入数量与带宽汇总，并继续保留灰色“未激活”标记。与此同时，`5GA / 5G+ / 5G` 判定仍只使用活跃载波统计，避免因为把未激活载波计入展示汇总而误判制式角标。
- 更正：`5GA / 5G+ / 5G` 角标也按**展示出来的 NR 载波与带宽**判断，而不只看当前活跃载波。这样像 `SA n78+n78` 但第二个 `n78` 处于未激活状态时，仍然可以按双载波 / 200MHz 能力显示 `5GA`。
- 刷新间隔卡片后续移除了 `0.5s` 档位，只保留 `停止 / 1s / 2s / 5s`。原因是当前设备上的 `u60-datad` 仍以 `-i 1000` 运行，前端即便切到 SSE，数据源本身默认也只会以秒级节奏产出完整新快照；`0.5s` 只会增加 UI 提交频率，却不会带来更快的真实数据。兼容上，旧配置里若残留 `refresh_ms=500`，启动时会自动归一到 `1000`。

## 2026-07-03 modem 页面整理
- 第二页、基站信息卡、`Signal Metrics` 对接、`Ports/SSB/Serving SSB/Grant/RB/TA` 这些页面行为说明，已经从本文末尾移出，统一整理到 [`modem.md`](modem.md)。
- 本文档从这里开始不再继续堆放 modem/信令页的排障流水账，后续只保留通用 UI 开发内容。

## Optional speedtest integration

The current speedtest UX is documented in [`SPEEDTEST.md`](SPEEDTEST.md). In short: DevUI does not bundle `better-speedtest`; when `/data/plugins/better-speedtest/better-speedtest` is executable, the "更多功能" page shows a `网络测速` tile that opens `ui/subpages/speedtest.html`. Duration `0` is a loop mode that repeats 15-second tests until stopped. Locked preview mode hides speedtest controls and native widgets. The old standalone `07-speedtest.html` swipe page has been removed and should not be shipped in current UI packages.

## 2026-07-08 信号卡片与锁屏预览补记

- 第一页默认模板使用 `{{SIGNALCARDS}}` 作为完整信号区；`{{CARRIERS}}` 只作为旧模板兼容令牌保留。信号卡片、未激活/高铁专网标签、ML1 附加指标和锁屏预览边界统一整理到 [`SIGNAL-CARDS.md`](SIGNAL-CARDS.md)。
- `{{NEIGHBORCARDS}}` 作为首页邻小区入口，信令解析关闭时返回空串；展开后按 `PCI / 频段 / RSRP / RSRQ` 显示 LTE/NR 邻区。
- `未激活` 和 `高铁专网` 标签继续使用 `.coff` / `.chsr`，文字在真机字体下上移约 1px 做视觉居中。不要为了修圆角把高铁紫色边框删掉；圆角突出应通过 renderer 按 `border-radius` 裁切背景，或卡片 `overflow:hidden` 处理。
- 锁屏预览继续复用第一页，但测速入口、测速展开面板和 native gauge/chart 均隐藏；`speedtest_poll()` 保留，避免解锁后测速后端状态滞后。
