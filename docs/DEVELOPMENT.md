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
- 支持多页面横向滑动，包含 Home 和 Network 详情页。
- 电源键逻辑已可用：
  - 短按切换背光
  - 长按打开电源菜单
- 经过全屏缓冲区和 flush 路径优化后，渲染已经流畅。

后续页面按设备需求的优先级如下：

- WiFi 分享，包含 SSID 和二维码
- 短信
- 设置

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
- PowerShell 5.1 传给原生命令的带引号参数容易被弄乱，复杂 shell 命令最好写成脚本文件再执行。

## 数据模型

`zwrt-datad` 的 `u60pro` 分支会轮询以下服务族，并把结果规范化成 JSON 快照：

- `zte_nwinfo_api` 提供的网络和信号信息
- `zwrt_bsp.*` 提供的电池和充电状态
- `zwrt_bsp.thermal` 提供的 CPU 温度
- `zwrt_router.api` 提供的已连接客户端数量
- `zwrt_router.api` 提供的 WAN 状态
- `zwrt_data` 提供的流量统计和速率
- `system info` 和 `system board` 提供的运行时间和内存信息

这个 JSON 快照是后端和 UI 之间的接口契约。如果字段新增、删除或改名，后端 schema 和 UI 的读取逻辑必须一起更新。

## UI 结构

当前 UI 以 tile view 组织：

- Home 页：仪表盘
- Network 页：无线和 WAN 详情

顶层 layer 也用于临时覆盖层，例如电源菜单。页码圆点是直接画在 top layer 上的 LVGL 对象，不是 label recolor。

## 性能说明

让 UI 恢复流畅的关键优化主要有：

- 改成全屏 draw buffer
- 简化旋转后的 flush 路径
- 降低主循环的 sleep 上限
- 把 LVGL 刷新周期设为 0 ms

这些改动很重要，因为这块屏更接近命令式显示而不是连续扫描屏。对它来说，完整页面刷新应该尽量表现为一次 flush，而不是很多零碎小块更新。

## 仓库约定

- 不提交父目录里的 vendor blobs 或分析产物。
- 项目必须能仅靠公开源码和标准接口构建。
- 如果要新增字体或 UI 资源，优先选择开源许可证资源。
- 如果某条经验对后续开发有帮助，但又不适合只留在本地记忆里，就写进这里或 [HARDWARE.md](HARDWARE.md)。

