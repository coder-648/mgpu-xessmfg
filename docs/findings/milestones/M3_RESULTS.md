# M3.1 结果：ReShade 深度可达，但**不是 DLSS 用的那一份**

日期：2026-11-06　探针：`src/m3_depth/` + `effects/mgpu_depth_tap.fx`
日志：`E:\...\bin\x64\m3_depth.log`（53 条采样，约 6400 帧）

---

## 1. 实测

| 项 | 结果 |
|---|---|
| `init_effect_runtime` | ✅ 触发 4 次，`buffer 5120x2880, back buffers=3` |
| 后台缓冲 | ✅ **5120×2880 `R10G10B10A2_UNORM`(24)**，与 M2 的 DLSS Output 尺寸一致 |
| 深度变量 | 首次 present 时 `variable not found` → 之后 `<none>` → 再之后**绑定成功** |
| ReShade 给的深度 | **5120×2880 `R32G8X24_TYPELESS`(19)**（= D32_FLOAT_S8X24） |
| MOTION 语义 | `<none>`，ReShade 不提供运动矢量（符合预期，MV 走 NGX） |

## 2. 关键结论：这不是同一个深度缓冲

| 来源 | 尺寸 | 格式 |
|---|---|---|
| ReShade `DEPTH` 语义 | **5120×2880** | `R32G8X24_TYPELESS`（带 8 位模板） |
| NGX（M2 实测，DLSS 真正用的） | **2560×1440** | `R32_TYPELESS`（纯 D32） |

**它们是两个不同的缓冲。** 证据链：
1. M2 显示 DLSS 的颜色输入是 2560×1440、输出是 5120×2880 → 游戏确实在 2560×1440 渲染再升采样。
2. XeSS-FG 硬性要求 **深度尺寸必须等于运动矢量尺寸**（2560×1440）。
3. ReShade 检测到的是"最匹配交换链尺寸"的深度（5K 带模板），多半是后期/post-upscale 用的那份，**不是**几何渲染深度。

→ **结论：深度不能走 ReShade 语义，必须和 MV 一样从 NGX 钩子里拿。**
这也顺带解决了"深度与 MV 必须同尺寸"的一致性问题——两者本来就是同一个 DLSS 调用里的同一批资源。

## 3. 设计修正

| 原计划 | 修正后 |
|---|---|
| 颜色 ← ReShade 后台缓冲 | ✅ 不变（5K R10G10B10A2，已验证） |
| 运动矢量 ← NGX hook | ✅ 不变（2560×1440 RG16F） |
| **深度 ← ReShade DEPTH 语义 + tap shader** | ❌ **改为深度也走 NGX hook**（2560×1440 R32_TYPELESS） |
| `.fx` 用于采深度 | 不再需要（保留文件，只作诊断/冗余） |

**副作用：M3 的范围缩小了。** 不需要写"采样 ReShade 深度再重采样"的 pass，
只需要把 NGX 给的两个资源（MV 2560×1440 RG16F、深度 2560×1440 R32）搬进邮箱。

## 4. M3.2 的实现方案（已确认 API 可行）

需要解决的核心问题：**游戏自己的深度/ MV 资源状态不受我们控制**。
直接 copy 需要正确的状态转换，猜错会导致 GPU 挂起。方案：

1. hook `addon_event::barrier`（签名已确认：`(cmd_list, count, resources, old_states, new_states)`）
   → 维护一个 `resource → 最后已知状态` 的表，全程跟踪 MV / 深度 / 后台缓冲的状态
2. hook `addon_event::reshade_begin_effects`（签名已确认：给 `cmd_list`）
   → 在帧内、present 之前拿到 ReShade 的命令列表
3. 在该列表上：
   - `barrier(深度/MV, 已知状态 → copy_source)`
   - `CopyResource(mailbox.producer_source(1/2), NGX 资源)`
   - `barrier(…, copy_source → 原状态)`（还原，不破坏游戏）
   - 颜色：`barrier(backbuffer, render_target → copy_source)` → `CopyResource(source(0))` → 还原
   - `mailbox.publish(frame)`
4. 邮箱在 addon 内创建：生产端用游戏设备，消费端现场枚举 AMD 适配器建设备

**本轮先不做 R16 转换**：把邮箱的深度流临时设为 `R32_FLOAT`（14.06 MB），
这样能直接用 `CopyResource` 打通全链路（总载荷 84.4 MB，即 M0.3 里 82.8% 预算的配置 B）。
R16 下采样 pass 放到 M3.3，收益是把 84.4 → 77.3 MB。

## 5. 已验证可用的 API 清单

| 用途 | API |
|---|---|
| 后台缓冲 | `effect_runtime::get_current_back_buffer()` / `get_back_buffer(i)` |
| 帧内命令列表 | `addon_event::reshade_begin_effects` |
| 状态跟踪 | `addon_event::barrier` |
| 视图→资源 | `device::get_resource_from_view(view)` |
| 资源描述 | `device::get_resource_desc(resource)` |
| 语义绑定（备用） | `find_texture_variable` + `get_texture_binding` |
