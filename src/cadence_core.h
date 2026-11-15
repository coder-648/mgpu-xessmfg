// ============================================================================
//  cadence_core.h  -  consumer 动态逐帧（--interp-cadence）的"任意上限"控制器
//
//  纯计算、无平台依赖（不 include Windows/D3D），可直接在 Windows 的
//  xess_mfg_consumer 里使用，也能在 Linux 上用 tests/cadence_core/sim.cpp
//  跑确定性仿真。与 src/xefg_dynamic_count.h 同一思路：把状态机从 I/O 里
//  拆出来，调用方只负责喂"每真实帧一个样本"和执行返回的 k。
//
//  为什么需要它（现状见 docs/design/DYNAMIC_CADENCE_ANY_CAP.md）：
//    * 现有 --interp-cadence 有三套互不相干的取 k 逻辑（governor / mean+phase /
//      tick），只有 governor 是稳定默认，另外两套在"上限 J 不够用"时没有定义
//      行为：k 被静默夹到 J，frameRenderTime 仍按输入间隔喂，输出间距既不是
//      整 vblank 也不是常量，表现为 1.3~1.5 vblank/帧的来回抖。
//    * 计数切换有成本（这份 pacer 上约等于丢一张输出），而 phase 路径在
//      drain 被延迟/被 DLL 拒绝时仍然把"想给的 k"从 phase 里减掉，长期均值
//      与实际不一致。
//    * 上限 K 变大后，切换点更多、GPU 单帧成本按 k 线性增长，控制律必须同时
//      受"面板 vblank 数"和"FG 卡单批成本"两个上限约束，否则 6x/8x 会把
//      消费卡压满，反过来拉长真实帧间隔。
//
//  本文件只负责"该给多少张 / 预算多少"，不负责调用 SetNumInterpolatedFrames
//  （见 Commit 的契约）。
//
//  SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mgpu_cad {

// ------------------------------------------------------------------ policy
enum Policy
{
    // 恒定整数倍率优先：一个 epoch 内 k 不变，只在输入持续变化时 ±1。
    // 视觉优先（默认）：少切换 = 少丢输出 = 少顿挫；代价是输出略低于面板。
    kPolicyEven = 0,
    // 每帧按相位补满：输出尽可能贴住面板，代价是接近每帧一次计数切换。
    // 仅当实测"一次切换不丢输出"时才自动启用（见 NoteSwitchLoss）。
    kPolicyFill = 1,
    // 理论最优：在 (m,k,paced) 候选里按代价函数选（见 UpdateOptimal）。
    // m = 输出间距（vblank 数），k = 每个真实帧的输出张数，paced = 是否
    // 在延迟预算内把输入量化到整数格。
    kPolicyOptimal = 2,
    // 锁一个整数 k（策略 v2 的 over-supply 分支）：k = ceil(target/输入率)，预算恒为
    // k×vblank，所以输出间距严格 1 vblank；非整除输入下多出的帧由面板丢掉。
    // 只在"输入率跨过某个 k 的边界"连续 3 窗确认后重选一次。carry 只用于转场。
    kPolicyLocked = 3
};

enum Regime
{
    kRegimeFillable    = 0, // want <= J：可以每 vblank 一张
    kRegimeCapLimited  = 1, // 上限不够：只能常量 k，输出率低于目标
    kRegimeInputLimited= 2  // 输入快于面板：只能 1:1（建议 --no-fg）
};

enum Reason
{
    kHeld = 0,
    kUp,
    kDown,
    kEnterCap,
    kExitCap,
    kCapEven,
    kCostCapped,
    kDeferred,
    kReuse,
    kHitch
};

struct Config
{
    double refreshHz        = 120.0;
    int    maxOutputFrames  = 4;      // J = min(--interp, DLL max) + 1
    double outHz            = 0.0;    // 0 = 面板刷新率（--fg-out-hz）
    int    policy           = kPolicyEven;
    // epoch 动态：倍率按输入中位数在秒级变化，epoch 内禁止换 k。
    // rounding: 0 = floor（输出永不超额，丝滑优先）, 1 = nearest, 2 = ceil。
    double epochMs          = 0.0;
    int    rounding         = 0;
    int    smooth           = 8;      // 到达间隔 EMA 窗口（--cadence-smooth）
    double deadband         = 0.60;   // fill 方案：允许的相位误差（帧）
    int    minDwell         = 4;      // 两次计数切换之间至少几个真实帧
    int    downBatches      = 10;     // even 方案：想降档需持续多少帧
    // 切换成本反馈是否自动调驻留。默认 false：只测量、写日志（NoteSwitchLoss），
    // 不改变控制律。打开后按"每次切换实测丢几张"增减 minDwell，并在 fill 明显
    // 亏本时退回 even。真机验证过再打开（--cad-switch-loss auto）。
    bool   autoTuneDwell    = false;
    double capEnterHyst     = 0.35;   // want 超过 J 这么多 -> cap-limited

    double capExitHyst      = 0.35;   // 回落到 J 以下这么多 -> 退出
    double evenImprove      = 0.10;   // cap-limited 降档：必须比当前整齐这么多 vblank
    double evenTolerance    = 0.25;   // 残差在此以内都算"可接受"，同等接受时取更大的 k
    double floorShare       = 0.96;   // 输出不低于目标的这个比例（旧 governor 语义）
    // FG 卡单张输出的实测成本（ms）。0 = 未知；>0 时 J 再受成本上限约束。
    // 双卡下这一项是"任意上限"能否成立的关键：k 张大图的成本 ≈ k * cost。
    double costPerOutputMs  = 0.0;
    double overheadMs       = 0.0;    // 每批固定开销（提交/等待）
    // ---- kPolicyOptimal 的代价权重与输入量化预算 ----
    // 代价是无量纲的每秒近似：q 是长保持率，帧率/延迟项都归一到 ~[0,1]。
    // 默认"平滑 > 帧率 > 延迟"；用户可通过 --cad-policy 选择。
    double wSmooth          = 1.00;
    double wFps             = 0.50;
    double wLatency         = 0.01;
    double wAlias           = 0.30;   // 主动 pace 输入时丢源帧的惩罚
    double wSwitch          = 0.05;   // 每丢一张输出的代价（配合 NoteSwitchLoss）
    // 超额供给的代价：k·f > R 时多出来的帧会被丢掉（内容跳），但 out 被 
    // 钳到 R 之后这个代价在 wFps 项里看不见。必须显式计入，否则
    // "3x@43fps（丢 7.5%）" 与 "3x@40fps（恰好 120）" 会被算成同价。
    double wOver            = 0.50;
    double latencyBudgetMs  = 2.0;    // Λ：机会式输入量化的最大等待
    bool   allowInputPacing = true;   // false = 只用固定刷新 + 不量化输入
    double switchMargin     = 0.02;   // 候选切换必须比当前好这么多才换
    // A/B：批次预算给 k*V（整 vblank）而不是实测输入间隔。
    // 默认 false —— 2026-11-09 实测 k*V 会反过来污染输入间隔测量。
    bool   feedWholeVblanks = false;
};

struct Sample
{
    double nowMs   = 0.0;
    double inputMs = 0.0;   // 上一个真实帧到这一帧的间隔（原始值）
    double waitMs  = 0.0;   // 其中在提交路径里等待的部分（未用于判决，仅供日志）
    bool   reuse   = false; // 重复/复用捕获帧：不推进相位、不重新决策
    bool   canSwitch = true;// 上一批输出已排空（cadence_present_drained）
    // 捕获会话/资源换代/配对身份变化：清空估计器、相位与 cap 状态（保留学习值）。
    // 对应 Magpie XeSSFGTiming 的 sequence/generation/timestamp 重置条件。
    bool   reset   = false;
};

struct Decision
{
    int    k             = 2;    // 本帧输出几张（= interp + 1），1..J
    int    m             = 1;    // 输出间距（vblank）；paced 时保持时长就是 m
    bool   paced         = false;// 是否把本批量化到 k*m*vblank（输入层会等）
    double q             = 0.0;  // 长保持率 |slots - k*m|/(k*m)
    double renderTimeMs  = 0.0;  // -> frameRenderTime / 输入 pacer 预算
    double wantFrames    = 0.0;  // 本帧"欠"多少张（小数）
    int    regime        = kRegimeFillable;
    int    reason        = kHeld;
    int    jEff          = 2;    // 生效上限（面板 / 成本 / DLL 取小）
    double spacingVblanks= 1.0;  // 输出间距（vblank）
    double outputHz      = 0.0;  // 预期输出帧率
    double targetHz      = 0.0;  // 目标输出帧率（--fg-out-hz 或面板）
    bool   switched      = false;
    bool   exact         = false; // 间距是否落在整 vblank（<= 0.06 vb 误差）
};

class CadenceCore
{
  public:
    void Configure(const Config& c)
    {
        _cfg = c;
        sanitize();
        _learnedMaxK = _cfg.maxOutputFrames;
        Reset();
    }

    // 运行中改上限（--interp / 换 DLL / 成本上限档位）：保留已学的驻留、
    // 成本与统计，只把 k 夹到新上限并清掉未决状态。不要用 Configure ——
    // 它会 Reset 到 2x 并清空学习值。
    void Reconfigure(const Config& c)
    {
        _cfg = c;
        sanitize();
        if (_lastK > _cfg.maxOutputFrames) { _lastK = _cfg.maxOutputFrames; }
        if (_lastK < 2) { _lastK = 2; }
        if (_learnedMaxK > _cfg.maxOutputFrames) { _learnedMaxK = _cfg.maxOutputFrames; }
        if (_learnedMaxK < 2) { _learnedMaxK = 2; }
        _plannedK = _lastK;
        _plannedM = _lastM;
        _plannedPaced = _lastPaced;
        _hasPending = false;
        _framesSinceSwitch = 0;   // 上限变化后的第一次切换不再等驻留
        _phase = 0.0;
        _lastOut.k = _lastK;
        _lastOut.m = _lastM;
        _lastOut.paced = _lastPaced;
    }

    const Config& GetConfig() const { return _cfg; }

    void Reset()
    {
        ResetRuntime();
        _lastK = 2;
        _lastM = 1;
        _lastPaced = false;
        _plannedK = _lastK;
        _plannedM = _lastM;
        _plannedPaced = _lastPaced;
        _pendingLam = 0.0;
        _hasPending = false;
        _framesSinceSwitch = 1 << 20;
        _switches = 0; _deferred = 0; _capFrames = 0; _costFrames = 0;
        _switchLoss = 0; _switchSamples = 0;
        _shownSwitchLoss = -1.0;
        _learnedDwell = _cfg.minDwell;
        _shortRun = 0; _okRun = 0;
        _lockedK = 0; _lockPending = 0; _lockVotes = 0;

        _lastOut = Decision {};
        _lastOut.k = _lastK;
        _lastOut.m = _lastM;
    }

    // ---- 每真实帧调用一次（判决）；真正 apply 在 Commit -------------------
    Decision Update(const Sample& s)
    {
        if (s.reuse) {
            // 复用帧：保持 applied k 与预算，不推进相位，也不喂估计器。
            Decision d = _lastOut;
            d.reason = kReuse;
            d.switched = false;
            return d;
        }

        // 上一帧的 Commit 没被调用（调用方漏了/被提前 return）：按"想给的
        // 已经生效"结算，保持长期均值不漂。
        if (_hasPending) { Commit(_plannedK, true); }

        if (s.reset) { ResetRuntime(); }

        const double raw = s.inputMs;
        if (!(raw > 0.0) || raw > 1000.0) {
            Decision d = _lastOut;
            d.reason = kHitch;
            return d;
        }
        // 单帧卡顿（>250 ms）或孤立尖峰不参与估计，也不改 k。
        if (raw > 250.0) {
            Decision d = _lastOut;
            d.reason = kHitch;
            return d;
        }
        if (_haveInput && raw > _ema * 2.5 && _hitchRun < 2) {
            ++_hitchRun;
            Decision d = _lastOut;
            d.reason = kHitch;
            return d;
        }
        _hitchRun = 0;

        pushRing(raw);
        const double T = estimate(raw);
        _haveInput = true;

        const double V = 1000.0 / _cfg.refreshHz;
        const double targetHz = (_cfg.outHz > 1.0)
                              ? std::min(_cfg.outHz, _cfg.refreshHz)
                              : _cfg.refreshHz;
        const double slots = T / V;                       // 输入间隔 = 几个 vblank
        const double want  = slots * (targetHz / _cfg.refreshHz);
        const double spacingTarget = _cfg.refreshHz / targetHz; // 目标输出间距（vblank）

        // ---- 上限：面板/DLL 与 FG 卡成本取小 ---------------------------------
        int jEff = _cfg.maxOutputFrames;
        bool costCapped = false;
        if (_cfg.costPerOutputMs > 0.05) {
            const double budget = std::max(T - _cfg.overheadMs, 0.1);
            int jc = static_cast<int>(std::floor(budget / _cfg.costPerOutputMs));
            if (jc < 1) { jc = 1; }
            if (jc < jEff) { jEff = jc; costCapped = true; }
        }
        if (jEff < 2) { jEff = 2; }   // 2 张 = 2x；cadence 不做 1X 的实时开关
        // 可实现性上限：XeFG 在 4K 4x 上实测会 ERR_OTHER/少出帧，
        // 连续不满 3 次就把有效上限永久降一档（见 NoteOutputShortfall）。
        if (_learnedMaxK < jEff) { jEff = _learnedMaxK; }
        if (jEff < 2) { jEff = 2; }
        // 上限被压低（成本上限生效、或运行中 Reconfigure 调小 --interp）时，
        // 当前 k 可能已经超过 J_eff：下面会立刻夹下来，且这次降档不受驻留约束
        // —— 向 DLL 申请超过它上限的 k 会直接失败，比一次模型切换更糟。
        const bool overCap = (_lastK > jEff);

        // ---- regime（带滞回）-------------------------------------------------
        const bool wasCap = _capLimited;
        if (!_capLimited && want > static_cast<double>(jEff) + _cfg.capEnterHyst) {
            _capLimited = true;
        } else if (_capLimited && want < static_cast<double>(jEff) - _cfg.capExitHyst) {
            _capLimited = false;
        }
        int regime = kRegimeFillable;
        if (slots < 1.0) {
            regime = kRegimeInputLimited;   // 输入快于面板：只能 1:1，建议 --no-fg
        } else if (_capLimited) {
            regime = kRegimeCapLimited;
        }
        if (_capLimited) { ++_capFrames; }
        if (costCapped)  { ++_costFrames; }

        // ---- 锁定策略（策略 v2）：k 由算术决定，预算恒为 k×vblank ----
        //
        // 目标：输出间距严格 1 vblank。k = ceil(target / 输入率)，所以输出率 ≥ 目标；
        // 非整除输入下多出的帧由面板在自己的刷新节拍上丢掉（这就是固定 3x 在 43 fps
        // 上仍然顺的原因，实测 C 组：show 119.5、p10 118、填充 99.6%）。
        // 只有当输入率跨过某个 k 的边界、连续 _lockVotesNeed 窗都指向同一个新 k 时才重选，
        // 避免"每帧追平均"重新变成 carry 的节奏抖动。
        if (_cfg.policy == kPolicyLocked) {
            const double wantK = targetHz * T / 1000.0;
            // 输入永远有抖动（RTSS 锁帧也会 ±1–2 帧），所以纯 ceil/round 都会在
            // 整数边界上翻（实测锁 40 时 wantK=3.002 → ceil=4，k 在 3/4 之间 76 次/53 窗）。
            // 借 optimal 的死区思路：取"失真最小"的整数 k，但只有当它比当前 k
            // 明显更好（输出率优势 > kLockMargin）且连续 kLockVotesNeed 帧都成立时才换。
            const auto errOf = [&](int k) {
                const double out = static_cast<double>(k) * 1000.0 / T;
                return std::fabs(out / targetHz - 1.0);
            };
            int kLo = static_cast<int>(std::floor(wantK + 1e-9));
            int kHi = static_cast<int>(std::ceil(wantK - 1e-9));
            if (kLo < 2) { kLo = 2; }
            if (kHi < 2) { kHi = 2; }
            if (kHi > jEff) { kHi = jEff; }
            if (kLo > jEff) { kLo = jEff; }
            const double fIn = (T > 0.1) ? (1000.0 / T) : 0.0;
            const double R_ = _cfg.refreshHz;
            const double underFrac = (R_ > 0.0)
                                   ? (R_ - static_cast<double>(kLo) * fIn) / R_ : 1.0;
            int kNear = (underFrac <= kImp) ? kLo : kHi;
            if (kNear < 2) { kNear = 2; }
            if (kNear > jEff) { kNear = jEff; }

            if (_lockedK < 2) {
                _lockedK = kNear;          // 首次直接采信
                _lockPending = kNear;
                _lockVotes = 0;
            }
            const bool better = (kNear != _lockedK) &&
                                (errOf(_lockedK) - errOf(kNear) > kLockMargin);
            if (!better) {
                _lockVotes = 0;
            } else if (kNear == _lockPending) {
                if (++_lockVotes >= kLockVotesNeed) { _lockedK = kNear; _lockVotes = 0; }
            } else {
                _lockPending = kNear;
                _lockVotes = 1;
            }

            int kNew = _lockedK;
            if (kNew > jEff) { kNew = jEff; }
            const bool changed = (kNew != _lastK);

            Decision d;
            d.k = kNew;
            d.m = 1;
            d.paced = false;
            d.q = std::fabs(wantK - static_cast<double>(kNew));
            // locked 的定义就是"每批正好覆盖 k 个 vblank"：预算恒为 k×V。
            // （V 来自 DWM 实测周期，所以 k×V 与面板的 k 个 vblank 严格相等。）
            // `--cad-budget input` 只对 fill/carry 有意义，不影响本策略。
            d.renderTimeMs = (regime == kRegimeFillable)
                           ? (static_cast<double>(kNew) * V) : T;


            d.wantFrames = wantK;
            d.regime = regime;
            d.reason = changed ? kUp : kHeld;
            d.jEff = jEff;
            d.spacingVblanks = slots / static_cast<double>(kNew);
            d.targetHz = targetHz;
            d.outputHz = std::min(targetHz,
                                  (T > 0.1) ? (1000.0 / T) * static_cast<double>(kNew) : 0.0);
            d.switched = changed;
            d.exact = std::fabs(d.spacingVblanks -
                                std::floor(d.spacingVblanks + 0.5)) <= 0.06;

            _plannedK = kNew;
            _plannedM = 1;
            _plannedPaced = false;
            _hasPending = true;
            _pendingLam = static_cast<double>(kNew);   // 相位恒为 0：不做平均修正
            _lastOut = d;
            return d;
        }

        // ---- 理论最优：在 (m,k,paced) 里按代价选 ----------------------------
        if (_cfg.policy == kPolicyOptimal) {
            return UpdateOptimal(T, V, slots, targetHz, jEff, regime,
                                 overCap, s, want);
        }

        // ---- 候选 k ----------------------------------------------------------
        int kNew = _lastK;
        Reason reason = kHeld;

        if (regime == kRegimeInputLimited) {
            kNew = 1;
            reason = kCostCapped;
        } else if (regime == kRegimeCapLimited) {
            kNew = (_cfg.policy == kPolicyFill)
                 ? jEff
                 : chooseAtCap(slots, spacingTarget, jEff, _lastK);
            reason = (kNew != _lastK) ? (!wasCap ? kEnterCap : kCapEven) : kHeld;
        } else {
            int kNeed;
            if (_cfg.epochMs > 0.5) {
                // 秒级 epoch 动态：倍率只跟随输入中位数，floor 表示输出永不超额。
                const double rawK = targetHz * T / 1000.0;
                kNeed = (_cfg.rounding == 0) ? static_cast<int>(std::floor(rawK + 0.02))
                      : (_cfg.rounding == 2) ? static_cast<int>(std::ceil(rawK))
                                             : static_cast<int>(std::llround(rawK));
            } else {
                const double floorHz = targetHz * _cfg.floorShare;
                kNeed = static_cast<int>(std::ceil(floorHz * T / 1000.0 - 1e-9));
            }
            if (kNeed < 2) { kNeed = 2; }
            if (kNeed > jEff) { kNeed = jEff; }

            if (_cfg.policy == kPolicyFill) {
                // 相位 + 死区：想要 kNeed ±1，但只在误差超过 deadband 时切换，
                // 于是 λ=3.4 的输入会产生 3,4,4,4... 这样的"长跑"，而不是每帧
                // 都在 3/4 之间来回（每次切换都约等于丢一张输出）。
                const double lam = std::min(want, static_cast<double>(jEff));
                const double acc = _phase + lam;
                int kRound = static_cast<int>(std::lround(acc));
                if (kRound < 2) { kRound = 2; }
                if (kRound > jEff) { kRound = jEff; }
                // bridge 的 over-refresh guard：绝不请求超过本帧实际经过的 vblank 数
                // （ceil(want)），否则 XeFG 会把一批塞进更少的刷新周期里。
                const int kCap = std::clamp(
                    static_cast<int>(std::ceil(want - 1e-9)), 2, jEff);
                if (kRound > kCap) { kRound = kCap; }
                const double errKeep = std::fabs(acc - static_cast<double>(_lastK));
                if (kRound != _lastK && errKeep > _cfg.deadband) {
                    const int step = (kRound > _lastK) ? 1 : -1;
                    kNew = std::clamp(_lastK + step, 2, jEff);
                    if (kNew > kCap) { kNew = kCap; }
                    reason = (step > 0) ? kUp : kDown;
                } else {
                    kNew = _lastK;
                }
                _pendingLam = lam;
            } else if (_cfg.epochMs > 0.5) {
                // epoch 动态：k 直接取中位数换算值；是否真的换由下面的 epoch gate 决定。
                kNew = kNeed;
                reason = (kNeed > _lastK) ? kUp : ((kNeed < _lastK) ? kDown : kHeld);
                _downRun = 0;
            } else {
                // even：向上立即（绝不让输出塌下去），向下需要持续 downBatches。
                if (kNeed > _lastK) {
                    kNew = kNeed;
                    reason = kUp;
                    _downRun = 0;
                } else if (kNeed < _lastK) {
                    ++_downRun;
                    if (_downRun >= _cfg.downBatches) {
                        kNew = std::max(kNeed, _lastK - 1);
                        reason = kDown;
                        _downRun = 0;
                    }
                } else {
                    _downRun = 0;
                }
                _pendingLam = want;
            }
        }

        // ---- 切换门限：排空 + 最小驻留 --------------------------------------
        if (kNew > jEff) { kNew = jEff; reason = kCostCapped; }   // 上限优先
        if (kNew != _lastK) {
            const int dwell = LearnedDwell();
            // 秒级 epoch：在 epoch 内禁止任何换 k（包括 cap 重选），除非上限被压低。
            const int epochFrames = (_cfg.epochMs > 0.5)
                ? std::max(1, static_cast<int>(std::floor(_cfg.epochMs / std::max(T, 5.0))))
                : 0;
            const bool epochBlocked = epochFrames > 0 && _framesSinceSwitch < epochFrames;
            // 升档允许越过驻留（塌下去比多一次切换更明显）；降档必须等驻留。
            // overCap 时也允许立刻降：J_eff 已经容不下当前 k。
            const bool urgentUp = (kNew > _lastK) && (regime != kRegimeCapLimited) &&
                                  (epochFrames == 0);
            const bool allowed = s.canSwitch &&
                                 (overCap ||
                                  (!epochBlocked &&
                                   (_framesSinceSwitch >= dwell || urgentUp ||
                                    (regime == kRegimeCapLimited && !wasCap))));
            if (!allowed) {
                reason = kDeferred;
                ++_deferred;
                kNew = _lastK;
            }
        }

        // ---- 预算（frameRenderTime）-----------------------------------------
        // 默认给实测输入间隔：这正是 k 张输出要覆盖的时间，也不会像 k*V 那样
        // 反过来污染"输入间隔"的测量（2026-11-09 实测的正反馈）。
        double rt = T;
        if (_cfg.feedWholeVblanks && regime == kRegimeFillable) {
            rt = static_cast<double>(kNew) * V;
        }

        Decision d;
        d.k = kNew;
        d.renderTimeMs = rt;
        d.wantFrames = want;
        d.regime = regime;
        d.reason = reason;
        d.jEff = jEff;
        d.spacingVblanks = (kNew > 0) ? (slots / static_cast<double>(kNew)) : slots;
        d.targetHz = targetHz;
        // 诚实的输出帧率：kNew 张/真实帧，再受面板刷新率约束。cap-limited 时它
        // *低于* targetHz（例如 21 fps 输入、J=4 -> 84 Hz），日志与 OSD 应当报
        // 这个值，而不是继续声称目标已经达到。even/fill 路径以前从不填这一项，
        // 于是日志一直是 "out=0.0Hz"（只有 optimal 路径填过）。
        d.outputHz = std::min(targetHz,
                              (T > 0.1) ? (1000.0 / T) * static_cast<double>(kNew) : 0.0);
        d.switched = (kNew != _lastK);

        d.exact = std::fabs(d.spacingVblanks -
                            std::floor(d.spacingVblanks + 0.5)) <= 0.06;

        _plannedK = kNew;
        _hasPending = true;
        if (_cfg.policy != kPolicyFill || regime != kRegimeFillable) {
            _pendingLam = want;
        }
        _lastOut = d;
        return d;
    }

    // ---- 判决之后的实际操作结果（SetNumInterpolatedFrames 返回后调用）------
    // appliedK: 真正生效的 k（被 DLL 拒绝 / 排空失败时仍是旧值）。
    // 相位只减 appliedK —— 这是旧实现对不上账的根因：它减的是"想给的 k"，
    // 一旦延迟或拒绝，长期均值就在悄悄漂。
    void Commit(int appliedK, bool ok)
    {
        if (appliedK < 1) { appliedK = 1; }
        if (!_hasPending) { _lastK = appliedK; return; }

        const int wantK = _plannedK;
        const double lam = _pendingLam;

        if (ok && appliedK == wantK) {
            if (appliedK != _lastK) {
                ++_switches;
                _framesSinceSwitch = 0;
                _downRun = 0;
            } else {
                ++_framesSinceSwitch;
            }
            _lastM = _plannedM;
            _lastPaced = _plannedPaced;
        } else {
            // 想切但没切成：保持 applied，误差留在相位里（不丢账）。
            ++_deferred;
            if (appliedK != _lastK) { ++_switches; _framesSinceSwitch = 0; }
            else { ++_framesSinceSwitch; }
            // k 与计划不一致时，不能再声称"按 k*m 的整数格在上屏"。
            _lastPaced = false;
        }
        // 只按真正生效的 k 结算本帧，想给多的部分留给下一帧。
        _phase += lam - static_cast<double>(appliedK);

        // 相位夹取：只保留"欠/多一张"的量级（±1 帧，且不小于 deadband）。
        //
        // 旧公式 `deadband + 1 + 2*dwell` 在 dwell=12/15 时允许 ±25/31 帧的误差。
        // 但 k 的上界是本帧的 ceil(want)，这笔"欠账"根本还不掉——它只会让 k 长期
        // 贴在档位顶端，平均值被拉低，而且每次换档都要补一次大跳。
        // 仿真（tests/cadence_core 表 5b，J=4、每切换丢 1 张）：
        //   lock35  h8 108.3→110.9  h12 106.3→113.5  h15 107.0→114.5 Hz
        //   fluct   h8 106.6→107.0  h12 107.0→108.0  h15 107.6→108.3 Hz
        // 且 hold 越大不再越慢（115.1 Hz @ lim=0.5 / h15），可以继续用驻留换平稳。
        const double lim = std::max(_cfg.deadband, 1.0);
        _phase = std::clamp(_phase, -lim, lim);

        _lastK = appliedK;
        _hasPending = false;
    }

    // 一次计数切换之后 presented 与 expected 的差（>0 = 丢了输出）。
    // 持续 >0 说明"每帧切换"在这份 pacer 上不成立：学一个更长的驻留，
    // 并把 fill 方案退回 even（这是自动降级，不需要重开进程）。
    void NoteSwitchLoss(int lostOutputs)
    {
        if (lostOutputs < -2 || lostOutputs > 8) { return; } // 噪声/统计错位
        _switchLoss += (lostOutputs > 0) ? lostOutputs : 0;
        ++_switchSamples;
        if (_switchSamples >= 8) {
            const double per = static_cast<double>(_switchLoss) /
                               static_cast<double>(_switchSamples);
            _shownSwitchLoss = per;
            // 默认只测量：per 出现在日志/OSD 里，控制律不动。自动调驻留必须
            // --cad-switch-loss auto 显式打开 —— 这不是"更聪明"，而是在
            // "切换丢一张"与"节奏偏慢"之间换账，只有实测确认后才该自动做。
            if (_cfg.autoTuneDwell) {
                if (per > 0.5) {
                    _learnedDwell = std::min(_learnedDwell + 1, 60);
                    if (_cfg.policy == kPolicyFill) {
                        _cfg.policy = kPolicyEven;
                    }
                } else if (per < 0.1 && _learnedDwell > _cfg.minDwell) {
                    --_learnedDwell;
                }
            }
            _switchLoss = 0;
            _switchSamples = 0;
        }
    }

    // 最近一个统计窗口里"每次计数切换丢掉几张输出"的均值（-1 = 还没有样本）。
    double SwitchLossPerChange() const { return _shownSwitchLoss; }


    // ---- 可实现性反馈：XeFG 实际没出满 k 张 --------------------------------
    // 4K 下 4x 可能返回 ERR_OTHER 或只出 1 张（实测 CP2077），这时"理论上限 J"
    // 是不可实现的。连续 3 次不满就永久降一档；连续 400 次满档再尝试升回一档。
    void NoteOutputShortfall(int requestedK, int presentedFrames)
    {
        if (requestedK < 3) { return; }
        if (presentedFrames >= requestedK) {
            _shortRun = 0;
            if (_learnedMaxK < _cfg.maxOutputFrames && ++_okRun >= 400) {
                ++_learnedMaxK;
                _okRun = 0;
            }
            return;
        }
        _okRun = 0;
        if (++_shortRun >= 3) {
            _learnedMaxK = std::max(2, std::min(_learnedMaxK, requestedK - 1));
            _shortRun = 0;
        }
    }

    int LearnedMaxK() const { return _learnedMaxK; }

    // 忘掉"连续出帧不足"的计数。MV 门控（菜单/加载）期间 XeFG 被 SetEnabled(0)，
    // 每帧只出 1 张；如果把这些帧也算进 NoteOutputShortfall，连续 3 帧就足以把
    // learnedMaxK 永久降一档（实测：carry15 跑到一半 j 4→2，之后 120 Hz 再也上不去）。
    void ForgetShortfall() { _shortRun = 0; _okRun = 0; }


    // 实测的 FG 卡单张输出成本（ms），来自消费端每批 GPU 时间 / 输出张数。
    void SetCostPerOutputMs(double ms) { _cfg.costPerOutputMs = std::max(ms, 0.0); }

    // ---- 状态（日志/OSD 用）----------------------------------------------
    Decision Last() const { return _lastOut; }
    int  AppliedK() const { return _lastK; }
    double Phase() const { return _phase; }
    int  Policy() const { return _cfg.policy; }
    int  LearnedDwell() const { return std::max(_cfg.minDwell, _learnedDwell); }
    uint64_t Switches() const { return _switches; }
    uint64_t Deferred() const { return _deferred; }
    uint64_t CapFrames() const { return _capFrames; }
    uint64_t CostFrames() const { return _costFrames; }
    double EstimateMs() const { return _ema; }

  private:
    void ResetRuntime()
    {
        // 估计器、相位与 regime 的会话级状态；学习值（驻留、成本、统计）保留。
        _ringN = 0; _ringI = 0;
        _ema = 0.0;
        _haveInput = false;
        _downRun = 0;
        _phase = 0.0;
        _capLimited = false;
        _hitchRun = 0;
    }

    void sanitize()
    {
        if (_cfg.maxOutputFrames < 2) { _cfg.maxOutputFrames = 2; }
        if (_cfg.refreshHz < 1.0) { _cfg.refreshHz = 120.0; }
        if (_cfg.smooth < 1) { _cfg.smooth = 1; }
        if (_cfg.minDwell < 1) { _cfg.minDwell = 1; }
        if (_cfg.downBatches < 1) { _cfg.downBatches = 1; }
        if (_cfg.deadband < 0.0) { _cfg.deadband = 0.0; }
        if (_cfg.evenTolerance < 0.0) { _cfg.evenTolerance = 0.0; }
    }

    // ---- 输入间隔估计 ----------------------------------------------------
    // 16 样本中位数环 + 均值（单样本夹到 3x 中位数，避免一次 alt-tab 买到
    // 十几张），最后按 --cadence-smooth 做 EMA。
    static constexpr int kRing = 16;
    double _ring[kRing] = {};
    int    _ringN = 0;
    int    _ringI = 0;
    double _ema = 0.0;
    bool   _haveInput = false;
    int    _hitchRun = 0;

    void pushRing(double v)
    {
        _ring[_ringI] = v;
        _ringI = (_ringI + 1) % kRing;
        if (_ringN < kRing) { ++_ringN; }
    }

    double median() const
    {
        if (_ringN == 0) { return _ema; }
        double v[kRing];
        const int n = _ringN;
        for (int i = 0; i < n; ++i) { v[i] = _ring[i]; }
        for (int i = 1; i < n; ++i) {             // n <= 16
            const double key = v[i];
            int j = i - 1;
            while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
            v[j + 1] = key;
        }
        return v[n / 2];
    }

    double meanClamped() const
    {
        if (_ringN == 0) { return _ema; }
        const double med = median();
        const double cap = (med > 0.5) ? (3.0 * med) : 0.0;
        double sum = 0.0;
        for (int i = 0; i < _ringN; ++i) {
            double v = _ring[i];
            if (cap > 0.0 && v > cap) { v = cap; }
            sum += v;
        }
        return sum / static_cast<double>(_ringN);
    }

    double estimate(double raw)
    {
        (void) raw;
        // 启动阶段（Magpie XeSSFGTiming）：只有两个有效样本时取较短的那个，
        // 避免一个 400 ms 的首帧把偶数窗口的上中位数顶起来，进而连续几帧
        // 误判为"输入很慢"（cap-limited + 大 k）。三个样本起才进入均值+EMA。
        if (_ringN == 1) { _ema = _ring[0]; return _ema; }
        if (_ringN == 2) { _ema = std::min(_ring[0], _ring[1]); return _ema; }

        const double m = meanClamped();
        if (_ema <= 0.0) { _ema = m; return _ema; }
        const double a = 2.0 / (static_cast<double>(_cfg.smooth) + 1.0);
        _ema += (m - _ema) * a;
        return _ema;
    }

    // ---- kPolicyOptimal：在 (m,k,paced) 候选里按代价选 --------------------
    // 候选：
    //   paced   m ∈ [1..ceil(slots)+2], k = ceil(slots/m)（把输入压到 k*m 个
    //           vblank 的整数格；只允许"变慢"，且每帧等待 ≤ Λ）
    //   unpaced k ∈ [2..min(J,ceil(slots)+1)], m = round(slots/k)
    // 代价（§12.1）：平滑 q + 帧率缺口 + 延迟 + 输入丢帧 + 切换成本；
    // 只有新候选比当前好出 switchMargin 才换，且必须排空/驻留。
    Decision UpdateOptimal(double T, double V, double slots, double targetHz,
                           int jEff, int regime, bool overCap,
                           const Sample& s, double want)
    {
        const double R = _cfg.refreshHz;
        const double fIn = (T > 0.1) ? (1000.0 / T) : 0.0;
        const double lvl = 1000.0 / 60.0;   // 延迟归一化（60 fps 周期）

        if (regime == kRegimeInputLimited) {
            Decision d;
            d.k = 1; d.m = 1; d.paced = false; d.q = 0.0;
            d.outputHz = fIn;
            d.renderTimeMs = T;
            d.wantFrames = want;
            d.regime = regime;
            d.reason = kCostCapped;
            d.jEff = jEff;
            d.spacingVblanks = slots;
            d.targetHz = targetHz;
            d.exact = false;
            _plannedK = 1; _plannedM = 1; _plannedPaced = false;
            _hasPending = true; _pendingLam = 0.0;
            _lastOut = d;
            return d;
        }

        auto costOf = [&](int m, int k, bool paced, double* qOut, double* outHz) {
            if (k < 2 || k > jEff || m < 1) { return 1e9; }
            const double km = static_cast<double>(k) * static_cast<double>(m);
            if (paced) {
                const double batch = km * V;
                const double paceWait = batch - T;
                if (paceWait < -1e-6) { return 1e9; }                 // 不能加速输入
                if (paceWait > _cfg.latencyBudgetMs + 1e-9) { return 1e9; }
                const double base = 1000.0 / batch;
                const double alias = (fIn > 0.01)
                    ? (std::max(0.0, fIn - base) / fIn) : 0.0;
                const double lat = 0.5 * (k - 1) * T + 0.5 * batch;
                const double out = R / static_cast<double>(m);
                if (qOut) { *qOut = 0.0; }
                if (outHz) { *outHz = out; }
                return _cfg.wFps * (std::fabs(out - targetHz) / std::max(targetHz, 1.0))
                     + _cfg.wLatency * (lat / lvl)
                     + _cfg.wAlias * alias;
            }
            // q = 被多保持一个 vblank 的"输出张数"占比：每批有 |slots - k*m|
            // 个时隙的偏差，摊到 k 张输出上。
            const double q = std::fabs(slots - km) / std::max(1.0, static_cast<double>(k));
            const double rawOut = fIn * static_cast<double>(k);
            const double out = std::min(R, rawOut);
            // 超额供给：k·f 超过 R 的部分只能靠丢输入帧吸收（内容跳）。
            // out 被钳到 R 后 wFps 项对它是瞎的，所以单独计一项。
            const double over = (rawOut > R) ? (rawOut - R) / R : 0.0;
            // 感知死区：<=kImp 的空洞/丢帧在这一轮实测中不可辨（2x@58fps 的 3.3% 主观顺），
            // 所以这一段不计代价，超过部分才线性计价。
            const double qEff = std::max(0.0, q - kImp);
            const double overEff = std::max(0.0, over - kImp);
            const double lat = 0.5 * (k - 1) * T;
            if (qOut) { *qOut = q; }
            if (outHz) { *outHz = out; }
            return _cfg.wSmooth * qEff
                 + _cfg.wFps * (std::fabs(out - targetHz) / std::max(targetHz, 1.0))
                 + _cfg.wLatency * (lat / lvl)
                 + _cfg.wOver * overEff;
        };

        int bestM = _lastM > 0 ? _lastM : 1;
        int bestK = _lastK;
        bool bestPaced = _lastPaced;
        double bestQ = 0.0, bestOut = 0.0;
        double bestCost = costOf(bestM, bestK, bestPaced, &bestQ, &bestOut);
        const bool currentOk = bestCost < 1e8;

        const int mMax = std::min(8, std::max(1, static_cast<int>(std::ceil(slots)) + 2));
        if (_cfg.allowInputPacing) {
            for (int m = 1; m <= mMax; ++m) {
                int k = static_cast<int>(std::ceil(slots / static_cast<double>(m) - 1e-9));
                if (k < 2) { k = 2; }
                if (k > jEff) { k = jEff; }
                if (static_cast<double>(k) * m < slots - 1e-9) { continue; }
                double q = 0.0, out = 0.0;
                const double c = costOf(m, k, true, &q, &out);
                if (c < bestCost - 1e-9) {
                    bestCost = c; bestM = m; bestK = k; bestPaced = true;
                    bestQ = q; bestOut = out;
                }
            }
        }
        const int kMax = std::min(jEff, std::max(2, static_cast<int>(std::ceil(slots)) + 1));
        for (int k = 2; k <= kMax; ++k) {
            int m = static_cast<int>(std::llround(slots / static_cast<double>(k)));
            if (m < 1) { m = 1; }
            double q = 0.0, out = 0.0;
            const double c = costOf(m, k, false, &q, &out);
            if (c < bestCost - 1e-9) {
                bestCost = c; bestM = m; bestK = k; bestPaced = false;
                bestQ = q; bestOut = out;
            }
        }

        // 切换代价：换 k / 换 m / 改变 paced 都要付费，且必须真比当前好。
        const bool changed = (bestK != _lastK) || (bestM != _lastM) || (bestPaced != _lastPaced);
        if (changed) {
            double qCur = 0.0, outCur = 0.0;
            const double curCost = costOf(_lastM, _lastK, _lastPaced, &qCur, &outCur);
            const double penalty = _cfg.wSwitch * ((bestPaced != _lastPaced) ? 2.0 : 1.0);
            // 当前状态本身不可行（例如 paced 的等待超出 Λ）时允许无条件换。
            if (curCost < 1e8 && bestCost + penalty + _cfg.switchMargin > curCost) {
                bestM = _lastM; bestK = _lastK; bestPaced = _lastPaced;
                bestQ = qCur; bestOut = outCur; bestCost = curCost;
            }
        }
        const bool nowChanged = (bestK != _lastK) || (bestM != _lastM) || (bestPaced != _lastPaced);
        Reason reason = kHeld;
        if (nowChanged) {
            const int dwell = LearnedDwell();
            const bool urgentUp = (bestK > _lastK) && !bestPaced;
            const bool allowed = s.canSwitch &&
                (overCap || !currentOk || _framesSinceSwitch >= dwell || urgentUp);
            if (!allowed) {
                ++_deferred;
                bestM = _lastM; bestK = _lastK; bestPaced = _lastPaced;
                bestQ = 0.0; bestOut = 0.0;
                costOf(bestM, bestK, bestPaced, &bestQ, &bestOut);
                reason = kDeferred;
            } else {
                reason = (bestK > _lastK) ? kUp : ((bestK < _lastK) ? kDown
                       : (bestPaced && !_lastPaced) ? kEnterCap : kCapEven);
            }
        }
        if (!overCap && bestK > jEff) { bestK = jEff; reason = kCostCapped; }

        Decision d;
        d.k = bestK;
        d.m = bestM;
        d.paced = bestPaced;
        d.q = bestQ;
        d.outputHz = bestOut;
        d.spacingVblanks = bestPaced ? static_cast<double>(bestM)
                                     : (slots / static_cast<double>(bestK));
        d.renderTimeMs = bestPaced
            ? static_cast<double>(bestK) * static_cast<double>(bestM) * V
            : T;
        d.wantFrames = want;
        d.regime = regime;
        d.reason = reason;
        d.jEff = jEff;
        d.targetHz = targetHz;
        d.switched = (d.k != _lastK) || (d.m != _lastM) || (d.paced != _lastPaced);
        d.exact = bestPaced ||
            (std::fabs(d.spacingVblanks - std::floor(d.spacingVblanks + 0.5)) <= 0.06);

        _plannedK = d.k;
        _plannedM = d.m;
        _plannedPaced = d.paced;
        _hasPending = true;
        _pendingLam = 0.0;               // optimal 不依赖相位
        _lastOut = d;
        return d;
    }

    // 只能选一个常量 k：输出间距 = slots/k 个 vblank，越接近整数越顺。
    // 在 [J-2, J] 里挑残差最小的（并列取更大的 k = 更高输出率），并且只有
    // 比当前好出 evenImprove 才换（滞回，避免在边界上抖动）。
    int chooseAtCap(double slots, double spacingTarget, int jEff, int applied) const
    {
        if (slots < 1.0) { return 1; }
        const int lo = std::max(2, jEff - 2);
        // 两步选择：
        //   1) 残差在 evenTolerance 以内的候选里取最大的 k（同样"够整齐"时选更高
        //      输出率，避免 J=6 却停在 2x 这种明显浪费）；
        //   2) 一个都不满足时，取残差最小的（并列取更大的 k）。
        int best = -1;
        double bestRes = 1e9;
        int fallback = jEff;
        double fallbackRes = 1e9;
        for (int k = lo; k <= jEff; ++k) {
            const double s = slots / static_cast<double>(k);
            if (s < 1.0) { continue; }
            const double res = std::fabs(s - std::floor(s + 0.5));
            if (res < fallbackRes - 1e-9 ||
                (std::fabs(res - fallbackRes) <= 1e-9 && k > fallback)) {
                fallbackRes = res;
                fallback = k;
            }
            if (res <= _cfg.evenTolerance + 1e-9 && k > best) {
                best = k;
                bestRes = res;
            }
        }
        if (best < 0) { best = fallback; bestRes = fallbackRes; }
        // --fg-out-hz 的间距也参与：目标 2 vblank/张时，k 必须落在
        // slots/spacingTarget 附近，否则会把输出率抬回刷新率。
        const double kIdeal = slots / std::max(spacingTarget, 1.0);
        const int kCap = std::clamp(static_cast<int>(std::floor(kIdeal + 0.5)), lo, jEff);
        const double si = slots / static_cast<double>(kCap);
        const double resIdeal = std::fabs(si - std::floor(si + 0.5));
        if (resIdeal < bestRes - 1e-9 && kCap > 0) { best = kCap; bestRes = resIdeal; }

        if (best == applied) { return best; }
        const int a = (applied > 0) ? applied : 1;
        const double sA = slots / static_cast<double>(a);
        const double resA = std::fabs(sA - std::floor(sA + 0.5));
        // 升档：新 k 仍然"够整齐"就换（输出率更高不亏），只拒绝明显更差的；
        // 降档：必须比当前确实更整齐（margin 要大于相邻候选的残差差，否则会在
        // 两个都够整齐的 k 之间来回；slow21/J=5 的 k=3 与 k=5 只差 0.04）。
        if (best > applied) {
            const bool acceptable = (bestRes <= _cfg.evenTolerance + 1e-9 &&
                                     bestRes <= resA + 0.15);
            if (!acceptable && bestRes + _cfg.evenImprove > resA) { return applied; }
        } else {
            if (bestRes + _cfg.evenImprove > resA) { return applied; }
        }
        // 一次只走一格，避免大跳变。
        if (best > applied + 1) { return applied + 1; }
        if (best < applied - 1) { return applied - 1; }
        return best;
    }

    Config  _cfg {};
    int     _lastK = 2;
    int     _lastM = 1;
    bool    _lastPaced = false;
    int     _plannedK = 2;
    int     _plannedM = 1;
    bool    _plannedPaced = false;
    double  _pendingLam = 0.0;
    bool    _hasPending = false;
    int     _framesSinceSwitch = 1 << 20;
    int     _downRun = 0;
    double  _phase = 0.0;
    bool    _capLimited = false;
    uint64_t _switches = 0;
    uint64_t _deferred = 0;
    uint64_t _capFrames = 0;
    uint64_t _costFrames = 0;
    int     _switchLoss = 0;
    int     _switchSamples = 0;
    double  _shownSwitchLoss = -1.0;  // 最近一次的"每切换丢几张"均值

    int     _learnedDwell = 4;
    int     _learnedMaxK = 4;   // 实测可实现的 k 上限（NoteOutputShortfall）
    int     _shortRun = 0;      // 连续少出帧次数
    int     _okRun = 0;         // 连续满帧次数（用于试探性升回）
    // 锁定策略：新 k 需要连续 kLockVotesNeed 帧指向它才切换。
    static constexpr int kLockVotesNeed = 3;
    // 换档所需的"输出率优势"：新 k 至少要少 3% 的失真才值得换（借 optimal 的死区）。
    static constexpr double kLockMargin = 0.03;
    // 感知死区：空洞/丢帧比例在这个以内不计代价（实测 3.3% 空洞主观可接受）。
    static constexpr double kImp = 0.05;


    int     _lockedK = 0;       // 当前锁定的 k（0 = 尚未初始化）
    int     _lockPending = 0;   // 候选 k
    int     _lockVotes = 0;     // 候选连续出现的次数

    Decision _lastOut {};
};

} // namespace mgpu_cad
