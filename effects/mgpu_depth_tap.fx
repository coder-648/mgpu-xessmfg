// ============================================================================
// mgpu_depth_tap.fx  -  M3 depth acquisition
//
// This effect deliberately does almost nothing. Its only job is to make ReShade
// bind the game's depth buffer to a texture variable declared with the DEPTH
// semantic, because that binding is the only supported way for an add-on to get
// hold of the real depth resource:
//
//     runtime->find_texture_variable("mgpu_depth_tap.fx", "MGPU_SceneDepthTex")
//     runtime->get_texture_binding(var, &srv, nullptr)
//     device->get_resource_from_view(srv)     -> the game's depth texture
//
// The pass writes into a private target (MGPU_DepthOutTex), so the visible
// image is never modified.
//
// Measured shape in Cyberpunk 2077 (5K + DLSS Performance):
//     depth  2560x1440  DXGI_FORMAT_R32_TYPELESS (D32_FLOAT)
// The add-on resamples this down to a 2560x1440 R16_UNORM payload, which is
// what the cross-adapter mailbox carries to the second GPU.
//
// No #include on purpose: ReShade.fxh ships with the standard effects pack, and
// depending on it would make this file silently fail to compile on an install
// that skipped that pack.
// ============================================================================

texture MGPU_SceneDepthTex : DEPTH;
sampler MGPU_SceneDepth { Texture = MGPU_SceneDepthTex; };

// Cheap probe: ReShade's semantic system is a PUSH, so if anything ever
// publishes motion vectors to ReShade they land here. The add-on reports
// whether this is bound. We already get real MV from the NGX hook, so this is
// only a fallback worth knowing about.
texture MGPU_MotionProbeTex : MOTION;
sampler MGPU_MotionProbe { Texture = MGPU_MotionProbeTex; };

// Bufferless fullscreen triangle, under our own name so it cannot collide with
// the standard header if both end up compiled into the same effect path.
void MGPU_FullscreenVS(in  uint id       : SV_VertexID,
                       out float4 position : SV_Position,
                       out float2 texcoord : TEXCOORD)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// Private sink. ReShade renders passes at buffer size, so this is output
// resolution; the add-on does the actual 2560x1440 conversion itself.
// The add-on reads this texture straight out of ReShade (1 frame later, so the state is
// known) and copies it into its depth stream, which is why the size is overridable from
// the preset: it must match the stream (MV size), e.g. 2562x1441 on UE5 titles.
#ifndef MGPU_DEPTH_W
#define MGPU_DEPTH_W BUFFER_WIDTH
#endif
#ifndef MGPU_DEPTH_H
#define MGPU_DEPTH_H BUFFER_HEIGHT
#endif

texture MGPU_DepthOutTex
{
    Width  = MGPU_DEPTH_W;
    Height = MGPU_DEPTH_H;
    Format = R32F;
};

float MGPU_TapPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // Raw depth, no linearization. Whether the title uses reversed-Z is a
    // decision that belongs to the model, not to this file; XeSS-FG gets told
    // via XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH once we have confirmed it.
    const float d = tex2Dlod(MGPU_SceneDepth, float4(uv, 0, 0)).x;

    // Sample the motion probe so the compiler cannot strip the texture (and
    // with it the semantic). The test is only true for NaN, which no real
    // sample produces, so the result is always `d`.
    const float2 mv = tex2Dlod(MGPU_MotionProbe, float4(uv, 0, 0)).xy;
    return (mv.x != mv.x) ? 0.0 : d;
}

technique MGPU_DepthTap
<
    ui_label   = "MGPU Depth Tap";
    ui_tooltip = "Keeps ReShade's DEPTH semantic bound so the add-on can read the "
                 "game's depth buffer. Draws nothing on screen.";
>
{
    pass
    {
        VertexShader = MGPU_FullscreenVS;
        PixelShader  = MGPU_TapPS;
        RenderTarget = MGPU_DepthOutTex;
    }
}
