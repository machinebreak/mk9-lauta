//
// plume
//
// Copyright (c) 2024 renderbag and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE file for details.
//
// mk9-lauta (2026-11-16): the Direct3D 11 back end (plume_d3d11.h says how it works).
//

#include "plume_d3d11.h"

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <d3dcompiler.h>
#include <dxgi1_5.h>

namespace plume {
    static constexpr uint32_t IdentitySwizzle = 0u | (1u << 3) | (2u << 6) | (3u << 9);
    static constexpr uint64_t PageSize = 4096;

    template <typename T>
    static void safeRelease(T *&object) {
        if (object != nullptr) {
            object->Release();
            object = nullptr;
        }
    }

    template <typename T>
    static T *stale() {
        return reinterpret_cast<T *>(uintptr_t(1));
    }

    static uint64_t roundUp(uint64_t value, uint64_t alignment) {
        return (value + alignment - 1) / alignment * alignment;
    }

    static std::string utf16ToUtf8(const wchar_t *value) {
        std::string out;
        const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (size > 1) {
            out.resize(size_t(size - 1));
            WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr);
        }
        return out;
    }

    // A message to plume's log (stderr), at most 'limit' times for the same call site.
    static void logLimited(std::atomic<uint32_t> &count, uint32_t limit, const char *format, ...) {
        if (count.fetch_add(1, std::memory_order_relaxed) >= limit) {
            return;
        }
        va_list args;
        va_start(args, format);
        fprintf(stderr, "[d3d11] ");
        vfprintf(stderr, format, args);
        fprintf(stderr, "\n");
        va_end(args);
    }

#define D3D11_LOG_LIMITED(limit, ...) do { static std::atomic<uint32_t> d3d11LogCount{0}; logLimited(d3d11LogCount, (limit), __VA_ARGS__); } while (0)

    // Formats.

    static DXGI_FORMAT toDXGI(RenderFormat format) {
        switch (format) {
        case RenderFormat::UNKNOWN: return DXGI_FORMAT_UNKNOWN;
        case RenderFormat::R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_TYPELESS;
        case RenderFormat::R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case RenderFormat::R32G32B32A32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
        case RenderFormat::R32G32B32A32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
        case RenderFormat::R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_TYPELESS;
        case RenderFormat::R32G32B32_FLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
        case RenderFormat::R32G32B32_UINT: return DXGI_FORMAT_R32G32B32_UINT;
        case RenderFormat::R32G32B32_SINT: return DXGI_FORMAT_R32G32B32_SINT;
        case RenderFormat::R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case RenderFormat::R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case RenderFormat::R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case RenderFormat::R16G16B16A16_UINT: return DXGI_FORMAT_R16G16B16A16_UINT;
        case RenderFormat::R16G16B16A16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case RenderFormat::R16G16B16A16_SINT: return DXGI_FORMAT_R16G16B16A16_SINT;
        case RenderFormat::R32G32_TYPELESS: return DXGI_FORMAT_R32G32_TYPELESS;
        case RenderFormat::R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
        case RenderFormat::R32G32_UINT: return DXGI_FORMAT_R32G32_UINT;
        case RenderFormat::R32G32_SINT: return DXGI_FORMAT_R32G32_SINT;
        case RenderFormat::R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case RenderFormat::R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RenderFormat::R8G8B8A8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
        case RenderFormat::R8G8B8A8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case RenderFormat::R8G8B8A8_SINT: return DXGI_FORMAT_R8G8B8A8_SINT;
        case RenderFormat::B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case RenderFormat::R16G16_TYPELESS: return DXGI_FORMAT_R16G16_TYPELESS;
        case RenderFormat::R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case RenderFormat::R16G16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
        case RenderFormat::R16G16_UINT: return DXGI_FORMAT_R16G16_UINT;
        case RenderFormat::R16G16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
        case RenderFormat::R16G16_SINT: return DXGI_FORMAT_R16G16_SINT;
        case RenderFormat::R32_TYPELESS: return DXGI_FORMAT_R32_TYPELESS;
        case RenderFormat::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
        case RenderFormat::D32_FLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case RenderFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case RenderFormat::R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case RenderFormat::R32_UINT: return DXGI_FORMAT_R32_UINT;
        case RenderFormat::R32_SINT: return DXGI_FORMAT_R32_SINT;
        case RenderFormat::R8G8_TYPELESS: return DXGI_FORMAT_R8G8_TYPELESS;
        case RenderFormat::R8G8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
        case RenderFormat::R8G8_UINT: return DXGI_FORMAT_R8G8_UINT;
        case RenderFormat::R8G8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
        case RenderFormat::R8G8_SINT: return DXGI_FORMAT_R8G8_SINT;
        case RenderFormat::R16_TYPELESS: return DXGI_FORMAT_R16_TYPELESS;
        case RenderFormat::R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
        case RenderFormat::D16_UNORM: return DXGI_FORMAT_D16_UNORM;
        case RenderFormat::R16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case RenderFormat::R16_UINT: return DXGI_FORMAT_R16_UINT;
        case RenderFormat::R16_SNORM: return DXGI_FORMAT_R16_SNORM;
        case RenderFormat::R16_SINT: return DXGI_FORMAT_R16_SINT;
        case RenderFormat::R8_TYPELESS: return DXGI_FORMAT_R8_TYPELESS;
        case RenderFormat::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
        case RenderFormat::R8_UINT: return DXGI_FORMAT_R8_UINT;
        case RenderFormat::R8_SNORM: return DXGI_FORMAT_R8_SNORM;
        case RenderFormat::R8_SINT: return DXGI_FORMAT_R8_SINT;
        case RenderFormat::BC1_TYPELESS: return DXGI_FORMAT_BC1_TYPELESS;
        case RenderFormat::BC1_UNORM: return DXGI_FORMAT_BC1_UNORM;
        case RenderFormat::BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
        case RenderFormat::BC2_TYPELESS: return DXGI_FORMAT_BC2_TYPELESS;
        case RenderFormat::BC2_UNORM: return DXGI_FORMAT_BC2_UNORM;
        case RenderFormat::BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_UNORM_SRGB;
        case RenderFormat::BC3_TYPELESS: return DXGI_FORMAT_BC3_TYPELESS;
        case RenderFormat::BC3_UNORM: return DXGI_FORMAT_BC3_UNORM;
        case RenderFormat::BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
        case RenderFormat::BC4_TYPELESS: return DXGI_FORMAT_BC4_TYPELESS;
        case RenderFormat::BC4_UNORM: return DXGI_FORMAT_BC4_UNORM;
        case RenderFormat::BC4_SNORM: return DXGI_FORMAT_BC4_SNORM;
        case RenderFormat::BC5_TYPELESS: return DXGI_FORMAT_BC5_TYPELESS;
        case RenderFormat::BC5_UNORM: return DXGI_FORMAT_BC5_UNORM;
        case RenderFormat::BC5_SNORM: return DXGI_FORMAT_BC5_SNORM;
        case RenderFormat::BC6H_TYPELESS: return DXGI_FORMAT_BC6H_TYPELESS;
        case RenderFormat::BC6H_UF16: return DXGI_FORMAT_BC6H_UF16;
        case RenderFormat::BC6H_SF16: return DXGI_FORMAT_BC6H_SF16;
        case RenderFormat::BC7_TYPELESS: return DXGI_FORMAT_BC7_TYPELESS;
        case RenderFormat::BC7_UNORM: return DXGI_FORMAT_BC7_UNORM;
        case RenderFormat::BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
        case RenderFormat::S8_UINT: return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    // A texture's resource format: depth formats typeless, so the texture has both depth and shader views.
    static DXGI_FORMAT toResourceFormat(RenderFormat format) {
        switch (format) {
        case RenderFormat::D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
        case RenderFormat::D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case RenderFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case RenderFormat::D32_FLOAT_S8_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        default: return toDXGI(format);
        }
    }

    // A typeless format as a view must have it (the formats' plain variant).
    static DXGI_FORMAT typedDefault(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
        case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
        default: return format;
        }
    }

    // A shader resource view's format for 'view' of a texture of 'texture' (depth formats as their depth or stencil).
    static DXGI_FORMAT toShaderViewFormat(RenderFormat view, RenderFormat texture) {
        if (view == RenderFormat::UNKNOWN) {
            view = texture;
        }
        switch (view) {
        case RenderFormat::D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case RenderFormat::D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case RenderFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case RenderFormat::D32_FLOAT_S8_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case RenderFormat::S8_UINT:
            return (texture == RenderFormat::D32_FLOAT_S8_UINT) ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        case RenderFormat::R32_TYPELESS:
            return (texture == RenderFormat::D32_FLOAT) ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_FLOAT;
        default: return typedDefault(toDXGI(view));
        }
    }

    static DXGI_FORMAT toDepthViewFormat(RenderFormat format) {
        switch (format) {
        case RenderFormat::D16_UNORM: return DXGI_FORMAT_D16_UNORM;
        case RenderFormat::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
        case RenderFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case RenderFormat::D32_FLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        default: return toDXGI(format);
        }
    }

    static DXGI_FORMAT toTargetViewFormat(RenderFormat format) {
        return typedDefault(toDXGI(format));
    }

    static D3D11_COMPARISON_FUNC toD3D11(RenderComparisonFunction function) {
        switch (function) {
        case RenderComparisonFunction::NEVER: return D3D11_COMPARISON_NEVER;
        case RenderComparisonFunction::LESS: return D3D11_COMPARISON_LESS;
        case RenderComparisonFunction::EQUAL: return D3D11_COMPARISON_EQUAL;
        case RenderComparisonFunction::LESS_EQUAL: return D3D11_COMPARISON_LESS_EQUAL;
        case RenderComparisonFunction::GREATER: return D3D11_COMPARISON_GREATER;
        case RenderComparisonFunction::NOT_EQUAL: return D3D11_COMPARISON_NOT_EQUAL;
        case RenderComparisonFunction::GREATER_EQUAL: return D3D11_COMPARISON_GREATER_EQUAL;
        case RenderComparisonFunction::ALWAYS: return D3D11_COMPARISON_ALWAYS;
        default: return D3D11_COMPARISON_ALWAYS;
        }
    }

    static D3D11_STENCIL_OP toD3D11(RenderStencilOp op) {
        switch (op) {
        case RenderStencilOp::KEEP: return D3D11_STENCIL_OP_KEEP;
        case RenderStencilOp::ZERO: return D3D11_STENCIL_OP_ZERO;
        case RenderStencilOp::REPLACE: return D3D11_STENCIL_OP_REPLACE;
        case RenderStencilOp::INCREMENT_AND_CLAMP: return D3D11_STENCIL_OP_INCR_SAT;
        case RenderStencilOp::DECREMENT_AND_CLAMP: return D3D11_STENCIL_OP_DECR_SAT;
        case RenderStencilOp::INVERT: return D3D11_STENCIL_OP_INVERT;
        case RenderStencilOp::INCREMENT_AND_WRAP: return D3D11_STENCIL_OP_INCR;
        case RenderStencilOp::DECREMENT_AND_WRAP: return D3D11_STENCIL_OP_DECR;
        default: return D3D11_STENCIL_OP_KEEP;
        }
    }

    static D3D11_BLEND toD3D11(RenderBlend blend, D3D11_BLEND fallback) {
        switch (blend) {
        case RenderBlend::ZERO: return D3D11_BLEND_ZERO;
        case RenderBlend::ONE: return D3D11_BLEND_ONE;
        case RenderBlend::SRC_COLOR: return D3D11_BLEND_SRC_COLOR;
        case RenderBlend::INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_COLOR;
        case RenderBlend::SRC_ALPHA: return D3D11_BLEND_SRC_ALPHA;
        case RenderBlend::INV_SRC_ALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
        case RenderBlend::DEST_ALPHA: return D3D11_BLEND_DEST_ALPHA;
        case RenderBlend::INV_DEST_ALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
        case RenderBlend::DEST_COLOR: return D3D11_BLEND_DEST_COLOR;
        case RenderBlend::INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_COLOR;
        case RenderBlend::SRC_ALPHA_SAT: return D3D11_BLEND_SRC_ALPHA_SAT;
        case RenderBlend::BLEND_FACTOR: return D3D11_BLEND_BLEND_FACTOR;
        case RenderBlend::INV_BLEND_FACTOR: return D3D11_BLEND_INV_BLEND_FACTOR;
        case RenderBlend::SRC1_COLOR: return D3D11_BLEND_SRC1_COLOR;
        case RenderBlend::INV_SRC1_COLOR: return D3D11_BLEND_INV_SRC1_COLOR;
        case RenderBlend::SRC1_ALPHA: return D3D11_BLEND_SRC1_ALPHA;
        case RenderBlend::INV_SRC1_ALPHA: return D3D11_BLEND_INV_SRC1_ALPHA;
        default: return fallback;
        }
    }

    // Direct3D 11 refuses the colour factors for alpha: their alpha variants, which they read for alpha anyway.
    static D3D11_BLEND toAlphaBlend(D3D11_BLEND blend) {
        switch (blend) {
        case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
        case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
        case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
        case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
        case D3D11_BLEND_SRC1_COLOR: return D3D11_BLEND_SRC1_ALPHA;
        case D3D11_BLEND_INV_SRC1_COLOR: return D3D11_BLEND_INV_SRC1_ALPHA;
        default: return blend;
        }
    }

    static D3D11_BLEND_OP toD3D11(RenderBlendOperation op) {
        switch (op) {
        case RenderBlendOperation::ADD: return D3D11_BLEND_OP_ADD;
        case RenderBlendOperation::SUBTRACT: return D3D11_BLEND_OP_SUBTRACT;
        case RenderBlendOperation::REV_SUBTRACT: return D3D11_BLEND_OP_REV_SUBTRACT;
        case RenderBlendOperation::MIN: return D3D11_BLEND_OP_MIN;
        case RenderBlendOperation::MAX: return D3D11_BLEND_OP_MAX;
        default: return D3D11_BLEND_OP_ADD;
        }
    }

    static D3D11_LOGIC_OP toD3D11(RenderLogicOperation op) {
        switch (op) {
        case RenderLogicOperation::CLEAR: return D3D11_LOGIC_OP_CLEAR;
        case RenderLogicOperation::SET: return D3D11_LOGIC_OP_SET;
        case RenderLogicOperation::COPY: return D3D11_LOGIC_OP_COPY;
        case RenderLogicOperation::COPY_INVERTED: return D3D11_LOGIC_OP_COPY_INVERTED;
        case RenderLogicOperation::NOOP: return D3D11_LOGIC_OP_NOOP;
        case RenderLogicOperation::INVERT: return D3D11_LOGIC_OP_INVERT;
        case RenderLogicOperation::AND: return D3D11_LOGIC_OP_AND;
        case RenderLogicOperation::NAND: return D3D11_LOGIC_OP_NAND;
        case RenderLogicOperation::OR: return D3D11_LOGIC_OP_OR;
        case RenderLogicOperation::NOR: return D3D11_LOGIC_OP_NOR;
        case RenderLogicOperation::XOR: return D3D11_LOGIC_OP_XOR;
        case RenderLogicOperation::EQUIV: return D3D11_LOGIC_OP_EQUIV;
        case RenderLogicOperation::AND_REVERSE: return D3D11_LOGIC_OP_AND_REVERSE;
        case RenderLogicOperation::AND_INVERTED: return D3D11_LOGIC_OP_AND_INVERTED;
        case RenderLogicOperation::OR_REVERSE: return D3D11_LOGIC_OP_OR_REVERSE;
        case RenderLogicOperation::OR_INVERTED: return D3D11_LOGIC_OP_OR_INVERTED;
        default: return D3D11_LOGIC_OP_NOOP;
        }
    }

    static D3D11_TEXTURE_ADDRESS_MODE toD3D11(RenderTextureAddressMode mode) {
        switch (mode) {
        case RenderTextureAddressMode::WRAP: return D3D11_TEXTURE_ADDRESS_WRAP;
        case RenderTextureAddressMode::MIRROR: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case RenderTextureAddressMode::CLAMP: return D3D11_TEXTURE_ADDRESS_CLAMP;
        case RenderTextureAddressMode::BORDER: return D3D11_TEXTURE_ADDRESS_BORDER;
        case RenderTextureAddressMode::MIRROR_ONCE: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        default: return D3D11_TEXTURE_ADDRESS_WRAP;
        }
    }

    static D3D11_PRIMITIVE_TOPOLOGY toD3D11(RenderPrimitiveTopology topology) {
        switch (topology) {
        case RenderPrimitiveTopology::POINT_LIST: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        case RenderPrimitiveTopology::LINE_LIST: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        case RenderPrimitiveTopology::LINE_STRIP: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case RenderPrimitiveTopology::TRIANGLE_LIST: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case RenderPrimitiveTopology::TRIANGLE_STRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }
    }

    // A view's component mapping as the guest shaders read it: 3 bits a component, 0-3 a channel, 4 zero, 5 one.
    static uint32_t packSwizzle(const RenderComponentMapping &mapping) {
        auto component = [](RenderSwizzle swizzle, uint32_t identity) -> uint32_t {
            switch (swizzle) {
            case RenderSwizzle::IDENTITY: return identity;
            case RenderSwizzle::ZERO: return 4;
            case RenderSwizzle::ONE: return 5;
            case RenderSwizzle::R: return 0;
            case RenderSwizzle::G: return 1;
            case RenderSwizzle::B: return 2;
            case RenderSwizzle::A: return 3;
            default: return identity;
            }
        };
        return component(mapping.r, 0) | (component(mapping.g, 1) << 3) | (component(mapping.b, 2) << 6) | (component(mapping.a, 3) << 9);
    }

    static RenderShaderStageFlags stageFlag(uint32_t stage) {
        switch (stage) {
        case D3D11StageVertex: return RenderShaderStageFlag::VERTEX;
        case D3D11StageGeometry: return RenderShaderStageFlag::GEOMETRY;
        case D3D11StagePixel: return RenderShaderStageFlag::PIXEL;
        default: return RenderShaderStageFlag::COMPUTE;
        }
    }

    static void setShaderResources(ID3D11DeviceContext *context, uint32_t stage, UINT start, UINT count, ID3D11ShaderResourceView *const *views) {
        switch (stage) {
        case D3D11StageVertex: context->VSSetShaderResources(start, count, views); break;
        case D3D11StageGeometry: context->GSSetShaderResources(start, count, views); break;
        case D3D11StagePixel: context->PSSetShaderResources(start, count, views); break;
        default: context->CSSetShaderResources(start, count, views); break;
        }
    }

    static void setSamplers(ID3D11DeviceContext *context, uint32_t stage, UINT start, UINT count, ID3D11SamplerState *const *samplers) {
        switch (stage) {
        case D3D11StageVertex: context->VSSetSamplers(start, count, samplers); break;
        case D3D11StageGeometry: context->GSSetSamplers(start, count, samplers); break;
        case D3D11StagePixel: context->PSSetSamplers(start, count, samplers); break;
        default: context->CSSetSamplers(start, count, samplers); break;
        }
    }

    static int findRoot(const D3D11PipelineLayout *layout, RenderRootDescriptorType type, uint32_t reg) {
        if (layout == nullptr) {
            return -1;
        }
        const size_t count = std::min<size_t>(layout->roots.size(), 8);
        for (size_t i = 0; i < count; i++) {
            if ((layout->roots[i].type == type) && (layout->roots[i].reg == reg)) {
                return int(i);
            }
        }
        return -1;
    }

    static D3D11Buffer *mutableBuffer(const RenderBuffer *buffer) {
        return const_cast<D3D11Buffer *>(static_cast<const D3D11Buffer *>(buffer));
    }

    // The fetch slot a stage's register (a sampler, or a stacked texture's array) is for: the low four bits, or the same
    // plus 16 when the shader declares a texture of that slot and none of the other.
    static uint32_t guestSlot(const D3D11ShaderInfo &info, uint32_t low) {
        auto declares = [&info](uint32_t slot) {
            return info.usesSrv(slot) || info.usesSrv(32 + slot) || info.usesSrv(64 + slot);
        };
        if ((low < 16) && declares(low + 16) && !declares(low)) {
            return low + 16;
        }
        return low;
    }

    // DXBC.

    bool D3D11ParseShader(const void *data, size_t size, D3D11ShaderInfo &info) {
        info = D3D11ShaderInfo();
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        if ((bytes == nullptr) || (size < 32) || (memcmp(bytes, "DXBC", 4) != 0)) {
            return false;
        }
        uint32_t chunkCount = 0;
        memcpy(&chunkCount, bytes + 28, 4);
        if (32 + uint64_t(chunkCount) * 4 > size) {
            return false;
        }
        for (uint32_t c = 0; c < chunkCount; c++) {
            uint32_t offset = 0;
            memcpy(&offset, bytes + 32 + c * 4, 4);
            if (uint64_t(offset) + 8 > size) {
                return false;
            }
            const uint8_t *chunk = bytes + offset;
            uint32_t chunkSize = 0;
            memcpy(&chunkSize, chunk + 4, 4);
            if (uint64_t(offset) + 8 + chunkSize > size) {
                return false;
            }
            if ((memcmp(chunk, "SHEX", 4) != 0) && (memcmp(chunk, "SHDR", 4) != 0)) {
                continue;
            }
            const uint32_t tokenCount = chunkSize / 4;
            if (tokenCount < 2) {
                return false;
            }
            std::vector<uint32_t> tokens(tokenCount);
            memcpy(tokens.data(), chunk + 8, size_t(tokenCount) * 4);
            info.programType = tokens[0] >> 16;
            const uint32_t length = std::min(tokens[1], tokenCount);
            for (uint32_t i = 2; i < length;) {
                const uint32_t opcodeToken = tokens[i];
                const uint32_t opcode = opcodeToken & 0x7FF;
                uint32_t instructionLength = (opcodeToken >> 24) & 0x7F;
                if ((opcode == 53) || (instructionLength == 0)) {
                    // Custom data and long declarations: the length is the next token.
                    if (i + 1 >= length) {
                        break;
                    }
                    instructionLength = tokens[i + 1];
                }
                if (instructionLength == 0) {
                    break;
                }
                const uint32_t end = std::min(i + instructionLength, length);
                uint32_t p = i + 1;
                if (opcode != 53) {
                    uint32_t extended = opcodeToken;
                    while ((extended & 0x80000000u) && (p < end)) {
                        extended = tokens[p++];
                    }
                }
                const bool declaration = (opcode == 88) || (opcode == 89) || (opcode == 90) || (opcode == 156) || (opcode == 157) || (opcode == 158) || (opcode == 161) || (opcode == 162);
                if (declaration && (p < end)) {
                    const uint32_t operand = tokens[p++];
                    uint32_t extended = operand;
                    while ((extended & 0x80000000u) && (p < end)) {
                        extended = tokens[p++];
                    }
                    const uint32_t dimensions = (operand >> 20) & 3;
                    const uint32_t index0 = (p < end) ? tokens[p] : 0;
                    const uint32_t index1 = ((dimensions >= 2) && (p + 1 < end)) ? tokens[p + 1] : 0;
                    switch (opcode) {
                    case 88:
                    case 161:
                    case 162:
                        if (index0 < 128) {
                            info.srvMask[index0 >> 6] |= uint64_t(1) << (index0 & 63);
                        }
                        break;
                    case 89:
                        if (index0 < 14) {
                            info.cbMask |= 1u << index0;
                            info.cbSize[index0] = uint16_t(std::min<uint32_t>(index1, 4096));
                        }
                        break;
                    case 90:
                        if (index0 < 16) {
                            info.samplerMask |= 1u << index0;
                        }
                        break;
                    default:
                        if (index0 < 64) {
                            info.uavMask |= uint64_t(1) << index0;
                        }
                        break;
                    }
                }
                i += instructionLength;
            }
            return info.programType != 0xFFFFFFFFu;
        }
        return false;
    }

    // D3D11BufferFormattedView

    D3D11BufferFormattedView::~D3D11BufferFormattedView() {
        safeRelease(srv);
        safeRelease(uav);
    }

    // D3D11Buffer

    D3D11Buffer::D3D11Buffer(D3D11Device *device, const RenderBufferDesc &desc) {
        this->device = device;
        this->desc = desc;
        const uint64_t size = std::max<uint64_t>(desc.size, 16);
        switch (desc.heapType) {
        case RenderHeapType::UPLOAD:
        case RenderHeapType::GPU_UPLOAD: {
            const SIZE_T bytes = SIZE_T(roundUp(size, PageSize));
            cpu = static_cast<uint8_t *>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
            writeWatch = (cpu != nullptr);
            if (cpu == nullptr) {
                cpu = static_cast<uint8_t *>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            }
            if (cpu == nullptr) {
                fprintf(stderr, "[d3d11] VirtualAlloc of %llu bytes failed.\n", (unsigned long long)bytes);
                return;
            }
            stalePages.assign(size_t((bytes / PageSize + 63) / 64), 0);
            break;
        }
        case RenderHeapType::READBACK: {
            D3D11_BUFFER_DESC bufferDesc = {};
            bufferDesc.ByteWidth = UINT(roundUp(size, 16));
            bufferDesc.Usage = D3D11_USAGE_STAGING;
            bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            HRESULT res = device->d3d->CreateBuffer(&bufferDesc, nullptr, &d3d);
            if (FAILED(res)) {
                fprintf(stderr, "[d3d11] CreateBuffer (readback, %llu bytes) failed with error code 0x%lX.\n", (unsigned long long)size, res);
            }
            break;
        }
        default: {
            D3D11_BUFFER_DESC bufferDesc = {};
            bufferDesc.ByteWidth = UINT(roundUp(size, 16));
            bufferDesc.Usage = D3D11_USAGE_DEFAULT;
            UINT bind = 0;
            UINT misc = 0;
            if (desc.flags & RenderBufferFlag::VERTEX) {
                bind |= D3D11_BIND_VERTEX_BUFFER;
            }
            if (desc.flags & RenderBufferFlag::INDEX) {
                bind |= D3D11_BIND_INDEX_BUFFER;
            }
            if (desc.flags & (RenderBufferFlag::UNORDERED_ACCESS | RenderBufferFlag::STORAGE)) {
                bind |= D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            }
            if (desc.flags & (RenderBufferFlag::DEVICE_ADDRESSABLE | RenderBufferFlag::FORMATTED)) {
                bind |= D3D11_BIND_SHADER_RESOURCE;
            }
            if ((desc.flags & RenderBufferFlag::CONSTANT) && (bind == 0)) {
                bind = D3D11_BIND_CONSTANT_BUFFER;
            }
            if (bind == 0) {
                bind = D3D11_BIND_SHADER_RESOURCE;
            }
            if (bind & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) {
                misc |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            }
            bufferDesc.BindFlags = bind;
            bufferDesc.MiscFlags = misc;
            HRESULT res = device->d3d->CreateBuffer(&bufferDesc, nullptr, &d3d);
            if (FAILED(res)) {
                fprintf(stderr, "[d3d11] CreateBuffer (%llu bytes, flags 0x%X) failed with error code 0x%lX.\n", (unsigned long long)size, desc.flags, res);
            }
            break;
        }
        }
    }

    D3D11Buffer::~D3D11Buffer() {
        if ((cpu != nullptr) && (d3d != nullptr)) {
            std::lock_guard<std::mutex> lock(device->uploadMutex);
            auto &list = device->uploadBuffers;
            list.erase(std::remove(list.begin(), list.end(), this), list.end());
        }
        if (mapped && (d3d != nullptr)) {
            std::lock_guard<std::recursive_mutex> lock(device->contextMutex);
            device->context->Unmap(d3d, 0);
        }
        safeRelease(rawSrv);
        safeRelease(rawUav);
        safeRelease(d3d);
        if (cpu != nullptr) {
            VirtualFree(cpu, 0, MEM_RELEASE);
            cpu = nullptr;
        }
    }

    void *D3D11Buffer::map(uint32_t subresource, const RenderRange *readRange) {
        if (cpu != nullptr) {
            return cpu;
        }
        if ((desc.heapType != RenderHeapType::READBACK) || (d3d == nullptr)) {
            return nullptr;
        }
        std::lock_guard<std::recursive_mutex> lock(device->contextMutex);
        if (mapped) {
            device->context->Unmap(d3d, 0);
            mapped = false;
        }
        D3D11_MAPPED_SUBRESOURCE mappedResource = {};
        HRESULT res = device->context->Map(d3d, 0, D3D11_MAP_READ, 0, &mappedResource);
        if (FAILED(res)) {
            fprintf(stderr, "[d3d11] Map (readback) failed with error code 0x%lX.\n", res);
            return nullptr;
        }
        mapped = true;
        return mappedResource.pData;
    }

    void D3D11Buffer::unmap(uint32_t subresource, const RenderRange *writtenRange) {
        if (cpu != nullptr) {
            if (!writeWatch) {
                if (writtenRange != nullptr) {
                    markWritten(writtenRange->begin, writtenRange->end);
                }
                else {
                    markWritten(0, desc.size);
                }
            }
            return;
        }
        if (mapped && (d3d != nullptr)) {
            std::lock_guard<std::recursive_mutex> lock(device->contextMutex);
            device->context->Unmap(d3d, 0);
            mapped = false;
        }
    }

    std::unique_ptr<RenderBufferFormattedView> D3D11Buffer::createBufferFormattedView(RenderFormat format) {
        auto view = std::make_unique<D3D11BufferFormattedView>();
        std::lock_guard<std::recursive_mutex> lock(device->contextMutex);
        ID3D11Buffer *resource = (cpu != nullptr) ? gpuResource(0, 0) : d3d;
        if (resource == nullptr) {
            return view;
        }
        const uint32_t elementSize = std::max<uint32_t>(RenderFormatSize(format), 1);
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = toDXGI(format);
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = UINT(desc.size / elementSize);
        device->d3d->CreateShaderResourceView(resource, &srvDesc, &view->srv);
        if (desc.flags & RenderBufferFlag::UNORDERED_ACCESS) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = toDXGI(format);
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = UINT(desc.size / elementSize);
            device->d3d->CreateUnorderedAccessView(resource, &uavDesc, &view->uav);
        }
        return view;
    }

    void D3D11Buffer::setName(const std::string &name) {
        this->name = name;
    }

    uint64_t D3D11Buffer::getDeviceAddress() const {
        return 0;
    }

    bool D3D11Buffer::valid() const {
        return (cpu != nullptr) || (d3d != nullptr);
    }

    void D3D11Buffer::markWritten(uint64_t begin, uint64_t end) {
        if ((cpu == nullptr) || (d3d == nullptr)) {
            return;
        }
        end = std::min<uint64_t>(end, roundUp(std::max<uint64_t>(desc.size, 16), PageSize));
        if (begin >= end) {
            return;
        }
        std::lock_guard<std::recursive_mutex> lock(device->contextMutex);
        for (uint64_t page = begin / PageSize; page < (end + PageSize - 1) / PageSize; page++) {
            stalePages[size_t(page >> 6)] |= uint64_t(1) << (page & 63);
        }
    }

    ID3D11Buffer *D3D11Buffer::gpuResource(uint64_t begin, uint64_t end) {
        if (cpu == nullptr) {
            return d3d;
        }
        const uint64_t size = std::max<uint64_t>(desc.size, 16);
        if (d3d == nullptr) {
            if (twinFailed) {
                return nullptr;
            }
            // The GPU's copy, as the CPU's now; from here on its pages are brought up to date as commands read them.
            if (writeWatch) {
                ResetWriteWatch(cpu, SIZE_T(roundUp(size, PageSize)));
            }
            D3D11_BUFFER_DESC bufferDesc = {};
            bufferDesc.ByteWidth = UINT(roundUp(size, 16));
            bufferDesc.Usage = D3D11_USAGE_DEFAULT;
            bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE;
            bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            D3D11_SUBRESOURCE_DATA initialData = {};
            initialData.pSysMem = cpu;
            HRESULT res = device->d3d->CreateBuffer(&bufferDesc, &initialData, &d3d);
            if (FAILED(res)) {
                fprintf(stderr, "[d3d11] CreateBuffer (upload copy, %llu bytes) failed with error code 0x%lX.\n", (unsigned long long)size, res);
                twinFailed = true;
                d3d = nullptr;
                return nullptr;
            }
            std::fill(stalePages.begin(), stalePages.end(), 0);
            std::lock_guard<std::mutex> lock(device->uploadMutex);
            device->uploadBuffers.push_back(this);
            return d3d;
        }
        end = std::min<uint64_t>(end, size);
        if (begin >= end) {
            return d3d;
        }
        const uint64_t firstPage = begin / PageSize;
        const uint64_t endPage = (end + PageSize - 1) / PageSize;
        uint64_t page = firstPage;
        while (page < endPage) {
            const uint64_t word = stalePages[size_t(page >> 6)];
            if (word == 0) {
                page = (page | 63) + 1;
                continue;
            }
            if (((word >> (page & 63)) & 1) == 0) {
                page++;
                continue;
            }
            const uint64_t runStart = page;
            while ((page < endPage) && ((stalePages[size_t(page >> 6)] >> (page & 63)) & 1)) {
                stalePages[size_t(page >> 6)] &= ~(uint64_t(1) << (page & 63));
                page++;
            }
            D3D11_BOX box = {};
            box.left = UINT(runStart * PageSize);
            box.right = UINT(std::min<uint64_t>(page * PageSize, roundUp(size, 16)));
            box.top = 0;
            box.bottom = 1;
            box.front = 0;
            box.back = 1;
            if (box.right > box.left) {
                device->context->UpdateSubresource(d3d, 0, &box, cpu + box.left, 0, 0);
            }
        }
        return d3d;
    }

    ID3D11ShaderResourceView *D3D11Buffer::getRawSrv() {
        if (rawSrv != nullptr) {
            return rawSrv;
        }
        ID3D11Buffer *resource = (cpu != nullptr) ? gpuResource(0, 0) : d3d;
        if (resource == nullptr) {
            return nullptr;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        srvDesc.BufferEx.FirstElement = 0;
        srvDesc.BufferEx.NumElements = UINT(roundUp(std::max<uint64_t>(desc.size, 16), 16) / 4);
        srvDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        HRESULT res = device->d3d->CreateShaderResourceView(resource, &srvDesc, &rawSrv);
        if (FAILED(res)) {
            D3D11_LOG_LIMITED(8, "CreateShaderResourceView (raw buffer) failed with error code 0x%lX.", res);
            rawSrv = nullptr;
        }
        return rawSrv;
    }

    ID3D11UnorderedAccessView *D3D11Buffer::getRawUav() {
        if ((rawUav != nullptr) || (d3d == nullptr) || (cpu != nullptr)) {
            return rawUav;
        }
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = UINT(roundUp(std::max<uint64_t>(desc.size, 16), 16) / 4);
        uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        HRESULT res = device->d3d->CreateUnorderedAccessView(d3d, &uavDesc, &rawUav);
        if (FAILED(res)) {
            D3D11_LOG_LIMITED(8, "CreateUnorderedAccessView (raw buffer) failed with error code 0x%lX.", res);
            rawUav = nullptr;
        }
        return rawUav;
    }

    // D3D11TextureView

    D3D11TextureView::D3D11TextureView(D3D11Texture *texture, const RenderTextureViewDesc &desc) {
        this->texture = texture;
        this->desc = desc;
        swizzle = packSwizzle(desc.componentMapping);
        if ((texture == nullptr) || (texture->d3d == nullptr)) {
            return;
        }
        const RenderTextureDesc &textureDesc = texture->desc;
        const uint32_t textureMips = std::max<uint32_t>(textureDesc.mipLevels, 1);
        const uint32_t textureArray = std::max<uint32_t>(textureDesc.arraySize, 1);
        const uint32_t mipSlice = std::min(desc.mipSlice, textureMips - 1);
        const uint32_t mipLevels = std::min(desc.mipLevels, textureMips - mipSlice);
        const uint32_t arrayIndex = std::min(desc.arrayIndex, textureArray - 1);
        const uint32_t arraySize = std::min(desc.arraySize, textureArray - arrayIndex);
        const bool msaa = textureDesc.multisampling.sampleCount > RenderSampleCount::COUNT_1;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = toShaderViewFormat(desc.format, textureDesc.format);
        RenderTextureViewDimension dimension = desc.dimension;
        if (dimension == RenderTextureViewDimension::UNKNOWN) {
            dimension = RenderTextureDimensionToView(textureDesc.dimension);
        }
        switch (dimension) {
        case RenderTextureViewDimension::TEXTURE_1D:
            if (arraySize > 1) {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
                srvDesc.Texture1DArray.MostDetailedMip = mipSlice;
                srvDesc.Texture1DArray.MipLevels = mipLevels;
                srvDesc.Texture1DArray.FirstArraySlice = arrayIndex;
                srvDesc.Texture1DArray.ArraySize = arraySize;
            }
            else {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
                srvDesc.Texture1D.MostDetailedMip = mipSlice;
                srvDesc.Texture1D.MipLevels = mipLevels;
            }
            break;
        case RenderTextureViewDimension::TEXTURE_2D:
            if (msaa) {
                if (arraySize > 1) {
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                    srvDesc.Texture2DMSArray.FirstArraySlice = arrayIndex;
                    srvDesc.Texture2DMSArray.ArraySize = arraySize;
                }
                else {
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                }
            }
            else if (arraySize > 1) {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                srvDesc.Texture2DArray.MostDetailedMip = mipSlice;
                srvDesc.Texture2DArray.MipLevels = mipLevels;
                srvDesc.Texture2DArray.FirstArraySlice = arrayIndex;
                srvDesc.Texture2DArray.ArraySize = arraySize;
            }
            else {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = mipSlice;
                srvDesc.Texture2D.MipLevels = mipLevels;
            }
            break;
        case RenderTextureViewDimension::TEXTURE_3D:
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            srvDesc.Texture3D.MostDetailedMip = mipSlice;
            srvDesc.Texture3D.MipLevels = mipLevels;
            break;
        case RenderTextureViewDimension::TEXTURE_CUBE:
            if (arraySize > 6) {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
                srvDesc.TextureCubeArray.MostDetailedMip = mipSlice;
                srvDesc.TextureCubeArray.MipLevels = mipLevels;
                srvDesc.TextureCubeArray.First2DArrayFace = arrayIndex;
                srvDesc.TextureCubeArray.NumCubes = arraySize / 6;
            }
            else {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = mipSlice;
                srvDesc.TextureCube.MipLevels = mipLevels;
            }
            break;
        default:
            return;
        }
        HRESULT res = texture->device->d3d->CreateShaderResourceView(texture->d3d, &srvDesc, &srv);
        if (FAILED(res)) {
            D3D11_LOG_LIMITED(16, "CreateShaderResourceView (format %u, dimension %u) failed with error code 0x%lX.", unsigned(srvDesc.Format), unsigned(srvDesc.ViewDimension), res);
            srv = nullptr;
        }
    }

    D3D11TextureView::~D3D11TextureView() {
        safeRelease(srv);
    }

    // D3D11Texture

    D3D11Texture::D3D11Texture(D3D11Device *device, const RenderTextureDesc &desc) {
        this->device = device;
        this->desc = desc;
        this->desc.mipLevels = std::max<uint32_t>(desc.mipLevels, 1);
        this->desc.arraySize = std::max<uint32_t>(desc.arraySize, 1);
        this->desc.depth = std::max<uint32_t>(desc.depth, 1);
        resourceFormat = toResourceFormat(desc.format);

        UINT bind = D3D11_BIND_SHADER_RESOURCE;
        if (desc.flags & RenderTextureFlag::RENDER_TARGET) {
            bind |= D3D11_BIND_RENDER_TARGET;
        }
        if (desc.flags & RenderTextureFlag::DEPTH_TARGET) {
            bind |= D3D11_BIND_DEPTH_STENCIL;
        }
        if (desc.flags & (RenderTextureFlag::UNORDERED_ACCESS | RenderTextureFlag::STORAGE)) {
            bind |= D3D11_BIND_UNORDERED_ACCESS;
        }
        const UINT misc = (desc.flags & RenderTextureFlag::CUBE) ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        HRESULT res = E_FAIL;
        switch (desc.dimension) {
        case RenderTextureDimension::TEXTURE_1D: {
            D3D11_TEXTURE1D_DESC textureDesc = {};
            textureDesc.Width = desc.width;
            textureDesc.MipLevels = this->desc.mipLevels;
            textureDesc.ArraySize = this->desc.arraySize;
            textureDesc.Format = resourceFormat;
            textureDesc.Usage = D3D11_USAGE_DEFAULT;
            textureDesc.BindFlags = bind;
            ID3D11Texture1D *texture = nullptr;
            res = device->d3d->CreateTexture1D(&textureDesc, nullptr, &texture);
            d3d = texture;
            break;
        }
        case RenderTextureDimension::TEXTURE_2D: {
            D3D11_TEXTURE2D_DESC textureDesc = {};
            textureDesc.Width = desc.width;
            textureDesc.Height = std::max<uint32_t>(desc.height, 1);
            textureDesc.MipLevels = this->desc.mipLevels;
            textureDesc.ArraySize = this->desc.arraySize;
            textureDesc.Format = resourceFormat;
            textureDesc.SampleDesc.Count = std::max<uint32_t>(desc.multisampling.sampleCount, 1);
            textureDesc.SampleDesc.Quality = 0;
            textureDesc.Usage = D3D11_USAGE_DEFAULT;
            textureDesc.BindFlags = bind;
            textureDesc.MiscFlags = misc;
            ID3D11Texture2D *texture = nullptr;
            res = device->d3d->CreateTexture2D(&textureDesc, nullptr, &texture);
            d3d = texture;
            break;
        }
        case RenderTextureDimension::TEXTURE_3D: {
            D3D11_TEXTURE3D_DESC textureDesc = {};
            textureDesc.Width = desc.width;
            textureDesc.Height = std::max<uint32_t>(desc.height, 1);
            textureDesc.Depth = this->desc.depth;
            textureDesc.MipLevels = this->desc.mipLevels;
            textureDesc.Format = resourceFormat;
            textureDesc.Usage = D3D11_USAGE_DEFAULT;
            textureDesc.BindFlags = bind & ~UINT(D3D11_BIND_DEPTH_STENCIL);
            ID3D11Texture3D *texture = nullptr;
            res = device->d3d->CreateTexture3D(&textureDesc, nullptr, &texture);
            d3d = texture;
            break;
        }
        default:
            break;
        }
        if (FAILED(res)) {
            fprintf(stderr, "[d3d11] CreateTexture (%ux%ux%u, %u mips, %u layers, format %u, %u samples, flags 0x%X) failed with error code 0x%lX.\n",
                desc.width, desc.height, desc.depth, desc.mipLevels, desc.arraySize, unsigned(resourceFormat), desc.multisampling.sampleCount, desc.flags, res);
            d3d = nullptr;
        }
    }

    D3D11Texture::~D3D11Texture() {
        releaseViews();
        safeRelease(d3d);
    }

    void D3D11Texture::releaseViews() {
        std::lock_guard<std::mutex> lock(viewMutex);
        safeRelease(defaultSrv);
        for (auto &entry : rtvs) {
            safeRelease(entry.second);
        }
        rtvs.clear();
        for (auto &entry : dsvs) {
            safeRelease(entry.second);
        }
        dsvs.clear();
    }

    std::unique_ptr<RenderTextureView> D3D11Texture::createTextureView(const RenderTextureViewDesc &desc) const {
        return std::make_unique<D3D11TextureView>(const_cast<D3D11Texture *>(this), desc);
    }

    void D3D11Texture::setName(const std::string &name) {
    }

    uint32_t D3D11Texture::subresourceIndex(uint32_t mip, uint32_t slice) const {
        return D3D11CalcSubresource(mip, slice, desc.mipLevels);
    }

    void D3D11Texture::mipSize(uint32_t mip, uint32_t &width, uint32_t &height, uint32_t &depth) const {
        width = std::max<uint32_t>(desc.width >> mip, 1);
        height = std::max<uint32_t>(desc.height >> mip, 1);
        depth = (desc.dimension == RenderTextureDimension::TEXTURE_3D) ? std::max<uint32_t>(desc.depth >> mip, 1) : 1;
    }

    ID3D11ShaderResourceView *D3D11Texture::getDefaultSrv() {
        std::lock_guard<std::mutex> lock(viewMutex);
        if ((defaultSrv != nullptr) || (d3d == nullptr)) {
            return defaultSrv;
        }
        RenderTextureViewDesc viewDesc;
        viewDesc.format = desc.format;
        viewDesc.dimension = (desc.flags & RenderTextureFlag::CUBE) ? RenderTextureViewDimension::TEXTURE_CUBE : RenderTextureDimensionToView(desc.dimension);
        D3D11TextureView view(this, viewDesc);
        defaultSrv = view.srv;
        view.srv = nullptr;
        return defaultSrv;
    }

    ID3D11RenderTargetView *D3D11Texture::getRtv(uint32_t mip, uint32_t slice, RenderFormat format) {
        if (d3d == nullptr) {
            return nullptr;
        }
        if (format == RenderFormat::UNKNOWN) {
            format = desc.format;
        }
        const uint64_t key = uint64_t(mip) | (uint64_t(slice) << 8) | (uint64_t(format) << 32);
        std::lock_guard<std::mutex> lock(viewMutex);
        auto it = rtvs.find(key);
        if (it != rtvs.end()) {
            return it->second;
        }
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = toTargetViewFormat(format);
        const bool msaa = desc.multisampling.sampleCount > RenderSampleCount::COUNT_1;
        switch (desc.dimension) {
        case RenderTextureDimension::TEXTURE_1D:
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
            rtvDesc.Texture1D.MipSlice = mip;
            break;
        case RenderTextureDimension::TEXTURE_3D:
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            rtvDesc.Texture3D.MipSlice = mip;
            rtvDesc.Texture3D.FirstWSlice = slice;
            rtvDesc.Texture3D.WSize = 1;
            break;
        default:
            if (desc.arraySize > 1) {
                if (msaa) {
                    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    rtvDesc.Texture2DMSArray.FirstArraySlice = slice;
                    rtvDesc.Texture2DMSArray.ArraySize = 1;
                }
                else {
                    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtvDesc.Texture2DArray.MipSlice = mip;
                    rtvDesc.Texture2DArray.FirstArraySlice = slice;
                    rtvDesc.Texture2DArray.ArraySize = 1;
                }
            }
            else if (msaa) {
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
            }
            else {
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                rtvDesc.Texture2D.MipSlice = mip;
            }
            break;
        }
        ID3D11RenderTargetView *rtv = nullptr;
        HRESULT res = device->d3d->CreateRenderTargetView(d3d, &rtvDesc, &rtv);
        if (FAILED(res)) {
            D3D11_LOG_LIMITED(16, "CreateRenderTargetView (format %u) failed with error code 0x%lX.", unsigned(rtvDesc.Format), res);
            return nullptr;
        }
        rtvs[key] = rtv;
        return rtv;
    }

    ID3D11DepthStencilView *D3D11Texture::getDsv(uint32_t mip, uint32_t slice, bool readOnly) {
        if (d3d == nullptr) {
            return nullptr;
        }
        const uint64_t key = uint64_t(mip) | (uint64_t(slice) << 8) | (uint64_t(readOnly) << 32);
        std::lock_guard<std::mutex> lock(viewMutex);
        auto it = dsvs.find(key);
        if (it != dsvs.end()) {
            return it->second;
        }
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = toDepthViewFormat(desc.format);
        if (readOnly) {
            dsvDesc.Flags = D3D11_DSV_READ_ONLY_DEPTH | (RenderFormatIsStencil(desc.format) ? D3D11_DSV_READ_ONLY_STENCIL : 0);
        }
        const bool msaa = desc.multisampling.sampleCount > RenderSampleCount::COUNT_1;
        if (desc.arraySize > 1) {
            if (msaa) {
                dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
                dsvDesc.Texture2DMSArray.FirstArraySlice = slice;
                dsvDesc.Texture2DMSArray.ArraySize = 1;
            }
            else {
                dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                dsvDesc.Texture2DArray.MipSlice = mip;
                dsvDesc.Texture2DArray.FirstArraySlice = slice;
                dsvDesc.Texture2DArray.ArraySize = 1;
            }
        }
        else if (msaa) {
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        }
        else {
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dsvDesc.Texture2D.MipSlice = mip;
        }
        ID3D11DepthStencilView *dsv = nullptr;
        HRESULT res = device->d3d->CreateDepthStencilView(d3d, &dsvDesc, &dsv);
        if (FAILED(res)) {
            D3D11_LOG_LIMITED(16, "CreateDepthStencilView (format %u) failed with error code 0x%lX.", unsigned(dsvDesc.Format), res);
            return nullptr;
        }
        dsvs[key] = dsv;
        return dsv;
    }

    // D3D11Sampler

    D3D11Sampler::D3D11Sampler(D3D11Device *device, const RenderSamplerDesc &desc) {
        D3D11_SAMPLER_DESC samplerDesc = {};
        const bool comparison = desc.comparisonEnabled;
        if (desc.anisotropyEnabled) {
            samplerDesc.Filter = comparison ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
        }
        else {
            const D3D11_FILTER_TYPE minFilter = (desc.minFilter == RenderFilter::NEAREST) ? D3D11_FILTER_TYPE_POINT : D3D11_FILTER_TYPE_LINEAR;
            const D3D11_FILTER_TYPE magFilter = (desc.magFilter == RenderFilter::NEAREST) ? D3D11_FILTER_TYPE_POINT : D3D11_FILTER_TYPE_LINEAR;
            const D3D11_FILTER_TYPE mipFilter = (desc.mipmapMode == RenderMipmapMode::NEAREST) ? D3D11_FILTER_TYPE_POINT : D3D11_FILTER_TYPE_LINEAR;
            samplerDesc.Filter = D3D11_ENCODE_BASIC_FILTER(minFilter, magFilter, mipFilter, comparison ? D3D11_FILTER_REDUCTION_TYPE_COMPARISON : D3D11_FILTER_REDUCTION_TYPE_STANDARD);
        }
        samplerDesc.AddressU = toD3D11(desc.addressU);
        samplerDesc.AddressV = toD3D11(desc.addressV);
        samplerDesc.AddressW = toD3D11(desc.addressW);
        samplerDesc.MipLODBias = desc.mipLODBias;
        samplerDesc.MaxAnisotropy = std::clamp<uint32_t>(desc.maxAnisotropy, 1, 16);
        samplerDesc.ComparisonFunc = comparison ? toD3D11(desc.comparisonFunc) : D3D11_COMPARISON_NEVER;
        switch (desc.borderColor) {
        case RenderBorderColor::TRANSPARENT_BLACK:
            break;
        case RenderBorderColor::OPAQUE_WHITE:
            samplerDesc.BorderColor[0] = samplerDesc.BorderColor[1] = samplerDesc.BorderColor[2] = samplerDesc.BorderColor[3] = 1.0f;
            break;
        default:
            samplerDesc.BorderColor[3] = 1.0f;
            break;
        }
        samplerDesc.MinLOD = desc.minLOD;
        samplerDesc.MaxLOD = desc.maxLOD;
        HRESULT res = device->d3d->CreateSamplerState(&samplerDesc, &d3d);
        if (FAILED(res)) {
            D3D11_LOG_LIMITED(8, "CreateSamplerState failed with error code 0x%lX.", res);
            d3d = nullptr;
        }
    }

    D3D11Sampler::~D3D11Sampler() {
        safeRelease(d3d);
    }

    // D3D11Shader

    D3D11Shader::D3D11Shader(D3D11Device *device, const void *data, uint64_t size) {
        this->device = device;
        if ((data == nullptr) || (size == 0)) {
            return;
        }
        bytecode.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
        if (!D3D11ParseShader(bytecode.data(), bytecode.size(), info)) {
            D3D11_LOG_LIMITED(8, "A shader is not DXBC with a shader model 5 program (%llu bytes).", (unsigned long long)size);
            return;
        }
        HRESULT res = E_FAIL;
        switch (info.programType) {
        case 0:
            res = device->d3d->CreatePixelShader(bytecode.data(), bytecode.size(), nullptr, &ps);
            break;
        case 1:
            res = device->d3d->CreateVertexShader(bytecode.data(), bytecode.size(), nullptr, &vs);
            break;
        case 2:
            res = device->d3d->CreateGeometryShader(bytecode.data(), bytecode.size(), nullptr, &gs);
            break;
        case 5:
            res = device->d3d->CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &cs);
            break;
        default:
            break;
        }
        if (FAILED(res)) {
            D3D11_LOG_LIMITED(16, "Creating a shader (program type %u, %llu bytes) failed with error code 0x%lX.", info.programType, (unsigned long long)size, res);
        }
    }

    D3D11Shader::~D3D11Shader() {
        safeRelease(vs);
        safeRelease(ps);
        safeRelease(gs);
        safeRelease(cs);
    }

    void D3D11Shader::setName(const std::string &name) {
    }

    bool D3D11Shader::valid() const {
        return (vs != nullptr) || (ps != nullptr) || (gs != nullptr) || (cs != nullptr);
    }

    // D3D11GraphicsPipeline

    D3D11GraphicsPipeline::D3D11GraphicsPipeline(D3D11Device *device, const RenderGraphicsPipelineDesc &desc) : D3D11Pipeline(device, Type::Graphics) {
        layout = static_cast<const D3D11PipelineLayout *>(desc.pipelineLayout);
        const D3D11Shader *vertexShader = static_cast<const D3D11Shader *>(desc.vertexShader);
        const D3D11Shader *geometryShader = static_cast<const D3D11Shader *>(desc.geometryShader);
        const D3D11Shader *pixelShader = static_cast<const D3D11Shader *>(desc.pixelShader);
        if ((vertexShader == nullptr) || (vertexShader->vs == nullptr)) {
            return;
        }
        vs = vertexShader->vs;
        vs->AddRef();
        stageInfo[D3D11StageVertex] = vertexShader->info;
        hasStage[D3D11StageVertex] = true;
        if (geometryShader != nullptr) {
            if (geometryShader->gs == nullptr) {
                return;
            }
            gs = geometryShader->gs;
            gs->AddRef();
            stageInfo[D3D11StageGeometry] = geometryShader->info;
            hasStage[D3D11StageGeometry] = true;
        }
        if (pixelShader != nullptr) {
            if (pixelShader->ps == nullptr) {
                return;
            }
            ps = pixelShader->ps;
            ps->AddRef();
            stageInfo[D3D11StagePixel] = pixelShader->info;
            hasStage[D3D11StagePixel] = true;
        }
        for (uint32_t s = 0; s < 3; s++) {
            if (hasStage[s]) {
                uavMask |= stageInfo[s].uavMask;
            }
        }

        // Input layout.
        if (desc.inputElementsCount > 0) {
            std::vector<D3D11_INPUT_ELEMENT_DESC> elements(desc.inputElementsCount);
            for (uint32_t i = 0; i < desc.inputElementsCount; i++) {
                const RenderInputElement &element = desc.inputElements[i];
                D3D11_INPUT_ELEMENT_DESC &out = elements[i];
                out.SemanticName = element.semanticName;
                out.SemanticIndex = element.semanticIndex;
                out.Format = toDXGI(element.format);
                out.InputSlot = element.slotIndex;
                out.AlignedByteOffset = element.alignedByteOffset;
                out.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                out.InstanceDataStepRate = 0;
                for (uint32_t s = 0; s < desc.inputSlotsCount; s++) {
                    if ((desc.inputSlots[s].index == element.slotIndex) && (desc.inputSlots[s].classification == RenderInputSlotClassification::PER_INSTANCE_DATA)) {
                        out.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
                        out.InstanceDataStepRate = 1;
                    }
                }
            }
            HRESULT res = device->d3d->CreateInputLayout(elements.data(), UINT(elements.size()), vertexShader->bytecode.data(), vertexShader->bytecode.size(), &inputLayout);
            if (FAILED(res)) {
                D3D11_LOG_LIMITED(16, "CreateInputLayout (%u elements) failed with error code 0x%lX.", desc.inputElementsCount, res);
                return;
            }
        }

        // Rasterizer.
        D3D11_RASTERIZER_DESC rasterizerDesc = {};
        rasterizerDesc.FillMode = D3D11_FILL_SOLID;
        switch (desc.cullMode) {
        case RenderCullMode::FRONT: rasterizerDesc.CullMode = D3D11_CULL_FRONT; break;
        case RenderCullMode::BACK: rasterizerDesc.CullMode = D3D11_CULL_BACK; break;
        default: rasterizerDesc.CullMode = D3D11_CULL_NONE; break;
        }
        rasterizerDesc.FrontCounterClockwise = (desc.frontFace == RenderFrontFace::COUNTER_CLOCKWISE);
        rasterizerDesc.DepthBias = desc.depthBias;
        rasterizerDesc.DepthBiasClamp = desc.depthBiasClamp;
        rasterizerDesc.SlopeScaledDepthBias = desc.slopeScaledDepthBias;
        rasterizerDesc.DepthClipEnable = desc.depthClipEnabled;
        rasterizerDesc.ScissorEnable = TRUE;
        rasterizerDesc.MultisampleEnable = desc.multisampling.sampleCount > RenderSampleCount::COUNT_1;
        rasterizerDesc.AntialiasedLineEnable = FALSE;
        if (FAILED(device->d3d->CreateRasterizerState(&rasterizerDesc, &rasterizer))) {
            return;
        }

        // Depth and stencil.
        D3D11_DEPTH_STENCIL_DESC depthDesc = {};
        depthDesc.DepthEnable = desc.depthEnabled;
        depthDesc.DepthWriteMask = desc.depthWriteEnabled ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        depthDesc.DepthFunc = toD3D11(desc.depthFunction);
        depthDesc.StencilEnable = desc.stencilEnabled;
        depthDesc.StencilReadMask = UINT8(desc.stencilReadMask & 0xFF);
        depthDesc.StencilWriteMask = UINT8(desc.stencilWriteMask & 0xFF);
        auto stencilFace = [](const RenderStencilFaceDesc &face) {
            D3D11_DEPTH_STENCILOP_DESC out = {};
            out.StencilFailOp = toD3D11(face.failOp);
            out.StencilDepthFailOp = toD3D11(face.depthFailOp);
            out.StencilPassOp = toD3D11(face.passOp);
            out.StencilFunc = toD3D11(face.compareFunction);
            return out;
        };
        depthDesc.FrontFace = stencilFace(desc.stencilFrontFace);
        depthDesc.BackFace = stencilFace(desc.stencilBackFace);
        if (FAILED(device->d3d->CreateDepthStencilState(&depthDesc, &depthStencil))) {
            return;
        }
        stencilReference = desc.stencilReference & 0xFF;

        // Blend.
        HRESULT res = E_FAIL;
        if ((device->d3d1 != nullptr) && desc.logicOpEnabled) {
            D3D11_BLEND_DESC1 blendDesc = {};
            blendDesc.AlphaToCoverageEnable = desc.alphaToCoverageEnabled;
            blendDesc.IndependentBlendEnable = FALSE;
            blendDesc.RenderTarget[0].LogicOpEnable = TRUE;
            blendDesc.RenderTarget[0].LogicOp = toD3D11(desc.logicOp);
            blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
            blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].RenderTargetWriteMask = desc.renderTargetCount ? desc.renderTargetBlend[0].renderTargetWriteMask : D3D11_COLOR_WRITE_ENABLE_ALL;
            ID3D11BlendState1 *blend1 = nullptr;
            res = device->d3d1->CreateBlendState1(&blendDesc, &blend1);
            blend = blend1;
        }
        else {
            D3D11_BLEND_DESC blendDesc = {};
            blendDesc.AlphaToCoverageEnable = desc.alphaToCoverageEnabled;
            blendDesc.IndependentBlendEnable = TRUE;
            for (uint32_t i = 0; i < 8; i++) {
                D3D11_RENDER_TARGET_BLEND_DESC &target = blendDesc.RenderTarget[i];
                if (i >= desc.renderTargetCount) {
                    target.BlendEnable = FALSE;
                    target.SrcBlend = D3D11_BLEND_ONE;
                    target.DestBlend = D3D11_BLEND_ZERO;
                    target.BlendOp = D3D11_BLEND_OP_ADD;
                    target.SrcBlendAlpha = D3D11_BLEND_ONE;
                    target.DestBlendAlpha = D3D11_BLEND_ZERO;
                    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                    continue;
                }
                const RenderBlendDesc &source = desc.renderTargetBlend[i];
                target.BlendEnable = source.blendEnabled;
                target.SrcBlend = toD3D11(source.srcBlend, D3D11_BLEND_ONE);
                target.DestBlend = toD3D11(source.dstBlend, D3D11_BLEND_ZERO);
                target.BlendOp = toD3D11(source.blendOp);
                target.SrcBlendAlpha = toAlphaBlend(toD3D11(source.srcBlendAlpha, D3D11_BLEND_ONE));
                target.DestBlendAlpha = toAlphaBlend(toD3D11(source.dstBlendAlpha, D3D11_BLEND_ZERO));
                target.BlendOpAlpha = toD3D11(source.blendOpAlpha);
                target.RenderTargetWriteMask = source.renderTargetWriteMask & 0xF;
            }
            res = device->d3d->CreateBlendState(&blendDesc, &blend);
        }
        if (FAILED(res)) {
            D3D11_LOG_LIMITED(16, "CreateBlendState failed with error code 0x%lX.", res);
            return;
        }

        if (desc.primitiveTopology == RenderPrimitiveTopology::TRIANGLE_FAN) {
            D3D11_LOG_LIMITED(4, "Triangle fans are not supported: drawn as lists.");
        }
        topology = toD3D11(desc.primitiveTopology);
        isValid = true;
    }

    D3D11GraphicsPipeline::~D3D11GraphicsPipeline() {
        safeRelease(vs);
        safeRelease(gs);
        safeRelease(ps);
        safeRelease(inputLayout);
        safeRelease(rasterizer);
        safeRelease(blend);
        safeRelease(depthStencil);
    }

    // D3D11ComputePipeline

    D3D11ComputePipeline::D3D11ComputePipeline(D3D11Device *device, const RenderComputePipelineDesc &desc) : D3D11Pipeline(device, Type::Compute) {
        layout = static_cast<const D3D11PipelineLayout *>(desc.pipelineLayout);
        const D3D11Shader *shader = static_cast<const D3D11Shader *>(desc.computeShader);
        if ((shader == nullptr) || (shader->cs == nullptr)) {
            return;
        }
        cs = shader->cs;
        cs->AddRef();
        info = shader->info;
        isValid = true;
    }

    D3D11ComputePipeline::~D3D11ComputePipeline() {
        safeRelease(cs);
    }

    // D3D11DescriptorSet

    D3D11DescriptorSet::D3D11DescriptorSet(D3D11Device *device, const RenderDescriptorSetDesc &desc) {
        this->device = device;
        for (uint32_t i = 0; i < desc.descriptorRangesCount; i++) {
            const RenderDescriptorRange &range = desc.descriptorRanges[i];
            uint32_t count = range.count;
            if (desc.lastRangeIsBoundless && (i + 1 == desc.descriptorRangesCount)) {
                count = std::max(count, desc.boundlessRangeSize);
            }
            types.insert(types.end(), count, range.type);
        }
        descriptors.resize(types.size());
        for (size_t i = 0; i < descriptors.size(); i++) {
            descriptors[i].type = types[i];
        }
    }

    D3D11DescriptorSet::~D3D11DescriptorSet() {
        for (D3D11Descriptor &descriptor : descriptors) {
            clearOwned(descriptor);
        }
    }

    void D3D11DescriptorSet::clearOwned(D3D11Descriptor &descriptor) {
        safeRelease(descriptor.srv);
        safeRelease(descriptor.uav);
        safeRelease(descriptor.sampler);
        descriptor.buffer = nullptr;
        descriptor.bufferSize = 0;
        descriptor.swizzle = IdentitySwizzle;
        safeRelease(descriptor.ownedSrv);
        safeRelease(descriptor.ownedUav);
    }

    // The set holds a reference to each view it has (a view destroyed while a set still names it stays valid).
    void D3D11DescriptorSet::setBuffer(uint32_t descriptorIndex, const RenderBuffer *buffer, uint64_t bufferSize, const RenderBufferStructuredView *bufferStructuredView, const RenderBufferFormattedView *bufferFormattedView) {
        if (descriptorIndex >= descriptors.size()) {
            return;
        }
        D3D11Descriptor &descriptor = descriptors[descriptorIndex];
        clearOwned(descriptor);
        D3D11Buffer *interfaceBuffer = mutableBuffer(buffer);
        if (interfaceBuffer == nullptr) {
            return;
        }
        const D3D11BufferFormattedView *formattedView = static_cast<const D3D11BufferFormattedView *>(bufferFormattedView);
        switch (descriptor.type) {
        case RenderDescriptorRangeType::CONSTANT_BUFFER:
        case RenderDescriptorRangeType::CONSTANT_BUFFER_DYNAMIC:
            descriptor.buffer = interfaceBuffer;
            descriptor.bufferSize = bufferSize ? bufferSize : interfaceBuffer->desc.size;
            break;
        case RenderDescriptorRangeType::FORMATTED_BUFFER:
            descriptor.srv = (formattedView != nullptr) ? formattedView->srv : nullptr;
            break;
        case RenderDescriptorRangeType::READ_WRITE_FORMATTED_BUFFER:
            descriptor.uav = (formattedView != nullptr) ? formattedView->uav : nullptr;
            break;
        case RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER:
        case RenderDescriptorRangeType::STRUCTURED_BUFFER: {
            std::lock_guard<std::recursive_mutex> lock(device->contextMutex);
            descriptor.srv = interfaceBuffer->getRawSrv();
            break;
        }
        case RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER:
        case RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER:
            descriptor.uav = interfaceBuffer->getRawUav();
            break;
        default:
            break;
        }
        if (descriptor.srv != nullptr) {
            descriptor.srv->AddRef();
        }
        if (descriptor.uav != nullptr) {
            descriptor.uav->AddRef();
        }
    }

    void D3D11DescriptorSet::setTexture(uint32_t descriptorIndex, const RenderTexture *texture, RenderTextureLayout textureLayout, const RenderTextureView *textureView) {
        if (descriptorIndex >= descriptors.size()) {
            return;
        }
        D3D11Descriptor &descriptor = descriptors[descriptorIndex];
        clearOwned(descriptor);
        D3D11Texture *interfaceTexture = const_cast<D3D11Texture *>(static_cast<const D3D11Texture *>(texture));
        if ((interfaceTexture == nullptr) || (interfaceTexture->d3d == nullptr)) {
            return;
        }
        const D3D11TextureView *view = static_cast<const D3D11TextureView *>(textureView);
        if (descriptor.type == RenderDescriptorRangeType::READ_WRITE_TEXTURE) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = toShaderViewFormat(view ? view->desc.format : interfaceTexture->desc.format, interfaceTexture->desc.format);
            switch (interfaceTexture->desc.dimension) {
            case RenderTextureDimension::TEXTURE_1D:
                uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
                uavDesc.Texture1D.MipSlice = view ? view->desc.mipSlice : 0;
                break;
            case RenderTextureDimension::TEXTURE_3D:
                uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
                uavDesc.Texture3D.MipSlice = view ? view->desc.mipSlice : 0;
                uavDesc.Texture3D.FirstWSlice = 0;
                uavDesc.Texture3D.WSize = UINT(-1);
                break;
            default:
                uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = view ? view->desc.mipSlice : 0;
                break;
            }
            interfaceTexture->device->d3d->CreateUnorderedAccessView(interfaceTexture->d3d, &uavDesc, &descriptor.ownedUav);
            descriptor.uav = descriptor.ownedUav;
            if (descriptor.uav != nullptr) {
                descriptor.uav->AddRef();
            }
            return;
        }
        if (view != nullptr) {
            descriptor.srv = view->srv;
            descriptor.swizzle = view->swizzle;
        }
        else {
            descriptor.srv = interfaceTexture->getDefaultSrv();
        }
        if (descriptor.srv != nullptr) {
            descriptor.srv->AddRef();
        }
    }

    void D3D11DescriptorSet::setSampler(uint32_t descriptorIndex, const RenderSampler *sampler) {
        if (descriptorIndex >= descriptors.size()) {
            return;
        }
        D3D11Descriptor &descriptor = descriptors[descriptorIndex];
        clearOwned(descriptor);
        const D3D11Sampler *interfaceSampler = static_cast<const D3D11Sampler *>(sampler);
        descriptor.sampler = (interfaceSampler != nullptr) ? interfaceSampler->d3d : nullptr;
        if (descriptor.sampler != nullptr) {
            descriptor.sampler->AddRef();
        }
    }

    void D3D11DescriptorSet::setAccelerationStructure(uint32_t descriptorIndex, const RenderAccelerationStructure *accelerationStructure) {
    }

    // D3D11PipelineLayout

    D3D11PipelineLayout::D3D11PipelineLayout(D3D11Device *device, const RenderPipelineLayoutDesc &desc) {
        this->device = device;
        for (uint32_t i = 0; i < desc.pushConstantRangesCount; i++) {
            const RenderPushConstantRange &range = desc.pushConstantRanges[i];
            PushRange out;
            out.offset = range.offset;
            out.size = range.size;
            out.reg = range.binding;
            out.stages = range.stageFlags;
            pushRanges.push_back(out);
        }
        for (uint32_t i = 0; i < desc.rootDescriptorDescsCount; i++) {
            Root out;
            out.type = desc.rootDescriptorDescs[i].type;
            out.reg = desc.rootDescriptorDescs[i].shaderRegister;
            roots.push_back(out);
        }
        for (uint32_t i = 0; i < desc.descriptorSetDescsCount; i++) {
            const RenderDescriptorSetDesc &setDesc = desc.descriptorSetDescs[i];
            Set out;
            out.ranges.assign(setDesc.descriptorRanges, setDesc.descriptorRanges + setDesc.descriptorRangesCount);
            out.table = setDesc.lastRangeIsBoundless;
            sets.push_back(out);
        }
    }

    D3D11PipelineLayout::~D3D11PipelineLayout() {
    }

    // D3D11Framebuffer

    D3D11Framebuffer::D3D11Framebuffer(D3D11Device *device, const RenderFramebufferDesc &desc) {
        for (uint32_t i = 0; i < desc.colorAttachmentsCount; i++) {
            D3D11Texture *texture = const_cast<D3D11Texture *>(static_cast<const D3D11Texture *>(desc.colorAttachments[i]));
            const D3D11TextureView *view = (desc.colorAttachmentViews != nullptr) ? static_cast<const D3D11TextureView *>(desc.colorAttachmentViews[i]) : nullptr;
            ID3D11RenderTargetView *rtv = nullptr;
            const uint32_t mip = view ? view->desc.mipSlice : 0;
            const uint32_t slice = view ? view->desc.arrayIndex : 0;
            uint32_t colorWidth = 0, colorHeight = 0, colorDepth = 0;
            RenderFormat format = RenderFormat::UNKNOWN;
            if (texture != nullptr) {
                format = (view && (view->desc.format != RenderFormat::UNKNOWN)) ? view->desc.format : texture->desc.format;
                rtv = texture->getRtv(mip, slice, format);
                texture->mipSize(mip, colorWidth, colorHeight, colorDepth);
                if ((width == 0) && (height == 0)) {
                    width = colorWidth;
                    height = colorHeight;
                }
                else if ((colorWidth != width) || (colorHeight != height)) {
                    mismatched = true;
                }
            }
            if (rtv != nullptr) {
                rtv->AddRef();
            }
            rtvs.push_back(rtv);
            colorTextures.push_back(texture);
            colorMips.push_back(mip);
            colorSlices.push_back(slice);
            colorWidths.push_back(colorWidth);
            colorHeights.push_back(colorHeight);
            colorFormats.push_back(format);
        }
        if (desc.depthAttachment != nullptr) {
            D3D11Texture *texture = const_cast<D3D11Texture *>(static_cast<const D3D11Texture *>(desc.depthAttachment));
            const D3D11TextureView *view = static_cast<const D3D11TextureView *>(desc.depthAttachmentView);
            const uint32_t mip = view ? view->desc.mipSlice : 0;
            dsv = texture->getDsv(mip, view ? view->desc.arrayIndex : 0, desc.depthAttachmentReadOnly);
            if (dsv != nullptr) {
                dsv->AddRef();
            }
            depthTexture = texture;
            uint32_t depthDepth = 0;
            texture->mipSize(mip, depthWidth, depthHeight, depthDepth);
            if ((width == 0) && (height == 0)) {
                width = depthWidth;
                height = depthHeight;
            }
            else if ((width != depthWidth) || (height != depthHeight)) {
                mismatched = true;
            }
            if (mismatched || !rtvs.empty()) {
                // The framebuffer's size is the depth target's: color targets of another size draw into proxies.
                width = depthWidth;
                height = depthHeight;
            }
        }
        if (mismatched) {
            D3D11_LOG_LIMITED(16, "A framebuffer's targets differ in size (color %ux%u, depth %ux%u): the color targets of another size are drawn through proxies.",
                colorWidths.empty() ? 0u : colorWidths[0], colorHeights.empty() ? 0u : colorHeights[0], depthWidth, depthHeight);
        }
    }

    D3D11Framebuffer::~D3D11Framebuffer() {
        for (ID3D11RenderTargetView *&rtv : rtvs) {
            safeRelease(rtv);
        }
        safeRelease(dsv);
    }

    uint32_t D3D11Framebuffer::getWidth() const {
        return width;
    }

    uint32_t D3D11Framebuffer::getHeight() const {
        return height;
    }

    // D3D11CommandFence

    D3D11CommandFence::D3D11CommandFence(D3D11Device *device) {
        this->device = device;
        if (device->d3d5 != nullptr) {
            if (SUCCEEDED(device->d3d5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                return;
            }
            fence = nullptr;
        }
        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_EVENT;
        if (FAILED(device->d3d->CreateQuery(&queryDesc, &query))) {
            query = nullptr;
        }
    }

    D3D11CommandFence::~D3D11CommandFence() {
        safeRelease(fence);
        safeRelease(query);
        if (event != nullptr) {
            CloseHandle(event);
        }
    }

    void D3D11CommandFence::signalLocked() {
        if ((fence != nullptr) && (device->context4 != nullptr)) {
            value++;
            device->context4->Signal(fence, value);
        }
        else if (query != nullptr) {
            device->context->End(query);
        }
        pending = true;
    }

    void D3D11CommandFence::wait() {
        if (!pending) {
            return;
        }
        if ((fence != nullptr) && (device->context4 != nullptr)) {
            if (fence->GetCompletedValue() < value) {
                {
                    std::lock_guard<std::recursive_mutex> lock(device->contextMutex);
                    device->context->Flush();
                }
                if (SUCCEEDED(fence->SetEventOnCompletion(value, event))) {
                    WaitForSingleObject(event, INFINITE);
                }
            }
            pending = false;
            return;
        }
        if (query == nullptr) {
            std::lock_guard<std::recursive_mutex> lock(device->contextMutex);
            device->context->Flush();
            pending = false;
            return;
        }
        for (uint32_t spin = 0;; spin++) {
            HRESULT res;
            {
                std::lock_guard<std::recursive_mutex> lock(device->contextMutex);
                res = device->context->GetData(query, nullptr, 0, 0);
            }
            if (res != S_FALSE) {
                break;
            }
            if (spin < 64) {
                YieldProcessor();
            }
            else {
                Sleep(0);
            }
        }
        pending = false;
    }

    // D3D11CommandList

    D3D11CommandList::D3D11CommandList(D3D11CommandQueue *queue) {
        this->queue = queue;
    }

    D3D11CommandList::~D3D11CommandList() {
    }

    D3D11Command &D3D11CommandList::emit(D3D11Command::Op op) {
        commands.emplace_back();
        D3D11Command &command = commands.back();
        command.op = op;
        return command;
    }

    uint32_t D3D11CommandList::store(const void *bytes, size_t size) {
        const size_t offset = (data.size() + 15) & ~size_t(15);
        data.resize(offset + size);
        if (size > 0) {
            memcpy(data.data() + offset, bytes, size);
        }
        return uint32_t(offset);
    }

    void D3D11CommandList::begin() {
        commands.clear();
        data.clear();
        recordGraphicsLayout = nullptr;
        recordComputeLayout = nullptr;
    }

    void D3D11CommandList::end() {
    }

    void D3D11CommandList::barriers(RenderBarrierStages stages, const RenderBufferBarrier *bufferBarriers, uint32_t bufferBarriersCount, const RenderTextureBarrier *textureBarriers, uint32_t textureBarriersCount) {
        // Direct3D 11 orders the commands itself.
    }

    void D3D11CommandList::dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) {
        D3D11Command &command = emit(D3D11Command::Op::Dispatch);
        command.a = threadGroupCountX;
        command.b = threadGroupCountY;
        command.c = threadGroupCountZ;
    }

    void D3D11CommandList::traceRays(uint32_t width, uint32_t height, uint32_t depth, RenderBufferReference shaderBindingTable, const RenderShaderBindingGroupsInfo &shaderBindingGroupsInfo) {
    }

    void D3D11CommandList::drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation) {
        D3D11Command &command = emit(D3D11Command::Op::Draw);
        command.a = vertexCountPerInstance;
        command.b = instanceCount;
        command.c = startVertexLocation;
        command.d = startInstanceLocation;
    }

    void D3D11CommandList::drawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation) {
        D3D11Command &command = emit(D3D11Command::Op::DrawIndexed);
        command.a = indexCountPerInstance;
        command.b = instanceCount;
        command.c = startIndexLocation;
        command.d = uint32_t(baseVertexLocation);
        command.e = startInstanceLocation;
    }

    void D3D11CommandList::setPipeline(const RenderPipeline *pipeline) {
        emit(D3D11Command::Op::SetPipeline).p0 = pipeline;
    }

    void D3D11CommandList::setComputePipelineLayout(const RenderPipelineLayout *pipelineLayout) {
        recordComputeLayout = static_cast<const D3D11PipelineLayout *>(pipelineLayout);
        emit(D3D11Command::Op::SetComputeLayout).p0 = pipelineLayout;
    }

    static uint32_t pushSize(const D3D11PipelineLayout *layout, uint32_t rangeIndex, uint32_t offset, uint32_t size) {
        if ((size == 0) && (layout != nullptr) && (rangeIndex < layout->pushRanges.size())) {
            size = layout->pushRanges[rangeIndex].size;
        }
        if (offset >= 256) {
            return 0;
        }
        return std::min(size, 256 - offset);
    }

    void D3D11CommandList::setComputePushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) {
        size = pushSize(recordComputeLayout, rangeIndex, offset, size);
        if ((size == 0) || (rangeIndex >= 4)) {
            return;
        }
        const uint32_t stored = store(data, size);
        D3D11Command &command = emit(D3D11Command::Op::ComputePush);
        command.a = rangeIndex;
        command.b = offset;
        command.c = size;
        command.data = stored;
    }

    void D3D11CommandList::setComputeDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) {
        D3D11Command &command = emit(D3D11Command::Op::ComputeSet);
        command.p0 = descriptorSet;
        command.a = setIndex;
    }

    void D3D11CommandList::setGraphicsPipelineLayout(const RenderPipelineLayout *pipelineLayout) {
        recordGraphicsLayout = static_cast<const D3D11PipelineLayout *>(pipelineLayout);
        emit(D3D11Command::Op::SetGraphicsLayout).p0 = pipelineLayout;
    }

    void D3D11CommandList::setGraphicsPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) {
        size = pushSize(recordGraphicsLayout, rangeIndex, offset, size);
        if ((size == 0) || (rangeIndex >= 4)) {
            return;
        }
        const uint32_t stored = store(data, size);
        D3D11Command &command = emit(D3D11Command::Op::GraphicsPush);
        command.a = rangeIndex;
        command.b = offset;
        command.c = size;
        command.data = stored;
    }

    void D3D11CommandList::setGraphicsDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) {
        D3D11Command &command = emit(D3D11Command::Op::GraphicsSet);
        command.p0 = descriptorSet;
        command.a = setIndex;
    }

    void D3D11CommandList::setGraphicsRootDescriptor(RenderBufferReference bufferReference, uint32_t rootDescriptorIndex) {
        D3D11Command &command = emit(D3D11Command::Op::GraphicsRoot);
        command.p0 = bufferReference.ref;
        command.u0 = bufferReference.offset;
        command.a = rootDescriptorIndex;
    }

    void D3D11CommandList::setRaytracingPipelineLayout(const RenderPipelineLayout *pipelineLayout) {
    }

    void D3D11CommandList::setRaytracingPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) {
    }

    void D3D11CommandList::setRaytracingDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) {
    }

    void D3D11CommandList::setIndexBuffer(const RenderIndexBufferView *view) {
        D3D11Command &command = emit(D3D11Command::Op::IndexBuffer);
        if (view != nullptr) {
            command.p0 = view->buffer.ref;
            command.u0 = view->buffer.offset;
            command.a = view->size;
            command.b = uint32_t(view->format);
        }
    }

    struct D3D11VertexBinding {
        const RenderBuffer *buffer;
        uint64_t offset;
        uint32_t size;
        uint32_t stride;
    };

    void D3D11CommandList::setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView *views, uint32_t viewCount, const RenderInputSlot *inputSlots) {
        std::vector<D3D11VertexBinding> bindings(viewCount);
        for (uint32_t i = 0; i < viewCount; i++) {
            bindings[i].buffer = views[i].buffer.ref;
            bindings[i].offset = views[i].buffer.offset;
            bindings[i].size = views[i].size;
            bindings[i].stride = (inputSlots != nullptr) ? inputSlots[i].stride : 0;
        }
        const uint32_t stored = store(bindings.data(), bindings.size() * sizeof(D3D11VertexBinding));
        D3D11Command &command = emit(D3D11Command::Op::VertexBuffers);
        command.a = startSlot;
        command.b = viewCount;
        command.data = stored;
    }

    void D3D11CommandList::setViewports(const RenderViewport *viewports, uint32_t count) {
        count = std::min<uint32_t>(count, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
        D3D11_VIEWPORT out[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        for (uint32_t i = 0; i < count; i++) {
            out[i].TopLeftX = viewports[i].x;
            out[i].TopLeftY = viewports[i].y;
            out[i].Width = viewports[i].width;
            out[i].Height = viewports[i].height;
            out[i].MinDepth = viewports[i].minDepth;
            out[i].MaxDepth = viewports[i].maxDepth;
        }
        const uint32_t stored = store(out, count * sizeof(D3D11_VIEWPORT));
        D3D11Command &command = emit(D3D11Command::Op::Viewports);
        command.a = count;
        command.data = stored;
    }

    void D3D11CommandList::setScissors(const RenderRect *scissorRects, uint32_t count) {
        count = std::min<uint32_t>(count, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
        D3D11_RECT out[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        for (uint32_t i = 0; i < count; i++) {
            out[i].left = scissorRects[i].left;
            out[i].top = scissorRects[i].top;
            out[i].right = scissorRects[i].right;
            out[i].bottom = scissorRects[i].bottom;
        }
        const uint32_t stored = store(out, count * sizeof(D3D11_RECT));
        D3D11Command &command = emit(D3D11Command::Op::Scissors);
        command.a = count;
        command.data = stored;
    }

    void D3D11CommandList::setFramebuffer(const RenderFramebuffer *framebuffer) {
        emit(D3D11Command::Op::Framebuffer).p0 = framebuffer;
    }

    void D3D11CommandList::setDepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias) {
        D3D11_LOG_LIMITED(1, "Dynamic depth bias is not supported (the pipelines' own is used).");
    }

    void D3D11CommandList::clearColor(uint32_t attachmentIndex, RenderColor colorValue, const RenderRect *clearRects, uint32_t clearRectsCount) {
        std::vector<uint8_t> payload(16 + size_t(clearRectsCount) * sizeof(D3D11_RECT));
        memcpy(payload.data(), colorValue.rgba, 16);
        for (uint32_t i = 0; i < clearRectsCount; i++) {
            D3D11_RECT rect = { clearRects[i].left, clearRects[i].top, clearRects[i].right, clearRects[i].bottom };
            memcpy(payload.data() + 16 + i * sizeof(D3D11_RECT), &rect, sizeof(rect));
        }
        const uint32_t stored = store(payload.data(), payload.size());
        D3D11Command &command = emit(D3D11Command::Op::ClearColor);
        command.a = attachmentIndex;
        command.b = clearRectsCount;
        command.data = stored;
    }

    void D3D11CommandList::clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue, uint32_t stencilValue, const RenderRect *clearRects, uint32_t clearRectsCount) {
        std::vector<D3D11_RECT> rects(clearRectsCount);
        for (uint32_t i = 0; i < clearRectsCount; i++) {
            rects[i] = { clearRects[i].left, clearRects[i].top, clearRects[i].right, clearRects[i].bottom };
        }
        const uint32_t stored = store(rects.data(), rects.size() * sizeof(D3D11_RECT));
        D3D11Command &command = emit(D3D11Command::Op::ClearDepth);
        command.a = (clearDepth ? 1u : 0u) | (clearStencil ? 2u : 0u);
        command.b = stencilValue;
        memcpy(&command.c, &depthValue, 4);
        command.d = clearRectsCount;
        command.data = stored;
    }

    void D3D11CommandList::copyBufferRegion(RenderBufferReference dstBuffer, RenderBufferReference srcBuffer, uint64_t size) {
        D3D11Command &command = emit(D3D11Command::Op::CopyBufferRegion);
        command.p0 = dstBuffer.ref;
        command.u0 = dstBuffer.offset;
        command.p1 = srcBuffer.ref;
        command.u1 = srcBuffer.offset;
        command.u2 = size;
    }

    struct D3D11TextureCopy {
        RenderTextureCopyLocation dst;
        RenderTextureCopyLocation src;
        RenderBox box;
        uint32_t hasBox;
    };

    void D3D11CommandList::copyTextureRegion(const RenderTextureCopyLocation &dstLocation, const RenderTextureCopyLocation &srcLocation, uint32_t dstX, uint32_t dstY, uint32_t dstZ, const RenderBox *srcBox) {
        D3D11TextureCopy copy;
        copy.dst = dstLocation;
        copy.src = srcLocation;
        copy.hasBox = (srcBox != nullptr) ? 1 : 0;
        if (srcBox != nullptr) {
            copy.box = *srcBox;
        }
        const uint32_t stored = store(&copy, sizeof(copy));
        D3D11Command &command = emit(D3D11Command::Op::CopyTextureRegion);
        command.a = dstX;
        command.b = dstY;
        command.c = dstZ;
        command.data = stored;
    }

    void D3D11CommandList::copyBuffer(const RenderBuffer *dstBuffer, const RenderBuffer *srcBuffer) {
        D3D11Command &command = emit(D3D11Command::Op::CopyBuffer);
        command.p0 = dstBuffer;
        command.p1 = srcBuffer;
    }

    void D3D11CommandList::copyTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) {
        D3D11Command &command = emit(D3D11Command::Op::CopyTexture);
        command.p0 = dstTexture;
        command.p1 = srcTexture;
    }

    void D3D11CommandList::resolveTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) {
        D3D11Command &command = emit(D3D11Command::Op::Resolve);
        command.p0 = dstTexture;
        command.p1 = srcTexture;
    }

    void D3D11CommandList::resolveTextureRegion(const RenderTexture *dstTexture, uint32_t dstX, uint32_t dstY, const RenderTexture *srcTexture, const RenderRect *srcRect, RenderResolveMode resolveMode) {
        if (resolveMode != RenderResolveMode::AVERAGE) {
            D3D11_LOG_LIMITED(1, "Resolves other than the average are not supported: averaged.");
        }
        uint32_t stored = 0;
        if (srcRect != nullptr) {
            stored = store(srcRect, sizeof(RenderRect));
        }
        D3D11Command &command = emit(D3D11Command::Op::ResolveRegion);
        command.p0 = dstTexture;
        command.p1 = srcTexture;
        command.a = dstX;
        command.b = dstY;
        command.c = (srcRect != nullptr) ? 1 : 0;
        command.data = stored;
    }

    void D3D11CommandList::buildBottomLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, const RenderBottomLevelASBuildInfo &buildInfo) {
    }

    void D3D11CommandList::buildTopLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, RenderBufferReference instancesBuffer, const RenderTopLevelASBuildInfo &buildInfo) {
    }

    void D3D11CommandList::discardTexture(const RenderTexture *texture) {
        emit(D3D11Command::Op::Discard).p0 = texture;
    }

    void D3D11CommandList::resetQueryPool(const RenderQueryPool *queryPool, uint32_t queryFirstIndex, uint32_t queryCount) {
    }

    void D3D11CommandList::writeTimestamp(const RenderQueryPool *queryPool, uint32_t queryIndex) {
    }

    // D3D11SwapChain

    D3D11SwapChain::D3D11SwapChain(D3D11CommandQueue *queue, const RenderSwapChainDesc &desc) {
        this->queue = queue;
        this->desc = desc;
        D3D11Device *device = queue->device;
        nativeFormat = toDXGI(desc.format);
        getWindowSize(width, height);

        IDXGIFactory2 *factory2 = nullptr;
        if (FAILED(device->renderInterface->dxgiFactory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
            fprintf(stderr, "[d3d11] IDXGIFactory2 is not available (DXGI 1.2).\n");
            return;
        }

        const uint32_t textureCount = std::max<uint32_t>(desc.textureCount, 2);
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = width;
        swapChainDesc.Height = height;
        swapChainDesc.Format = nativeFormat;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = textureCount;
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

        // Flip discard (Windows 10), flip sequential (Windows 8), then the blit model (Windows 7).
        const DXGI_SWAP_EFFECT effects[] = { DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SWAP_EFFECT_DISCARD };
        HRESULT res = E_FAIL;
        for (DXGI_SWAP_EFFECT effect : effects) {
            swapChainDesc.SwapEffect = effect;
            flipModel = (effect != DXGI_SWAP_EFFECT_DISCARD);
            swapChainFlags = (flipModel && device->renderInterface->allowTearing) ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            swapChainDesc.Flags = swapChainFlags;
            swapChainDesc.BufferCount = flipModel ? textureCount : 1;
            res = factory2->CreateSwapChainForHwnd(device->d3d, desc.renderWindow, &swapChainDesc, nullptr, nullptr, &d3d);
            if (SUCCEEDED(res)) {
                break;
            }
            d3d = nullptr;
        }
        if (FAILED(res)) {
            fprintf(stderr, "[d3d11] CreateSwapChainForHwnd failed with error code 0x%lX.\n", res);
            factory2->Release();
            return;
        }
        factory2->MakeWindowAssociation(desc.renderWindow, DXGI_MWA_NO_ALT_ENTER);
        factory2->Release();

        IDXGIDevice1 *dxgiDevice = nullptr;
        if (SUCCEEDED(device->d3d->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            dxgiDevice->SetMaximumFrameLatency(std::max<uint32_t>(desc.maxFrameLatency, 1));
            dxgiDevice->Release();
        }

        // Direct3D 11 renders to buffer 0, which is the current back buffer whatever the model: every index the
        // application acquires is a texture of that buffer.
        textures.resize(std::max<uint32_t>(desc.textureCount, 1));
        for (auto &texture : textures) {
            texture = std::make_unique<D3D11Texture>();
            texture->device = device;
            texture->swapChainBuffer = true;
            texture->desc.dimension = RenderTextureDimension::TEXTURE_2D;
            texture->desc.format = desc.format;
            texture->desc.depth = 1;
            texture->desc.mipLevels = 1;
            texture->desc.arraySize = 1;
            texture->desc.flags = RenderTextureFlag::RENDER_TARGET;
            texture->resourceFormat = nativeFormat;
        }
        setTextures();
    }

    D3D11SwapChain::~D3D11SwapChain() {
        for (auto &texture : textures) {
            texture->releaseViews();
            safeRelease(texture->d3d);
        }
        safeRelease(d3d);
    }

    void D3D11SwapChain::setTextures() {
        for (auto &texture : textures) {
            texture->releaseViews();
            safeRelease(texture->d3d);
            ID3D11Texture2D *buffer = nullptr;
            if ((d3d != nullptr) && SUCCEEDED(d3d->GetBuffer(0, IID_PPV_ARGS(&buffer)))) {
                texture->d3d = buffer;
            }
            texture->desc.width = width;
            texture->desc.height = height;
        }
    }

    bool D3D11SwapChain::present(uint32_t textureIndex, RenderCommandSemaphore **waitSemaphores, uint32_t waitSemaphoreCount) {
        if (d3d == nullptr) {
            return false;
        }
        std::lock_guard<std::recursive_mutex> lock(queue->device->contextMutex);
        const UINT syncInterval = vsyncEnabled ? 1 : 0;
        const UINT flags = (!vsyncEnabled && (swapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        HRESULT res = d3d->Present(syncInterval, flags);
        if (FAILED(res)) {
            D3D11_LOG_LIMITED(8, "Present failed with error code 0x%lX.", res);
        }
        return SUCCEEDED(res);
    }

    void D3D11SwapChain::wait() {
    }

    bool D3D11SwapChain::resize() {
        getWindowSize(width, height);
        if ((width == 0) || (height == 0) || (d3d == nullptr)) {
            return false;
        }
        D3D11Device *device = queue->device;
        std::lock_guard<std::recursive_mutex> lock(device->contextMutex);
        for (auto &texture : textures) {
            texture->releaseViews();
            safeRelease(texture->d3d);
        }
        // The context's references to the old buffers go too.
        device->context->ClearState();
        device->resetReplayLocked();
        device->context->Flush();
        HRESULT res = d3d->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapChainFlags);
        if (FAILED(res)) {
            fprintf(stderr, "[d3d11] ResizeBuffers failed with error code 0x%lX.\n", res);
            setTextures();
            return false;
        }
        setTextures();
        return true;
    }

    bool D3D11SwapChain::needsResize() const {
        uint32_t windowWidth, windowHeight;
        getWindowSize(windowWidth, windowHeight);
        return (d3d == nullptr) || (windowWidth != width) || (windowHeight != height);
    }

    void D3D11SwapChain::setVsyncEnabled(bool vsyncEnabled) {
        this->vsyncEnabled = vsyncEnabled;
    }

    bool D3D11SwapChain::isVsyncEnabled() const {
        return vsyncEnabled;
    }

    uint32_t D3D11SwapChain::getWidth() const {
        return width;
    }

    uint32_t D3D11SwapChain::getHeight() const {
        return height;
    }

    RenderTexture *D3D11SwapChain::getTexture(uint32_t textureIndex) {
        return (textureIndex < textures.size()) ? textures[textureIndex].get() : nullptr;
    }

    uint32_t D3D11SwapChain::getTextureCount() const {
        return uint32_t(textures.size());
    }

    bool D3D11SwapChain::acquireTexture(RenderCommandSemaphore *signalSemaphore, uint32_t *textureIndex) {
        if ((d3d == nullptr) || textures.empty()) {
            return false;
        }
        *textureIndex = nextIndex;
        nextIndex = (nextIndex + 1) % uint32_t(textures.size());
        return true;
    }

    RenderWindow D3D11SwapChain::getWindow() const {
        return desc.renderWindow;
    }

    bool D3D11SwapChain::isEmpty() const {
        return (d3d == nullptr) || (width == 0) || (height == 0);
    }

    uint32_t D3D11SwapChain::getRefreshRate() const {
        return 0;
    }

    void D3D11SwapChain::getWindowSize(uint32_t &dstWidth, uint32_t &dstHeight) const {
        RECT rect = {};
        GetClientRect(desc.renderWindow, &rect);
        dstWidth = uint32_t(std::max<LONG>(rect.right - rect.left, 0));
        dstHeight = uint32_t(std::max<LONG>(rect.bottom - rect.top, 0));
    }

    // D3D11CommandQueue

    D3D11CommandQueue::D3D11CommandQueue(D3D11Device *device, RenderCommandListType type) {
        this->device = device;
        this->type = type;
    }

    D3D11CommandQueue::~D3D11CommandQueue() {
    }

    std::unique_ptr<RenderCommandList> D3D11CommandQueue::createCommandList() {
        return std::make_unique<D3D11CommandList>(this);
    }

    std::unique_ptr<RenderSwapChain> D3D11CommandQueue::createSwapChain(const RenderSwapChainDesc &desc) {
        auto swapChain = std::make_unique<D3D11SwapChain>(this, desc);
        if (swapChain->d3d == nullptr) {
            return nullptr;
        }
        return swapChain;
    }

    void D3D11CommandQueue::executeCommandLists(const RenderCommandList **commandLists, uint32_t commandListCount, RenderCommandSemaphore **waitSemaphores, uint32_t waitSemaphoreCount, RenderCommandSemaphore **signalSemaphores, uint32_t signalSemaphoreCount, RenderCommandFence *signalFence) {
        std::lock_guard<std::recursive_mutex> lock(device->contextMutex);
        device->collectWrittenPages();
        for (uint32_t i = 0; i < commandListCount; i++) {
            device->execute(static_cast<const D3D11CommandList *>(commandLists[i]));
        }
        if (signalFence != nullptr) {
            static_cast<D3D11CommandFence *>(signalFence)->signalLocked();
        }
        device->context->Flush();
    }

    void D3D11CommandQueue::waitForCommandFence(RenderCommandFence *fence) {
        if (fence != nullptr) {
            static_cast<D3D11CommandFence *>(fence)->wait();
        }
    }

    // D3D11Pool

    std::unique_ptr<RenderBuffer> D3D11Pool::createBuffer(const RenderBufferDesc &desc) {
        return device->createBuffer(desc);
    }

    std::unique_ptr<RenderTexture> D3D11Pool::createTexture(const RenderTextureDesc &desc) {
        return device->createTexture(desc);
    }

    // D3D11Device

    D3D11Device::D3D11Device(D3D11Interface *renderInterface, const std::string &preferredDeviceName) {
        this->renderInterface = renderInterface;

        // The named adapter, else the one with the most video memory (never a software one).
        IDXGIAdapter1 *chosen = nullptr;
        uint64_t chosenMemory = 0;
        bool chosenPreferred = false;
        for (UINT i = 0;; i++) {
            IDXGIAdapter1 *candidate = nullptr;
            if (renderInterface->dxgiFactory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 adapterDesc = {};
            candidate->GetDesc1(&adapterDesc);
            const std::string name = utf16ToUtf8(adapterDesc.Description);
            const bool preferred = !preferredDeviceName.empty() && (name == preferredDeviceName);
            // A software adapter (WARP) only by its name (the tests).
            if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !preferred) {
                candidate->Release();
                continue;
            }
            const bool better = (chosen == nullptr) || (preferred && !chosenPreferred) || (!chosenPreferred && (adapterDesc.DedicatedVideoMemory > chosenMemory));
            if (better) {
                safeRelease(chosen);
                chosen = candidate;
                chosenMemory = adapterDesc.DedicatedVideoMemory;
                chosenPreferred = preferred;
                description.name = name;
                description.dedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
                description.vendor = RenderDeviceVendor(adapterDesc.VendorId);
            }
            else {
                candidate->Release();
            }
        }
        if (chosen == nullptr) {
            fprintf(stderr, "[d3d11] No graphics card.\n");
            return;
        }
        adapter = chosen;

        char checksEnv[4] = {};
        checks = (GetEnvironmentVariableA("PLUME_D3D11_CHECKS", checksEnv, sizeof(checksEnv)) > 0) && (checksEnv[0] == '1');
        UINT flags = 0;
        char debugEnv[4] = {};
        if ((GetEnvironmentVariableA("PLUME_D3D11_DEBUG", debugEnv, sizeof(debugEnv)) > 0) && (debugEnv[0] == '1')) {
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        }
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT res = E_FAIL;
        for (int attempt = 0; attempt < 4; attempt++) {
            const bool withDebug = (flags & D3D11_CREATE_DEVICE_DEBUG) && (attempt < 2);
            const bool with111 = (attempt % 2) == 0;
            res = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, withDebug ? flags : (flags & ~UINT(D3D11_CREATE_DEVICE_DEBUG)),
                with111 ? levels : levels + 1, with111 ? 2 : 1, D3D11_SDK_VERSION, &d3d, &featureLevel, &context);
            if (SUCCEEDED(res)) {
                break;
            }
        }
        if (FAILED(res)) {
            fprintf(stderr, "[d3d11] D3D11CreateDevice on %s failed with error code 0x%lX (it needs feature level 11_0).\n", description.name.c_str(), res);
            d3d = nullptr;
            context = nullptr;
            return;
        }
        d3d->QueryInterface(IID_PPV_ARGS(&d3d1));
        context->QueryInterface(IID_PPV_ARGS(&context1));
        if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&d3d5))) && FAILED(context->QueryInterface(IID_PPV_ARGS(&context4)))) {
            safeRelease(d3d5);
            context4 = nullptr;
        }

        LARGE_INTEGER umdVersion = {};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umdVersion))) {
            description.driverVersion = uint64_t(umdVersion.QuadPart);
        }
        description.type = (description.dedicatedVideoMemory > (512ull << 20)) ? RenderDeviceType::DISCRETE : RenderDeviceType::INTEGRATED;
        D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2 = {};
        if (SUCCEEDED(d3d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2)))) {
            capabilities.uma = options2.UnifiedMemoryArchitecture;
            if (options2.UnifiedMemoryArchitecture) {
                description.type = RenderDeviceType::INTEGRATED;
            }
        }
        capabilities.geometryShader = true;
        capabilities.maxTextureSize = 16384;
        capabilities.resolveRegion = true;
        capabilities.queryPools = false;
        capabilities.descriptorIndexing = false;
        capabilities.bufferDeviceAddress = false;
        fprintf(stderr, "[d3d11] %s, feature level %s, Direct3D 11.1 %s, fences %s.\n", description.name.c_str(),
            (featureLevel >= D3D_FEATURE_LEVEL_11_1) ? "11_1" : "11_0", (context1 != nullptr) ? "yes" : "no", (context4 != nullptr) ? "yes" : "no");
    }

    D3D11Device::~D3D11Device() {
        if (context != nullptr) {
            context->ClearState();
            context->Flush();
        }
        for (auto &copy : replay.rootCopies) {
            safeRelease(copy.buffer);
        }
        for (auto &buffers : replay.pushBuffers) {
            for (ID3D11Buffer *&buffer : buffers) {
                safeRelease(buffer);
            }
        }
        for (ID3D11Buffer *&buffer : replay.extrasBuffers) {
            safeRelease(buffer);
        }
        for (auto &entry : resolveScratch) {
            safeRelease(entry.second);
        }
        for (auto &entry : proxyRtvs) {
            safeRelease(entry.second);
        }
        for (auto &entry : proxyTextures) {
            safeRelease(entry.second);
        }
        safeRelease(clearVs);
        safeRelease(clearPs);
        for (ID3D11DepthStencilState *&state : clearDepthStates) {
            safeRelease(state);
        }
        safeRelease(clearRasterizer);
        safeRelease(clearBlend);
        safeRelease(clearConstants);
        safeRelease(context4);
        safeRelease(d3d5);
        safeRelease(context1);
        safeRelease(d3d1);
        safeRelease(context);
        safeRelease(d3d);
        safeRelease(adapter);
    }

    bool D3D11Device::isValid() const {
        return (d3d != nullptr) && (context != nullptr);
    }

    std::unique_ptr<RenderDescriptorSet> D3D11Device::createDescriptorSet(const RenderDescriptorSetDesc &desc) {
        return std::make_unique<D3D11DescriptorSet>(this, desc);
    }

    std::unique_ptr<RenderShader> D3D11Device::createShader(const void *data, uint64_t size, const char *entryPointName, RenderShaderFormat format) {
        if (format != RenderShaderFormat::DXBC) {
            D3D11_LOG_LIMITED(4, "A shader that is not DXBC (format %u).", unsigned(format));
            return nullptr;
        }
        auto shader = std::make_unique<D3D11Shader>(this, data, size);
        if (!shader->valid()) {
            return nullptr;
        }
        return shader;
    }

    std::unique_ptr<RenderSampler> D3D11Device::createSampler(const RenderSamplerDesc &desc) {
        return std::make_unique<D3D11Sampler>(this, desc);
    }

    std::unique_ptr<RenderPipeline> D3D11Device::createComputePipeline(const RenderComputePipelineDesc &desc) {
        return std::make_unique<D3D11ComputePipeline>(this, desc);
    }

    std::unique_ptr<RenderPipeline> D3D11Device::createGraphicsPipeline(const RenderGraphicsPipelineDesc &desc) {
        return std::make_unique<D3D11GraphicsPipeline>(this, desc);
    }

    std::unique_ptr<RenderPipeline> D3D11Device::createRaytracingPipeline(const RenderRaytracingPipelineDesc &desc, const RenderPipeline *previousPipeline) {
        return nullptr;
    }

    std::unique_ptr<RenderCommandQueue> D3D11Device::createCommandQueue(RenderCommandListType type) {
        return std::make_unique<D3D11CommandQueue>(this, type);
    }

    std::unique_ptr<RenderBuffer> D3D11Device::createBuffer(const RenderBufferDesc &desc) {
        auto buffer = std::make_unique<D3D11Buffer>(this, desc);
        if (!buffer->valid()) {
            return nullptr;
        }
        return buffer;
    }

    std::unique_ptr<RenderTexture> D3D11Device::createTexture(const RenderTextureDesc &desc) {
        return std::make_unique<D3D11Texture>(this, desc);
    }

    std::unique_ptr<RenderAccelerationStructure> D3D11Device::createAccelerationStructure(const RenderAccelerationStructureDesc &desc) {
        return nullptr;
    }

    std::unique_ptr<RenderPool> D3D11Device::createPool(const RenderPoolDesc &desc) {
        return std::make_unique<D3D11Pool>(this);
    }

    std::unique_ptr<RenderPipelineLayout> D3D11Device::createPipelineLayout(const RenderPipelineLayoutDesc &desc) {
        return std::make_unique<D3D11PipelineLayout>(this, desc);
    }

    std::unique_ptr<RenderCommandFence> D3D11Device::createCommandFence() {
        return std::make_unique<D3D11CommandFence>(this);
    }

    std::unique_ptr<RenderCommandSemaphore> D3D11Device::createCommandSemaphore() {
        return std::make_unique<D3D11CommandSemaphore>();
    }

    std::unique_ptr<RenderFramebuffer> D3D11Device::createFramebuffer(const RenderFramebufferDesc &desc) {
        return std::make_unique<D3D11Framebuffer>(this, desc);
    }

    std::unique_ptr<RenderQueryPool> D3D11Device::createQueryPool(uint32_t queryCount) {
        return std::make_unique<D3D11QueryPool>(queryCount);
    }

    void D3D11Device::setBottomLevelASBuildInfo(RenderBottomLevelASBuildInfo &buildInfo, const RenderBottomLevelASMesh *meshes, uint32_t meshCount, bool preferFastBuild, bool preferFastTrace) {
    }

    void D3D11Device::setTopLevelASBuildInfo(RenderTopLevelASBuildInfo &buildInfo, const RenderTopLevelASInstance *instances, uint32_t instanceCount, bool preferFastBuild, bool preferFastTrace) {
    }

    void D3D11Device::setShaderBindingTableInfo(RenderShaderBindingTableInfo &tableInfo, const RenderShaderBindingGroups &groups, const RenderPipeline *pipeline, RenderDescriptorSet **descriptorSets, uint32_t descriptorSetCount) {
    }

    const RenderDeviceCapabilities &D3D11Device::getCapabilities() const {
        return capabilities;
    }

    const RenderDeviceDescription &D3D11Device::getDescription() const {
        return description;
    }

    RenderSampleCounts D3D11Device::getSampleCountsSupported(RenderFormat format) const {
        RenderSampleCounts counts = RenderSampleCount::COUNT_1;
        const DXGI_FORMAT dxgiFormat = RenderFormatIsDepth(format) ? toDepthViewFormat(format) : toTargetViewFormat(format);
        for (uint32_t count = 2; count <= 16; count *= 2) {
            UINT quality = 0;
            if (SUCCEEDED(d3d->CheckMultisampleQualityLevels(dxgiFormat, count, &quality)) && (quality > 0)) {
                counts |= count;
            }
        }
        return counts;
    }

    bool D3D11Device::beginCapture() {
        return false;
    }

    bool D3D11Device::endCapture() {
        return false;
    }

    // The replay.

    void D3D11Device::collectWrittenPages() {
        std::lock_guard<std::mutex> lock(uploadMutex);
        for (D3D11Buffer *buffer : uploadBuffers) {
            if (!buffer->writeWatch || (buffer->cpu == nullptr) || (buffer->d3d == nullptr)) {
                continue;
            }
            const SIZE_T bytes = SIZE_T(roundUp(std::max<uint64_t>(buffer->desc.size, 16), PageSize));
            ULONG_PTR count = ULONG_PTR(bytes / PageSize);
            if (watchScratch.size() < count) {
                watchScratch.resize(count);
            }
            DWORD granularity = 0;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, buffer->cpu, bytes, watchScratch.data(), &count, &granularity) != 0) {
                std::fill(buffer->stalePages.begin(), buffer->stalePages.end(), ~uint64_t(0));
                continue;
            }
            for (ULONG_PTR i = 0; i < count; i++) {
                const uint64_t page = uint64_t(static_cast<uint8_t *>(watchScratch[i]) - buffer->cpu) / PageSize;
                const uint64_t pages = std::max<uint64_t>(granularity / PageSize, 1);
                for (uint64_t p = page; p < page + pages; p++) {
                    if (size_t(p >> 6) < buffer->stalePages.size()) {
                        buffer->stalePages[size_t(p >> 6)] |= uint64_t(1) << (p & 63);
                    }
                }
            }
        }
    }

    void D3D11Device::resetReplayLocked() {
        context->ClearState();
        D3D11Replay::RootCopy rootCopies[8];
        for (uint32_t i = 0; i < 8; i++) {
            rootCopies[i].buffer = replay.rootCopies[i].buffer;
            rootCopies[i].capacity = replay.rootCopies[i].capacity;
        }
        ID3D11Buffer *pushBuffers[2][4];
        uint32_t pushCapacity[2][4];
        memcpy(pushBuffers, replay.pushBuffers, sizeof(pushBuffers));
        memcpy(pushCapacity, replay.pushCapacity, sizeof(pushCapacity));
        ID3D11Buffer *extrasBuffers[4];
        memcpy(extrasBuffers, replay.extrasBuffers, sizeof(extrasBuffers));
        replay = D3D11Replay();
        for (uint32_t i = 0; i < 8; i++) {
            replay.rootCopies[i].buffer = rootCopies[i].buffer;
            replay.rootCopies[i].capacity = rootCopies[i].capacity;
        }
        memcpy(replay.pushBuffers, pushBuffers, sizeof(pushBuffers));
        memcpy(replay.pushCapacity, pushCapacity, sizeof(pushCapacity));
        memcpy(replay.extrasBuffers, extrasBuffers, sizeof(extrasBuffers));
        for (uint32_t i = 0; i < 4; i++) {
            replay.graphicsPushDirty[i] = true;
            replay.computePushDirty[i] = true;
        }
    }

    void D3D11Device::invalidateBindingsLocked(bool srvs, bool inputs, bool constants) {
        D3D11Replay &r = replay;
        if (srvs) {
            for (uint32_t stage = 0; stage < D3D11StageCount; stage++) {
                std::fill(std::begin(r.boundSrvs[stage]), std::end(r.boundSrvs[stage]), stale<ID3D11ShaderResourceView>());
            }
        }
        if (inputs) {
            std::fill(std::begin(r.boundVertexBuffers), std::end(r.boundVertexBuffers), stale<ID3D11Buffer>());
            r.boundIndexBuffer = stale<ID3D11Buffer>();
            r.vertexDirty = 0xFFFF;
            r.indexDirty = true;
        }
        if (constants) {
            for (uint32_t stage = 0; stage < D3D11StageCount; stage++) {
                std::fill(std::begin(r.boundCbs[stage]), std::end(r.boundCbs[stage]), stale<ID3D11Buffer>());
            }
        }
    }

    const uint8_t *D3D11Device::rootCpuData(uint32_t rootIndex, uint32_t bytes) const {
        if (rootIndex >= 8) {
            return nullptr;
        }
        const RenderBufferReference &ref = replay.roots[rootIndex];
        const D3D11Buffer *buffer = static_cast<const D3D11Buffer *>(ref.ref);
        if ((buffer == nullptr) || (buffer->cpu == nullptr) || (ref.offset + bytes > std::max<uint64_t>(buffer->desc.size, 16))) {
            return nullptr;
        }
        return buffer->cpu + ref.offset;
    }

    const D3D11Descriptor *D3D11Device::guestTexture(uint32_t reg, const uint32_t *words, const D3D11ShaderInfo &info, D3D11DescriptorSet *const *sets, const D3D11GuestBinding &guest) const {
        if ((words == nullptr) || (guest.textureSet >= 8)) {
            return nullptr;
        }
        const D3D11DescriptorSet *table = sets[guest.textureSet];
        if (table == nullptr) {
            return nullptr;
        }
        uint32_t kind, slot;
        if (reg < 32) {
            kind = 0;
            slot = reg;
        }
        else if (reg < 64) {
            kind = 1;
            slot = reg - 32;
        }
        else if (reg < 96) {
            kind = 2;
            slot = reg - 64;
        }
        else if (reg < 112) {
            kind = 3;
            slot = guestSlot(info, reg - 96);
        }
        else {
            return nullptr;
        }
        const uint32_t word = words[(guest.textureIndicesOffset / 4) + ((kind == 3) ? 1u : kind) * 32 + slot];
        const bool stacked = (word & guest.stackedFlag) != 0;
        if (((kind == 1) && stacked) || ((kind == 3) && !stacked)) {
            return nullptr;
        }
        const uint32_t index = word & guest.indexMask;
        if (index >= table->descriptors.size()) {
            return nullptr;
        }
        return &table->descriptors[index];
    }

    const D3D11Descriptor *D3D11Device::tableDescriptor(D3D11TableBinding::Kind kind, uint32_t reg, const D3D11PipelineLayout *layout, D3D11DescriptorSet *const *sets, bool compute) const {
        if (layout == nullptr) {
            return nullptr;
        }
        for (const D3D11TableBinding &binding : layout->tableBindings) {
            if ((binding.kind != kind) || (binding.reg != reg) || (binding.pushRange >= 4) || (binding.setIndex >= 8) || (binding.pushOffset + 4 > 256)) {
                continue;
            }
            const uint8_t *push = compute ? replay.computePush[binding.pushRange] : replay.graphicsPush[binding.pushRange];
            uint32_t index = 0;
            memcpy(&index, push + binding.pushOffset, 4);
            index &= binding.indexMask;
            const D3D11DescriptorSet *set = sets[binding.setIndex];
            if ((set == nullptr) || (index >= set->descriptors.size())) {
                return nullptr;
            }
            return &set->descriptors[index];
        }
        return nullptr;
    }

    // Range classes: 0 views, 1 unordered access, 2 samplers, 3 constant buffers.
    static int rangeClass(RenderDescriptorRangeType type) {
        switch (type) {
        case RenderDescriptorRangeType::TEXTURE:
        case RenderDescriptorRangeType::FORMATTED_BUFFER:
        case RenderDescriptorRangeType::STRUCTURED_BUFFER:
        case RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER:
            return 0;
        case RenderDescriptorRangeType::READ_WRITE_TEXTURE:
        case RenderDescriptorRangeType::READ_WRITE_FORMATTED_BUFFER:
        case RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER:
        case RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER:
            return 1;
        case RenderDescriptorRangeType::SAMPLER:
            return 2;
        case RenderDescriptorRangeType::CONSTANT_BUFFER:
        case RenderDescriptorRangeType::CONSTANT_BUFFER_DYNAMIC:
            return 3;
        default:
            return -1;
        }
    }

    const D3D11Descriptor *D3D11Device::directDescriptor(int classWanted, uint32_t reg, const D3D11PipelineLayout *layout, D3D11DescriptorSet *const *sets) const {
        if (layout == nullptr) {
            return nullptr;
        }
        const size_t setCount = std::min<size_t>(layout->sets.size(), 8);
        for (size_t s = 0; s < setCount; s++) {
            const D3D11PipelineLayout::Set &setLayout = layout->sets[s];
            const D3D11DescriptorSet *set = sets[s];
            if (setLayout.table || (set == nullptr)) {
                continue;
            }
            uint32_t start = 0;
            for (const RenderDescriptorRange &range : setLayout.ranges) {
                if ((rangeClass(range.type) == classWanted) && (reg >= range.binding) && (reg < range.binding + range.count)) {
                    const uint32_t index = start + (reg - range.binding);
                    return (index < set->descriptors.size()) ? &set->descriptors[index] : nullptr;
                }
                start += range.count;
            }
        }
        return nullptr;
    }

    ID3D11Buffer *D3D11Device::rootConstantBufferLocked(uint32_t rootIndex, uint32_t bytes) {
        D3D11Replay &r = replay;
        const RenderBufferReference &ref = r.roots[rootIndex];
        const D3D11Buffer *source = static_cast<const D3D11Buffer *>(ref.ref);
        if ((source == nullptr) || (source->cpu == nullptr)) {
            return nullptr;
        }
        const uint32_t want = uint32_t(roundUp(std::clamp<uint32_t>(bytes, 16, 65536), 16));
        D3D11Replay::RootCopy &copy = r.rootCopies[rootIndex];
        if ((copy.buffer != nullptr) && (copy.source == source) && (copy.offset == ref.offset) && (copy.size >= want)) {
            return copy.buffer;
        }
        if ((copy.buffer == nullptr) || (copy.capacity < want)) {
            safeRelease(copy.buffer);
            D3D11_BUFFER_DESC bufferDesc = {};
            bufferDesc.ByteWidth = UINT(roundUp(want, 256));
            bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
            bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(d3d->CreateBuffer(&bufferDesc, nullptr, &copy.buffer))) {
                copy.buffer = nullptr;
                copy.capacity = 0;
                return nullptr;
            }
            copy.capacity = bufferDesc.ByteWidth;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(copy.buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return nullptr;
        }
        const uint64_t sourceSize = std::max<uint64_t>(source->desc.size, 16);
        const uint64_t available = (ref.offset < sourceSize) ? (sourceSize - ref.offset) : 0;
        const uint32_t copied = uint32_t(std::min<uint64_t>(want, available));
        memcpy(mapped.pData, source->cpu + ref.offset, copied);
        if (copied < want) {
            memset(static_cast<uint8_t *>(mapped.pData) + copied, 0, want - copied);
        }
        context->Unmap(copy.buffer, 0);
        copy.source = source;
        copy.offset = ref.offset;
        copy.size = want;
        return copy.buffer;
    }

    ID3D11Buffer *D3D11Device::pushConstantBufferLocked(bool compute, uint32_t rangeIndex, const D3D11PipelineLayout *layout) {
        if ((layout == nullptr) || (rangeIndex >= 4) || (rangeIndex >= layout->pushRanges.size())) {
            return nullptr;
        }
        D3D11Replay &r = replay;
        const uint32_t size = uint32_t(roundUp(std::clamp<uint32_t>(layout->pushRanges[rangeIndex].size, 16, 256), 16));
        ID3D11Buffer *&buffer = r.pushBuffers[compute ? 1 : 0][rangeIndex];
        uint32_t &capacity = r.pushCapacity[compute ? 1 : 0][rangeIndex];
        bool &dirty = compute ? r.computePushDirty[rangeIndex] : r.graphicsPushDirty[rangeIndex];
        if ((buffer == nullptr) || (capacity < size)) {
            safeRelease(buffer);
            D3D11_BUFFER_DESC bufferDesc = {};
            bufferDesc.ByteWidth = 256;
            bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
            bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(d3d->CreateBuffer(&bufferDesc, nullptr, &buffer))) {
                buffer = nullptr;
                capacity = 0;
                return nullptr;
            }
            capacity = 256;
            dirty = true;
        }
        if (dirty) {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                return nullptr;
            }
            memcpy(mapped.pData, compute ? r.computePush[rangeIndex] : r.graphicsPush[rangeIndex], 256);
            context->Unmap(buffer, 0);
            dirty = false;
        }
        return buffer;
    }

    void D3D11Device::setStageConstantBuffer(uint32_t stage, uint32_t reg, ID3D11Buffer *buffer, uint32_t first, uint32_t count) {
        D3D11Replay &r = replay;
        if ((reg >= 14) || ((r.boundCbs[stage][reg] == buffer) && (r.boundCbFirst[stage][reg] == first) && (r.boundCbCount[stage][reg] == count))) {
            return;
        }
        r.boundCbs[stage][reg] = buffer;
        r.boundCbFirst[stage][reg] = first;
        r.boundCbCount[stage][reg] = count;
        if ((count != 0) && (context1 != nullptr)) {
            switch (stage) {
            case D3D11StageVertex: context1->VSSetConstantBuffers1(reg, 1, &buffer, &first, &count); break;
            case D3D11StageGeometry: context1->GSSetConstantBuffers1(reg, 1, &buffer, &first, &count); break;
            case D3D11StagePixel: context1->PSSetConstantBuffers1(reg, 1, &buffer, &first, &count); break;
            default: context1->CSSetConstantBuffers1(reg, 1, &buffer, &first, &count); break;
            }
            return;
        }
        switch (stage) {
        case D3D11StageVertex: context->VSSetConstantBuffers(reg, 1, &buffer); break;
        case D3D11StageGeometry: context->GSSetConstantBuffers(reg, 1, &buffer); break;
        case D3D11StagePixel: context->PSSetConstantBuffers(reg, 1, &buffer); break;
        default: context->CSSetConstantBuffers(reg, 1, &buffer); break;
        }
    }

    void D3D11Device::setStageSrvs(uint32_t stage, ID3D11ShaderResourceView *const *srvs, const uint64_t mask[2]) {
        ID3D11ShaderResourceView **bound = replay.boundSrvs[stage];
        auto used = [mask](uint32_t reg) { return ((mask[reg >> 6] >> (reg & 63)) & 1) != 0; };
        uint32_t reg = 0;
        while (reg < 128) {
            if ((mask[reg >> 6] >> (reg & 63)) == 0) {
                reg = (reg | 63) + 1;
                continue;
            }
            if (!used(reg) || (bound[reg] == srvs[reg])) {
                reg++;
                continue;
            }
            const uint32_t start = reg;
            while ((reg < 128) && used(reg) && (bound[reg] != srvs[reg])) {
                bound[reg] = srvs[reg];
                reg++;
            }
            setShaderResources(context, stage, start, reg - start, srvs + start);
        }
    }

    void D3D11Device::setStageSamplers(uint32_t stage, ID3D11SamplerState *const *samplers, uint32_t mask) {
        ID3D11SamplerState **bound = replay.boundSamplers[stage];
        uint32_t reg = 0;
        while (reg < 16) {
            if (!((mask >> reg) & 1) || (bound[reg] == samplers[reg])) {
                reg++;
                continue;
            }
            const uint32_t start = reg;
            while ((reg < 16) && ((mask >> reg) & 1) && (bound[reg] != samplers[reg])) {
                bound[reg] = samplers[reg];
                reg++;
            }
            setSamplers(context, stage, start, reg - start, samplers + start);
        }
    }

    void D3D11Device::bindStageResourcesLocked(uint32_t stage, const D3D11ShaderInfo &info, const D3D11PipelineLayout *layout, D3D11DescriptorSet *const *sets, bool compute) {
        D3D11Replay &r = replay;
        const bool guest = (layout != nullptr) && layout->hasGuest && !compute;
        const D3D11GuestBinding guestBinding = guest ? layout->guest : D3D11GuestBinding();
        const uint32_t *words = nullptr;
        if (guest) {
            const uint32_t need = std::max(guestBinding.textureIndicesOffset + 3 * 32 * 4, guestBinding.samplerIndicesOffset + 32 * 4);
            words = reinterpret_cast<const uint32_t *>(rootCpuData(guestBinding.rootIndex, need));
        }
        uint32_t extras[132] = {};

        // Views.
        if (info.srvMask[0] | info.srvMask[1]) {
            ID3D11ShaderResourceView *srvs[128];
            for (uint32_t reg = 0; reg < 128; reg++) {
                if (!info.usesSrv(reg)) {
                    continue;
                }
                ID3D11ShaderResourceView *srv = nullptr;
                uint32_t swizzle = IdentitySwizzle;
                const int root = findRoot(layout, RenderRootDescriptorType::SHADER_RESOURCE, reg);
                if (root >= 0) {
                    D3D11Buffer *buffer = mutableBuffer(r.roots[root].ref);
                    if (buffer != nullptr) {
                        if (buffer->cpu != nullptr) {
                            buffer->gpuResource(r.roots[root].offset, std::max<uint64_t>(buffer->desc.size, 16));
                        }
                        srv = buffer->getRawSrv();
                    }
                    if (guest && (uint32_t(root) == guestBinding.fetchRootIndex)) {
                        extras[128] = uint32_t(r.roots[root].offset);
                    }
                }
                else {
                    const D3D11Descriptor *descriptor = guest ? guestTexture(reg, words, info, sets, guestBinding) : tableDescriptor(D3D11TableBinding::Kind::SRV, reg, layout, sets, compute);
                    if ((descriptor == nullptr) && !guest) {
                        descriptor = directDescriptor(0, reg, layout, sets);
                    }
                    if (descriptor != nullptr) {
                        srv = descriptor->srv;
                        swizzle = descriptor->swizzle;
                    }
                }
                srvs[reg] = srv;
                extras[reg] = swizzle;
                if (checks && guest && (srv == nullptr) && (reg < 112)) {
                    const uint32_t kind = (reg < 32) ? 0 : (reg < 64) ? 1 : (reg < 96) ? 2 : 3;
                    const uint32_t slot = (kind == 3) ? guestSlot(info, reg - 96) : (reg & 31);
                    const uint32_t word = (words != nullptr) ? words[(guestBinding.textureIndicesOffset / 4) + ((kind == 3) ? 1u : kind) * 32 + slot] : 0xFFFFFFFFu;
                    D3D11_LOG_LIMITED(24, "check: stage %u register t%u (slot %u) has no view (index word 0x%08X).", stage, reg, slot, word);
                }
                if (checks && (srv != nullptr)) {
                    ID3D11Resource *resource = nullptr;
                    srv->GetResource(&resource);
                    if (resource != nullptr) {
                        bool written = false;
                        for (uint32_t i = 0; i < r.boundRtvCount; i++) {
                            ID3D11Resource *target = nullptr;
                            if (r.boundRtvs[i] != nullptr) {
                                r.boundRtvs[i]->GetResource(&target);
                                written = written || (target == resource);
                                safeRelease(target);
                            }
                        }
                        if (r.boundDsv != nullptr) {
                            ID3D11Resource *target = nullptr;
                            r.boundDsv->GetResource(&target);
                            written = written || (target == resource);
                            safeRelease(target);
                        }
                        if (written) {
                            D3D11_LOG_LIMITED(24, "check: stage %u register t%u reads a resource the draw's outputs write (Direct3D 11 unbinds it).", stage, reg);
                        }
                        resource->Release();
                    }
                }
            }
            setStageSrvs(stage, srvs, info.srvMask);
        }

        // Samplers.
        if (info.samplerMask) {
            ID3D11SamplerState *samplers[16];
            for (uint32_t reg = 0; reg < 16; reg++) {
                if (!((info.samplerMask >> reg) & 1)) {
                    continue;
                }
                const D3D11Descriptor *descriptor = nullptr;
                if (guest) {
                    if ((words != nullptr) && (guestBinding.samplerSet < 8) && (sets[guestBinding.samplerSet] != nullptr)) {
                        const uint32_t slot = guestSlot(info, reg);
                        const uint32_t index = words[(guestBinding.samplerIndicesOffset / 4) + slot] & guestBinding.indexMask;
                        const D3D11DescriptorSet *table = sets[guestBinding.samplerSet];
                        if (index < table->descriptors.size()) {
                            descriptor = &table->descriptors[index];
                        }
                    }
                }
                else {
                    descriptor = tableDescriptor(D3D11TableBinding::Kind::SAMPLER, reg, layout, sets, compute);
                    if (descriptor == nullptr) {
                        descriptor = directDescriptor(2, reg, layout, sets);
                    }
                }
                samplers[reg] = (descriptor != nullptr) ? descriptor->sampler : nullptr;
            }
            setStageSamplers(stage, samplers, info.samplerMask);
        }

        // Unordered access of a compute shader (the graphics stages' go with the outputs).
        if (compute && info.uavMask) {
            ID3D11UnorderedAccessView *uavs[8] = {};
            uint32_t count = 0;
            for (uint32_t reg = 0; reg < 8; reg++) {
                if (!((info.uavMask >> reg) & 1)) {
                    continue;
                }
                ID3D11UnorderedAccessView *uav = nullptr;
                const int root = findRoot(layout, RenderRootDescriptorType::UNORDERED_ACCESS, reg);
                if (root >= 0) {
                    D3D11Buffer *buffer = mutableBuffer(r.roots[root].ref);
                    uav = (buffer != nullptr) ? buffer->getRawUav() : nullptr;
                }
                else {
                    const D3D11Descriptor *descriptor = directDescriptor(1, reg, layout, sets);
                    uav = (descriptor != nullptr) ? descriptor->uav : nullptr;
                }
                uavs[reg] = uav;
                count = reg + 1;
            }
            if (memcmp(uavs, r.boundComputeUavs, count * sizeof(uavs[0])) != 0) {
                context->CSSetUnorderedAccessViews(0, count, uavs, nullptr);
                memcpy(r.boundComputeUavs, uavs, count * sizeof(uavs[0]));
            }
        }

        // Constant buffers.
        if (guest && (guestBinding.exportRootIndex < 8)) {
            extras[129] = uint32_t(r.roots[guestBinding.exportRootIndex].offset);
        }
        for (uint32_t reg = 0; reg < 14; reg++) {
            if (!((info.cbMask >> reg) & 1)) {
                continue;
            }
            if (guest && (reg == guestBinding.extrasRegister)) {
                ID3D11Buffer *&buffer = r.extrasBuffers[stage];
                if (buffer == nullptr) {
                    D3D11_BUFFER_DESC bufferDesc = {};
                    bufferDesc.ByteWidth = sizeof(extras);
                    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
                    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    if (FAILED(d3d->CreateBuffer(&bufferDesc, nullptr, &buffer))) {
                        buffer = nullptr;
                        continue;
                    }
                    r.extrasValid[stage] = false;
                }
                if (!r.extrasValid[stage] || (memcmp(r.extras[stage], extras, sizeof(extras)) != 0)) {
                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    if (SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        memcpy(mapped.pData, extras, sizeof(extras));
                        context->Unmap(buffer, 0);
                        memcpy(r.extras[stage], extras, sizeof(extras));
                        r.extrasValid[stage] = true;
                    }
                }
                setStageConstantBuffer(stage, reg, buffer, 0, 0);
                continue;
            }
            const int root = findRoot(layout, RenderRootDescriptorType::CONSTANT_BUFFER, reg);
            if (root >= 0) {
                const uint32_t bytes = std::max<uint32_t>(info.cbSize[reg], 1) * 16;
                ID3D11Buffer *buffer = rootConstantBufferLocked(uint32_t(root), bytes);
                if (buffer != nullptr) {
                    setStageConstantBuffer(stage, reg, buffer, 0, 0);
                }
                else {
                    // A constant buffer of the GPU's own, at its offset (Direct3D 11.1: 256-byte steps).
                    const D3D11Buffer *source = static_cast<const D3D11Buffer *>(r.roots[root].ref);
                    if ((source != nullptr) && (source->d3d != nullptr) && (source->cpu == nullptr) && (source->desc.flags & RenderBufferFlag::CONSTANT)) {
                        const uint32_t first = uint32_t(r.roots[root].offset / 16);
                        const uint32_t count = uint32_t(roundUp(bytes / 16, 16));
                        setStageConstantBuffer(stage, reg, source->d3d, first & ~15u, count);
                    }
                    else {
                        setStageConstantBuffer(stage, reg, nullptr, 0, 0);
                    }
                }
                continue;
            }
            bool pushed = false;
            if (layout != nullptr) {
                for (uint32_t i = 0; (i < layout->pushRanges.size()) && (i < 4); i++) {
                    if ((layout->pushRanges[i].reg == reg) && (layout->pushRanges[i].stages & stageFlag(stage))) {
                        setStageConstantBuffer(stage, reg, pushConstantBufferLocked(compute, i, layout), 0, 0);
                        pushed = true;
                        break;
                    }
                }
            }
            if (!pushed) {
                const D3D11Descriptor *descriptor = directDescriptor(3, reg, layout, sets);
                const D3D11Buffer *source = (descriptor != nullptr) ? descriptor->buffer : nullptr;
                if ((source != nullptr) && (source->d3d != nullptr) && (source->cpu == nullptr)) {
                    setStageConstantBuffer(stage, reg, source->d3d, 0, 0);
                }
                else if (source != nullptr) {
                    D3D11_LOG_LIMITED(1, "A constant buffer in a descriptor set from CPU memory is not supported.");
                }
            }
        }
    }

    void D3D11Device::bindOutputsLocked(ID3D11UnorderedAccessView *uav, uint32_t uavSlot) {
        D3D11Replay &r = replay;
        const D3D11Framebuffer *framebuffer = r.framebuffer;
        const uint32_t count = (framebuffer != nullptr) ? uint32_t(std::min<size_t>(framebuffer->rtvs.size(), 8)) : 0;
        if (uav != nullptr) {
            uavSlot = std::max(uavSlot, count);
        }
        if (r.outputsValid && (framebuffer == r.boundFramebuffer) && (uav == r.boundGraphicsUav) && ((uav == nullptr) || (uavSlot == r.boundGraphicsUavSlot))) {
            return;
        }
        flushProxiesLocked();
        ID3D11RenderTargetView *rtvs[8] = {};
        ID3D11DepthStencilView *dsv = nullptr;
        if (framebuffer != nullptr) {
            for (uint32_t i = 0; i < count; i++) {
                rtvs[i] = framebuffer->rtvs[i];
                if (framebuffer->mismatched && (rtvs[i] != nullptr) && ((framebuffer->colorWidths[i] != framebuffer->width) || (framebuffer->colorHeights[i] != framebuffer->height))) {
                    // Drawn into a proxy of the framebuffer's size, the target's part copied in now and back later.
                    ID3D11Texture2D *proxy = nullptr;
                    ID3D11RenderTargetView *proxyRtv = proxyTargetLocked(framebuffer, i, framebuffer->width, framebuffer->height, &proxy);
                    const D3D11Texture *texture = framebuffer->colorTextures[i];
                    if ((proxyRtv != nullptr) && (texture != nullptr)) {
                        D3D11Replay::Proxy entry;
                        entry.real = texture->d3d;
                        entry.subresource = texture->subresourceIndex(framebuffer->colorMips[i], framebuffer->colorSlices[i]);
                        entry.proxy = proxy;
                        entry.width = std::min(framebuffer->colorWidths[i], framebuffer->width);
                        entry.height = std::min(framebuffer->colorHeights[i], framebuffer->height);
                        if (texture->desc.multisampling.sampleCount <= RenderSampleCount::COUNT_1) {
                            const D3D11_BOX box = { 0, 0, 0, entry.width, entry.height, 1 };
                            context->CopySubresourceRegion(proxy, 0, 0, 0, 0, entry.real, entry.subresource, &box);
                        }
                        else {
                            D3D11_LOG_LIMITED(4, "A multisampled color target of another size than its framebuffer: drawn through a proxy without its contents.");
                        }
                        r.proxies.push_back(entry);
                        rtvs[i] = proxyRtv;
                    }
                }
            }
            dsv = framebuffer->dsv;
        }
        if (uav != nullptr) {
            context->OMSetRenderTargetsAndUnorderedAccessViews(count, rtvs, dsv, uavSlot, 1, &uav, nullptr);
        }
        else {
            context->OMSetRenderTargets(count, rtvs, dsv);
        }
        memcpy(r.boundRtvs, rtvs, sizeof(rtvs));
        r.boundRtvCount = count;
        r.boundDsv = dsv;
        r.boundGraphicsUav = uav;
        r.boundGraphicsUavSlot = uavSlot;
        r.boundFramebuffer = framebuffer;
        r.outputsValid = true;
        // Direct3D 11 unbinds what the new outputs write from the inputs of every stage.
        invalidateBindingsLocked(true, uav != nullptr, false);
    }

    void D3D11Device::flushProxiesLocked() {
        D3D11Replay &r = replay;
        if (r.proxies.empty()) {
            return;
        }
        for (const D3D11Replay::Proxy &proxy : r.proxies) {
            const D3D11_BOX box = { 0, 0, 0, proxy.width, proxy.height, 1 };
            context->CopySubresourceRegion(proxy.real, proxy.subresource, 0, 0, 0, proxy.proxy, 0, &box);
        }
        r.proxies.clear();
        // The next draw binds the targets again (and copies their contents in).
        r.outputsValid = false;
    }

    ID3D11RenderTargetView *D3D11Device::proxyTargetLocked(const D3D11Framebuffer *framebuffer, uint32_t index, uint32_t width, uint32_t height, ID3D11Texture2D **proxy) {
        const D3D11Texture *texture = framebuffer->colorTextures[index];
        const uint32_t samples = std::max<uint32_t>(texture->desc.multisampling.sampleCount, 1);
        const uint64_t key = uint64_t(texture->resourceFormat) | (uint64_t(samples) << 8) | (uint64_t(index & 7) << 13) | (uint64_t(width & 0xFFFF) << 16) | (uint64_t(height & 0xFFFF) << 32);
        ID3D11Texture2D *&target = proxyTextures[key];
        if (target == nullptr) {
            D3D11_TEXTURE2D_DESC textureDesc = {};
            textureDesc.Width = width;
            textureDesc.Height = height;
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            textureDesc.Format = texture->resourceFormat;
            textureDesc.SampleDesc.Count = samples;
            textureDesc.Usage = D3D11_USAGE_DEFAULT;
            textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(d3d->CreateTexture2D(&textureDesc, nullptr, &target))) {
                target = nullptr;
                return nullptr;
            }
        }
        const DXGI_FORMAT format = toTargetViewFormat(framebuffer->colorFormats[index]);
        ID3D11RenderTargetView *&rtv = proxyRtvs[std::make_pair(target, format)];
        if (rtv == nullptr) {
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = format;
            rtvDesc.ViewDimension = (samples > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(d3d->CreateRenderTargetView(target, &rtvDesc, &rtv))) {
                rtv = nullptr;
                return nullptr;
            }
        }
        *proxy = target;
        return rtv;
    }

    void D3D11Device::unbindOutputsLocked() {
        D3D11Replay &r = replay;
        flushProxiesLocked();
        if (r.outputsValid && (r.boundRtvCount == 0) && (r.boundDsv == nullptr) && (r.boundGraphicsUav == nullptr)) {
            return;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        memset(r.boundRtvs, 0, sizeof(r.boundRtvs));
        r.boundRtvCount = 0;
        r.boundDsv = nullptr;
        r.boundGraphicsUav = nullptr;
        r.boundFramebuffer = nullptr;
        // Not valid for a framebuffer: the next draw binds its own.
        r.outputsValid = false;
    }

    void D3D11Device::flushGraphicsLocked() {
        D3D11Replay &r = replay;
        const D3D11GraphicsPipeline *pipeline = r.graphicsPipeline;
        const D3D11PipelineLayout *layout = (r.graphicsLayout != nullptr) ? r.graphicsLayout : pipeline->layout;
        if (r.computeLast) {
            // The dispatch's unordered access unbound its buffers from the graphics inputs.
            invalidateBindingsLocked(true, true, false);
            r.computeLast = false;
        }

        if (r.boundShaders[D3D11StageVertex] != pipeline->vs) {
            context->VSSetShader(pipeline->vs, nullptr, 0);
            r.boundShaders[D3D11StageVertex] = pipeline->vs;
        }
        if (r.boundShaders[D3D11StageGeometry] != pipeline->gs) {
            context->GSSetShader(pipeline->gs, nullptr, 0);
            r.boundShaders[D3D11StageGeometry] = pipeline->gs;
        }
        if (r.boundShaders[D3D11StagePixel] != pipeline->ps) {
            context->PSSetShader(pipeline->ps, nullptr, 0);
            r.boundShaders[D3D11StagePixel] = pipeline->ps;
        }
        if (r.boundInputLayout != pipeline->inputLayout) {
            context->IASetInputLayout(pipeline->inputLayout);
            r.boundInputLayout = pipeline->inputLayout;
        }
        if (r.boundTopology != pipeline->topology) {
            context->IASetPrimitiveTopology(pipeline->topology);
            r.boundTopology = pipeline->topology;
        }
        if (r.boundRasterizer != pipeline->rasterizer) {
            context->RSSetState(pipeline->rasterizer);
            r.boundRasterizer = pipeline->rasterizer;
        }
        if ((r.boundDepthStencil != pipeline->depthStencil) || (r.boundStencilRef != pipeline->stencilReference)) {
            context->OMSetDepthStencilState(pipeline->depthStencil, pipeline->stencilReference);
            r.boundDepthStencil = pipeline->depthStencil;
            r.boundStencilRef = pipeline->stencilReference;
        }
        if ((r.boundBlend != pipeline->blend) || r.blendDirty) {
            context->OMSetBlendState(pipeline->blend, r.blendFactor, 0xFFFFFFFFu);
            r.boundBlend = pipeline->blend;
            r.blendDirty = false;
        }

        // Outputs, with the root unordered access view the shaders write (memexport).
        ID3D11UnorderedAccessView *uav = nullptr;
        uint32_t uavSlot = 0;
        if (pipeline->uavMask && (layout != nullptr)) {
            for (size_t i = 0; (i < layout->roots.size()) && (i < 8); i++) {
                const D3D11PipelineLayout::Root &root = layout->roots[i];
                if ((root.type != RenderRootDescriptorType::UNORDERED_ACCESS) || (root.reg >= 64) || !((pipeline->uavMask >> root.reg) & 1)) {
                    continue;
                }
                D3D11Buffer *buffer = mutableBuffer(r.roots[i].ref);
                if (buffer != nullptr) {
                    uav = buffer->getRawUav();
                    uavSlot = root.reg;
                }
                break;
            }
        }
        bindOutputsLocked(uav, uavSlot);

        if (r.viewportsDirty) {
            context->RSSetViewports(r.viewportCount, r.viewports);
            r.viewportsDirty = false;
        }
        if (r.scissorsDirty) {
            context->RSSetScissorRects(r.scissorCount, r.scissors);
            r.scissorsDirty = false;
        }

        for (uint32_t stage = 0; stage < 3; stage++) {
            if (pipeline->hasStage[stage]) {
                bindStageResourcesLocked(stage, pipeline->stageInfo[stage], layout, r.graphicsSets, false);
            }
        }

        // Vertex and index buffers (the CPU's pages of an UPLOAD buffer brought up to date for the range read).
        for (uint32_t slot = 0; slot < 16; slot++) {
            if (!((r.vertexDirty >> slot) & 1)) {
                continue;
            }
            const D3D11Replay::Vertex &vertex = r.vertex[slot];
            ID3D11Buffer *buffer = nullptr;
            UINT offset = 0;
            UINT stride = vertex.stride;
            if (vertex.buffer != nullptr) {
                buffer = mutableBuffer(vertex.buffer)->gpuResource(vertex.offset, vertex.offset + vertex.size);
                offset = UINT(vertex.offset);
            }
            if ((r.boundVertexBuffers[slot] != buffer) || (r.boundVertexOffsets[slot] != offset) || (r.boundVertexStrides[slot] != stride)) {
                context->IASetVertexBuffers(slot, 1, &buffer, &stride, &offset);
                r.boundVertexBuffers[slot] = buffer;
                r.boundVertexOffsets[slot] = offset;
                r.boundVertexStrides[slot] = stride;
            }
        }
        r.vertexDirty = 0;
        if (r.indexDirty) {
            ID3D11Buffer *buffer = nullptr;
            if (r.indexBuffer != nullptr) {
                buffer = mutableBuffer(r.indexBuffer)->gpuResource(r.indexOffset, r.indexOffset + r.indexSize);
            }
            const UINT offset = UINT(r.indexOffset);
            if ((r.boundIndexBuffer != buffer) || (r.boundIndexOffset != offset) || (r.boundIndexFormat != r.indexFormat)) {
                context->IASetIndexBuffer(buffer, r.indexFormat, offset);
                r.boundIndexBuffer = buffer;
                r.boundIndexOffset = offset;
                r.boundIndexFormat = r.indexFormat;
            }
            r.indexDirty = false;
        }
    }

    void D3D11Device::flushComputeLocked() {
        D3D11Replay &r = replay;
        const D3D11ComputePipeline *pipeline = r.computePipeline;
        const D3D11PipelineLayout *layout = (r.computeLayout != nullptr) ? r.computeLayout : pipeline->layout;
        // The outputs go: a compute view of something a draw writes would be refused.
        unbindOutputsLocked();
        if (r.boundShaders[D3D11StageCompute] != pipeline->cs) {
            context->CSSetShader(pipeline->cs, nullptr, 0);
            r.boundShaders[D3D11StageCompute] = pipeline->cs;
        }
        bindStageResourcesLocked(D3D11StageCompute, pipeline->info, layout, r.computeSets, true);
    }

    void D3D11Device::copyBufferRegionLocked(const D3D11Buffer *dst, uint64_t dstOffset, const D3D11Buffer *src, uint64_t srcOffset, uint64_t size) {
        if ((dst == nullptr) || (src == nullptr) || (size == 0)) {
            return;
        }
        const uint64_t dstSize = std::max<uint64_t>(dst->desc.size, 16);
        const uint64_t srcSize = std::max<uint64_t>(src->desc.size, 16);
        if ((dstOffset >= dstSize) || (srcOffset >= srcSize)) {
            return;
        }
        size = std::min(size, std::min(dstSize - dstOffset, srcSize - srcOffset));
        if (dst->cpu != nullptr) {
            // Into CPU memory: from CPU memory only.
            if (src->cpu != nullptr) {
                memmove(dst->cpu + dstOffset, src->cpu + srcOffset, size_t(size));
                mutableBuffer(dst)->markWritten(dstOffset, dstOffset + size);
            }
            else {
                D3D11_LOG_LIMITED(1, "A copy from a GPU buffer into an UPLOAD buffer is not supported.");
            }
            return;
        }
        if (dst->d3d == nullptr) {
            return;
        }
        if (src->cpu != nullptr) {
            if (dst->desc.heapType == RenderHeapType::READBACK) {
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (SUCCEEDED(context->Map(dst->d3d, 0, D3D11_MAP_WRITE, 0, &mapped))) {
                    memcpy(static_cast<uint8_t *>(mapped.pData) + dstOffset, src->cpu + srcOffset, size_t(size));
                    context->Unmap(dst->d3d, 0);
                }
                return;
            }
            D3D11_BOX box = { UINT(dstOffset), 0, 0, UINT(dstOffset + size), 1, 1 };
            context->UpdateSubresource(dst->d3d, 0, &box, src->cpu + srcOffset, 0, 0);
            return;
        }
        if (src->d3d == nullptr) {
            return;
        }
        D3D11_BOX box = { UINT(srcOffset), 0, 0, UINT(srcOffset + size), 1, 1 };
        if (src->d3d == dst->d3d) {
            // Within one buffer: through a scratch buffer.
            D3D11_BUFFER_DESC bufferDesc = {};
            bufferDesc.ByteWidth = UINT(roundUp(size, 16));
            bufferDesc.Usage = D3D11_USAGE_DEFAULT;
            ID3D11Buffer *scratch = nullptr;
            if (FAILED(d3d->CreateBuffer(&bufferDesc, nullptr, &scratch))) {
                return;
            }
            context->CopySubresourceRegion(scratch, 0, 0, 0, 0, src->d3d, 0, &box);
            D3D11_BOX all = { 0, 0, 0, UINT(size), 1, 1 };
            context->CopySubresourceRegion(dst->d3d, 0, UINT(dstOffset), 0, 0, scratch, 0, &all);
            scratch->Release();
            return;
        }
        context->CopySubresourceRegion(dst->d3d, 0, UINT(dstOffset), 0, 0, src->d3d, 0, &box);
    }

    void D3D11Device::copyTextureRegionLocked(const RenderTextureCopyLocation &dst, const RenderTextureCopyLocation &src, uint32_t dstX, uint32_t dstY, uint32_t dstZ, const RenderBox *srcBox) {
        if ((dst.type == RenderTextureCopyType::SUBRESOURCE) && (src.type == RenderTextureCopyType::PLACED_FOOTPRINT)) {
            // A buffer into a texture: from CPU memory.
            const D3D11Texture *texture = static_cast<const D3D11Texture *>(dst.texture);
            const D3D11Buffer *buffer = static_cast<const D3D11Buffer *>(src.buffer);
            if ((texture == nullptr) || (texture->d3d == nullptr) || (buffer == nullptr)) {
                return;
            }
            if (buffer->cpu == nullptr) {
                D3D11_LOG_LIMITED(1, "A copy from a GPU buffer into a texture is not supported.");
                return;
            }
            const RenderFormat format = src.placedFootprint.format;
            const uint32_t blockWidth = std::max<uint32_t>(RenderFormatBlockWidth(format), 1);
            const uint32_t blockSize = RenderFormatSize(format);
            const uint32_t rowPitch = ((src.placedFootprint.rowWidth + blockWidth - 1) / blockWidth) * blockSize;
            const uint32_t footprintHeight = ((src.placedFootprint.height + blockWidth - 1) / blockWidth) * blockWidth;
            const uint32_t depthPitch = rowPitch * (footprintHeight / blockWidth);
            uint32_t left = 0, top = 0, front = 0;
            uint32_t width = src.placedFootprint.width, height = src.placedFootprint.height, depth = std::max<uint32_t>(src.placedFootprint.depth, 1);
            if (srcBox != nullptr) {
                left = uint32_t(std::max(srcBox->left, 0));
                top = uint32_t(std::max(srcBox->top, 0));
                front = uint32_t(std::max(srcBox->front, 0));
                width = uint32_t(std::max(srcBox->right - srcBox->left, 0));
                height = uint32_t(std::max(srcBox->bottom - srcBox->top, 0));
                depth = uint32_t(std::max(srcBox->back - srcBox->front, 1));
            }
            const uint64_t offset = src.placedFootprint.offset + uint64_t(front) * depthPitch + uint64_t(top / blockWidth) * rowPitch + uint64_t(left / blockWidth) * blockSize;
            const uint64_t bufferSize = std::max<uint64_t>(buffer->desc.size, 16);
            if (offset >= bufferSize) {
                return;
            }
            uint32_t mipWidth, mipHeight, mipDepth;
            texture->mipSize(dst.subresource.mipLevel, mipWidth, mipHeight, mipDepth);
            D3D11_BOX box = {};
            box.left = dstX;
            box.top = dstY;
            box.front = dstZ;
            box.right = std::min(dstX + width, mipWidth);
            box.bottom = std::min(dstY + height, mipHeight);
            box.back = std::min(dstZ + depth, mipDepth);
            if ((box.right <= box.left) || (box.bottom <= box.top) || (box.back <= box.front)) {
                return;
            }
            // The rows of the box's last depth slice must be in the buffer.
            const uint64_t rowsOfBlocks = (uint64_t(box.bottom - box.top) + blockWidth - 1) / blockWidth;
            const uint64_t needed = uint64_t(box.back - box.front - 1) * depthPitch + (rowsOfBlocks - 1) * rowPitch + ((uint64_t(box.right - box.left) + blockWidth - 1) / blockWidth) * blockSize;
            if (offset + needed > bufferSize) {
                D3D11_LOG_LIMITED(4, "A texture upload reads past its buffer: skipped.");
                return;
            }
            context->UpdateSubresource(texture->d3d, texture->subresourceIndex(dst.subresource.mipLevel, dst.subresource.arrayIndex), &box, buffer->cpu + offset, rowPitch, depthPitch);
            return;
        }
        if ((dst.type == RenderTextureCopyType::PLACED_FOOTPRINT) && (src.type == RenderTextureCopyType::SUBRESOURCE)) {
            // A texture into a buffer: through a staging texture, read now.
            const D3D11Texture *texture = static_cast<const D3D11Texture *>(src.texture);
            D3D11Buffer *buffer = mutableBuffer(dst.buffer);
            if ((texture == nullptr) || (texture->d3d == nullptr) || (buffer == nullptr) || (texture->desc.dimension == RenderTextureDimension::TEXTURE_3D)) {
                return;
            }
            uint32_t mipWidth, mipHeight, mipDepth;
            texture->mipSize(src.subresource.mipLevel, mipWidth, mipHeight, mipDepth);
            uint32_t left = 0, top = 0, width = mipWidth, height = mipHeight;
            if (srcBox != nullptr) {
                left = uint32_t(std::max(srcBox->left, 0));
                top = uint32_t(std::max(srcBox->top, 0));
                width = uint32_t(std::max(srcBox->right - srcBox->left, 0));
                height = uint32_t(std::max(srcBox->bottom - srcBox->top, 0));
            }
            if ((width == 0) || (height == 0)) {
                return;
            }
            D3D11_TEXTURE2D_DESC stagingDesc = {};
            stagingDesc.Width = width;
            stagingDesc.Height = height;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = texture->resourceFormat;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D *staging = nullptr;
            if (FAILED(d3d->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
                return;
            }
            const bool whole = (left == 0) && (top == 0) && (width == mipWidth) && (height == mipHeight);
            D3D11_BOX box = { left, top, 0, left + width, top + height, 1 };
            context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture->d3d, texture->subresourceIndex(src.subresource.mipLevel, src.subresource.arrayIndex), whole ? nullptr : &box);
            D3D11_MAPPED_SUBRESOURCE mappedTexture = {};
            if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mappedTexture))) {
                const RenderFormat format = dst.placedFootprint.format;
                const uint32_t blockWidth = std::max<uint32_t>(RenderFormatBlockWidth(format), 1);
                const uint32_t blockSize = RenderFormatSize(format);
                const uint32_t rowPitch = ((dst.placedFootprint.rowWidth + blockWidth - 1) / blockWidth) * blockSize;
                const uint32_t rowBytes = ((width + blockWidth - 1) / blockWidth) * blockSize;
                const uint32_t rows = (height + blockWidth - 1) / blockWidth;
                uint8_t *target = nullptr;
                D3D11_MAPPED_SUBRESOURCE mappedBuffer = {};
                bool bufferMapped = false;
                if (buffer->cpu != nullptr) {
                    target = buffer->cpu;
                }
                else if (buffer->d3d != nullptr) {
                    if (buffer->mapped) {
                        context->Unmap(buffer->d3d, 0);
                        buffer->mapped = false;
                    }
                    if (SUCCEEDED(context->Map(buffer->d3d, 0, D3D11_MAP_WRITE, 0, &mappedBuffer))) {
                        target = static_cast<uint8_t *>(mappedBuffer.pData);
                        bufferMapped = true;
                    }
                }
                const uint64_t bufferSize = std::max<uint64_t>(buffer->desc.size, 16);
                if (target != nullptr) {
                    for (uint32_t row = 0; row < rows; row++) {
                        const uint64_t at = dst.placedFootprint.offset + uint64_t(row) * rowPitch;
                        if (at + rowBytes > bufferSize) {
                            break;
                        }
                        memcpy(target + at, static_cast<const uint8_t *>(mappedTexture.pData) + size_t(row) * mappedTexture.RowPitch, std::min<uint32_t>(rowBytes, mappedTexture.RowPitch));
                    }
                }
                if (bufferMapped) {
                    context->Unmap(buffer->d3d, 0);
                }
                if (buffer->cpu != nullptr) {
                    buffer->markWritten(dst.placedFootprint.offset, dst.placedFootprint.offset + uint64_t(rows) * rowPitch);
                }
                context->Unmap(staging, 0);
            }
            staging->Release();
            return;
        }
        if ((dst.type == RenderTextureCopyType::SUBRESOURCE) && (src.type == RenderTextureCopyType::SUBRESOURCE)) {
            const D3D11Texture *dstTexture = static_cast<const D3D11Texture *>(dst.texture);
            const D3D11Texture *srcTexture = static_cast<const D3D11Texture *>(src.texture);
            if ((dstTexture == nullptr) || (srcTexture == nullptr) || (dstTexture->d3d == nullptr) || (srcTexture->d3d == nullptr)) {
                return;
            }
            const UINT dstSubresource = dstTexture->subresourceIndex(dst.subresource.mipLevel, dst.subresource.arrayIndex);
            const UINT srcSubresource = srcTexture->subresourceIndex(src.subresource.mipLevel, src.subresource.arrayIndex);
            uint32_t mipWidth, mipHeight, mipDepth;
            srcTexture->mipSize(src.subresource.mipLevel, mipWidth, mipHeight, mipDepth);
            D3D11_BOX box = {};
            bool whole = true;
            if (srcBox != nullptr) {
                box.left = UINT(std::max(srcBox->left, 0));
                box.top = UINT(std::max(srcBox->top, 0));
                box.front = UINT(std::max(srcBox->front, 0));
                box.right = std::min(UINT(std::max(srcBox->right, 0)), mipWidth);
                box.bottom = std::min(UINT(std::max(srcBox->bottom, 0)), mipHeight);
                box.back = std::min(UINT(std::max(srcBox->back, 1)), mipDepth);
                if ((box.right <= box.left) || (box.bottom <= box.top) || (box.back <= box.front)) {
                    return;
                }
                whole = (box.left == 0) && (box.top == 0) && (box.front == 0) && (box.right == mipWidth) && (box.bottom == mipHeight) && (box.back == mipDepth);
            }
            const bool restricted = (srcTexture->desc.flags & RenderTextureFlag::DEPTH_TARGET) || (srcTexture->desc.multisampling.sampleCount > RenderSampleCount::COUNT_1);
            if (restricted && (!whole || dstX || dstY || dstZ)) {
                // Depth and multisampled textures copy whole subresources only.
                D3D11_LOG_LIMITED(4, "A partial copy of a depth or multisampled texture is not supported: skipped.");
                return;
            }
            context->CopySubresourceRegion(dstTexture->d3d, dstSubresource, dstX, dstY, dstZ, srcTexture->d3d, srcSubresource, (whole && restricted) ? nullptr : (srcBox != nullptr ? &box : nullptr));
            return;
        }
        D3D11_LOG_LIMITED(1, "A buffer to buffer copy through texture locations is not supported.");
    }

    void D3D11Device::resolveRegionLocked(const D3D11Texture *dst, uint32_t dstX, uint32_t dstY, const D3D11Texture *src, const RenderRect *rect) {
        if ((dst == nullptr) || (src == nullptr) || (dst->d3d == nullptr) || (src->d3d == nullptr)) {
            return;
        }
        const DXGI_FORMAT format = toTargetViewFormat(dst->desc.format);
        const uint32_t srcWidth = src->desc.width, srcHeight = src->desc.height;
        RenderRect region(0, 0, int32_t(srcWidth), int32_t(srcHeight));
        if (rect != nullptr) {
            region = *rect;
        }
        region.left = std::max(region.left, 0);
        region.top = std::max(region.top, 0);
        region.right = std::min(region.right, int32_t(srcWidth));
        region.bottom = std::min(region.bottom, int32_t(srcHeight));
        if ((region.right <= region.left) || (region.bottom <= region.top)) {
            return;
        }
        const bool whole = (region.left == 0) && (region.top == 0) && (uint32_t(region.right) == srcWidth) && (uint32_t(region.bottom) == srcHeight) &&
            (dstX == 0) && (dstY == 0) && (dst->desc.width == srcWidth) && (dst->desc.height == srcHeight);
        if (whole) {
            context->ResolveSubresource(dst->d3d, 0, src->d3d, 0, format);
            return;
        }
        // Direct3D 11 resolves whole subresources: into a scratch texture of the source's size, then the region copied.
        const uint64_t key = uint64_t(format) | (uint64_t(srcWidth) << 16) | (uint64_t(srcHeight) << 40);
        ID3D11Texture2D *&scratch = resolveScratch[key];
        if (scratch == nullptr) {
            D3D11_TEXTURE2D_DESC scratchDesc = {};
            scratchDesc.Width = srcWidth;
            scratchDesc.Height = srcHeight;
            scratchDesc.MipLevels = 1;
            scratchDesc.ArraySize = 1;
            scratchDesc.Format = src->resourceFormat;
            scratchDesc.SampleDesc.Count = 1;
            scratchDesc.Usage = D3D11_USAGE_DEFAULT;
            scratchDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(d3d->CreateTexture2D(&scratchDesc, nullptr, &scratch))) {
                scratch = nullptr;
                return;
            }
        }
        context->ResolveSubresource(scratch, 0, src->d3d, 0, format);
        D3D11_BOX box = { UINT(region.left), UINT(region.top), 0, UINT(region.right), UINT(region.bottom), 1 };
        context->CopySubresourceRegion(dst->d3d, 0, dstX, dstY, 0, scratch, 0, &box);
    }

    bool D3D11Device::createClearShadersLocked() {
        if (clearShadersTried) {
            return (clearVs != nullptr) && (clearPs != nullptr);
        }
        clearShadersTried = true;
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        if (compiler == nullptr) {
            fprintf(stderr, "[d3d11] d3dcompiler_47.dll is not available: depth clears of rectangles clear the whole target.\n");
            return false;
        }
        pD3DCompile compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void *>(GetProcAddress(compiler, "D3DCompile")));
        if (compile == nullptr) {
            return false;
        }
        static const char source[] =
            "cbuffer ClearConstants : register(b0) { float g_Depth; };\n"
            "float4 VS(uint id : SV_VertexID) : SV_Position {\n"
            "    float2 uv = float2((id << 1) & 2, id & 2);\n"
            "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), g_Depth, 1.0);\n"
            "}\n"
            "void PS() {}\n";
        ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errors = nullptr;
        if (FAILED(compile(source, sizeof(source) - 1, "plume_d3d11_clear", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsBlob, &errors)) ||
            FAILED(compile(source, sizeof(source) - 1, "plume_d3d11_clear", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psBlob, &errors))) {
            fprintf(stderr, "[d3d11] The depth clear shaders did not compile: %s\n", errors ? static_cast<const char *>(errors->GetBufferPointer()) : "");
            safeRelease(vsBlob);
            safeRelease(psBlob);
            safeRelease(errors);
            return false;
        }
        safeRelease(errors);
        d3d->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &clearVs);
        d3d->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &clearPs);
        vsBlob->Release();
        psBlob->Release();
        for (uint32_t mode = 1; mode < 4; mode++) {
            D3D11_DEPTH_STENCIL_DESC depthDesc = {};
            depthDesc.DepthEnable = (mode & 1) ? TRUE : FALSE;
            depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
            depthDesc.StencilEnable = (mode & 2) ? TRUE : FALSE;
            depthDesc.StencilReadMask = 0xFF;
            depthDesc.StencilWriteMask = 0xFF;
            depthDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_REPLACE;
            depthDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_REPLACE;
            depthDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
            depthDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
            depthDesc.BackFace = depthDesc.FrontFace;
            d3d->CreateDepthStencilState(&depthDesc, &clearDepthStates[mode]);
        }
        D3D11_RASTERIZER_DESC rasterizerDesc = {};
        rasterizerDesc.FillMode = D3D11_FILL_SOLID;
        rasterizerDesc.CullMode = D3D11_CULL_NONE;
        rasterizerDesc.DepthClipEnable = FALSE;
        rasterizerDesc.ScissorEnable = TRUE;
        d3d->CreateRasterizerState(&rasterizerDesc, &clearRasterizer);
        D3D11_BLEND_DESC blendDesc = {};
        for (D3D11_RENDER_TARGET_BLEND_DESC &target : blendDesc.RenderTarget) {
            target.SrcBlend = D3D11_BLEND_ONE;
            target.DestBlend = D3D11_BLEND_ZERO;
            target.BlendOp = D3D11_BLEND_OP_ADD;
            target.SrcBlendAlpha = D3D11_BLEND_ONE;
            target.DestBlendAlpha = D3D11_BLEND_ZERO;
            target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            target.RenderTargetWriteMask = 0;
        }
        d3d->CreateBlendState(&blendDesc, &clearBlend);
        D3D11_BUFFER_DESC bufferDesc = {};
        bufferDesc.ByteWidth = 16;
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d3d->CreateBuffer(&bufferDesc, nullptr, &clearConstants);
        return (clearVs != nullptr) && (clearPs != nullptr) && (clearRasterizer != nullptr) && (clearBlend != nullptr) && (clearConstants != nullptr);
    }

    void D3D11Device::clearDepthRectsLocked(bool clearDepth, bool clearStencil, float depth, uint32_t stencil, const RenderRect *rects, uint32_t count) {
        D3D11Replay &r = replay;
        const D3D11Framebuffer *framebuffer = r.framebuffer;
        if ((framebuffer == nullptr) || (framebuffer->dsv == nullptr)) {
            return;
        }
        const UINT flags = (clearDepth ? D3D11_CLEAR_DEPTH : 0) | (clearStencil ? D3D11_CLEAR_STENCIL : 0);
        if (!createClearShadersLocked()) {
            context->ClearDepthStencilView(framebuffer->dsv, flags, depth, UINT8(stencil));
            return;
        }
        const uint32_t mode = (clearDepth ? 1u : 0u) | (clearStencil ? 2u : 0u);
        flushProxiesLocked();
        context->OMSetRenderTargets(0, nullptr, framebuffer->dsv);
        r.outputsValid = false;
        invalidateBindingsLocked(true, false, false);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(clearConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            float values[4] = { depth, 0.0f, 0.0f, 0.0f };
            memcpy(mapped.pData, values, sizeof(values));
            context->Unmap(clearConstants, 0);
        }
        context->VSSetShader(clearVs, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(clearPs, nullptr, 0);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetConstantBuffers(0, 1, &clearConstants);
        context->RSSetState(clearRasterizer);
        context->OMSetBlendState(clearBlend, nullptr, 0xFFFFFFFFu);
        context->OMSetDepthStencilState(clearDepthStates[mode], stencil & 0xFF);
        const D3D11_VIEWPORT viewport = { 0.0f, 0.0f, float(framebuffer->width), float(framebuffer->height), 0.0f, 1.0f };
        context->RSSetViewports(1, &viewport);
        for (uint32_t i = 0; i < count; i++) {
            const D3D11_RECT rect = { rects[i].left, rects[i].top, rects[i].right, rects[i].bottom };
            context->RSSetScissorRects(1, &rect);
            context->Draw(3, 0);
        }
        r.boundShaders[D3D11StageVertex] = clearVs;
        r.boundShaders[D3D11StageGeometry] = nullptr;
        r.boundShaders[D3D11StagePixel] = clearPs;
        r.boundInputLayout = nullptr;
        r.boundTopology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        r.boundCbs[D3D11StageVertex][0] = clearConstants;
        r.boundCbFirst[D3D11StageVertex][0] = 0;
        r.boundCbCount[D3D11StageVertex][0] = 0;
        r.boundRasterizer = clearRasterizer;
        r.boundBlend = clearBlend;
        r.blendDirty = true;
        r.boundDepthStencil = clearDepthStates[mode];
        r.boundStencilRef = stencil & 0xFF;
        r.viewportsDirty = r.viewportCount > 0;
        r.scissorsDirty = r.scissorCount > 0;
    }

    void D3D11Device::execute(const D3D11CommandList *list) {
        if (list == nullptr) {
            return;
        }
        resetReplayLocked();
        D3D11Replay &r = replay;
        const uint8_t *data = list->data.data();
        for (const D3D11Command &command : list->commands) {
            switch (command.op) {
            case D3D11Command::Op::SetPipeline: {
                const D3D11Pipeline *pipeline = static_cast<const D3D11Pipeline *>(command.p0);
                if (pipeline == nullptr) {
                    break;
                }
                if (pipeline->type == D3D11Pipeline::Type::Graphics) {
                    r.graphicsPipeline = static_cast<const D3D11GraphicsPipeline *>(pipeline);
                }
                else {
                    r.computePipeline = static_cast<const D3D11ComputePipeline *>(pipeline);
                }
                break;
            }
            case D3D11Command::Op::SetGraphicsLayout:
                if (r.graphicsLayout != command.p0) {
                    r.graphicsLayout = static_cast<const D3D11PipelineLayout *>(command.p0);
                    std::fill(std::begin(r.graphicsSets), std::end(r.graphicsSets), nullptr);
                    for (auto &root : r.roots) {
                        root = RenderBufferReference();
                    }
                }
                break;
            case D3D11Command::Op::SetComputeLayout:
                if (r.computeLayout != command.p0) {
                    r.computeLayout = static_cast<const D3D11PipelineLayout *>(command.p0);
                    std::fill(std::begin(r.computeSets), std::end(r.computeSets), nullptr);
                }
                break;
            case D3D11Command::Op::GraphicsPush:
                if ((command.a < 4) && (command.b + command.c <= 256)) {
                    memcpy(r.graphicsPush[command.a] + command.b, data + command.data, command.c);
                    r.graphicsPushDirty[command.a] = true;
                }
                break;
            case D3D11Command::Op::ComputePush:
                if ((command.a < 4) && (command.b + command.c <= 256)) {
                    memcpy(r.computePush[command.a] + command.b, data + command.data, command.c);
                    r.computePushDirty[command.a] = true;
                }
                break;
            case D3D11Command::Op::GraphicsSet:
                if (command.a < 8) {
                    r.graphicsSets[command.a] = const_cast<D3D11DescriptorSet *>(static_cast<const D3D11DescriptorSet *>(command.p0));
                }
                break;
            case D3D11Command::Op::ComputeSet:
                if (command.a < 8) {
                    r.computeSets[command.a] = const_cast<D3D11DescriptorSet *>(static_cast<const D3D11DescriptorSet *>(command.p0));
                }
                break;
            case D3D11Command::Op::GraphicsRoot:
                if (command.a < 8) {
                    r.roots[command.a] = RenderBufferReference(static_cast<const RenderBuffer *>(command.p0), command.u0);
                }
                break;
            case D3D11Command::Op::IndexBuffer:
                r.indexBuffer = static_cast<const D3D11Buffer *>(command.p0);
                r.indexOffset = command.u0;
                r.indexSize = command.a;
                r.indexFormat = (RenderFormat(command.b) == RenderFormat::R32_UINT) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
                r.indexDirty = true;
                break;
            case D3D11Command::Op::VertexBuffers: {
                const D3D11VertexBinding *bindings = reinterpret_cast<const D3D11VertexBinding *>(data + command.data);
                for (uint32_t i = 0; i < command.b; i++) {
                    const uint32_t slot = command.a + i;
                    if (slot >= 16) {
                        break;
                    }
                    r.vertex[slot].buffer = static_cast<const D3D11Buffer *>(bindings[i].buffer);
                    r.vertex[slot].offset = bindings[i].offset;
                    r.vertex[slot].size = bindings[i].size;
                    r.vertex[slot].stride = bindings[i].stride;
                    r.vertexDirty |= 1u << slot;
                }
                break;
            }
            case D3D11Command::Op::Viewports:
                r.viewportCount = command.a;
                memcpy(r.viewports, data + command.data, command.a * sizeof(D3D11_VIEWPORT));
                r.viewportsDirty = true;
                break;
            case D3D11Command::Op::Scissors:
                r.scissorCount = command.a;
                memcpy(r.scissors, data + command.data, command.a * sizeof(D3D11_RECT));
                r.scissorsDirty = true;
                break;
            case D3D11Command::Op::Framebuffer:
                r.framebuffer = static_cast<const D3D11Framebuffer *>(command.p0);
                break;
            case D3D11Command::Op::ClearColor: {
                flushProxiesLocked();
                const D3D11Framebuffer *framebuffer = r.framebuffer;
                if ((framebuffer == nullptr) || (command.a >= framebuffer->rtvs.size()) || (framebuffer->rtvs[command.a] == nullptr)) {
                    break;
                }
                float color[4];
                memcpy(color, data + command.data, sizeof(color));
                const D3D11_RECT *rects = reinterpret_cast<const D3D11_RECT *>(data + command.data + 16);
                if ((command.b == 0) || (context1 == nullptr)) {
                    context->ClearRenderTargetView(framebuffer->rtvs[command.a], color);
                }
                else {
                    context1->ClearView(framebuffer->rtvs[command.a], color, rects, command.b);
                }
                break;
            }
            case D3D11Command::Op::ClearDepth: {
                flushProxiesLocked();
                const D3D11Framebuffer *framebuffer = r.framebuffer;
                if ((framebuffer == nullptr) || (framebuffer->dsv == nullptr)) {
                    break;
                }
                float depth;
                memcpy(&depth, &command.c, 4);
                const bool clearDepth = (command.a & 1) != 0;
                const bool clearStencil = (command.a & 2) != 0;
                const UINT flags = (clearDepth ? D3D11_CLEAR_DEPTH : 0) | (clearStencil ? D3D11_CLEAR_STENCIL : 0);
                const D3D11_RECT *rects = reinterpret_cast<const D3D11_RECT *>(data + command.data);
                bool whole = (command.d == 0);
                for (uint32_t i = 0; (i < command.d) && !whole; i++) {
                    whole = (rects[i].left <= 0) && (rects[i].top <= 0) && (uint32_t(rects[i].right) >= framebuffer->width) && (uint32_t(rects[i].bottom) >= framebuffer->height);
                }
                if (whole) {
                    context->ClearDepthStencilView(framebuffer->dsv, flags, depth, UINT8(command.b));
                }
                else if (flags != 0) {
                    std::vector<RenderRect> renderRects(command.d);
                    for (uint32_t i = 0; i < command.d; i++) {
                        renderRects[i] = RenderRect(rects[i].left, rects[i].top, rects[i].right, rects[i].bottom);
                    }
                    clearDepthRectsLocked(clearDepth, clearStencil, depth, command.b, renderRects.data(), command.d);
                }
                break;
            }
            case D3D11Command::Op::Draw:
                if ((r.graphicsPipeline != nullptr) && r.graphicsPipeline->isValid) {
                    flushGraphicsLocked();
                    context->DrawInstanced(command.a, command.b, command.c, command.d);
                }
                break;
            case D3D11Command::Op::DrawIndexed:
                if ((r.graphicsPipeline != nullptr) && r.graphicsPipeline->isValid) {
                    flushGraphicsLocked();
                    context->DrawIndexedInstanced(command.a, command.b, command.c, int32_t(command.d), command.e);
                }
                break;
            case D3D11Command::Op::Dispatch:
                if ((r.computePipeline != nullptr) && r.computePipeline->isValid) {
                    flushComputeLocked();
                    context->Dispatch(command.a, command.b, command.c);
                    // The unordered access views go, or Direct3D 11 would refuse their buffers to later draws.
                    ID3D11UnorderedAccessView *nulls[8] = {};
                    context->CSSetUnorderedAccessViews(0, 8, nulls, nullptr);
                    memset(r.boundComputeUavs, 0, sizeof(r.boundComputeUavs));
                    r.computeLast = true;
                }
                break;
            case D3D11Command::Op::CopyBufferRegion:
                copyBufferRegionLocked(static_cast<const D3D11Buffer *>(command.p0), command.u0, static_cast<const D3D11Buffer *>(command.p1), command.u1, command.u2);
                break;
            case D3D11Command::Op::CopyTextureRegion: {
                flushProxiesLocked();
                const D3D11TextureCopy *copy = reinterpret_cast<const D3D11TextureCopy *>(data + command.data);
                copyTextureRegionLocked(copy->dst, copy->src, command.a, command.b, command.c, copy->hasBox ? &copy->box : nullptr);
                break;
            }
            case D3D11Command::Op::CopyBuffer: {
                const D3D11Buffer *dst = static_cast<const D3D11Buffer *>(command.p0);
                const D3D11Buffer *src = static_cast<const D3D11Buffer *>(command.p1);
                if ((dst != nullptr) && (src != nullptr)) {
                    copyBufferRegionLocked(dst, 0, src, 0, std::min(dst->desc.size, src->desc.size));
                }
                break;
            }
            case D3D11Command::Op::CopyTexture: {
                flushProxiesLocked();
                const D3D11Texture *dst = static_cast<const D3D11Texture *>(command.p0);
                const D3D11Texture *src = static_cast<const D3D11Texture *>(command.p1);
                if ((dst != nullptr) && (src != nullptr) && (dst->d3d != nullptr) && (src->d3d != nullptr)) {
                    context->CopyResource(dst->d3d, src->d3d);
                }
                break;
            }
            case D3D11Command::Op::Resolve: {
                flushProxiesLocked();
                const D3D11Texture *dst = static_cast<const D3D11Texture *>(command.p0);
                const D3D11Texture *src = static_cast<const D3D11Texture *>(command.p1);
                if ((dst != nullptr) && (src != nullptr) && (dst->d3d != nullptr) && (src->d3d != nullptr)) {
                    context->ResolveSubresource(dst->d3d, 0, src->d3d, 0, toTargetViewFormat(dst->desc.format));
                }
                break;
            }
            case D3D11Command::Op::ResolveRegion: {
                flushProxiesLocked();
                RenderRect rect;
                if (command.c) {
                    memcpy(&rect, data + command.data, sizeof(rect));
                }
                resolveRegionLocked(static_cast<const D3D11Texture *>(command.p0), command.a, command.b, static_cast<const D3D11Texture *>(command.p1), command.c ? &rect : nullptr);
                break;
            }
            case D3D11Command::Op::Discard: {
                flushProxiesLocked();
                const D3D11Texture *texture = static_cast<const D3D11Texture *>(command.p0);
                if ((texture != nullptr) && (texture->d3d != nullptr) && (context1 != nullptr)) {
                    context1->DiscardResource(texture->d3d);
                }
                break;
            }
            case D3D11Command::Op::BlendConstant:
                memcpy(r.blendFactor, data + command.data, sizeof(r.blendFactor));
                r.blendDirty = true;
                break;
            }
        }
        // The proxies' contents back into their targets before the list ends.
        flushProxiesLocked();
    }

    // D3D11Interface

    D3D11Interface::D3D11Interface() {
        capabilities.shaderFormat = RenderShaderFormat::DXBC;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)))) {
            dxgiFactory = nullptr;
            fprintf(stderr, "[d3d11] CreateDXGIFactory1 failed.\n");
            return;
        }
        IDXGIFactory5 *factory5 = nullptr;
        if (SUCCEEDED(dxgiFactory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
            BOOL allow = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
                allowTearing = (allow != FALSE);
            }
            factory5->Release();
        }
        for (UINT i = 0;; i++) {
            IDXGIAdapter1 *adapter = nullptr;
            if (dxgiFactory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 adapterDesc = {};
            adapter->GetDesc1(&adapterDesc);
            if (!(adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                deviceNames.push_back(utf16ToUtf8(adapterDesc.Description));
            }
            adapter->Release();
        }
    }

    D3D11Interface::~D3D11Interface() {
        safeRelease(dxgiFactory);
    }

    std::unique_ptr<RenderDevice> D3D11Interface::createDevice(const std::string &preferredDeviceName) {
        auto device = std::make_unique<D3D11Device>(this, preferredDeviceName);
        if (!device->isValid()) {
            return nullptr;
        }
        return device;
    }

    const RenderInterfaceCapabilities &D3D11Interface::getCapabilities() const {
        return capabilities;
    }

    const std::vector<std::string> &D3D11Interface::getDeviceNames() const {
        return deviceNames;
    }

    bool D3D11Interface::isValid() const {
        return dxgiFactory != nullptr;
    }

    // Configuration and diagnostics.

    void D3D11SetTableBindings(RenderPipelineLayout *layout, const D3D11TableBinding *bindings, uint32_t count) {
        D3D11PipelineLayout *interfaceLayout = static_cast<D3D11PipelineLayout *>(layout);
        if (interfaceLayout == nullptr) {
            return;
        }
        interfaceLayout->tableBindings.assign(bindings, bindings + count);
        for (uint32_t i = 0; i < count; i++) {
            if (bindings[i].setIndex < interfaceLayout->sets.size()) {
                interfaceLayout->sets[bindings[i].setIndex].table = true;
            }
        }
    }

    void D3D11SetGuestBinding(RenderPipelineLayout *layout, const D3D11GuestBinding &binding) {
        D3D11PipelineLayout *interfaceLayout = static_cast<D3D11PipelineLayout *>(layout);
        if (interfaceLayout == nullptr) {
            return;
        }
        interfaceLayout->hasGuest = true;
        interfaceLayout->guest = binding;
        for (D3D11PipelineLayout::Set &set : interfaceLayout->sets) {
            set.table = true;
        }
    }

    void D3D11SetRootRegister(RenderPipelineLayout *layout, uint32_t rootIndex, uint32_t reg) {
        D3D11PipelineLayout *interfaceLayout = static_cast<D3D11PipelineLayout *>(layout);
        if ((interfaceLayout != nullptr) && (rootIndex < interfaceLayout->roots.size())) {
            interfaceLayout->roots[rootIndex].reg = reg;
        }
    }

    bool D3D11SetBlendConstant(RenderCommandList *commandList, const float rgba[4]) {
        D3D11CommandList *interfaceList = static_cast<D3D11CommandList *>(commandList);
        if (interfaceList == nullptr) {
            return false;
        }
        const uint32_t stored = interfaceList->store(rgba, 16);
        interfaceList->emit(D3D11Command::Op::BlendConstant).data = stored;
        return true;
    }

    bool D3D11DeviceRemoved(RenderDevice *device, HRESULT *reason) {
        D3D11Device *interfaceDevice = static_cast<D3D11Device *>(device);
        if ((interfaceDevice == nullptr) || (interfaceDevice->d3d == nullptr)) {
            return false;
        }
        const HRESULT removed = interfaceDevice->d3d->GetDeviceRemovedReason();
        if (reason != nullptr) {
            *reason = removed;
        }
        return FAILED(removed);
    }

    IDXGIAdapter1 *D3D11Adapter(RenderDevice *device) {
        D3D11Device *interfaceDevice = static_cast<D3D11Device *>(device);
        return (interfaceDevice != nullptr) ? interfaceDevice->adapter : nullptr;
    }

    bool D3D11TextureCreated(const RenderTexture *texture) {
        return (texture != nullptr) && (static_cast<const D3D11Texture *>(texture)->d3d != nullptr);
    }

    bool D3D11PipelineCreated(const RenderPipeline *pipeline) {
        return (pipeline != nullptr) && static_cast<const D3D11Pipeline *>(pipeline)->isValid;
    }

    std::string D3D11FeatureLevelName(RenderDevice *device) {
        D3D11Device *interfaceDevice = static_cast<D3D11Device *>(device);
        if (interfaceDevice == nullptr) {
            return std::string();
        }
        return (interfaceDevice->featureLevel >= D3D_FEATURE_LEVEL_11_1) ? "11_1" : "11_0";
    }

    std::unique_ptr<RenderInterface> CreateD3D11Interface() {
        auto renderInterface = std::make_unique<D3D11Interface>();
        if (!renderInterface->isValid()) {
            return nullptr;
        }
        return renderInterface;
    }
};
