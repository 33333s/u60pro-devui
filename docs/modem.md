# Modem / 信令页

这份文档只讲 `u60pro-devui` 里和 modem/信令第二页有关的页面行为。后端抓取、解码、回放和字段来源的主说明在配套 `zwrt-datad` 后端文档里。

## 范围

这里整理的是前端第二页以及相关设置逻辑：

- 信令页显示条件
- 第二页卡片结构
- 第二页各字段的显示语义
- 与后端 `/modem/*` 接口的对接边界

## 后端兼容

`/modem/*`（`signal-metrics`、`latest`、`latest-signals`、`recent`、`control`）只有私有版 datad 提供，公开版 datad（0.10.x Rust）没有这些路由。DevUI 在连上 datad 后探测一次 `GET /modem/signal-metrics`：

- 返回 404：按公开版处理，不再请求任何 `/modem/*`。“读取信令”改为 `neighbor.set` 开关 DIAG 邻区采集，“解析信令”只控制页面显示，刷新频率走 `state.set_interval`。第二页依赖解码的 RRC/NAS、TA、MCS、BLER、Grant/RB、Ports/SSB 等字段公开版不提供，显示为等待态 `-`；载波卡片本身来自 `/state.net`，不受影响。
- 其他状态：按私有版处理，行为与下文一致。

探测结果约 5 分钟复查一次，datad 重连（重启或换版本）时立即重测。

## 页面入口

当前入口在“设置 -> 高级设置”里，相关开关包括：

- `读取信令`
- `解析信令`
- `重新搜网`

显示规则：

- 只有在“解析信令”开启后，第二页才显示
- `SA`：只显示 `NR` 卡片
- `NSA / EN-DC`：同时显示 `NR` 和 `LTE`
- `LTE`：只显示 `LTE`

## 数据来源

前端已经不再自己扮演半个解码器，当前边界是：

- `最近 RRC / NAS`：读后端 `/modem/latest-signals` 一类最后已知控制面结果
- 射频/调度指标：读后端 `/modem/signal-metrics`
- 页面只负责格式化和显示，不再把 raw log 当成长期真源

## 第二页字段口径

### `Ports`

- 只显示真正的 `RRC CSI-RS ports`
- 不拿 `MIMO`、`layers` 或别的 PHY 值冒充
- 后端没有给出可信 ports 时，页面就保持等待态

### `MIMO` / `layers`

- `MIMO` 是当前 PHY 侧观测
- `layers` 是当前层数
- 它们和 `Ports` 分开显示，避免把三种概念混成一个数

### `SSB Index` / `Serving SSB`

- `SSB Index` 偏配置侧
- `Serving SSB` 偏实时 ML1
- 配置里的 `ssb_index` 不能直接代替 `Serving SSB`

### `Grant / RB`

- NR 当前排版为两行：
  - `DL/UL Grant`
  - `DL/UL RB`
- 值按 `DL/UL` 顺序显示，例如 `2/2`
- LTE 仍分成：
  - `Grant`
  - `RB`

### `TA`

- `TA` 和距离粗估属于同一个基站信息视角
- 前端只展示后端给出的 `ta`
- 距离粗估只做界面展示，不把换算逻辑下沉进后端
- LTE / 15 kHz 按每 TA 单位约 `78.12m`；NR 若能从 `cell_key`/ARFCN 或 `nr_band` 判断为 n41/n78/n79 等 30 kHz 场景，则按每 TA 单位约 `39.06m`，否则回退到 15 kHz 口径。

### `Vendor`

- 基站品牌卡只展示后端已经确认或缓存过的结果
- 小区变化时，页面会跟着 `cell_key` 重置 sticky 值，避免串台

## 页面行为约束

### 安全刷新

“重新搜网”现在走的是安全刷新思路：

- 打开信令相关抓取
- 触发邻区/状态刷新
- 不再主动把 modem 切到 offline
- 不再顺手杀掉和重启后端

前端的目标是“尽量刷新信令窗口”，不是“强制重注册一切都重来”。

### Sticky 值

第二页对这类低频字段允许保守 keep-last：

- `vendor`
- `ports`
- `SSB`
- 部分 `NAS`

这样做是为了避免短暂空包把页面一下刷回 `-`。但 keep-last 会受 `cell_key` 约束，换小区后不会继续沿用旧值。

## 当前已收口的实现约定

- 第二页的长文本要优先做 compact，避免 320px 屏幕溢出
- `Ports` 等待态要明确写成“等待 RRC 配置”这类语义化文案，不要裸 `-`
- `CSI-SSB` 候选集不再对用户直接显示，避免把候选集误读成真实 serving 值
- 第二页现在把后端 `/modem/signal-metrics` 当成真源，不再继续扩散一堆页面侧 raw fallback

## 这份文档替代了什么

原先大量第二页/信令页演进记录混在 [DEVELOPMENT.md](DEVELOPMENT.md) 里，导致总开发文档同时承担：

- UI 架构说明
- 页面交互说明
- modem/信令细节
- 实机排障日志

现在拆开后：

- 通用 UI 架构、构建、硬件说明继续留在 `DEVELOPMENT.md`
- 第二页/信令页行为统一收在这份 `modem.md`
- 后端抓取与解码细节统一收在配套 `zwrt-datad` 后端文档里
