# 信号卡片与锁屏预览

这份文档收口 `u60pro-devui` 第一页信号卡片的当前行为。后端抓取、
modem 解码、回放和 raw log 归 `zwrt-datad` 维护；这里只记录 DevUI 如何
消费已经归一化的值并渲染到屏幕。

## 页面入口

- `ui/01-signal.html` 使用 `{{SIGNALCARDS}}` 作为默认第一页的完整信号区。
- `{{NEIGHBORCARDS}}` 放在 `{{SIGNALCARDS}}` 后面，作为可展开的邻小区列表入口。
- `{{CARRIERS}}` 继续保留给旧自定义模板，只展开载波列表。
- 测速入口已经迁移到“更多功能”二级页；首页不再直接放 `{{STHOMEBTN}}`
  或 `{{STHOMEINLINE}}`。

## 载波数据归属

- PCell / SCC 身份仍以 ZTE/datad 归一化状态为准，例如 `net.nr_band`、
  `net.nr_bw`、`net.nrca`、`net.lteca`、`net.ltecasig`。
- 信令解析出来的 modem 指标只作为附加行追加到既有载波卡片下面，不能替换
  原本的 SCC 数据来源。
- NR 附加指标可显示 MIMO、layers、DL/UL grants、DL/UL RB、BLER、SSB、
  ML1 RSRP/RSRQ/SINR。
- LTE 附加指标可显示 MCS/modulation、layers、grants、RB、BLER、ML1
  RSRP/RSRQ/SINR。
- 没有新鲜可信值时显示 `-`；不要在不同载波之间编造或串用旧值。

## 邻小区

- 公开版 datad：邻小区来自 `/state.neighbor`。“读取信令”开启时下发
  `neighbor.set` 打开 DIAG 采集，显示合并后的 `neighbor.cells`（含 NR）；
  未开启时显示厂商扫描的 `neighbor.lte.cells`（仅驻留 LTE / NSA 时有）。
  `null` 的频点、频段、RSRP/RSRQ 显示为 `-`，不按频点猜测。
- 私有版 datad（提供 `/modem/*`）：仍读 `/modem/signal-metrics` 中
  `lte.neighbors` 和 `nr.neighbors` 数组。
- 默认首页只显示一张“邻小区”入口卡，点击后展开列表；再次点击“收起”折叠。
- 每个 LTE/NR 邻区一行，从左到右固定显示 `PCI / 频段 / RSRP / RSRQ`。
- 信令解析开关关闭时，`{{NEIGHBORCARDS}}` 返回空串，入口和展开卡片都隐藏。
- 邻小区仅作为附加观测，不替换 PCell/SCC 的原始数据来源。

## 未激活载波

- RSRP 小于等于 floor sentinel `-140` 的载波视为“已配置但未激活”。
- 未激活卡片仍保留正常的频段、频宽、EARFCN/ARFCN、PCI 展示；只有信号值走
  `.q-off`，并增加 `未激活` 标签。
- `未激活` 是状态提示，不代表“信号极差”。

## 高铁专网标签

- `net.HSR=true` 是 datad 根据信令确认的高铁模式，会让第一页主信号卡片进入
  紫色高铁模式。
- 单载波上的 `高铁专网` 标签只是白名单提示：仅当 `mcc == 460` 且 LTE/NR
  的 EARFCN/ARFCN 命中维护的白名单时显示。
- 单载波 `高铁专网` 标签不是 `net.HSR` 的真值来源，两者不要混用。
- 高铁载波卡片应保留紫色边框和左侧强调线。圆角露出方形背景时，应修渲染器
  的 `border-radius` 裁切或卡片 overflow，不要删掉紫色边框来规避。

## 标签布局

- `.coff` 渲染 `未激活`。
- `.chsr` 渲染 `高铁专网`。
- 两个标签都是紧凑的 `inline-block`，真机字体下文字需要视觉上移约 1px 才
  更接近上下居中。
- 如果以后继续调标签，优先真机截图验证；桌面浏览器的字体盒模型不能代表
  litehtml + 设备 CJK 字体的最终效果。

## 锁屏预览

- 锁屏预览态（`g_lock_state == 1`）复用第一页 `g_pages[0]`，并在屏幕下方画
  原生锁图标。
- 预览态是只读概览：滑动进入 PIN 键盘，普通页面动作不应该暴露为可用控件。
- 预览态隐藏可选测速 UI；测速二级页入口和 native gauge/chart 都不暴露。
- `speedtest_poll()` 保持运行，这样解锁后仍能立即显示测速后端是否存在以及
  正在测速/已完成的状态。

## 真机预览标签

做视觉 QA 时，可以临时把 `01-signal.html` 换成只包含 `.card`、`.ccd`、
`.coff`、`.chsr` 的最小预览页。检查完必须恢复正式模板。

```sh
cp /data/plugins/u60pro-devui/ui/01-signal.html \
   /data/plugins/u60pro-devui/ui/01-signal.html.bak-label-preview
# 在这里推送临时预览版 01-signal.html
touch /tmp/u60-dumpfb
killall -9 u60pro-devui
nohup sh /data/plugins/u60pro-devui/start.sh legacy >/tmp/u60pro-boot.log 2>&1 &

# 检查 framebuffer 后恢复
mv /data/plugins/u60pro-devui/ui/01-signal.html.bak-label-preview \
   /data/plugins/u60pro-devui/ui/01-signal.html
rm -f /tmp/u60-dumpfb /tmp/u60-force-hsr
killall -9 u60pro-devui
nohup sh /data/plugins/u60pro-devui/start.sh legacy >/tmp/u60pro-boot.log 2>&1 &
```

Framebuffer dump、截图和临时二进制都是本地 QA 产物，不能提交。
