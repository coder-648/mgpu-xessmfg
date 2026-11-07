# 历史探针（不参与正式构建）

这些程序是各里程碑用来**验证某一层是否可行**的最小样例，保留它们是因为
`docs/findings/` 里每条结论都指向"当时用哪个程序测的"。**它们不进任何发布物。**

| 目录 | 当时在验证什么 | 对应里程碑 |
|---|---|---|
| `m0_probe/` | 同一块卡能否把跨适配器堆 `OpenExistingHeapFromFileMapping` | M0 |
| `m0b_transport/` | 两块卡之间的 fence 能不能等到 | M0 |
| `m0c_payload/` | 真的往跨卡堆里搬一张纹理 | M0.3 |
| `m1_transport/` | 命名映射 + 循环 fence 的完整搬运 | M1 |
| `wgc_probe/` | Windows.Graphics.Capture 抓帧的格式/时序/帧池行为 | M4 |
| `bridge_probe/` | 消费端空转（只拿帧、不插帧）的基线 | M4 |
| `../src/m2_probe/` | ReShade add-on 里读 NGX 参数（MV/深度/jitter） | M2 |
| `../src/m3_depth/` | ReShade 深度纹理的格式与时效 | M3 |
| `../src/bridge/osd_test.cpp` | 覆盖窗口 / DirectComposition / OSD 自绘 | M4 |

构建：

```bash
./scripts/build_m0.sh m0c_payload      # -> build/m0c_payload.addon64 或 .exe
./scripts/build_osd_test.sh            # -> build/osd_test.exe
```

结论对应表：

| 结论 | 证据 |
|---|---|
| 同一进程里创建第二家厂商的 D3D12 设备会死锁 ⇒ 必须拆进程 | `../docs/findings/milestones/M3.2_RESULTS.md` |
| 跨卡 fence 在消费节点读不出来 ⇒ 只能靠 header 发布值 | `../docs/findings/HANDOFF.md` §8.6 |
| WGC 帧池必须"排空取最新"，缩小帧池是裁剪不是缩放 | `../docs/findings/milestones/M4_WGC_RESULTS.md` §5.2 / §5.5 |
| ReShade 只在"至少有一个启用效果"时才调 add-on | `../docs/ops/DEPLOY.md` |
