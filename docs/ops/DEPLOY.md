# 部署（当前链路，2026-11-14）

当前部署形态：**把 `game/` 里的文件复制到游戏 exe 目录**，不需要 ReShade、不需要安装器。
旧 ReShade add-on 部署包（`mgpu_reshade_pack.zip`）已归档到
[`archive/DEPLOY_addon_pack.md`](archive/DEPLOY_addon_pack.md)。

---

## 1. 发布包从哪来

```bash
./scripts/build_faker_fg.sh          # -> build/fakerFG.dll
./scripts/build_producer.sh          # -> build/xess_mfg_producer.dll
./scripts/build_consumer.sh          # -> build/xess_mfg_consumer.exe
./scripts/build_ui.sh                # -> build/mgpu_ui.exe（可选）
./scripts/make_release_fg.sh         # -> dist/mgpu-<ver>/ + .zip
```

产物结构：

```text
dist/mgpu-<ver>/
├─ README.md              （= scripts/release_README.md）
├─ CHANGELOG.md
├─ SHA256SUMS.txt
├─ game/                  ★ 全部复制到游戏 exe 目录
│   ├─ dxgi.dll           = fakerFG.dll
│   ├─ nvngx_dlssg.dll    = fakerFG.dll
│   ├─ fakerFG.dll        = fakerFG.dll
│   ├─ fakerFG.ini
│   ├─ xess_mfg_producer.dll
│   ├─ xess_mfg_consumer.exe
│   ├─ mgpu_ui.exe
│   ├─ run_consumer.bat
│   └─ libxess_fg.dll / libxell.dll（可选，Intel）
└─ docs/
```

> `game/` 里的三个 fakerFG 副本必须是**同一份二进制**（脚本会复制同一文件）。
> 手工替换时必须三个名字一起换。

---

## 2. 手动部署（推荐用于开发/单机）

1. 退出游戏；
2. 把 `game/*` 复制到 **exe 所在目录**（不是上一级）：
   - `Cyberpunk 2077\bin\x64`
   - `PBZ\platform\steam`
   - `Wuchang ...\Binaries\Win64`
3. 如果原目录已有 `dxgi.dll` / `nvngx_dlssg.dll`，先备份为 `*.bak`；
4. 启动游戏，确认日志出现 `loaded as dxgi.dll` / `xefg: the swap chain is ours now`；
5. 运行 `run_consumer.bat`（或 UI）。

开发期可以用脚本完成同样的复制与备份/还原：

```bash
./scripts/deploy_faker_fg.sh --dry-run "<GAME_DIR>"   # 先看要动哪些文件
./scripts/deploy_faker_fg.sh "<GAME_DIR>"             # 部署
./scripts/deploy_faker_fg.sh --uninstall "<GAME_DIR>" # 还原
```

> 脚本从 `build/` 取产物，所以先跑 §1 的前四个构建。

---

## 3. 消费端部署

- `xess_mfg_consumer.exe`、`mgpu_ui.exe`、`libxess_fg.dll`、`libxell.dll`
  放**同一目录**（任意位置）；
- UI 按自身路径找 consumer，所以两者必须同目录；
- 编辑 `run_consumer.bat` 的 `GAME_EXE`；
- 建议把该目录加入杀软白名单（代理 DLL + 覆盖窗口容易被误报）。

---

## 4. 部署后自检

游戏侧日志（`<游戏exe>_fakerFG.log`）：

```text
DllMain: attached as dxgi.dll
arch: answering 0x90 ...
xefg: the swap chain is ours now (3840x2160) - generation OFF (consume)
producer: ...\xess_mfg_producer.dll -> start=0
record: upscaler lane (tagged, post-MvResample) ...
marker draw: #0(...) ... code=0x...
publish #1 ...
```

producer 日志（`<游戏exe>_mfg_producer.log`）：

```text
producer: subscribed to the fakerFG record
shared ready: name=Local\mgpu_mailbox_v1 ... used=... MB slots=4
[mv] copy 2560x1440 ...
[depth] copy 2560x1440 ...
publish #1 slot=0 ...
shared: credit window ON - consumer is ACKing
```

消费端日志（`mgpu_consumer.log`）：

```text
[consumer] capture item 3840x2160
[consumer] marker code: seq12=... crc8=... ok
[consumer] cadence: input=25.0ms = 3.00 vblanks ... xN=2.00
[consumer] breakdown: ...
```

三者都对 = 部署成功。

---

## 5. 回滚

| 场景 | 做法 |
|---|---|
| 只想去掉游戏侧 | 删除 `dxgi.dll` / `nvngx_dlssg.dll` / `fakerFG.dll` / `xess_mfg_producer.dll`（有备份就还原） |
| 想临时停用 fakerFG | `fakerFG.ini` 里 `Passthrough=1`（不回答、不接管） |
| 想回旧 add-on 链路 | 按归档文档重新部署旧包（不推荐，和当前 consumer 不兼容） |
| 恢复原 DLL | `deploy_faker_fg.sh --uninstall "<GAME_DIR>"` 或手动把 `*.bak` 改回原名 |

---

## 6. 常见部署坑

1. **只换了 `fakerFG.dll`**：游戏加载的是 `dxgi.dll`，必须三个名字一起换；
2. **放错目录**：必须是 exe 所在层，不是上一级；
3. **游戏在运行**：DLL 被占用，复制会失败或只更新一半；
4. **版本混用**：`producer` / `consumer` / `fakerFG` 必须同一次构建
   （协议 `kSharedVersion = 17` 硬拒绝）；
5. **缺 Intel DLL**：`libxess_fg.dll` / `libxell.dll` 不在包内时 consumer 起不来；
6. **HDR/SDR 不匹配**：HDR 游戏默认 `HDR10`；SDR 游戏要加 `--sdr`。

---

## 7. 验证打包（每次改包应重跑）

```bash
# 1. 文件齐全
ls dist/mgpu-<ver>/game | sort

# 2. 三个 fakerFG 副本一致
sha256sum dist/mgpu-<ver>/game/{dxgi.dll,nvngx_dlssg.dll,fakerFG.dll}

# 3. 校验清单
( cd dist/mgpu-<ver> && sha256sum -c SHA256SUMS.txt )
```
