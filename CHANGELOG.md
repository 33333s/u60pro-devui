# 更新日志

## v0.3.1 - 2026-06-14

### 新增

- **第五页系统卡**新增充电器/电池电压电流（充电器一行、电池一行，各两列，位于内存上方）。
- **第四页新增电池图表**：充电功率折线 + 电池温度折线，右上角显示温度与功率数值；**未充电时显示放电功率**（电池 V×I）。
- 后端 `u60-datad` 新增 `battery.chg_uv/chg_ua/bat_uv/bat_ua`（充电器/电池电压µV、电流µA，读自 `power_supply` sysfs）。

### 调整

- **自动息屏**改为与选网方式一致的**分段控件**（关/30秒/1/2/5/10分，蓝色高亮跟手滑动 + 单击）。
- 分段控件单元格显式等宽（litehtml 不自动平分 `table-layout:fixed`）。
- 删除可滚动页右侧的滚动条指示（滚动功能保留）。

## v0.3.0 - 2026-06-14

### 新增

- **新增图表页**：CPU 占用率+温度同图双色折线、内存占用率折线、网速上下行双色折线（单位跟随设置）。折线用 Bresenham 原生绘制到占位框（litehtml 无 canvas），UI 端保留 48 秒滚动历史、每秒采样。
- **新增锁频页**（第 3 页）：
  - 顶部「选网方式」**分段控件**（自动 / 5G SA / 5G NSA / 4G），蓝色高亮原生绘制、跟手滑动、松手吸附并应用；单击亦可。值映射：自动=`WL_AND_5G`、5G SA=`Only_5G`、5G NSA=`LTE_AND_5G`、4G=`Only_LTE`。
  - **5G SA / 5G NSA / 4G 三张锁频卡**，卡片底部显示当前锁定频段；点开弹出**二级弹窗**（约 90% 屏幕、背景变暗仍可见、状态栏留空、禁操作一级菜单）。弹窗内频段一行四个居中排列，底部「全选/全不选 · 反选 · 应用」；应用后弹出居中**提示**并回到一级菜单。
  - 底部「恢复默认配置」一键解锁并恢复默认（`nwinfo_reset_band_cell_setting`）。
  - 底层用厂商 API：`nwinfo_set_netselect`、`nwinfo_set_nrbandlock`(type 0=SA/1=NSA)、`nwinfo_set_lte_ext_band`（逗号频段列表）。
- **WiFi 页**新增：WiFi 开关、NFC 碰一碰开关、已连接设备列表（名称/IP，来自 DHCP 租约）、DHCP 信息（网关/地址池/租期）。
- **系统页**：删除小区 PCI；CPU/内存显示占用率（内存附 `已用/总 MB`）、CPU 温度、电池温度；系统版本号、IMEI（默认打码）；**屏幕亮度滑条**（点/拖直接调背光）、**自动息屏**预设（关/30秒/1/2/5/10分，当前项高亮）、**双击点亮**开关、ADB / 速率单位 / 界面主题开关。
- **长页面竖向滚动**：内容超一屏时可上下拖动（预渲染整页到缓冲再窗口贴图，跟手流畅），右侧有滚动条。
- **自动息屏 + 双击唤醒**：无操作超时自动关背光，双击屏幕唤醒（可开关）。
- 渲染器新增原语：原生折线、占位框定位（`get_placement`）、整页高度（修正 `render()` 返回宽度的坑）、覆盖层渲染（modal）、矩形/滚动条绘制、竖向滚动偏移。
- 后端 `u60-datad` 新增字段：`system.cpu_usage`/`mem_total`/`mem_avail`/`sw_version`/`imei`、`clients.list`、`wlan.enabled`、`nfc.switch`、`dhcp.*`、`net.net_select`/`sa_bands`/`nsa_bands`/`lte_bands`；qci/ambr 日志窗口扩大并缓存最后已知值。

### 调整

- **信号页载波显示按组网模式重写**：表头随 `5G SA / 5G NSA / EN-DC / 4G` 变化，显示「L LTE + M NR 载波 · 总 X MHz」；4G/NSA 下 LTE 与 NR 载波都显示，LTE 频段/EARFCN/PCI 解析自 `lteca`。频宽规则：SA=NR、4G=LTE、NSA=有 NR 取 NR 否则 LTE（不相加）、EN-DC=LTE+NR 相加。
- 充电流光动画裁剪到电池框内（`overflow:hidden`），不再越过边框。
- ARFCN/EARFCN 移到信号页每张载波卡右上角（PCI 正上方）；系统页 ARFCN 行改为 Cell ID（默认打码 + 点击显示）。
- 运营商对中国大陆四家显示中文（中国移动 / 联通 / 电信 / 广电）。
- 页面从 3 页增加到 5 页：信号 / WiFi / 锁频 / 图表 / 系统（+电源菜单）。

### 修复

- 选网分段控件松手/单击后高亮回弹（改用乐观 pending，高亮立即停在所选模式直至模组切换完成）。
- 纯 SA 误显示 LTE 载波；NSA 锁频可用频段被启动瞬时子集带偏（改为取历来最大集合，已选实时跟随模组）。
- 系统页 CPU 占用率/版本号/IMEI 读取错误（`data.c` 漏解析）。
- WiFi 页内容超出被裁、第四页底部开关被翻页圆点遮挡（加底部留白）。

## v0.2.0 - 2026-06-13

这一版是一次**架构级改动**：界面从内置的 LVGL 代码改为**运行时渲染 HTML/CSS**。程序变成固定的"框架"，真正的界面是 `/data/ui` 目录里的 HTML 文件，用户改界面不用再重新编译。详见新增的 [docs/UI-GUIDE.md](docs/UI-GUIDE.md)。

### 架构

- **改用 litehtml + FreeType 渲染 HTML/CSS** 到 RGB565 framebuffer，全静态链接 aarch64 musl 单文件。
- 界面与程序解耦：`/data/ui/NN-名字.html` 每个文件一页，`style.css` 共享样式，`menu.html` 为电源菜单。
- HTML 里的 `{{令牌}}` 在渲染前由程序替换为实时数据；`href="act:xxx"` 为交互动作。每次刷新重读文件，改完即时生效。
- 后端从 `zwrt-datad` 切换为 `u60-datad`，快照路径统一为 `/tmp/u60-datad/state.json`。

### 新增

- **信号页**：按载波分卡片显示 频段·频宽 / PCI / RSRP / SINR，按质量上色；未激活副载波（RSRP 为 -140 哨兵值）置灰并标"未激活"。
- **状态栏**：时间 · 实时网速(`↑上 ↓下`，单位可切) · 网络代际文字(5GA/5G+/5G/4G/LTE/3G) · 格数信号强度 · 电池(带充电流光) · 电量。
- **WiFi 页**：SSID、密码（默认打码，点按才明文）、加密方式、设备数。
- **系统/设置页**：速率流量、PCI、ARFCN（默认隐藏）、型号/运行/温度/内存，以及 ADB 调试 / 速率单位 / 界面主题三个开关。
- **网络代际徽章**：按 MCC/MNC + 载波聚合判定 5GA / 5G+ / 5G / 4G / LTE / 3G。
- **运营商中文名**：中国移动 / 联通 / 电信 / 广电。
- **QCI / AMBR**：从后端 QoS 读取并显示（AMBR 统一 Mbps）。
- **跟手翻页动画**、**充电流光动画**（宿主驱动帧，绕过 litehtml 无 CSS 动画的限制）。
- **底部圆点**翻页指示。
- 文档：新增《自定义界面教程》[docs/UI-GUIDE.md](docs/UI-GUIDE.md)；DEVELOPMENT.md 全面重写为 HTML 架构。

### 修复

- 纯 NR-SA 下误显示 LTE 载波（modem 残留 lte_rsrp）。
- 后端补回/新增 `nrca`/`lteca`/`usb_mode`/`wlan` 字段（缺 `wlan` 会导致 WiFi 页读空）。
- 状态栏制式徽章背景色因裸 class 名泄漏到纯文字 span（紫色框），改为带 `.gen` 前缀。
- 状态栏文字与 inline-block 元素对齐错位；电池正极脱节。

### 安全

- WiFi 密码、ARFCN 默认打码，需显式点按才明文显示。

## Unreleased（LVGL 时期）

### 新增

- 新增 WiFi 页面，可显示主 WiFi 的 SSID、密码、加密方式，并生成可扫码加入的二维码。
- 新增开机自启辅助脚本，便于把 `u60pro-devui` 和后端一起放到设备启动流程里。
- 文档中补充了默认后端依赖关系，明确 `u60pro-devui` 默认配套 `zwrt-datad` 仓库的 `u60pro` 分支。

### 调整

- 后端数据读取路径改为 `/tmp/zwrt-datad/state.json`，与 `zwrt-datad` 的 `u60pro` 分支保持一致。
- 信号区改成分段式指示样式，页面圆点也从两页扩展为三页。
- `data.h` 扩展了 WiFi 相关字段，便于 UI 直接消费后端快照。
- `README.md` 和 `docs/DEVELOPMENT.md` 补充了后端分支引用说明。

### 修复

- UI 不再默认依赖旧的 `u60-datad` 路径，避免和当前后端分支不一致。
- 将 UI 和后端关系写入文档，减少后续维护时的歧义。

## 2026-06-12

### 初始导入

- 建立 clean-room 的 U60Pro 前面板 UI 项目。
- 接入 LVGL + DRM/KMS + evdev 的基础显示、触摸和页面框架。
- 增加与 `zwrt-datad` 后端配套的快照读取模型。

