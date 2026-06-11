# U60Pro 屏幕 UI 开发说明

这份文档记录项目当前最重要的开发事实，方便后续维护、移植，也便于随仓库版本化后公开分享。目标是让关键经验不只停留在本地记忆里，而是和代码一起长期保存。

## 项目目标

`u60pro-devui` 是 ZTE U60Pro 前面板屏幕 UI 的一个 clean-room 开源替代实现。目标形态是一份单独的静态 `aarch64` 二进制，只依赖标准 Linux/OpenWRT 接口，不链接 ZTE 私有库，也不提交任何专有资源。

当前 UI 默认配套的后端是 `zwrt-datad` 仓库里的 `u60pro` 分支。UI 只读取该分支输出的 `/tmp/zwrt-datad/state.json`，不直接调用 `ubus`。

核心设计原则是把 UI 和后端解耦，这样代码就能被审计、分享，并且可以从源代码重新构建。

## 当前状态

截至 2026-06-12，已在真机上验证：

- 显示输出在 320x480 屏幕上工作正常。
- 触摸输入工作正常，包括 180 度旋转映射。
- UI 已经从早期 demo 页面切换为实时仪表盘。
- 网络、电池、信号、速度、客户端数量、运行时间、CPU 和内存信息都能从后端快照中读取并显示。
- 支持多页面横向滑动：Home 仪表盘、Network 详情、WiFi 分享（SSID/密码/二维码）三页，底部圆点指示当前页。
- 主页信号改为 5 段递增信号条，按强度变色（弱红 / 中橙 / 强绿）。
- 电源键逻辑已可用：短按切换背光（亮屏/息屏），长按打开电源菜单（关机 / 重启 / 取消）。
- 已实现开机自启：二进制安装到持久化目录 `/data/u60pro/`，并在 `/etc/rc.local` 中挂钩启动。
- 经过全屏缓冲区和 flush 路径优化后，渲染已经流畅。

后续页面按设备需求的优先级如下：

- 短信
- 设置
- WiFi 页的多频段 / 访客网络展示（当前只展示主 SSID）

## 架构

仓库里有两个协作的二进制：

- `u60pro-devui`：基于 LVGL 实现的屏幕 UI，运行在 DRM/KMS 和 evdev 之上
- `zwrt-datad` 的 `u60pro` 分支：单进程数据采集器，轮询 ubus 并写出 JSON 快照供 UI 读取

UI 不直接调用 ubus，而是读取 `/tmp/zwrt-datad/state.json`，并以 1 Hz 刷新。这样可以保持显示进程简单，也不会频繁打扰后端服务。

```text
ubus 服务 -> zwrt-datad/u60pro -> /tmp/zwrt-datad/state.json -> u60pro-devui
```

`u60pro-devui` 当前使用：

- LVGL v9.2
- `/dev/dri/card0` 上的 DRM/KMS
- `/dev/input/event*` 上的 evdev 触摸输入
- RGB565 dumb framebuffer，并通过 `DIRTYFB` 更新

## 硬件事实

- 显示：`/dev/dri/card0`，约 320x480，RGB565，命令式刷新风格
- 旋转：面板安装方向相对 framebuffer 扫描顺序等效旋转 180 度
- 触摸：多点触摸 evdev 设备，由 `/dev/input/event*` 自动探测
- 电源键：`/dev/input/event0` 上的 `pmic_pwrkey`，键码 `KEY_POWER`（116）
- 背光：`/sys/class/leds/led:lcd/brightness`

完整设备接口说明请看 [HARDWARE.md](HARDWARE.md)。

## 构建

项目设计目标是不需要 root，也不要求宿主机安装 `make`。当前已验证可用的工具链是 Bootlin 的 `aarch64 musl GCC 14.3.0`，由 `scripts/_setup_toolchain.sh` 下载到本地。

典型构建流程：

```sh
bash scripts/_setup_toolchain.sh
bash scripts/build.sh
```

构建结果是单个静态 `aarch64` ELF 二进制。LVGL 位于 `third_party/lvgl`。

## 部署与运行

真机部署流程是：

1. 先停止 vendor UI，避免抢占面板。
2. 把 `u60pro-devui` 推送到设备上。
3. 以脱离 adb 会话的方式运行，确保断开连接后进程仍在。

已知可用的恢复命令：

```sh
adb shell "killall -9 u60pro-devui; /etc/init.d/zte_topsw_devui start"
```

重要坑位：

- 要完全接管面板时，先执行 `/etc/init.d/zte_topsw_devui stop`。单纯 `killall` 不够，因为 procd 可能会把它重新拉起来。
- 在 `adb push` 之前先杀掉正在运行的 `u60pro-devui`，否则可能出现 `Text file busy`，并且会悄悄保留旧二进制。
- Busybox 不提供 `setsid`，所以后台运行要用 `nohup ... &`，否则 adb 会话结束时进程会收到 `SIGHUP`。
- PowerShell 5.1 传给原生命令的带引号参数容易被弄乱（如 `grep "a|b"` 会按 `|` 拆开），复杂 shell 命令最好写成脚本文件再 `adb push` 后执行。

## 开机自启

`/tmp` 是 tmpfs，重启即清空，所以自启必须把二进制装到持久化分区。设备上 `/data` 是既定的扩展目录（ufi-tools、kano 插件都装在这里），因此安装到 `/data/u60pro/`：

```text
/data/u60pro/u60pro-devui    # UI
/data/u60pro/zwrt-datad      # 数据后端
/data/u60pro/start.sh        # 启动脚本：停原厂 UI，nohup 拉起后端 + UI
```

`/etc/rc.local` 存在且可写（其它插件也在这里挂钩），结尾是 `exit 0`。`scripts/install-autostart.sh` 用 awk 在 `exit 0` 之前**幂等**插入一行：

```sh
[ -x /data/u60pro/start.sh ] && sh /data/u60pro/start.sh >/tmp/u60pro-boot.log 2>&1 &
```

安装步骤：

```sh
adb shell "mkdir -p /data/u60pro"
adb push u60pro-devui.stripped /data/u60pro/u60pro-devui
adb push zwrt-datad.stripped   /data/u60pro/zwrt-datad
adb push scripts/start.sh      /data/u60pro/start.sh
adb push scripts/install-autostart.sh /tmp/ && adb shell sh /tmp/install-autostart.sh
```

`start.sh` 用 `nohup ... &` 启动（busybox 无 setsid）。开机时原厂 UI 可能先起来，rc.local 较晚执行时由 `start.sh` 停掉它再接管，会有短暂切换。

## 数据模型

`zwrt-datad` 的 `u60pro` 分支会轮询以下服务族，并把结果规范化成 JSON 快照：

- `zte_nwinfo_api` 提供的网络和信号信息
- `zwrt_bsp.*` 提供的电池和充电状态
- `zwrt_bsp.thermal` 提供的 CPU 温度
- `zwrt_router.api` 提供的已连接客户端数量
- `zwrt_router.api` 提供的 WAN 状态
- `zwrt_data` 提供的流量统计和速率（`get_wwandst`，参数须为 `cid:1, type:1`，否则返回 Invalid argument）
- `system info` 和 `system board` 提供的运行时间和内存信息
- `uci wireless.main_2g.{ssid,key,encryption}` 提供的主 WiFi 名称/密码（2.4G/5G 共用一个 SSID）

这个 JSON 快照是后端和 UI 之间的接口契约。如果字段新增、删除或改名，后端 schema 和 UI 的读取逻辑必须一起更新。

注意：快照里 WiFi 信息段的键名用 `wlan` 而不是 `wifi`。因为 UI 端的 `json_get` 是子串匹配，`wifi` 会先命中 `clients.wifi`（客户端计数）导致解析错位、SSID/密码读成空。

## UI 结构

当前 UI 以 LVGL `lv_tileview` 横向分页组织，三页可左右滑动切换：

- **Home 页**：仪表盘。运营商 / 制式 / 频段、5 段信号条 + RSRP/SNR/RSSI、电池条 + 温度/充电、上下行速率、客户端数、运行时间/CPU/内存。
- **Network 页**：5G NR 频段/带宽/信道、RSRP/RSRQ/SNR/RSSI、PCI/Cell ID、PLMN、LTE、WAN 状态。
- **WiFi 页**：主 SSID、密码，以及用 `lv_qrcode` 生成的标准 WiFi 二维码（`WIFI:T:WPA;S:..;P:..;;`），扫码即可连。二维码只在凭据变化时重建，避免每秒重绘。

顶层 layer（`lv_layer_top`）用于覆盖层：电源菜单和页码圆点都画在这里。圆点是直接画的 LVGL 对象（`lv_obj` 小圆），不是 label recolor —— LVGL v9 已移除 `lv_label_set_recolor`。获取当前页用 `lv_tileview_get_tile_active()`。

### 电源键与背光

- 真实电源键是 `/dev/input/event0`（`pmic_pwrkey`），键码 `KEY_POWER`(116)。注意触摸屏（event3）也会上报 KEY_POWER，所以探测时要求设备**有 KEY_POWER 且没有 EV_ABS**，以排除触摸屏。
- 背光走 `/sys/class/leds/led:lcd/brightness`（0..255，没有 `/sys/class/backlight`）。
- 交互：**短按 = 亮屏/息屏**；**长按（≥1.2s）= 切换电源菜单**（Power Off→`poweroff` / Reboot→`reboot` / Cancel）。按键用 50ms 的 `lv_timer` 轮询。
- 菜单按钮目前是英文，中文需另加 FreeType + 开源 CJK 字体。

## 性能说明

早期"刷新率很低 / 滑动卡顿"的根因是：LVGL draw buffer 只有 1/8 屏（一次重绘要多次零碎 flush），以及 flush 回调里对整屏做逐像素旋转拷贝。关键优化：

- draw buffer 改成全屏双缓冲：一次整屏重绘（翻页）= 一次 flush + 一次 DIRTYFB，而不是很多小块。
- 旋转拷贝用指针递增重写；非旋转路径直接按行 `memcpy`。
- 主循环 sleep 上限 16ms → 8ms；`lv_conf.h` 的 `LV_DEF_REFR_PERIOD` 33ms → 20ms。

这些改动很重要，因为这块屏更接近命令式显示（DIRTYFB 推帧、`vrefresh=1`）而不是连续扫描屏。对它来说，完整页面刷新应该尽量表现为一次 flush，而不是很多零碎小块更新。

## 仓库约定

- 不提交父目录里的 vendor blobs 或分析产物。
- 项目必须能仅靠公开源码和标准接口构建。
- 如果要新增字体或 UI 资源，优先选择开源许可证资源。
- 如果某条经验对后续开发有帮助，但又不适合只留在本地记忆里，就写进这里或 [HARDWARE.md](HARDWARE.md)。

