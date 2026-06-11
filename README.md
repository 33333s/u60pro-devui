# u60pro-devui

这是 ZTE U60Pro 以及类似 SDX 系列 5G MiFi 设备前面板屏幕 UI 的一个 clean-room 开源替代实现。项目基于 **LVGL**，运行在标准 Linux 的 **DRM/KMS** 和 **evdev** 接口之上，目标是：

- 独立于原厂设备 UI 运行
- 提供可显示、可交互、可扩展的界面
- 最终编译成单个静态 `aarch64` 二进制，拷到设备上就能运行

> 这是一个基于公开 Linux/OpenWRT 接口的独立重实现，不隶属于 ZTE，也不受其认可。硬件相关说明见 [docs/HARDWARE.md](docs/HARDWARE.md)。

> 当前 UI 默认配套的后端是 `zwrt-datad` 仓库里的 `u60pro` 分支；UI 只读取该分支产出的 `/tmp/zwrt-datad/state.json`，不直接轮询 `ubus`。

## 架构

```text
            +-----------------------------+
   LVGL --> |  ui.c        （你的界面）   |
   (MIT)    +-----------------------------+
                 |                |
        flush_cb |                | read_cb
                 v                v
   src/drm_disp.c (DRM/KMS)   src/touch_input.c (evdev)
        |  /dev/dri/card0           |  /dev/input/event*
        v                           v
   RGB565 dumb buffer + DIRTYFB   ABS_MT 触摸，自动缩放
```

- **显示**：[src/drm_disp.c](src/drm_disp.c) 打开 `/dev/dri/card0`，运行时枚举面板、crtc 和 mode，映射 RGB565 dumb framebuffer，并通过 `DIRTYFB` 提交更新。
- **触摸**：[src/touch_input.c](src/touch_input.c) 自动探测 `/dev/input/event*` 中的触摸屏设备，并把原始坐标缩放到屏幕坐标。
- **GUI**：[src/ui.c](src/ui.c) 中放置 LVGL 组件。仓库内置的 demo 页面展示了一个实时运行时间计数器和一个点击计数按钮。
- **后端**：默认配套 `zwrt-datad` 仓库的 `u60pro` 分支，它负责轮询 `ubus` 并写出 `/tmp/zwrt-datad/state.json`，UI 只负责读取快照。
- 设备相关的默认值和 fallback 都放在 [include/devui_config.h](include/devui_config.h)。

## 构建

构建需要一个 POSIX shell（WSL / Linux）和自带的 aarch64 musl 工具链。仓库提供了两个辅助脚本，因此不需要 root，也不要求宿主机预装 `make`。

```sh
# 一次性：拉取 LVGL v9
git clone --depth 1 -b release/v9.2 https://github.com/lvgl/lvgl.git third_party/lvgl

# 一次性：把可搬运的 aarch64 musl 工具链下载安装到 $HOME
bash scripts/_setup_toolchain.sh

# 编译 -> ./u60pro-devui（以及 stripped 版本）
bash scripts/build.sh
```

验证结果是一个单独的 **静态 AArch64 ELF**，大小约 606K（strip 后约 438K），没有 `NEEDED` 依赖，也没有解释器，直接拷到设备上运行即可。

> 如果你的环境里已经有 `make` 和交叉编译器，也可以使用 `Makefile`：`make CROSS_COMPILE=aarch64-linux-`。

## 在设备上运行

```powershell
# 停止 vendor UI，运行本项目，Ctrl-C 后恢复 vendor UI
pwsh scripts/deploy.ps1

# 或者让本项目持续运行，不自动恢复
pwsh scripts/deploy.ps1 -Persist
```

如果你想手动启动，前提是 `adb` 已经能连上设备，并且后端 `zwrt-datad` 的 `u60pro` 分支已经在设备上运行：

```sh
adb push u60pro-devui /tmp/ && adb shell '
  killall -9 zte_topsw_devui 2>/dev/null; sleep 1
  chmod 755 /tmp/u60pro-devui && /tmp/u60pro-devui'
```

## 配置与移植

可以在 [include/devui_config.h](include/devui_config.h) 中调整，也可以通过 `-D` 编译参数覆盖以下内容：

- 面板旋转
- 触摸轴交换 / 翻转
- DRM 节点
- fallback 的 connector / crtc ID

## 路线图

- [ ] ubus/uci 数据层，提供真实状态（信号、WiFi、客户端、电池、短信）
- [ ] 把 demo 页面替换成可导航菜单和设置控件
- [ ] 通过 FreeType + 开源字体支持中英文显示
- [ ] 在设备上做触摸校准工具

## 许可证

本项目采用 [MIT](LICENSE) 许可证。LVGL 以 MIT 方式作为子模块引入。

只应添加开源许可证的字体和资源，不要加入 vendor blobs；`.gitignore` 已经拦截了常见的分析产物。

