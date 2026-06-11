# 更新日志

## Unreleased

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

