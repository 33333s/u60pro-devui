# v0.3.8 发布变更记录

记录时间：2026-06-19

## 发布前 GitHub 对账

- 当前本地分支：`main`
- 本地 HEAD：`6c24d18 Revert "Release v0.3.7 USB mode and band lock fixes"`
- 远端 `origin/main`：同为 `6c24d18`
- `HEAD...origin/main`：`0 0`，说明本地提交历史没有领先或落后 GitHub。
- 发布前差异全部来自本地工作区改动，计划直接发布为 `v0.3.8`，跳过已回滚的 `v0.3.7`。

## 发布内容文件

本次计划纳入 `v0.3.8` 的源码/文档改动：

- `src/htmlmain.c`
- `src/html_view.cpp`
- `ui/06-system.html`
- `ui/style.css`
- `version.json`
- `CHANGELOG.md`
- `.gitignore`
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
