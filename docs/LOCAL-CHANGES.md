# 发布与本地修复记录

记录时间：2026-06-25

## v1.1.0 发布状态

- 本次发布 tag：`v1.1.0`
- 前端仓库：`33333s/u60pro-devui`
- 本次为**devui 二进制功能版**：`devui` 升到 `1.1.0`，`ui` 保持 `0.4.0`

## v1.1.0 本次发布内容

这次发布围绕“把外部画面接口正式内建进 DevUI，并保留原生状态栏与系统返回手势”展开，重点如下：

- `src/devui_ext.c`、`include/devui_ext.h`：新增内建 `DEVUI-IPC` 渲染通道，提供本地 Unix Domain Socket `/tmp/u60-devui.sock` 与事件日志 `/tmp/u60-devui-events.log`，支持 `PING`、`CLOSE`、`FRAME`、`IMAGE`、`DRAW`、`TEXT`。
- `src/htmlmain.c`：把外部画面接入主循环，统一处理外部画面前台/超时/锁屏退出/电源键兜底退出；外部画面前台时保留原生状态栏，只接管状态栏下方内容区。
- `src/htmlmain.c`、`src/touch_input.c`、`include/touch_input.h`：新增内容区点击回传和左边缘右滑返回；点击写入事件日志，系统手势直接关闭外部画面，不再落给原生 HTML UI。
- `src/html_view.cpp`：补充按指定尺寸渲染的文字页接口，供 `TEXT` 能直接排版到状态栏下方内容区。
- `CHANGELOG.md`、`README.md`、`docs/DEVELOPMENT.md`、`docs/DEVUI-IPC.md`：同步整理为正式接口文档和发布说明。

## v1.1.0 实机验证结论

已在设备上实测确认：

- 通过 `DEVUI-IPC` 可以正常显示像素帧、图片和可交互页面。
- 外部画面前台时，顶部状态栏继续由 DevUI 原生显示和刷新。
- 内容区点击可正常回传到事件日志。
- 内容区左边缘右滑可正常返回原生 DevUI。
- 返回原生页面后，DevUI 自带界面和原有交互仍然可用。

## v1.1.0 本次发布文件

- `src/devui_ext.c`
- `include/devui_ext.h`
- `src/htmlmain.c`
- `src/html_view.cpp`
- `src/touch_input.c`
- `include/touch_input.h`
- `scripts/_build_htmlpoc.sh`
- `version.json`
- `README.md`
- `CHANGELOG.md`
- `docs/DEVELOPMENT.md`
- `docs/DEVUI-IPC.md`
- `docs/LOCAL-CHANGES.md`

## v1.1.0 Release 资产

- `u60pro-devui-aarch64`
- `ui.tar.gz`
- `version.json`

## v0.4.1 发布状态

- 本次发布 tag：`v0.4.1`
- 前端仓库：`33333s/u60pro-devui`
- 本次为**devui 二进制修复版**：`devui` 升到 `0.4.1`，`ui` 保持 `0.4.0`

## v0.4.1 本次发布内容

这次发布围绕“关机插电仍会进入普通 DevUI，且界面无触控”做修正，重点如下：

- `src/htmlmain.c`：新增 `g_charge_boot` 全屏充电启动分支，关机充电时直接渲染独立的电池充电页，不再进入普通页面流。
- `src/htmlmain.c`：补充 `boot_is_charge_mode()`、`boot_has_external_power()` 等判定；若启动时检测到**有外部供电但触控初始化失败**，则强制切到充电页，覆盖实机上 `mode_power_on` / `silent_boot.mode=nonsilent` 也会出现在关机充电路径里的异常情况。
- `src/htmlmain.c`：重画闪电图标，改成非自交多边形，修复“闪电只显示半个”的填充问题。
- `scripts/start.sh`：不再把离线充电让回原厂 UI，而是改为记录 `boot-trace.log`，按 `mode_main_state` 区分正常开机 / 充电启动；充电启动仅拉起 `u60pro-devui`，不启动 `u60-datad`。
- `CHANGELOG.md`、`README.md`、`docs/DEVELOPMENT.md`：同步更新到当前真实启动策略，并把 `version.json` 示例切到 `devui 0.4.1 / ui 0.4.0`。

## v0.4.1 实机验证结论

已在设备上实测确认：

- 关机状态插电直接进入 DevUI 的全屏充电页。
- 长按电源键不再落入原厂那个无触控的充电界面。
- 新版闪电图标完整显示，不再只剩半个。
- 设备当前运行的 `/data/u60pro/u60pro-devui` 已替换到新 MD5：`c2f76a29f5fa29af1eb76f41ff5b1907`。

## v0.4.1 本次发布文件

- `scripts/start.sh`
- `src/htmlmain.c`
- `version.json`
- `README.md`
- `CHANGELOG.md`
- `docs/DEVELOPMENT.md`
- `docs/LOCAL-CHANGES.md`

## v0.4.1 Release 资产

- `u60pro-devui-aarch64`
- `ui.tar.gz`
- `version.json`

## v0.4.0 发布状态

- 本次发布 tag：`v0.4.0`
- 前端仓库：`33333s/u60pro-devui`
- 后端仓库：`33333s/zwrt-datad` 的 `u60pro` 分支
- 本次开始统一以 GitHub release 为更新源，不再维护仓库内手动网盘同步步骤。

## v0.4.0 本次发布内容

这次发布围绕第一页信号页和 U60Pro 配套后端字段补齐，重点如下：

- `src/htmlmain.c`、`ui/01-signal.html`：ENDC 和 LTE-NSA 改为 **NR 主卡 + LTE 子卡** 上下拆分；NR only / LTE only 保持原布局；第一页内容可继续上下滚动。
- `src/htmlmain.c`：ENDC / LTE-NSA 的总频宽改为 **NR + LTE 相加**；LTE 子卡增加 `X LTE 载波 · Y MHz` 摘要。
- `include/data.h`、`src/data.c`：补充 `nr_band`、`ltecasig` 字段，并在刷新开始时统一清零结构体，减少脏数据残留。
- `src/htmlmain.c`：NR 主卡优先显示真实 `nr_band`，修复把 LTE 频段错显示到 NR 的问题。
- `src/htmlmain.c`：当设备侧 `lteca` 为旧 5 字段格式、LTE 副载波信号单独落在 `ltecasig` 时，自动用 `ltecasig` 补齐 4G SCC 的 `RSRP/SINR`；同时处理 `lteca` 含 `PCell + SCC`、但 `ltecasig` 只给 `SCC` 的组数错位问题。
- `zwrt-datad/src/main.c`：补回 `qos`、`system`、`battery`、`wlan`、`nfc`、`dhcp`、`clients` 等字段，修复此前部署过程里引入的首页/系统页/WiFi 页读空问题；新增透传 `nr_band`、`lteca`、`ltecasig`、`nrca`、`net_select`、`sa_bands`、`nsa_bands`、`lte_bands`。
- `README.md`、`docs/DEVELOPMENT.md`：更新 `version.json` 示例到 `0.4.0`，并把发布说明整理为 GitHub-only 流程。

## 实机验证结论

已在设备上实测确认：

- ENDC / LTE-NSA 模式下第一页可以正常拆分显示 NR 与 LTE。
- 第一页内容超过一屏时可继续上下滚动。
- NR 频段不再误显示成 LTE 频段。
- 4G 副载波 `RSRP` 和最后一个在播 `SINR` 可正常显示。
- 系统页原本能显示的版本号、IMEI、电池电压、CPU 占用已恢复。
- WiFi 页面字段恢复正常读取。

## 本次发布文件

前端仓库需要纳入本次 release 的源码/文档：

- `include/data.h`
- `src/data.c`
- `src/htmlmain.c`
- `ui/01-signal.html`
- `version.json`
- `README.md`
- `CHANGELOG.md`
- `docs/DEVELOPMENT.md`
- `docs/LOCAL-CHANGES.md`

后端仓库需要纳入本次 release 的源码/文档：

- `src/main.c`
- `version.json`

## Release 资产

`u60pro-devui`：

- `u60pro-devui-aarch64`
- `ui.tar.gz`
- `version.json`

`zwrt-datad`：

- `u60-datad-aarch64`
- `version.json`

## 发布后约定

- 管理插件继续读取两个仓库各自的 `latest release / version.json`。
- 如果后续外部网盘需要分发，直接镜像 GitHub release 的同名文件即可，不再单独维护仓库内同步脚本或额外文档步骤。

## v0.4.0 发版后补提交（未重发 release）

- `zwrt-datad/src/main.c`：修复 `AMBR` 读空。实机日志中 `qci` 会比 `apn_ambr_*` 更频繁刷新，单纯读统一尾窗时，`AMBR` 很容易被其它新日志挤出窗口；现改为分别读取**最后一条 `qci`** 和 **最后一条 `apn_ambr_*`** 日志，再缓存最后已知值。实机已验证 `state.json` 恢复输出 `ambr_dl=20008.641`、`ambr_ul=10008.640`。
- `u60pro-devui/docs/DEVELOPMENT.md`：补充 GitHub release 打包约束。`ui.tar.gz` 必须保持与旧版一致的平铺结构，不能混入 `ui/` 目录、`./` 前缀或 macOS `._*` 条目。
- GitHub 上的 `u60pro-devui v0.4.0` release 资产已原位替换为干净的 `ui.tar.gz`，仅修正打包结构，不重新发新 tag / 新 release。
