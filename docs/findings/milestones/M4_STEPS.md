# M4 执行步骤：跨进程传输（生产者已就绪）

## 背景：为什么必须拆进程

M3.2 的排障结论（`docs/findings/milestones/M3.2_RESULTS.md`）：**ReShade 的 D3D12 钩子是进程级的**，
在游戏进程里创建 AMD 设备会被它当成游戏设备接管，直接死锁。
所以架构定为：

```
游戏进程（ReShade addon）                 bridge 进程（独立）
  ├ 只碰 NVIDIA 设备                        ├ 只碰 AMD 设备
  ├ NGX 钩子拿 MV/深度                      ├ 打开同名共享内存
  ├ 拷进命名共享内存的槽位                  ├ 在共享内存上放置纹理
  └ 每帧 signal 跨进程 fence                └ 等 fence → 取帧（后续接 XeSS-FG）
```

好处：副卡崩溃不会拖死游戏；游戏进程里彻底没有第二张卡的东西。

## 已部署

| 项 | 内容 |
|---|---|
| `src/mgpu_shared.h` | 两侧共享的控制块布局（magic/version/pid/流描述/fence 句柄值） |
| `src/game_capture/`（改造） | 命名映射 `Local\mgpu_mailbox_v1` + 每流 2 槽位 + 共享 fence，每帧 `Signal` |
| `tests/bridge_probe/` | 消费端探针：开映射 → AMD 设备 → 放置纹理 → 复制 fence → 收帧计数 |
| `<TOOLS_DIR>\bridge_probe.exe` + `run_bridge.bat` | 可执行的消费端 |

关键实现细节：
- 生产者在控制块里写入 **fence 的 NT 句柄值**，消费端用 `OpenProcess(PROCESS_DUP_HANDLE)`
  + `DuplicateHandle` 把 fence 复制到自己进程，再 `OpenSharedHandle` 成 `ID3D12Fence`；
- 控制块的 `magic` **最后写**，消费端见到 magic 才认为数据完整；
- 生产者在 `reshade_present`（帧已提交之后）对游戏队列 `Signal`，保证槽位数据完整。

## 你要做的

**顺序很重要**：

1. **启动游戏**，进入存档、站在世界里（等 10 秒让采集跑起来）
2. **双击 `<TOOLS_DIR>\run_bridge.bat`**（它会等 30 秒并自动退出）
3. 把游戏退出
4. 告诉我，我读两边日志：
   - `E:\...\bin\x64\game_capture.log`（生产者，会有一行 `shared ready: ... pid=...`）
   - `<TOOLS_DIR>\bridge_run.txt`（消费者）

## 预期输出

生产者 `game_capture.log`：
```
shared ready: name=Local\mgpu_mailbox_v1 pid=12345 size=169.0 MB slots=2
  [mv] first copy from 0x..., tracked state = 0x40
  [depth] first copy from 0x..., tracked state = 0x40
-- frame 121 stage=2 ... record=0.05 ms
  stats: seen=241 copied=241 skipped=0
```

消费者 `bridge_run.txt`：
```
=== bridge_probe: waiting for 'Local\mgpu_mailbox_v1' ===
[bridge] control block: pid=12345 slots=2 streams=3 size=169.0 MB
  stream 0: 5120x2880 fmt=24 offset=65536 slot=56.25 MB
  stream 1: 2560x1440 fmt=34 offset=...
  stream 2: 2560x1440 fmt=39 offset=...
[bridge] AMD device ready
[bridge] mapping wrapped as a heap
[bridge] placed 3 x 2 textures on the shared heap
[bridge] fences duplicated
  frame     41  slot 0  fence=...  (40.2 fps)
  ...
[bridge] done: N frames received in 30 s
```

## 判读

| 现象 | 含义 |
|---|---|
| `shared ready` + 消费者收到 40fps 左右 | ✅ **跨进程传输打通**，可进 M4.2（XeSS-FG） |
| 消费者一直 `waiting for mapping` | 生产者没创建：看 `game_capture.log` 是否有 `shared: ... FAILED` |
| `control block` 但放置纹理失败 | 共享堆在 AMD 侧不接受该格式/布局，需换格式或改用缓冲区 |
| `DuplicateHandle` 失败 | 权限问题（`Local\` 命名 + 同会话应可）——可能需要换同步方式 |

## 已知限制（本阶段故意简化）

1. **颜色流（stream 0）只预留空间、尚未写入**（56 MB×2 槽），
   所以映射总共 169 MB 但只有约 42 MB 在被使用。颜色方案另定。
2. **没有背压**：2 槽位轮转，消费者慢时可能读到正在被覆写的槽（撕裂）。
   M4.2 接 XeSS-FG 时会用 back-fence 做真正的节流。
3. 消费者只**计数**，还没把数据用于任何东西——这一版只验证通路。

## 回滚

删掉 `bin\x64\game_capture.addon64` 即可；游戏文件不受影响。
