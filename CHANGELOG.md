# 更新日志

## Unreleased

### 调整

- 充电流光动画裁剪到电池框内（电池框加 `overflow:hidden`），不再越过边框跑到正极图标外。
- ARFCN/EARFCN 移到信号页每张载波卡的右上角，位于 PCI 正上方。
- 第三页原 ARFCN 行改为显示 Cell ID（`nr5g_cell_id`），保留默认打码 + 点击显示/隐藏。

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

