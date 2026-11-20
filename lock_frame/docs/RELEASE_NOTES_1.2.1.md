# lock_frame 1.2.1 发布说明

**1.2.1 是 1.2.0 的现场修复版**：解决"装好了、游戏能跑，但按热键一直提示
`target ... is not the producer (producer_pid=0)`、producer 从不启动"的问题
（Cyberpunk 2077 实测）。

## 根因

`deploy` 只把 `fakerFG.dll`（3 个别名）/`xess_mfg_producer.dll`/`fakerFG.ini`
写进游戏目录，**Intel 运行时只留在 UI 旁边**。XeFG 是在**游戏进程里**加载
`libxess_fg.dll` 的（`<游戏>\fakerFG\` → `<游戏>\` → 搜索路径），于是游戏加载到
它自带的那份旧 DLL：

```text
xefg: libxess_fg not loadable (missing exports: xefgSwapChainD3D12GetProperties)
dxgi: XeFG could not take the swap chain - the game keeps its own
```

XeFG 接不到 swap chain ⇒ 游戏 Present 没有 detour ⇒ `producer_present()` 从不执行
⇒ producer 只收到一条 record（过了 1.2 s 发布门后就没有了）⇒ 共享内存
`Local\mgpu_mailbox_v1` 从不创建 ⇒ UI 的 `producer_pid=0`。

**这不是颜色格式问题，也不是协议版本不符**（`expect=28` 是协议版本号）。

## 修复

- **安装时会写入 Intel 运行时**：复制到 `<游戏>\fakerFG\libxess_fg.dll` 与
  `libxell.dll`（我们自己的子目录，不覆盖游戏文件）；「还原」会清掉这两个文件
  （只删与本包同尺寸的那份）。
- **启动门槛检查游戏目录**：只认 `<游戏>\fakerFG\libxess_fg.dll`；缺了会提示
  `游戏目录 fakerFG 里缺少 libxess_fg.dll（点[重新应用配置]自动写入）`，
  启动前弹窗也会提示。
- **运行时加载更稳**：`libxess_fg.dll` 的每个候选路径都会尝试，而不是第一个能
  `LoadLibrary` 的就停；失败时日志逐条列出原因
  （`<path> -> missing exports: ...`）。所以"游戏自带旧 DLL + 包里的新 DLL"并存
  时也能选对。
- **传输自愈与诊断**：
  - fakerFG：present 边界一直不来时，超过 100 ms 的新 record 会释放挂起位继续
    传输，并打 `record: pending record never reached a present ...`；
  - producer：映射已就绪但 5 s 内没有 present tick 时，明确打
    `producer: transport ready for N ms but no present tick arrived - fakerFG's
    Present detour is NOT installed`，并指出该查哪一行日志。

## 升级步骤（重要）

1. 关闭游戏与旧的 `lock_frame_ui.exe`；
2. 用本包解压覆盖（或整目录替换）；
3. 打开 UI → 选游戏 → 点 **[重新应用配置]**（这一步才会把运行时写进游戏目录）；
4. 重启游戏，进入实际画面后再按热键。

验收：`<游戏>_fakerFG.log` 出现 `xefg: libxess_fg ...\fakerFG\libxess_fg.dll` 与
`xefg: the title keeps the library's swap chain object ... (hooked)`；
`<游戏>_mfg_producer.log` 出现 `shared ready: name=Local\mgpu_mailbox_v1`。

## 兼容性

- 共享协议仍为 **v28**；游戏侧 `fakerFG.dll` / `xess_mfg_producer.dll`
  必须与本包同版本（本包三者是一起构建的）。
- 其余功能与 1.2.0 相同（2x/3x/4x、游戏为中心配置、安装/还原、中英双语）。
