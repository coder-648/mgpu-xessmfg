# 我们自己的中介层：照 ReShade 的设计逻辑（2026-11-11）

`Host=0`（只留解锁代理）稳定进入里世界 → **崩溃来自我们的采集/钩子层**。
下面是 ReShade 源码里可直接照搬的六条设计，以及它们在我们代码里的落点。

## 1. 只信任"观察到的"状态，不信任 API 声明

ReShade 的状态表由**包装层拦截到的 barrier** 填充（`d3d12_command_list.cpp`
`ResourceBarrier`/`Barrier`），它从不把"游戏说这个资源现在是什么状态"当真。

我们的违规：`track_observed()` 把 Streamline tag 里声明的状态当成观测值写进状态表
（`in_fg.cpp`）。一旦声明过期（CP2077 的 HUDLESS 在 lc=0/lc=1 之间跳，MV 常驻 UAV），
我们就会发出非法转换 —— 这正是它之前"Gpu Crash"的样子。

**已回退**：两个调用点恢复成 `track()`（只作 hint，状态仍由观察到的 barrier 决定）。

## 2. 对象级 vtable 补丁，而不是给共享函数打补丁

ReShade 的 `hook::vtable`（`hook_manager.cpp`）是**替换对象自己的 vtable 指针**
（拷一份 vtable，改其中若干项），所以只影响那一个对象，别的对象/线程不受牵连。

我们的违规：`hook_raw_device_vtable()` 直接 `MH_CreateHook(vt[8]/vt[9])`，
补的是 d3d12.dll 里所有设备共享的函数。

**改为**：包装 `IDXGIFactory::CreateSwapChain*` 拿到 swapchain 对象后做对象级补丁；
设备侧则继续走"包装对象 + 回答所有本 SDK 声明的接口"（现状），
对更新的接口（Device10+）不硬答，而是记录并在日志里说明。

## 3. Present 只记录，提交发生在游戏的 ExecuteCommandLists 里

ReShade：`dxgi_swapchain.cpp` 的 `on_present()` 只把命令记进 immediate command list；
真正的提交在 `d3d12_command_queue.cpp:205`——`flush_immediate_command_list()` 之后
把这条 list 追加进游戏的数组一起转发。**它从不在 Present 里 ExecuteCommandLists。**

我们的违规：`present_record_copy()` 在 Present 栈里 `ExecuteCommandLists` + `Signal`。
CP2077 连"空操作"都判 GPU 崩溃，说明这个栈位置本身不安全。

**改为**：Present 里只 `GetBuffer` + 记录；提交交给包装层的 `ExecuteCommandLists`。

## 4. immediate list 用 8 组 allocator/fence 轮转，Reset 前等 fence

`d3d12_impl_command_list_immediate.cpp`：`NUM_COMMAND_FRAMES=8`，
`flush()` 里 Close → Execute → Signal(fence) → **`GetCompletedValue() < value` 时等事件** → Reset。

这条我们已经照做了（`kPresentFrames=8` + fence 等待），保留。

## 5. 钩子只在初始化期装，且不在 loader 锁内

ReShade 的 `AddonInit`/设备创建路径装钩子；loader 通知只入队。
我们已经照做（`notify_sources` 入队 + 5 ms 定时消费），保留。

## 6. 最少必要面：只包装真正需要的对象

ReShade 包装 device / queue / command list / swapchain，其它一律透传
（`DXGI_*` 大量方法都是 `_orig->...`）。

我们的现状类似（device/queue/list），但**新增的每条"顺手钩一下"都要能一键关闭**。
现在已有 `PresentHook` / `RawVtableHooks` / `FollowConsumer` / `Color` / `Ui` 五个开关，
基线是全关。

## 落地顺序（每一步都可单独跑、单独回退）

```text
S1  ✅ 回退 track_observed（只信观察到的 barrier）        ← 已部署，待验证
S2  把"提交"从 Present 挪到包装层 ExecuteCommandLists     ← 改 present_record_copy
S3  包装 IDXGIFactory::CreateSwapChain*，用对象级 vtable 补丁替换 raw 函数补丁
S4  颜色：优先 hudless（lc>=1），否则 present 图；ui = present − hudless（副卡差值 pass）
S5  恢复 raw device vtable 钩子为"可选兜底"，默认关闭
```

验证判据（每一档）：

```text
稳定 30 s 以上 + wrap: ExecuteCommandLists / publish done 持续
+ stats: copied 接近 seen（尺寸不匹配时用 RenderWidth/RenderHeight 固定，StreamHeight 已移除）
```

## 7. 第二次踩同一条线（2026-11-11）：ReShade 从不"合成" StateBefore

把 tag 声明的状态当成可用状态（2 帧保鲜窗口）→ CP2077 再次 `Gpu Crash for unknown reasons`。
所以这条不是运气问题，是原则问题。回看 ReShade 的源码，它的做法比我们更严格：

```text
ResourceBarrier  (d3d12_command_list.cpp:467-478)
    → 原样转发给驱动，**不改** StateBefore/StateAfter
    → 只是把游戏自己写的这对状态作为事件报告给 add-on
Barrier (enhanced, :1218-1229)
    → 同样只做 access→usage 的转换用于报告
```

也就是说：**ReShade 从不自己编一个 StateBefore**。它需要动某个资源时，用的都是"游戏在
当前这次 barrier 里自己声明的状态"。它的状态表是**被动的观测记录**，不是可以拿来当输入去
发 transition 的东西。

我们两次违反的都是这一点：先是用 tag 的声明（`track_observed`），再是用 2 帧保鲜的 hint。

### 由此得到的正确做法

```text
不要在"present/提交时刻"去猜资源状态，而是在【观察到 barrier 的那一刻】决定能不能拷：

  on_barrier(resources[], old_states[], new_states[])
    对每个我们关心的资源：
      if new_state 是我们可以读的状态（copy_source / 全 SRV / general）
         且本帧还没拷过
          → 把拷贝记录到【当前这条命令表】上，用 new_state 作为 before
          → 拷完立刻用一次反向 transition 把它还原成 new_state（或保持不动）

  游戏从不 transition 的资源（CP2077 的 MV 常驻 UAV）→ 我们永远读不到
     → 结论：这类标题的 MV 无法在 present 时刻合法读取，需要换数据源
       （例如它自己的 SCALING_OUTPUT/back buffer，或别的 lane）
```

这正是 ReShade 的 add-on 能在任意标题上安全工作的原因：
**它们只做"在这条已经声明的命令表上再加一点东西"，从不试图改变别人声明的状态。**

### 我们是怎样才会观察到 0xC0 的

早期日志里出现过 `[mv] convert pass … tracked state = 0xC0` —— 说明 CP2077 **有时**确实会把
MV 转到全 SRV（例如 DLSS-G 关闭/开启切换的那几帧）。也就是说这条路径不是完全不可用，
只是不能用"每帧都拷"的方式去做，必须**在观察到的那一刻**拷。

## 8. 方案 A：按"数据完整性"决定是否补帧（2026-11-11 决定）

前提（用户提出，很重要）：**游戏全流程并不都是可补帧的画面**。加载屏、菜单、特殊 UI 场景
本来就不需要 FG，游戏也不会给完整数据。所以判据不是"每帧都要"，而是"这一帧的数据够不够"。

### 8.1 三层判据

```text
① 合法性  这一帧该资源有没有出现可读状态（观察到的 barrier）
          没有 → 我们不拷（不猜状态，不违反 ReShade 纪律）
② 完整性  本帧实际拿到了什么：MV / depth / 颜色 / 相机矩阵 / jitter
          → 已经通过 active_mask + producer.input_flags 发布给消费者
③ 决策（消费者侧）
          MV + 颜色 + 相机齐全  → 正常补帧
          只有 MV                → 降级：单帧外推或跳过（XeFG 允许无 depth）
          什么都没有             → 本帧不补：直接呈现真实帧（1x），不产生伪影
```

这正是"打断 FG 但不崩"的正确形态：游戏认为 FG 开着，我们按帧决定到底生不生成。
菜单里数据不全 → 我们输出 1x，画面完全正常；回到游戏 → 数据齐全 → 恢复 2x/3x/4x。

### 8.2 采集侧的落地（A 的具体实现）

```text
在 on_barrier(资源, 旧状态, 新状态) 里，而不是在提交/present 时刻决定：
    若 资源 == 当前帧关心的 MV/depth  且 新状态 ∈ {copy_source, 全 SRV, general}
       且 本帧尚未拷过
       → 记录拷贝到【当前这条命令表】（before = 新状态）
       → 立刻还原成新状态，游戏后续使用不受影响
    → 拷过的流在本帧的 active_mask 里置位，其余保持 0

注意：这要求拷贝所需的"槽位"在录制期就已经确定（当前槽位每帧在提交时轮转，
      需要改为"barrier 录制期也可见"），这是实现时唯一的实质改动。
```

### 8.3 为什么这条符合 ReShade 纪律

```text
· before 永远来自"刚刚观察到的那个 barrier"，不是我们合成的
· 拷贝与还原都在同一条命令表上，顺序由游戏自己的 barrier 保证
· 游戏从不 transition 的资源，我们永不触碰（CP2077 常驻 UAV 的那些帧就直接跳过）
```
