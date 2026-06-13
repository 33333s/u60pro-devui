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

当前三页：

- **01-signal.html — 信号**：状态栏（时间 / 上下行速率 / 网络制式徽章 / 电池 / 电量）；运营商 + 制式，右上角 QCI 与 AMBR；信号强度阶梯条；按载波分卡片显示 `频段·频宽 / PCI / RSRP / SINR`，RSRP/SINR 按质量上色（绿好 / 黄中 / 红差）。LTE 段仅在存在 LTE 锚点时出现（纯 SA 不显示）。
- **02-wifi.html — WiFi**：SSID、密码（默认打码，点“显示密码”才明文）、加密方式、已连接设备数。只放 WiFi，不再混入连接/网络信息。
- **03-system.html — 系统/设置**：速率与本次流量、PCI、ARFCN（默认隐藏，点“显示”才出）、型号/运行时间/CPU/内存；底部两个开关——**ADB 调试**（读后端 `usb_mode`，拨动即发 ubus 改 USB 模式）、**界面主题**（深色 / 浅色）。

### 模板令牌（`{{TOKEN}}`）

令牌在 `src/htmlmain.c` 的 `build_kv()` 里一次性从 `data_refresh()` 的快照填好，`apply_template()` 做一次性替换（单遍，不递归，所以含子令牌的整段 HTML 必须在 C 里拼好）。

由 C 直接拼成整段 HTML 的“复合令牌”：

- `{{STATUSBAR}}`：整条状态栏（时间 / 上下行速率 / 制式徽章 / 电池图标 / 电量）。三页共用，改一处即可。
- `{{DOTS}}`：底部居中的翻页圆点，当前页高亮。无文字、无边框。
- `{{SIGBARS}}`：5 段阶梯信号条，按 `bars` 点亮、按 RSRP 上色。
- `{{NR_ROWS}}` / `{{LTE_ROWS}}`：每载波一张卡片。NR 主载波取主字段，副载波解析 `nrca`；LTE 同理解析 `lteca`。

标量令牌（节选）：`TIME BAT GEN GENCLASS OPER NETTYPE QCI AMBR SSID KEY KEYBTN ENC CLIENTS RXSPEED TXSPEED RXBYTES TXBYTES PCI ARFCN ARFCNBTN MODEL UPTIME CPU MEM CA_CC CA_BW LTE_SHOW THEME ADBCLASS ADBSTATE THEMECLASS THEMESTATE`。

### 动作（`act:`）

`src/htmlmain.c` 主循环里处理点击命中返回的 `act:` 字符串：

- `poweroff` / `reboot` / `close` / `menu`：电源菜单。
- `theme`：深色 ↔ 浅色。
- `revealkey`：明文 ↔ 打码显示 WiFi 密码（默认打码，安全考虑）。
- `revealarfcn`：显示 ↔ 隐藏小区 ARFCN（默认隐藏）。
- `adb`：读当前 `usb_mode`，`debug`↔`user` 取反后执行 `ubus call zwrt_bsp.usb set '{"mode":...}'`。`debug`=ADB 开，`user`=ADB 关。

### 网络制式徽章逻辑

状态栏电池左侧显示 `5GA / 5G+ / 5G / 4G / LTE / 3G`，由 MCC/MNC 判断运营商 + NR 载波聚合情况：

- 纯 NR SA 且（移动/广电 ≥3CC）或（电信/联通 聚合频宽 ≥200MHz）→ **5GA**
- 纯 NR SA 且聚合频宽 >100MHz → **5G+**
- 其余 NR SA / NSA / NR → **5G**
- 中国大陆 LTE → **4G**；境外 LTE → **LTE**；更低 → **3G**

运营商按 MCC=460 + MNC 区分：移动 0/2/4/7/8，联通 1/6/9，电信 3/5/11，广电 15。

### 电池与充电动画

litehtml 不支持 CSS 动画/JS，所以充电动画由**宿主驱动帧**实现：检测到 `charger_connect` 时，主循环把刷新周期从 1s 缩短到 ~220ms，每帧推进 `g_phase`，状态栏电池内叠一条半透明高光 `.glow`，其 `left:%` 随相位扫过电池 = 充电流光效果（满电也可见）。电池图标用 CSS 画（圆角边框 + 右侧正极小凸点 `.tip` + 绿色填充，低电量转红）。

## 数据模型（`u60-datad` 快照）

后端轮询并归一化的字段段：

- `net`：制式、信号条、运营商、频段、NR/LTE 的 RSRP/RSRQ/SNR/RSSI、MCC/MNC、PCI/Cell ID/ARFCN、`nr_bw`，以及载波聚合描述符 `nrca` / `lteca`（直接透传自 `zte_nwinfo_api`），WAN 状态。
  - **`nrca` 格式**（实测）：`;` 分隔载波，`,` 分隔字段，每个副载波 11 个字段：`idx, PCI, ?, band, arfcn, bw, ?, rsrp, rsrq, sinr, rssi`。例：`0,273,1,41,504990,100,0,-140.0,-43.0,-23.0,-120.0;` = n41 / 100MHz / PCI273 / RSRP-140 / SINR-23。所以解析取 `band=f[3]`、`bw=f[5]`、`pci=f[1]`、`rsrp=f[7]`、`sinr=f[9]`。`lteca` 同结构（band 前缀 `B`）。
  - **没有载波聚合时 `nrca`/`lteca` 是空串**——此时信号页只有主载波一张卡片，属正常，不是 bug。CA 激活后副载波卡片自动出现。
- `battery`：电量、温度、充电状态、`charger_connect` 等（`zwrt_bsp.battery` / `zwrt_bsp.charger`）。
- `clients`：接入设备数（`zwrt_router.api`）。
- `wlan`：主 WiFi 的 `ssid` / `key` / `enc`，读自 `uci wireless.main_2g.{ssid,key,encryption}`。**键名必须是 `wlan` 不能是 `wifi`**——否则消费端按子串找 key 会先命中 `clients.wifi` 计数，导致 SSID/密码读空。
- `traffic`：实时会话上下行速率与字节数（`zwrt_data get_wwandst`，参数须 `cid:1,type:1`，否则 Invalid argument）。
- `qos`：`qci`、`ambr_dl`/`ambr_ul`（Mbps）及原始值/单位，解析自 `/data/logfs/key.log` 的 `[DATA]` 行；外加 `usb_mode`（读 `zwrt_bsp.usb list` 的 `mode`，`debug`/`user`）。
- `system`：运行时间、CPU 温度、内存占用、型号、固件（`system info` / `system board`，board 信息每小时刷新一次）。

这份 JSON 是后端与 UI 之间的接口契约。字段增删改名时，后端 schema 和 UI 的 `data.c` / 令牌逻辑必须一起改。

## 硬件事实

- 显示：`/dev/dri/card0`，320x480，RGB565，命令式刷新（DIRTYFB 推帧，`vrefresh=1`），自动枚举 connector/crtc。
- 旋转：面板安装方向相对 framebuffer 扫描顺序等效旋转 180°；`html_view` 的 `put_px` 把逻辑 (x,y) 映射到物理 (W-1-x, H-1-y)。
- 触摸：`/dev/input/event3`（sitronix_ts，多点 ABS_MT），自动探测。
- 电源键：`/dev/input/event0`（`pmic_pwrkey`），键码 `KEY_POWER`(116)。注意触摸屏也会上报 KEY_POWER，所以探测要求“有 KEY_POWER 且无 EV_ABS”以排除触摸屏。
- 背光：`/sys/class/leds/led:lcd/brightness`（0..255，没有 `/sys/class/backlight`）。息屏=写 0，与原厂 `ZTD_SetLcdBrightnessByFile` 一致。

完整设备接口说明见 [HARDWARE.md](HARDWARE.md)。

### 电源键与息屏

- 短按 = 亮屏/息屏；长按（≥1.2s）= 切换 `menu.html` 电源菜单。
- 息屏时除了写背光 0，还会把 framebuffer memset 成黑再 DIRTYFB 推一帧——否则面板 GRAM 里残留的画面用手电筒照能看到很淡的影。

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

## 仓库约定

- 不提交父目录里的 vendor blobs 或分析产物。
- 项目必须能仅靠公开源码和标准接口构建。
- 新增字体或 UI 资源优先选开源许可证资源；仓库**不打包任何 ZTE 字体**（设备上运行时从 `/usr/ui/fonts/ZTEZhengYuan.ttf` 加载，保持 clean-room）。CJK 字体没有 `↑↓◔` 等符号字形，会显示成豆腐块——状态栏等处用纯文字（如“下/上”）而非箭头符号。
- 对后续开发有帮助但不适合只留在本地记忆里的经验，写进这里或 [HARDWARE.md](HARDWARE.md)。
