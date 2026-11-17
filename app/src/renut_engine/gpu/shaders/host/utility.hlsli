// Shared bindings for the renderer's own (non-guest) shaders.
//   set 0 : bindless Texture2D table   (t0, space0)
//   set 1 : bindless sampler table     (s0, space1)
//   push  : UtilityPush                (b0, space2), vertex + pixel stages
// Must match BuildUtilityLayout in video.cpp.

struct UtilityPush
{
    float2 scale;
    float2 translate;
    uint textureIndex;
    uint samplerIndex;
    float param0;
    float param1;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<UtilityPush> g_Push;
#elif defined(RENUT_D3D11)
// mk9-lauta (2026-11-16): Direct3D 11 (FXC, shader model 5.0): no ConstantBuffer<T>, no NonUniformResourceIndex and no
// descriptor tables. The back end binds the views the push constants' indices name (plume_d3d11
// D3D11SetTableBindings, BuildUtilityLayout in video.cpp): t0 the texture, s0 the sampler, t1 the gamma ramp.
cbuffer UtilityPushBuffer : register(b0)
{
    UtilityPush g_Push;
};
#define NonUniformResourceIndex(index) (index)
#else
ConstantBuffer<UtilityPush> g_Push : register(b0, space2);
#endif

// A shader that reads the table as another texture type (depth_resolve.hlsli)
// defines UTILITY_OWN_TEXTURE_TABLE and declares t0, space0 itself.
#ifndef UTILITY_OWN_TEXTURE_TABLE
#ifdef RENUT_D3D11
Texture2D<float4> g_Texture : register(t0);
SamplerState g_Sampler : register(s0);
Texture2D<float4> g_RampTexture : register(t1);
// The view of an index: the one the back end bound for it.
#define UTILITY_TEXTURE(index) g_Texture
#define UTILITY_RAMP(index) g_RampTexture

float4 SampleBindless(uint textureIndex, uint samplerIndex, float2 uv)
{
    return g_Texture.Sample(g_Sampler, uv);
}
#else
Texture2D<float4> g_Textures[] : register(t0, space0);
SamplerState g_Samplers[] : register(s0, space1);
#define UTILITY_TEXTURE(index) g_Textures[NonUniformResourceIndex(index)]
#define UTILITY_RAMP(index) g_Textures[NonUniformResourceIndex(index)]

float4 SampleBindless(uint textureIndex, uint samplerIndex, float2 uv)
{
    return g_Textures[NonUniformResourceIndex(textureIndex)].Sample(
        g_Samplers[NonUniformResourceIndex(samplerIndex)], uv);
}
#endif
#endif
