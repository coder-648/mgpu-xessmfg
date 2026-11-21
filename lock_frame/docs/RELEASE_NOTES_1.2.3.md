# lock_frame 1.2.3 发布说明（本地版）

**1.2.3 的核心是传输层**：MV/depth 现在默认走无损码流（协议 **v30**），
在硬件或格式不支持时自动退回 1.2.2 的「跨卡直接拷贝 + 升降采样」路径。
插帧行为、UI、捕获方式与 1.2.2 相同。

## 新增：无损码流传输（MV + depth）

- 渲染卡上一次 compute pass 把 MV/depth 编码进跨卡槽（tile 32×8，每 tile 最多 264 words），
  消费卡上解码回**逐位一致**的原数据；两条流各自 1:1，不再统一降到某个 StreamPercent。
- 协议 **v29 → v30**：`StreamShared` 增加 `codec` 字段。`fakerFG.dll` 与 `lock_frame.exe`
  必须同版本（混合版本会直接拒绝启动传输，不会静默错数据）。
- 开关：`fakerFG.ini` 的 `MvCodec` / `DepthCodec`
  - `auto`（默认）：能跑就用，跑不了自动退回老路径（探测 PSO/缓冲/格式，日志有
    `fr: codec capability: ...` 一行）
  - `1` 强制开、`0` 强制关（等价 1.2.2）
  - 两条流互相独立，`MvCodec=auto + DepthCodec=0` 这类混合档合法

## 实测（真机，三个环境）

| | 结果 |
|---|---|
| CP2077 4K/3x，MV+depth 双 codec | 103/107 报告窗口 `presents=120/120`，`load 68%` |
| PBZ 40x3，含 wide depth（fmt19→R32 转换） | 67/69 窗口 120/120，`gpu 8.6–9.9ms`，`load 64–69%` |
| Wuchang 40x3，only-depth（MV 走光流） | depth 解码 1:1 成功，`load 20–67%` |
| 逐位一致 | `--mv-verify` 三次采样 **BIT-EXACT**，34,214,400 B 与生产端槽逐字节相同 |
| 真实压缩 | Wuchang depth **7.0–7.6 MB/帧（raw 14.06，1.86–2.21x）** |

单次成本（4K 单流）：NV 编码 1.02 ms、NV 解码 0.58–0.72 ms；AMD 编码 1.38–1.41 ms、
AMD 解码 1.98–2.08 ms。总线占用 2–6%，这条链路买的是画质不是带宽。

## 兼容性

- **N/A 混搭**（NV 渲染 + AMD 消费）是主目标；反向混搭成本更低。
- **游戏不提供 MV**：只用 depth codec，MV 走光流或 dummy（消费端自动判定，日志有
  `codec MV slot exists but the game publishes no MV`）。
- **MV/depth 源尺寸不同**（不同 DLSS 档位常见）：codec 保持各自 1:1，消费端发现尺寸不一致时
  自动打开联合重建 pass 把两者归一，再 tag（XeFG 要求同尺寸）。
- **预算看门狗**：codec 开着且 `load` 连续 3 个窗口 ≥85% 会打 WARNING，建议改回 0。

## 其它

- `lock_frame.exe` 现在也内嵌 Encode 核（22 KB），供 `--mv-verify` 做往返验证。
- `--d3d-debug` / `D3dDebug=1`：打开 D3D12 校验层并把这台设备的校验消息写进日志（诊断用，
  会显著变慢）。
- 回退：把 `MvCodec` / `DepthCodec` 改成 `0` 重启游戏即可，无需换回旧 exe（协议仍是 v30，
  老路径在同一份二进制里）。
