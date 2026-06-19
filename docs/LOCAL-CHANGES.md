# 发布与本地修复记录

记录时间：2026-06-19

## v0.3.9 发布状态

- 当前本地分支：`main`
- 基于上一版：`v0.3.8`
- 本次发布 tag：`v0.3.9`
- GitHub release：`https://github.com/33333s/u60pro-devui/releases/tag/v0.3.9`
- 本次同时需要后端 `zwrt-datad v0.3.3`，用于配套短信列表 32 条上限。

## v0.3.9 本次发布内容

这些改动已在实机验证，并纳入 `v0.3.9`：

- `src/htmlmain.c`：`menu.html` / `lockscreen.html` 强制 `scroll=0` 渲染，修复设置页滑到底后长按电源，电源菜单被当前页面滚动偏移卷走的问题。
- `scripts/start.sh`：自启前检查 `/proc/cmdline`，只有 `silent_boot.mode=nonsilent` 的正常开机才接管屏幕；关机插电的离线充电启动保持原厂充电动画。
- `src/htmlmain.c`、`src/data.c`、`include/data.h`、`ui/style.css`：未读短信信封改为固定绘制在状态栏时间右侧，不再跟随页面滚动；短信列表读取/渲染上限扩大到 32 条，短信页可通过竖向滚动查看更多内容。
- `src/htmlmain.c`：优化滑动跟手性，屏幕亮着时缩短主循环等待时间，降低拖动起始阈值；竖向滚动时只重绘状态栏下方内容区，固定状态栏不再参与每帧重画。
- `src/backlight.c`、`include/backlight.h`：亮屏淡入动画时长缩短约一半，息屏淡出保持原节奏。
- `D:\devices\U60Pro\system\screen\u60-datad`：后端 `zte_libwms_get_sms_data` 从 6 条扩大到 32 条，并扩大状态 JSON / 短信缓存，配合屏幕端长短信列表。
- `docs/DEVELOPMENT.md`、`docs/UI-GUIDE.md`、`CHANGELOG.md`、`docs/LOCAL-CHANGES.md`：补充上述修复、实测结果和后续发布注意事项。
- 仓库外的管理插件 `D:\devices\U60Pro\ufi_plugins\u60屏幕管理插件.txt` 也已同步修改：写入 `/etc/rc.local` 的内联 `# u60pro_devui` 自启行现在带同样的 `silent_boot.mode=nonsilent` guard；若检测到旧自启行，会替换为新行。
- 同一个管理插件的 UI 更新逻辑已加固：下载 `ui.tar.gz` 后先在临时目录解压并确认新页面数量，再清空 `/data/ui` 顶层旧 `*.html` / `*.css`，复制完成后核对数量，防止用户点击更新后旧页面和新页面并存导致页数翻倍；插件界面新增「重装UI」按钮，远端版本没变时也能手动重铺模板。

已验证：

- 设置页滑到底后打开电源菜单，菜单固定在正常位置。
- 关机状态插电进入原厂充电画面。
- 正常开机进入 DevUI。
- 管理插件文本已通过本地 JavaScript 语法解析检查。
- 流畅性优化版已部署到实机；`u60pro-devui` 重新启动后成功打开 DRM、触摸和电源键输入，帧缓冲健康检查不是黑屏。
- 亮屏淡入减半版已部署到实机；`u60pro-devui` 重新启动正常。

## 发布内容文件

已纳入 `v0.3.9` 的源码/文档改动：

- `src/htmlmain.c`
- `src/backlight.c`
- `src/data.c`
- `include/backlight.h`
- `include/data.h`
- `scripts/start.sh`
- `ui/style.css`
- `version.json`
- `README.md`
- `CHANGELOG.md`
- `docs/DEVELOPMENT.md`
- `docs/UI-GUIDE.md`
- `docs/LOCAL-CHANGES.md`

已清理的本地调试产物：

- `u60-fb.dump`
- `u60-screen-op.bmp`
- `u60-screen-op.dump`
- `u60-screen.bmp`

这些截图/帧缓冲转储只用于本地调试，已从工作区删除。

## 功能变化概览

### USB-C 供电方向与网络共享

- 设置页移除了 ADB 开关，新增 USB 供电方向、USB 网络共享两个开关。
- USB 网络开启默认走系统 `9057` composition，也就是 `RNDIS : ECM`。
- 电脑场景验证为 `idProduct=0x9057`、`usbnet_type=rndis`、`rndis0 carrier=1`。
- 手机场景会在 `9057` 无 carrier 时降到 `90B1` 纯 ECM。
- 反向供电 + 手机 USB 共享的稳定顺序是：
  - 先确保 Type-C 是 `device/sink`
  - 必要时把 `/sys/bus/platform/devices/a600000.ssusb/mode` 拉回 `peripheral`
  - 先用 `90B1` 等到 `ecm0 carrier=1`
  - 再切 `DR_Swap=device + PR_Swap=source`
  - 再开 `powerbank state=1`
  - 最后补一次 `DR_Swap=device` 和 `90B1`
- 已验证成功状态：
  - `power_role=source`
  - `data_role=device`
  - `cc_attch_state=1`
  - `otg_powerbank_state=1`
  - `android_usb=CONFIGURED`
  - `ecm0 carrier=1`

### RNDIS/ECM 自适应修正

- 修复从手机切回电脑时粘在 `90B1/ECM` 的问题。
- 如果残留 `/tmp/u60-typec-source`，watchdog 会先尝试手机 ECM 路径。
- 只有 `idProduct=0x90B1/0x90b1` 且 `ecm0 carrier=1` 才允许继续进入反向供电。
- 如果 `90B1` 没有 `ecm0 carrier`，会自动清掉 source 标记并回到 `9057/RNDIS`。
- 已复现验证：人为放回 source 标记后，电脑最终自动恢复为 `0x9057 + rndis0 carrier=1`。

### ADB 路径分析

仅分析，未继续改动 ADB 逻辑。

- 系统 Qualcomm `9059` 是 `DIAG+ADB+RNDIS : ECM`：
  - config 1：`gsi.rndis + ffs.diag + ffs.adb`
  - config 2：`gsi.ecm`
- ZTE legacy `/sbin/usb/compositions/usb_switch` 路径在原厂日志中出现过：
  - `0x19d2:0x1225 mass_storage`
  - `0x19d2:0x1403 rndis_gsi`
  - `0x19d2:0x1404 rndis_gsi,diag,serial,modem,mass_storage,ffs,dpl,qdss`
- 后续如果要恢复或重做 ADB 开关，应优先沿用 `9059` 或 ZTE legacy `1404` 路径，不要手搓新的 ADB+ECM/RNDIS config。

### 状态栏 UI

- 状态栏改为更多原生绘制：
  - 时间靠左
  - 网速靠近信号图标
  - 信号条使用曲线/统一高度布局
  - 5G/5GA/5G+ 等制式文本嵌入信号条上方空间
  - 电池框内显示百分比
- 新增“状态栏电量百分比”设置，默认开启并持久化。
- 页面纵向滚动和横向切页时状态栏保持常驻，不再跟内容一起消失。
- 增加 `/tmp/u60-dumpfb` 帧缓冲转储辅助，用于实机截图调试。

### 渲染辅助

- `src/html_view.cpp` 增加文本宽度、文本边界、原生文字绘制和高对比文字绘制接口。
- `src/htmlmain.c` 使用这些接口绘制状态栏文字、电池百分比等元素。

## 已做验证

- 本地编译通过：
  - `bash scripts/_build_htmlpoc.sh`
  - 输出 `HTMLPOC-OK`
- 已实机部署到 U60。
- 电脑 USB 网络共享验证：
  - `idProduct=0x9057`
  - `usbnet_type=rndis`
  - `rndis0 carrier=1`
- 手机反向供电 + USB ECM 共享验证：
  - `power_role=source`
  - `data_role=device`
  - `otg_powerbank_state=1`
  - `ecm0 carrier=1`

## 提交前建议

- 不要提交 `u60-*.bmp`、`u60-*.dump` 调试产物。
- 提交前再跑一次编译。
- 最好再分别插电脑和手机做一次冷插拔验证：
  - 电脑应自动到 `9057/RNDIS`
  - 手机应能在需要时到 `90B1/ECM`
  - 反向供电开启时应保持 `source/device + ecm0 carrier=1`
- ADB 逻辑目前只是分析记录，建议单独拆成后续改动，不和这次 USB 网络/状态栏改动混在一个提交里。
