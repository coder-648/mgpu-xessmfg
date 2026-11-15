// ============================================================================
//  tests/cadence_core/sim.cpp  -  consumer 动态逐帧控制器（任意上限）的仿真
//
//  纯 C++17，无平台依赖：
//      g++ -std=c++17 -O2 -Wall -Wextra tests/cadence_core/sim.cpp -o /tmp/sim
//      /tmp/sim
//
//  目的：在改动 Windows 侧之前，用确定性输入把"输出间距是否整齐 / 计数切换
//  频率 / 空刷新与重复帧比例"量化出来，覆盖 J = 2..9 的任意上限，以及
//  "输入慢到上限不够"（cap-limited）这一当前实现没有定义行为的区间。
//
//  模型（与 cadence.cpp / frame.cpp 一一对应）：
//    * 每来一个真实帧，控制器给出 k（输出张数）与 renderTime（批次预算）；
//    * 批次把 k 张均匀铺在 [t, t + renderTime] 上（XeFG 的 pacer 行为）；
//    * 面板只在 vblank 上显示：每张输出向上取到最近的 vblank，同一 vblank
//      的第二张被丢弃（over-subscribe = double）；
//    * 相邻两张输出之间空出的 vblank 记为 hole（漏拍）；
//    * switchLoss > 0 时，一次计数切换让该批少出一张（这份 pacer 的实测行为）。
//
//  SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "../../src/cadence_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

struct Pattern
{
    const char* name;
    double baseMs;
    double jitterMs;
    int    kind;      // 0 steady, 1 uniform, 2 occasional long, 3 step
};

std::vector<double> makeIntervals(const Pattern& p, int n)
{
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::vector<double> v;
    v.reserve(static_cast<size_t>(n));
    double base = p.baseMs;
    for (int i = 0; i < n; ++i) {
        double dt = p.baseMs;
        switch (p.kind) {
        case 1:
            dt = p.baseMs + p.jitterMs * u(rng);
            break;
        case 2:
            dt = p.baseMs + p.jitterMs * u(rng);
            if ((rng() % 100u) < 10u) { dt += p.baseMs * 1.8; }
            break;
        case 3:
            if (i > 0 && (i % 240) == 0) {
                base = (base < 30.0) ? 50.0 : ((base < 45.0) ? 30.0 : 25.0);
            }
            dt = base;
            break;
        default:
            dt = p.baseMs;
            break;
        }
        v.push_back(dt);
    }
    return v;
}

enum Ctrl
{
    kCtrlGov = 0,      // 现状默认：--cadence-source governor
    kCtrlBres = 1,     // 现状 A/B：mean+phase（每帧都可能改）
    kCtrlCoreEven = 2, // 新核心：even（恒定 k 优先）
    kCtrlCoreFill = 3, // 新核心：fill（死区相位，少切换地补满）
    kCtrlCoreOptimal = 4, // 新核心：optimal（(m,k,paced) 代价最小）
    kCtrlCoreLocked = 5   // 策略 v2：锁一个整数 k，预算 k*vblank（间距 = 1 vb）
};

const char* ctrlName(int c)
{
    switch (c) {
    case kCtrlGov:      return "gov";
    case kCtrlBres:     return "bres";
    case kCtrlCoreEven: return "even";
    case kCtrlCoreFill: return "fill";
    case kCtrlCoreLocked: return "lock";
    default:            return "opt";
    }
}

struct Result
{
    double outHz = 0.0;
    double unevenPct = 0.0;     // 相邻输出间距偏离"均值取整"的比例（节奏不齐）
    double doublePct = 0.0;     // 同一 vblank 被塞两张而丢弃
    double switchesPerSec = 0.0;
    double spacingMean = 0.0;   // vblank / 张
    double spacingStd = 0.0;    // 间距抖动（越小越顺）
    double capPct = 0.0;        // 处于 cap-limited 的时间占比
    double maxPhase = 0.0;
    long long lostBySwitch = 0;
    double exactPct = 0.0;      // 决策时"间距是整 vblank"的占比
    int    maxKAfterChange = 0; // Reconfigure 之后出现过的最大 k
    double intervalP50 = 0, intervalP95 = 0, intervalP99 = 0; // 输出提交间隔分位（vb）
    double finalEstimateMs = 0; // 结束时的输入间隔估计
    double pacedPct = 0.0;      // 决策中"主动量化输入"的占比
    double meanK = 0.0, meanM = 0.0, meanQ = 0.0;
};

struct Options
{
    int    cap = 4;
    double refreshHz = 120.0;
    double outHz = 0.0;
    int    switchLoss = 0;     // 每次计数切换丢几张输出
    double deadband = 0.60;    // fill 方案的相位死区
    int    hold = 4;           // fill 方案的驻留（== --cadence-hold N，core 的 minDwell）

    double costPerOutputMs = 0.0;
    double latencyBudgetMs = 2.0; // optimal：Λ
    int    warmup = 80;        // 前 N 个真实帧不统计
    int    capAfter = 0;       // >0：在 changeAt 处把 J 改成这个值（Reconfigure）
    size_t changeAt = static_cast<size_t>(-1);
    size_t resetAt = static_cast<size_t>(-1); // 该帧 Sample.reset = true（会话/换代）
};

Result simulate(int ctrl, const std::vector<double>& dt, const Options& o)
{
    const double V = 1000.0 / o.refreshHz;
    const double targetHz = (o.outHz > 1.0) ? std::min(o.outHz, o.refreshHz) : o.refreshHz;

    mgpu_cad::Config cc;
    cc.refreshHz = o.refreshHz;
    cc.maxOutputFrames = o.cap;
    cc.outHz = o.outHz;
    cc.policy = (ctrl == kCtrlCoreOptimal) ? mgpu_cad::kPolicyOptimal
              : (ctrl == kCtrlCoreFill) ? mgpu_cad::kPolicyFill
              : (ctrl == kCtrlCoreLocked) ? mgpu_cad::kPolicyLocked
                                        : mgpu_cad::kPolicyEven;
    cc.deadband = o.deadband;
    cc.minDwell = (o.hold >= 1) ? o.hold : 1;
    cc.costPerOutputMs = o.costPerOutputMs;

    cc.latencyBudgetMs = o.latencyBudgetMs;
    mgpu_cad::CadenceCore core;
    core.Configure(cc);

    int    applied = 2;       // 这一帧之前 DLL 里的 k
    double ema = 0.0;
    double phase = 0.0;
    int    downRun = 0;

    struct Out { double t; long long idx; };
    std::vector<Out> outs;
    outs.reserve(dt.size() * static_cast<size_t>(o.cap) + 64);

    double t = 0.0;
    double tStart = 0.0;
    bool   haveStart = false;
    long long switches = 0;
    long long lostBySwitch = 0;
    long long capDecisions = 0, decisions = 0;
    long long exactDecisions = 0;
    int r_maxKAfter = 0;
    double maxPhase = 0.0;
    double tPaceNext = 0.0;                 // 主动量化时的下一批最早提交时间
    long long pacedN = 0;
    double sumK = 0.0, sumM = 0.0, sumQ = 0.0;

    for (size_t i = 0; i < dt.size(); ++i) {
        const double gap = dt[i];
        t += gap;

        int    k  = applied;
        double rt = gap;
        double tBase = t;                   // 本批实际开始时间（paced 时被推迟）

        if (ctrl == kCtrlGov) {
            if (ema <= 0.0) { ema = gap; } else { ema += 0.1 * (gap - ema); }
            const double T = ema;
            int kNeed = static_cast<int>(std::ceil(0.96 * targetHz * T / 1000.0 - 1e-9));
            if (kNeed < 2) { kNeed = 2; }
            if (kNeed > o.cap) { kNeed = o.cap; }
            if (kNeed > applied) { k = kNeed; downRun = 0; }
            else if (kNeed < applied) {
                ++downRun;
                if (downRun >= 24) { k = kNeed; downRun = 0; }
            } else {
                downRun = 0;
            }
            rt = T;
        } else if (ctrl == kCtrlBres) {
            const double slots = gap / V;
            phase += std::min(slots * (targetHz / o.refreshHz),
                              static_cast<double>(o.cap));
            int kk = static_cast<int>(std::lround(phase));
            if (kk < 2) { kk = 2; }
            if (kk > o.cap) { kk = o.cap; }
            phase -= kk;
            phase = std::clamp(phase, -0.5, 0.5);
            k = kk;
            rt = gap;
            maxPhase = std::max(maxPhase, std::fabs(phase));
        } else {
            if (o.capAfter >= 2 && i == o.changeAt) {
                mgpu_cad::Config cc2 = cc;
                cc2.maxOutputFrames = o.capAfter;
                core.Reconfigure(cc2);
            }
            mgpu_cad::Sample s;
            s.nowMs = t;
            s.inputMs = gap;
            s.canSwitch = true;
            s.reset = (i == o.resetAt);
            const mgpu_cad::Decision d = core.Update(s);
            core.Commit(d.k, true);
            k = d.k;
            rt = d.renderTimeMs;
            // 主动量化：把本批推迟到上一个 batch 的整数格；积压超过一批就
            // 丢弃旧帧重新起锚（真实 consumer 也只会拿最新帧）。
            if (d.paced) {
                const double batch = static_cast<double>(d.k) * d.m * V;
                tBase = std::max(t, tPaceNext);
                if (tBase > t + batch) { tBase = t; }
                tPaceNext = tBase + batch;
            } else {
                tPaceNext = t;
            }
            if (o.changeAt != static_cast<size_t>(-1) && i >= o.changeAt) {
                if (k > r_maxKAfter) { r_maxKAfter = k; }
            }
            maxPhase = std::max(maxPhase, std::fabs(core.Phase()));
            if (static_cast<int>(i) >= o.warmup) {
                ++decisions;
                if (d.regime == mgpu_cad::kRegimeCapLimited) { ++capDecisions; }
                if (d.exact) { ++exactDecisions; }
                if (d.paced) { ++pacedN; }
                sumK += d.k; sumM += d.m; sumQ += d.q;
            }
        }

        const bool switched = (k != applied);
        if (switched) { ++switches; }
        if (static_cast<int>(i) >= o.warmup && !haveStart) {
            haveStart = true;
            tStart = t;
        }

        int emit = switched ? (k - o.switchLoss) : k;
        if (emit < 1) { emit = 1; }
        if (k < 1) { k = 1; }
        if (switched && o.switchLoss > 0) {
            lostBySwitch += o.switchLoss;   // 统计"切换成本"造成的丢失
        }
        for (int j = 0; j < emit; ++j) {
            const double tOut = tBase + static_cast<double>(j) * rt / static_cast<double>(k);
            long long idx = static_cast<long long>(std::ceil(tOut / V - 1e-9));
            outs.push_back(Out { tOut, idx });
        }
        applied = k;
    }

    Result r;
    if (!haveStart || outs.empty()) { return r; }

    std::vector<long long> diffs;
    diffs.reserve(outs.size());
    long long doubles = 0, shown = 0;
    long long lastIdx = -1;
    double tFirst = 0.0, tLast = 0.0;

    size_t start = 0;
    while (start < outs.size() && outs[start].t < tStart) { ++start; }
    if (start >= outs.size()) { return r; }
    for (size_t i = start; i < outs.size(); ++i) {
        const Out& oo = outs[i];
        if (lastIdx < 0) {
            lastIdx = oo.idx;
            tFirst = oo.t;
            ++shown;
            continue;
        }
        if (oo.idx <= lastIdx) {
            ++doubles;                      // 同一 vblank 的第二张：被面板丢弃
            continue;
        }
        diffs.push_back(oo.idx - lastIdx);
        lastIdx = oo.idx;
        tLast = oo.t;
        ++shown;
    }
    if (!diffs.empty()) {
        double sum = 0.0, sum2 = 0.0;
        for (long long d : diffs) {
            const double dv = static_cast<double>(d);
            sum += dv;
            sum2 += dv * dv;
        }
        r.spacingMean = sum / static_cast<double>(diffs.size());
        const double var = sum2 / static_cast<double>(diffs.size()) -
                           r.spacingMean * r.spacingMean;
        r.spacingStd = std::sqrt(std::max(var, 0.0));
        // 输出提交间隔分位（与 Magpie PresentationIntervalQueue 的 p50/p95/p99
        // 同一口径：这是提交间隔，不是显示事件）。
        std::vector<long long> sorted = diffs;
        std::sort(sorted.begin(), sorted.end());
        const auto pick = [&sorted](double p) {
            const size_t idx = static_cast<size_t>(
                (static_cast<double>(sorted.size() - 1)) * p / 100.0);
            return static_cast<double>(sorted[idx]);
        };
        r.intervalP50 = pick(50.0);
        r.intervalP95 = pick(95.0);
        r.intervalP99 = pick(99.0);
        // 节奏不齐：某段间距与"均值取整"相差 >= 1 个 vblank（1,1,2 这类
        // 交替在这里各占一半，正是眼睛看到的"抖"）。
        const long long whole = static_cast<long long>(std::floor(r.spacingMean + 0.5));
        long long off = 0;
        for (long long d : diffs) {
            const long long dev = (d > whole) ? (d - whole) : (whole - d);
            if (dev >= 1) { ++off; }
        }
        r.unevenPct = 100.0 * static_cast<double>(off) /
                      static_cast<double>(diffs.size());
    }
    if (tLast > tFirst) {
        const double secs = (tLast - tFirst) / 1000.0;
        r.outHz = static_cast<double>(shown) / secs;
        const double span = (t / 1000.0) - (tStart / 1000.0);
        if (span > 0.2) { r.switchesPerSec = static_cast<double>(switches) / span; }
    }
    if (shown > 0) {
        r.doublePct = 100.0 * static_cast<double>(doubles) /
                      static_cast<double>(shown + doubles);
    }
    if (decisions > 0) {
        r.capPct = 100.0 * static_cast<double>(capDecisions) /
                   static_cast<double>(decisions);
        r.exactPct = 100.0 * static_cast<double>(exactDecisions) /
                     static_cast<double>(decisions);
        r.pacedPct = 100.0 * static_cast<double>(pacedN) /
                     static_cast<double>(decisions);
        r.meanK = sumK / static_cast<double>(decisions);
        r.meanM = sumM / static_cast<double>(decisions);
        r.meanQ = sumQ / static_cast<double>(decisions);
    }
    r.maxPhase = maxPhase;
    r.lostBySwitch = lostBySwitch;
    r.maxKAfterChange = r_maxKAfter;
    r.finalEstimateMs = (ctrl == kCtrlCoreEven || ctrl == kCtrlCoreFill)
                      ? core.EstimateMs() : ema;
    return r;
}

} // namespace

namespace {

void printRow(const char* pat, int cap, int ctrl, int loss, const Result& r)
{
    std::printf("%-12s J=%d %-5s loss=%d | out %6.1f Hz  uneven %5.2f%%  dbl %5.2f%%"
                "  sw %5.2f/s  spacing %5.2f+-%.2f vb  cap %5.1f%%  lost %lld\n",
                pat, cap, ctrlName(ctrl), loss,
                r.outHz, r.unevenPct, r.doublePct, r.switchesPerSec,
                r.spacingMean, r.spacingStd, r.capPct, r.lostBySwitch);
}

const Pattern* findPat(const Pattern* pats, int n, const char* name)
{
    for (int i = 0; i < n; ++i) {
        if (std::string(pats[i].name) == name) { return &pats[i]; }
    }
    return nullptr;
}

} // namespace

int main()
{
    const Pattern pats[] = {
        { "steady16.7", 16.67, 0.0, 0 },   // 120/8 = 15 -> 8x
        { "locked40",   25.00, 0.0, 0 },   // 3.00 vblank
        { "lock35",     28.57, 0.0, 0 },   // 3.43 vblank
        { "fluct25-40", 32.50, 7.5, 1 },   // 2.4..4.8 vblank
        { "bursty",     30.00, 3.0, 2 },   // 偶发长帧
        { "loadstep",   25.00, 0.0, 3 },   // 25/50/30 阶跃
        { "slow21",     47.60, 2.0, 1 },   // 5.7 vblank -> cap-limited at J<6
        { "slow15",     66.67, 3.0, 1 },   // 8.0 vblank -> cap-limited at J<8
    };
    const int nPats = static_cast<int>(sizeof pats / sizeof pats[0]);
    const int N = 1400;

    std::printf("=== table 1: J=4, one lost output per count switch (measured pacer) ===\n");
    for (int pi = 0; pi < nPats; ++pi) {
        const std::vector<double> dt = makeIntervals(pats[pi], N);
        for (int ctrl : { kCtrlGov, kCtrlBres, kCtrlCoreEven, kCtrlCoreFill, kCtrlCoreOptimal }) {
            Options o; o.cap = 4; o.switchLoss = 1;
            printRow(pats[pi].name, o.cap, ctrl, o.switchLoss, simulate(ctrl, dt, o));
        }
        std::printf("\n");
    }

    std::printf("=== table 2: cap sweep (J = 2..8), switchLoss=1 ===\n");
    for (const char* want : { "fluct25-40", "slow21", "slow15" }) {
        const Pattern* pp = findPat(pats, nPats, want);
        if (!pp) { continue; }
        const std::vector<double> dt = makeIntervals(*pp, N);
        for (int cap : { 2, 3, 4, 5, 6, 8 }) {
            for (int ctrl : { kCtrlCoreEven, kCtrlCoreFill, kCtrlCoreOptimal }) {
                Options o; o.cap = cap; o.switchLoss = 1;
                printRow(pp->name, cap, ctrl, o.switchLoss, simulate(ctrl, dt, o));
            }
        }
        std::printf("\n");
    }

    std::printf("=== table 3: switch-loss sensitivity, J=4 ===\n");
    for (const char* want : { "locked40", "lock35", "fluct25-40" }) {
        const Pattern* pp = findPat(pats, nPats, want);
        if (!pp) { continue; }
        const std::vector<double> dt = makeIntervals(*pp, N);
        for (int loss : { 0, 1, 2 }) {
            for (int ctrl : { kCtrlCoreEven, kCtrlCoreFill, kCtrlCoreOptimal }) {
                Options o; o.cap = 4; o.switchLoss = loss;
                printRow(pp->name, o.cap, ctrl, loss, simulate(ctrl, dt, o));
            }
        }
        std::printf("\n");
    }

    std::printf("=== table 4: --fg-out-hz 60 (2 vblank/张), J=4, switchLoss=1 ===\n");
    for (const char* want : { "locked40", "fluct25-40", "slow15" }) {
        const Pattern* pp = findPat(pats, nPats, want);
        if (!pp) { continue; }
        const std::vector<double> dt = makeIntervals(*pp, N);
        for (int ctrl : { kCtrlCoreEven, kCtrlCoreFill, kCtrlCoreOptimal }) {
            Options o; o.cap = 4; o.switchLoss = 1; o.outHz = 60.0;
            printRow(pp->name, o.cap, ctrl, o.switchLoss, simulate(ctrl, dt, o));
        }
        std::printf("\n");
    }

    std::printf("=== table 5: fill 方案死区扫描（J=4, switchLoss=1）——切换成本 vs 节奏 ===\n");
    for (const char* want : { "lock35", "fluct25-40" }) {
        const Pattern* pp = findPat(pats, nPats, want);
        if (!pp) { continue; }
        const std::vector<double> dt = makeIntervals(*pp, N);
        for (double db : { 0.5, 0.8, 1.2, 1.8 }) {
            Options o; o.cap = 4; o.switchLoss = 1; o.deadband = db;
            const Result r = simulate(kCtrlCoreFill, dt, o);
            std::printf("%-12s J=4 fill  deadband=%.1f | out %6.1f Hz  uneven %5.2f%%"
                        "  dbl %5.2f%%  sw %5.2f/s  spacing %5.2f+-%.2f vb  lost %lld\n",
                        pp->name, db, r.outHz, r.unevenPct, r.doublePct,
                        r.switchesPerSec, r.spacingMean, r.spacingStd, r.lostBySwitch);
        }
        std::printf("\n");
    }

    std::printf("=== table 5b: fill 驻留（hold = core.minDwell, 对应 --cadence-hold）扫描 ===\n");
    std::printf("    U1 的 carry8/12/15 实机 A/B 之前，先看纯模型里驻留把开关次数压到多少、\n");
    std::printf("    代价是输出均值掉多少（J=4, switchLoss=1）。\n");
    for (const char* want : { "locked40", "lock35", "fluct25-40", "loadstep" }) {
        const Pattern* pp = findPat(pats, nPats, want);
        if (!pp) { continue; }
        const std::vector<double> dt = makeIntervals(*pp, N);
        for (int hold : { 4, 8, 12, 15 }) {
            Options o; o.cap = 4; o.switchLoss = 1; o.hold = hold;
            const Result r = simulate(kCtrlCoreFill, dt, o);
            std::printf("%-12s J=4 fill  hold=%2d | out %6.1f Hz  uneven %5.2f%%"
                        "  dbl %5.2f%%  sw %5.2f/s  spacing %5.2f+-%.2f vb  lost %lld\n",
                        pp->name, hold, r.outHz, r.unevenPct, r.doublePct,
                        r.switchesPerSec, r.spacingMean, r.spacingStd, r.lostBySwitch);
        }
        std::printf("\n");
    }
    std::printf("=== table 5c: locked (k=ceil, budget=k*vblank) vs fill(carry) ===\n");
    for (const char* want : { "locked40", "lock35", "fluct25-40", "loadstep", "slow21" }) {
        const Pattern* pp = findPat(pats, nPats, want);
        if (!pp) { continue; }
        const std::vector<double> dt = makeIntervals(*pp, N);
        for (int ctrl : { kCtrlCoreFill, kCtrlCoreLocked }) {
            Options o; o.cap = 4; o.switchLoss = 1;
            const Result r = simulate(ctrl, dt, o);
            std::printf("%-12s %-5s | out %6.1f Hz  uneven %5.2f%%  dbl %5.2f%%"
                        "  sw %5.2f/s  spacing %5.2f+-%.2f vb  lost %lld\n",
                        pp->name, ctrlName(ctrl), r.outHz, r.unevenPct, r.doublePct,
                        r.switchesPerSec, r.spacingMean, r.spacingStd, r.lostBySwitch);
        }
        std::printf("\n");
    }




    {
        // 首帧 400 ms，随后 16.7 ms：bootstrap 取前两个样本的较短值，
        // 估计必须在第 2 个样本就回到真实量级。
        std::vector<double> dt;
        dt.reserve(600);
        dt.push_back(400.0);
        for (int i = 1; i < 600; ++i) { dt.push_back(16.7); }
        Options o; o.cap = 8; o.switchLoss = 1; o.warmup = 10;
        for (int ctrl : { kCtrlCoreEven, kCtrlCoreFill, kCtrlCoreOptimal }) {
            const Result r = simulate(ctrl, dt, o);
            std::printf("cold-start 400ms->16.7ms %-5s | out %6.1f Hz  uneven %5.2f%%"
                        "  p50 %3.0f p95 %3.0f p99 %3.0f vb  est %.2f ms\n",
                        ctrlName(ctrl), r.outHz, r.unevenPct,
                        r.intervalP50, r.intervalP95, r.intervalP99, r.finalEstimateMs);
        }
    }
    {
        // 会话/资源换代：15 fps -> 60 fps，reset 后必须重新起锚，不沿用旧相位。
        std::vector<double> dt;
        dt.reserve(1100);
        for (int i = 0; i < 700; ++i) { dt.push_back(66.67 + ((i % 7) - 3) * 0.5); }
        for (int i = 0; i < 400; ++i) { dt.push_back(16.7); }
        Options o; o.cap = 4; o.switchLoss = 1; o.warmup = 700; o.resetAt = 700;
        for (int ctrl : { kCtrlCoreEven, kCtrlCoreFill, kCtrlCoreOptimal }) {
            const Result r = simulate(ctrl, dt, o);
            std::printf("reset@700 15fps->60fps %-5s | out %6.1f Hz  uneven %5.2f%%"
                        "  p50 %3.0f p95 %3.0f p99 %3.0f vb  est %.2f ms\n",
                        ctrlName(ctrl), r.outHz, r.unevenPct,
                        r.intervalP50, r.intervalP95, r.intervalP99, r.finalEstimateMs);
        }
    }
    std::printf("\n");

    std::printf("=== table 7: optimal（(m,k,paced) 代价最小），J=4 ===\n");
    for (int pi = 0; pi < nPats; ++pi) {
        const std::vector<double> dt = makeIntervals(pats[pi], N);
        Options o; o.cap = 4; o.switchLoss = 1;
        const Result r = simulate(kCtrlCoreOptimal, dt, o);
        std::printf("%-12s J=4 opt | out %6.1f Hz  k %.2f  m %.2f  paced %5.1f%%"
                    "  q %.3f  uneven %5.2f%%  sw %5.2f/s  cap %5.1f%%\n",
                    pats[pi].name, r.outHz, r.meanK, r.meanM, r.pacedPct,
                    r.meanQ, r.unevenPct, r.switchesPerSec, r.capPct);
    }
    {
        // 240 Hz 面板 + 30 fps 输入：m=2,k=4 应给出 120 Hz、q=0。
        const std::vector<double> dt = makeIntervals(Pattern { "steady30", 33.333, 0.0, 0 }, N);
        Options o; o.cap = 4; o.switchLoss = 1; o.refreshHz = 240.0;
        const Result r = simulate(kCtrlCoreOptimal, dt, o);
        std::printf("%-12s J=4 opt | out %6.1f Hz  k %.2f  m %.2f  paced %5.1f%%"
                    "  q %.3f  uneven %5.2f%%  sw %5.2f/s  cap %5.1f%%\n",
                    "steady30@240", r.outHz, r.meanK, r.meanM, r.pacedPct,
                    r.meanQ, r.unevenPct, r.switchesPerSec, r.capPct);
    }
    std::printf("\n");

    std::printf("=== table 8: 阶跃响应 43fps(23.26ms) -> 30fps(33.33ms) ===\n");
    std::printf("    k 从 3 切到 4 需要多少帧（串联平滑的总滞后）\n");
    for (int ctrl : { kCtrlCoreOptimal, kCtrlCoreFill, kCtrlCoreLocked }) {
        mgpu_cad::Config cc;
        cc.refreshHz = 120.0;
        cc.maxOutputFrames = 4;
        cc.policy = (ctrl == kCtrlCoreOptimal) ? mgpu_cad::kPolicyOptimal
                  : (ctrl == kCtrlCoreFill) ? mgpu_cad::kPolicyFill
                                            : mgpu_cad::kPolicyLocked;
        mgpu_cad::CadenceCore core;
        core.Configure(cc);
        int settle = -1; double t = 0.0; int kAt = 3;
        for (int i = 0; i < 400; ++i) {
            const double gap = (i < 200) ? 23.26 : 33.33;
            t += gap;
            mgpu_cad::Sample s; s.nowMs = t; s.inputMs = gap; s.canSwitch = true;
            const mgpu_cad::Decision d = core.Update(s);
            core.Commit(d.k, true);
            if (i >= 200) { kAt = d.k; if (d.k >= 4 && settle < 0) { settle = i - 200 + 1; } }
        }
        if (settle < 0) { settle = -1; }
        std::printf("  %-5s settle=%3d frames (%.0f ms)  k=%d\n", ctrlName(ctrl), settle,
                    (settle > 0) ? settle * 33.33 : -1.0, kAt);
    }
    std::printf("\n");

    // ---- structural checks（不依赖调参，只验证设计不变量）----------------
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) { ++failures; }
    };

    {
        // 任意上限都不能把输出推过面板（允许 pacer 的 2% 数值误差）
        bool ok = true;
        for (int cap : { 2, 3, 4, 5, 6, 8 }) {
            const std::vector<double> dt = makeIntervals(pats[3], N);
            Options o; o.cap = cap; o.switchLoss = 1;
            for (int ctrl : { kCtrlCoreEven, kCtrlCoreFill, kCtrlCoreOptimal }) {
                const Result r = simulate(ctrl, dt, o);
                if (r.outHz > 122.5) { ok = false; }
            }
        }
        check(ok, "任意 J：输出帧率不超过面板（<= 102%）");
    }
    {
        // 上限不够时必须进入 cap-limited，且不允许再按"每 vblank 一张"抖动
        const std::vector<double> dt = makeIntervals(pats[7], N);  // slow15: 8 vblank
        for (int cap : { 2, 3, 4, 5, 6 }) {
            Options o; o.cap = cap; o.switchLoss = 1;
            const Result r = simulate(kCtrlCoreEven, dt, o);
            char buf[200];
            std::snprintf(buf, sizeof buf,
                          "slow15 + J=%d 识别为 cap-limited（capPct %.0f%%, out %.1f Hz,"
                          " 间距 %.2f+-%.2f vb）",
                          cap, r.capPct, r.outHz, r.spacingMean, r.spacingStd);
            check(r.capPct > 90.0 && r.spacingMean > 1.4, buf);
        }
    }
    {
        // 恒定倍率在稳定输入下不应反复切换
        const std::vector<double> dt = makeIntervals(pats[1], N);  // locked40
        Options o; o.cap = 4; o.switchLoss = 1;
        const Result r = simulate(kCtrlCoreEven, dt, o);
        check(r.switchesPerSec < 1.0, "locked 40 + J=4：计数切换 < 1 次/秒");
        check(r.unevenPct < 1.0 && r.doublePct < 1.0,
              "locked 40 + J=4：节奏不齐与重复帧均 < 1%");
    }
    {
        // 相位必须有界（长期均值不丢账）
        const std::vector<double> dt = makeIntervals(pats[3], N);
        Options o; o.cap = 4; o.switchLoss = 1;
        const Result r = simulate(kCtrlCoreFill, dt, o);
        check(r.maxPhase < 6.0, "fill：相位有界（|phase| < 6 帧）");
    }
    {
        // cap-limited 必须"如实报输出率"：21 fps 输入、J=4 最高 4x = 84 Hz，
        // outputHz 不能继续声称 120。fill/even 路径以前从不填这一项（日志恒为
        // out=0.0Hz），第 4 项"cap-limited 明确化"就无从谈起。
        mgpu_cad::Config cc;
        cc.refreshHz = 120.0;
        cc.maxOutputFrames = 4;
        cc.policy = mgpu_cad::kPolicyFill;
        cc.minDwell = 8;
        mgpu_cad::CadenceCore core;
        core.Configure(cc);
        mgpu_cad::Decision d;
        for (int i = 0; i < 160; ++i) {
            mgpu_cad::Sample s;
            s.nowMs = 47.6 * static_cast<double>(i + 1);
            s.inputMs = 47.6;
            s.canSwitch = true;
            d = core.Update(s);
            core.Commit(d.k, true);
        }
        check(d.regime == mgpu_cad::kRegimeCapLimited &&
              d.outputHz > 60.0 && d.outputHz < 110.0,
              "fill cap-limited：outputHz 报真实输出（~84 Hz，而不是 120）");
        check(d.outputHz <= 4.0 * (1000.0 / 47.6) * 1.02,
              "fill：outputHz 不超过 J x 输入率");
    }

    {
        // 输入快于面板：只能 1:1，不允许再叠加生成
        const std::vector<double> dt = makeIntervals(Pattern { "fast200", 5.0, 0.0, 0 }, 800);
        Options o; o.cap = 4; o.switchLoss = 1;
        const Result r = simulate(kCtrlCoreEven, dt, o);
        check(r.outHz <= 122.0, "输入 200fps + J=4：不会把输出抬过面板");
    }

    {
        // --fg-out-hz 60 配 40 fps 输入：1.5 张/输入，任何整数 k 都落不到整
        // vblank。控制器必须如实标记（exact=false）而不是假装平顺。
        const std::vector<double> dt = makeIntervals(pats[1], N);  // locked40
        Options o; o.cap = 4; o.switchLoss = 1; o.outHz = 60.0;
        const Result r = simulate(kCtrlCoreEven, dt, o);
        check(r.exactPct < 10.0,
              "--fg-out-hz 60 + 40 fps 输入：标记为无整 vblank 节奏（exact=false）");
    }

    {
        // 双卡下的"任意上限"还有第二个上限：FG 卡的单批成本。J=8 但每张输出
        // 要 20 ms 时，66.7 ms 的输入最多只能排 3 张 —— 控制器必须自己降下来，
        // 而不是把消费卡压满、反过来拉长真实帧间隔。
        const std::vector<double> dt = makeIntervals(pats[7], N);  // slow15
        Options o; o.cap = 8; o.switchLoss = 1; o.costPerOutputMs = 20.0;
        const Result r = simulate(kCtrlCoreOptimal, dt, o);
        check(r.capPct > 90.0 && r.outHz < 70.0,
              "成本上限：J=8 + 20 ms/张 -> 自动降档（输出 < 70 Hz）");
    }

    {
        // 运行中把 J 从 6 降到 3：必须在排空后立即夹住，不出现 k>J 的帧，
        // 且不需要重启进程（Reconfigure 保留学习值）。
        const std::vector<double> dt = makeIntervals(pats[6], N);  // slow21
        Options o; o.cap = 6; o.switchLoss = 1;
        o.changeAt = 700; o.capAfter = 3; o.warmup = 700;   // 只统计改上限之后
        const Result r = simulate(kCtrlCoreEven, dt, o);
        const Result ro = simulate(kCtrlCoreOptimal, dt, o);
        check(r.maxKAfterChange <= 3 && ro.maxKAfterChange <= 3,
              "运行中 J 6->3：之后不再出现 k>3（立即夹住）");
        check(r.outHz < 70.0 && ro.outHz < 70.0,
              "运行中 J 6->3：输出回落到 cap-limited 速率");
    }

    {
        // 冷启动：一个 400 ms 的首帧不能把估计拖住（bootstrap 取前两个样本的较短值）。
        std::vector<double> dt;
        dt.push_back(400.0);
        for (int i = 1; i < 600; ++i) { dt.push_back(16.7); }
        Options o; o.cap = 8; o.switchLoss = 1; o.warmup = 10;
        const Result r = simulate(kCtrlCoreEven, dt, o);
        check(r.finalEstimateMs < 25.0 && r.outHz > 100.0,
              "冷启动 400ms 首帧：估计在第 2 个样本回到真实量级");
    }
    {
        // 会话/资源换代：reset 后不沿用旧相位，恢复高帧率与整 vblank 节奏。
        std::vector<double> dt;
        for (int i = 0; i < 700; ++i) { dt.push_back(66.67 + ((i % 7) - 3) * 0.5); }
        for (int i = 0; i < 400; ++i) { dt.push_back(16.7); }
        Options o; o.cap = 4; o.switchLoss = 1; o.warmup = 700; o.resetAt = 700;
        const Result r = simulate(kCtrlCoreEven, dt, o);
        check(r.outHz > 100.0 && r.unevenPct < 5.0 && r.intervalP95 <= 2.0,
              "reset 15fps->60fps：重新起锚（out >100 Hz，p95 <=2 vb）");
    }

    {
        // 40 fps / 120 Hz / J=4：3.00 vblank 整除，最优解应 q≈0、120 Hz。
        const std::vector<double> dt = makeIntervals(pats[1], N);
        Options o; o.cap = 4; o.switchLoss = 1;
        const Result r = simulate(kCtrlCoreOptimal, dt, o);
        check(r.outHz > 115.0 && r.unevenPct < 3.0 && r.meanQ < 0.02,
              "optimal 40fps/120Hz：q≈0、120 Hz、切换少");
    }
    {
        // 240 Hz 面板 + 30 fps 输入：m=2,k=4 -> 120 Hz、q=0。
        const std::vector<double> dt = makeIntervals(Pattern { "steady30", 33.333, 0.0, 0 }, N);
        Options o; o.cap = 4; o.switchLoss = 1; o.refreshHz = 240.0;
        const Result r = simulate(kCtrlCoreOptimal, dt, o);
        check(r.outHz > 115.0 && r.outHz < 125.0 && r.unevenPct < 3.0 && r.meanQ < 0.02,
              "optimal 30fps/240Hz：m=2、120 Hz、q≈0");
    }
    {
        // 21 fps / 120 Hz / J=4：Λ=2 ms 不够量化（需 2.4 ms），最优解应选
        // k=3,m=2（60 Hz，q≈5%）而不是 k=4（84 Hz，q≈43%）。
        const std::vector<double> dt = makeIntervals(pats[6], N);
        Options o; o.cap = 4; o.switchLoss = 1;
        const Result r = simulate(kCtrlCoreOptimal, dt, o);
        check(r.outHz > 52.0 && r.outHz < 70.0 && r.meanK > 2.5 && r.meanK < 3.5 &&
              r.meanM > 1.5 && r.meanM < 2.5 && r.unevenPct < 20.0 && r.pacedPct < 10.0,
              "optimal 21fps/J=4：选 k=3,m=2（60 Hz，而非 84 Hz 的 43% 长保持）");
    }
    {
        // Λ 放大到 6 ms：35 fps 应主动量化到 4 vblank/输入（30 fps 基率），
        // 输出 120 Hz、q=0，代价是 14% 源帧被丢弃。
        const std::vector<double> dt = makeIntervals(pats[2], N);  // lock35
        Options o; o.cap = 4; o.switchLoss = 1;
        o.latencyBudgetMs = 6.0;
        const Result r = simulate(kCtrlCoreOptimal, dt, o);
        check(r.pacedPct > 80.0 && r.unevenPct < 3.0 && r.outHz > 115.0,
              "optimal Λ=6ms：35fps 量化到 120 Hz、q≈0（paced）");
    }

    {
        // 可实现性反馈：4K 4x 只出 1 张时，连续 3 次应把 learned J 降到 3，
        // 之后决策不得再请求 k=4；再连续 3 次降不到 2。
        mgpu_cad::Config c;
        c.refreshHz = 120; c.maxOutputFrames = 4; c.policy = mgpu_cad::kPolicyOptimal;
        mgpu_cad::CadenceCore core; core.Configure(c);
        mgpu_cad::Sample s; s.inputMs = 21.0; s.canSwitch = true;
        auto d0 = core.Update(s); core.Commit(d0.k, true);
        for (int i = 0; i < 3; ++i) { core.NoteOutputShortfall(4, 1); }
        auto d1 = core.Update(s); core.Commit(d1.k, true);
        check(core.LearnedMaxK() == 3 && d1.k <= 3,
              "shortfall 4x->1 张：learned max k 降到 3，决策不再请求 4");
        for (int i = 0; i < 3; ++i) { core.NoteOutputShortfall(3, 1); }
        auto d2 = core.Update(s); core.Commit(d2.k, true);
        check(core.LearnedMaxK() == 2 && d2.k <= 2,
              "shortfall 3x->1 张：learned max k 降到 2");
    }

    std::printf("\n%s (%d failure(s))\n",
                failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", failures);
    return failures == 0 ? 0 : 1;
}
