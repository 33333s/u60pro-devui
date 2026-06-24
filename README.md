# u60pro-devui

这是 ZTE U60Pro 以及类似 SDX 系列 5G MiFi 设备前面板屏幕 UI 的一个 clean-room 开源替代实现。运行在标准 Linux 的 **DRM/KMS** 和 **evdev** 接口之上，目标是：

- 独立于原厂设备 UI 运行
- 界面用 **HTML/CSS** 描述，**不改程序就能换界面**
- 编译成单个静态 `aarch64` 二进制，拷到设备上就能运行

> 这是一个基于公开 Linux/OpenWRT 接口的独立重实现。硬件相关说明见 [docs/HARDWARE.md](docs/HARDWARE.md)。

> 💬 QQ 交流讨论群 **625439164**，欢迎进群讨论。

## 核心理念：程序固定，界面是数据

程序本身不内置任何画面。它在运行时去 `/data/ui` 读取你写的 **HTML/CSS** 并渲染到屏幕。想改界面，**完全不用重新编译**——改 HTML、推到设备、约 1 秒内自动生效。

👉 **想自己写界面，看这份教程：[docs/UI-GUIDE.md](docs/UI-GUIDE.md)**

```text
后端 u60-datad ──▶ /tmp/u60-datad/state.json ──┐
                                                ├─▶ u60pro-devui ──▶ 屏幕（DRM/KMS）
你写的 /data/ui/*.html + style.css ─────────────┘
```

- **渲染**：[litehtml](https://github.com/litehtml/litehtml)（HTML/CSS 排版）+ FreeType（含 CJK 字体）→ 直接画进 RGB565 framebuffer。无浏览器、无 JavaScript、无网络。
- **显示**：[src/drm_disp.c](src/drm_disp.c) 打开 `/dev/dri/card0`，运行时枚举面板/crtc/mode，映射 RGB565 dumb framebuffer，通过 `DIRTYFB` 提交。
- **触摸**：[src/touch_input.c](src/touch_input.c) 自动探测触摸屏并缩放坐标；电源键短按息屏、长按菜单。
- **界面**：`/data/ui` 下每个 `NN-名字.html` 一页，`style.css` 共享样式。HTML 里的 `{{令牌}}` 由程序替换成实时数据，`href="act:xxx"` 触发交互。
- **外部接口**：内建本地 `DEVUI-IPC`，保留原生状态栏；其他进程可直接把内容投到状态栏下方的内容区，并通过点击事件日志驱动自己的交互逻辑。
- **后端**：配套 `u60-datad`（[github.com/33333s/zwrt-datad](https://github.com/33333s/zwrt-datad)），轮询 `ubus` 并写出 `/tmp/u60-datad/state.json`，UI 只读快照，自己从不碰 ubus。

自带的示例界面（[ui/](ui/)）含六页：信号、短信、WiFi、选网/锁频、图表、系统设置。系统页包含亮度/息屏/锁屏、USB-C 供电方向、USB 网络共享、速率单位和主题等开关。

## 构建

需要一个 POSIX shell（WSL / Linux）和自带的 aarch64 musl 工具链，不需要 root，也不要求宿主机预装 `make`。

```sh
# 一次性：下载可搬运的 aarch64 musl 工具链到 $HOME
bash scripts/_setup_toolchain.sh

# 一次性：编译静态 FreeType 和 litehtml
bash scripts/_build_freetype.sh
bash scripts/_build_litehtml.sh

# 编译 UI 二进制 -> ./html-poc(.stripped)
bash scripts/_build_htmlpoc.sh
```

产物是单个**静态 AArch64 ELF**，无动态依赖，直接拷到设备运行。

> 也可以直接到 [Releases](https://github.com/33333s/u60pro-devui/releases) 下载编译好的二进制。

## 在设备上运行

```sh
# 推送界面文件
adb push ui/*.html ui/*.css /data/ui/

# 推送并运行（先停原厂 UI 释放面板）
adb push html-poc.stripped /data/u60pro/u60pro-devui
adb shell '/etc/init.d/zte_topsw_devui stop; sleep 1;
           chmod 755 /data/u60pro/u60pro-devui;
           nohup /data/u60pro/u60pro-devui >/tmp/devui.log 2>&1 &'
```

> Windows 下用 Git-Bash 跑 `adb push /data/...` 可能因路径翻译卡住，建议用 PowerShell 跑 adb。

开机自启：把二进制和后端放到持久化的 `/data/u60pro/`，用 `scripts/install-autostart.sh` 在 `/etc/rc.local` 挂钩 `start.sh`。详见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 版本清单与更新

每个 release 附一个 `version.json`，声明各组件版本，供配套的「U60 DevUI 管理插件」检测更新。组件分三块、可各自独立升级：**后端 datad**、**渲染器 devui**（二进制）、**ui**（界面）。

- 本仓库 release 的 `version.json` 含 `devui` 与 `ui` 两项；后端 [zwrt-datad](https://github.com/33333s/zwrt-datad) 的 release 里有它自己的 `version.json`（只含 `datad`）。

```jsonc
// 本仓库 version.json
{ "schema": 1,
  "devui": { "version": "0.4.1", "asset": "u60pro-devui-aarch64" },
  "ui":    { "version": "0.4.0", "asset": "ui.tar.gz" } }
```

插件读各项目 **latest release** 的 `version.json`，与本地记录比对，支持**单独更新** datad / devui / ui 或一键更新全部。默认更新源就是 GitHub release；如果你自己在外部做镜像，只要保持 `u60pro-devui-aarch64` / `ui.tar.gz` / `version.json` 这些文件名不变即可，本仓库不再维护单独的网盘同步流程。

**发版**：改 `version.json` 里对应组件的版本号（ui 改动只升 `ui`，二进制改动只升 `devui`）→ 把 `version.json`、二进制、`ui.tar.gz` 一起传到 GitHub release。

## 文档

- [docs/UI-GUIDE.md](docs/UI-GUIDE.md) — **自定义界面教程**（令牌、动作、限制、示例）
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) — 架构、构建、数据模型、踩坑记录
- [docs/DEVUI-IPC.md](docs/DEVUI-IPC.md) — DevUI 内建外部画面接口（像素帧、图片、绘图命令、文字）
- [docs/HARDWARE.md](docs/HARDWARE.md) — 设备硬件接口
- [CHANGELOG.md](CHANGELOG.md) — 更新日志

## 许可证

本项目采用 [MIT](LICENSE) 许可证。litehtml（BSD）、FreeType（FTL/GPL 双授权）、stb（public domain）按各自许可证引入。

只应添加开源许可证的字体和资源，**不要加入 vendor blobs**；仓库不打包任何 ZTE 字体（运行时从设备加载），`.gitignore` 已拦截常见分析产物。
