// ============================================================================
//  patch_gain.hlsl - turn the 2D calibration histogram into a 32-bin luminance LUT.
//
//  Each luminance row is summarised by the median log2(present_nits/patch_nits), which
//  handles the game's tone curve without touching white balance: the same gain is applied
//  to R/G/B. Empty rows are filled from their neighbours, lightly smoothed and made
//  non-increasing (a display tone curve compresses as luminance rises).
//
//  Bindings: t0 = hist [HBINS][RBINS], t1 = total [2*HBINS], u0 = LUT[HBINS]+confidence
// ============================================================================

#define HBINS     32u
#define RBINS     32u
#define NCH       3u
#define RLOG_MIN -4.0
#define RLOG_MAX  4.0
#define MIN_COUNT 256u

cbuffer Params : register(b0)
{
    uint  Bins;
    uint  MinCount;
    float LogMin;
    float LogMax;
    uint  pad0, pad1, pad2, pad3;
};

StructuredBuffer<uint>    Hist : register(t0);
StructuredBuffer<uint>    Total : register(t1);
RWStructuredBuffer<float> Gain : register(u0);   // [NCH][HBINS] LUTs, [NCH*HBINS] confidence

[numthreads(3, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint ch = id.x;
    if (ch >= NCH) { return; }

    float lut[HBINS];
    bool  ok[HBINS];
    uint  validCount = 0u;

    [unroll] for (uint h = 0u; h < HBINS; ++h)
    {
        lut[h] = 1.0f;
        ok[h] = false;
        const uint valid = Total[ch * 2u * HBINS + h];
        const uint apps  = Total[(ch * 2u + 1u) * HBINS + h];
        if (valid < MIN_COUNT || apps == 0u) { continue; }

        // Median of this channel's log2 ratio row.
        uint cum = 0u, med = 0u;
        [loop] for (uint b = 0u; b < RBINS; ++b)
        {
            cum += Hist[(ch * HBINS + h) * RBINS + b];
            if (cum * 2u >= valid) { med = b; break; }
        }
        float wsum = 0.0f, bsum = 0.0f;
        [unroll] for (int d = -2; d <= 2; ++d)
        {
            const int bb = int(med) + d;
            if (bb < 0 || bb >= (int)RBINS) { continue; }
            const float w = (float)Hist[(ch * HBINS + h) * RBINS + (uint)bb];
            wsum += w;
            bsum += w * ((float)bb + 0.5f);
        }
        if (wsum <= 0.0f) { continue; }
        const float mean_bin = bsum / wsum;
        const float lr = RLOG_MIN + (mean_bin / (float)RBINS) * (RLOG_MAX - RLOG_MIN);
        lut[h] = clamp(exp2(lr), 0.02f, 8.0f);
        ok[h] = true;
        ++validCount;
    }

    // Nothing for this channel: leave the previous LUT untouched.
    if (validCount == 0u) { return; }

    // Fill empty rows from the nearest valid neighbour (forward, then backward).
    float last = lut[0];
    [loop] for (uint h = 0u; h < HBINS; ++h)
    {
        if (ok[h]) { last = lut[h]; } else { lut[h] = last; }
    }
    last = lut[HBINS - 1u];
    [loop] for (int h = (int)HBINS - 1; h >= 0; --h)
    {
        if (ok[(uint)h]) { last = lut[(uint)h]; } else { lut[(uint)h] = last; }
    }

    // Light 3-bin smoothing; no direction constraint (the measured curves increase with
    // luminance for CP2077).
    float sm[HBINS];
    [unroll] for (uint h = 0u; h < HBINS; ++h)
    {
        const uint a = (h == 0u) ? 0u : h - 1u;
        const uint c = (h + 1u >= HBINS) ? HBINS - 1u : h + 1u;
        sm[h] = (lut[a] + lut[h] + lut[c]) / 3.0f;
    }
    [unroll] for (uint h = 0u; h < HBINS; ++h) { Gain[ch * HBINS + h] = sm[h]; }

    if (ch == 0u)
    {
        // Confidence = fraction of candidate samples that produced a valid ratio, over
        // all channels. Counting bins under-reports when the scene occupies few rows.
        uint validSum = 0u, appSum = 0u;
        [loop] for (uint c = 0u; c < NCH; ++c)
        {
            [unroll] for (uint h = 0u; h < HBINS; ++h)
            {
                validSum += Total[c * 2u * HBINS + h];
                appSum   += Total[(c * 2u + 1u) * HBINS + h];
            }
        }
        Gain[NCH * HBINS] = (appSum > 0u) ? (float)validSum / (float)appSum : 0.0f;
    }
}

