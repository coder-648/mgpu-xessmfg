# 记录用的旧实现（不参与构建）

`version.cpp` 是 `version.dll` 代理的第一版（约 1000 行：NVAPI 应答 + NGX 能力/需求/Init 钩子
+ 版本号改写 + DXGI 适配器 vtable + GetProcAddress 钩子）。它被 `../version_min.cpp`（约 300 行）
取代，原因写在 `docs/design/FG_CLAIM.md` §20：四个子系统全是猜测，且每一个都造成过崩溃或死锁。

```text
NGX 能力/需求/Init 三钩子   CP2077 崩溃（0x400），且并非必需
版本号改写                  版本块越界写 -> 堆损坏 -> 崩溃
DXGI 适配器 vtable          首版截断 vtable -> fatal error
GetProcAddress 钩子         loader lock 死锁 -> 进程卡住
```

保留它是为了回答"当初为什么那样做"，**不要**把它重新加进构建：现在只编译 `version_min.cpp`，
导出表在 `../version.def`，构建脚本是 `../build.bat` / `scripts/build_proxy.sh`。
