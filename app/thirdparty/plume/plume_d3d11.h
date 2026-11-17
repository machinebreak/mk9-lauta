//
// plume
//
// Copyright (c) 2024 renderbag and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE file for details.
//
// mk9-lauta (2026-11-16, docs/renderer/d3d11.md): a Direct3D 11 back end, for GPUs and drivers whose Direct3D 12 or
// Vulkan cannot run the game (an AMD Radeon HD 7800 on an old driver crashed in its Direct3D 12 shader compiler). It
// takes DXBC (shader model 5.0, FXC) and does what Direct3D 11 lacks itself:
//  - Command lists are recorded and replayed on the immediate context when they are executed (under one lock).
//  - UPLOAD buffers live in CPU memory (write-watched pages). What the GPU reads of them directly (vertex and index
//    buffers, raw views) is copied, a page at a time when it changed, into a GPU twin when a command reads it; a
//    constant buffer bound from one is copied into a dynamic constant buffer of its own; copies from one write the
//    destination from CPU memory (UpdateSubresource).
//  - Descriptor tables (bindless) do not exist: a pipeline layout says where each draw's indices are, and the replay
//    binds the views of the registers each shader declares (D3D11SetGuestBinding, D3D11SetTableBindings).
//  - Views have no component mapping: the replay writes each bound view's mapping to a constant buffer a guest
//    shader reads (D3D11GuestBinding::extrasRegister), with the root views' offsets.
//

#pragma once

#include "plume_render_interface.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <d3d11_4.h>
#include <dxgi1_2.h>

namespace plume {
    struct D3D11Buffer;
    struct D3D11CommandList;
    struct D3D11CommandQueue;
    struct D3D11Device;
    struct D3D11Interface;
    struct D3D11Pipeline;
    struct D3D11PipelineLayout;
    struct D3D11Texture;
    struct D3D11TextureView;

    // ---------------------------------------------------------------------------------------------------------------
    // Bindless emulation, configured on a pipeline layout after it is created.

    // A register bound from a descriptor table by an index the draw's push constants hold (Renut's own shaders).
    struct D3D11TableBinding {
        enum class Kind : uint8_t { SRV, SAMPLER };
        Kind kind = Kind::SRV;
        uint8_t reg = 0;              // t# or s#
        uint8_t setIndex = 0;         // the descriptor set (a table)
        uint8_t pushRange = 0;        // the push constant range holding the index
        uint32_t pushOffset = 0;      // the index's byte offset in that range's data
        uint32_t indexMask = 0xFFFFFFFFu;
    };

    // The guest draws (Renut's translated Xenos shaders, shader_common.h RENUT_D3D11): each texture register is a
    // Xenos fetch slot, 2D at t0-t31, 3D at t32-t63, cube at t64-t95, the stacked textures' arrays at t96-t111 (by the
    // slot's low four bits); sampler s# is slot # or # + 16 (the slot whose texture the shader declares). The indices
    // are words of a root constant buffer's data (SharedConstants), read where the CPU wrote them.
    struct D3D11GuestBinding {
        uint32_t rootIndex = 2;               // the root constant buffer holding the indices
        uint32_t textureIndicesOffset = 0;    // uint[3][32]: 2D, 3D, cube indices per slot
        uint32_t samplerIndicesOffset = 384;  // uint[32]
        uint32_t indexMask = 0x03FFFFFFu;
        uint32_t stackedFlag = 0x80000000u;   // on a slot's 3D index: its view is the array (t96 + slot % 16)
        uint32_t textureSet = 0;              // the views' table
        uint32_t samplerSet = 3;              // the samplers' table
        uint32_t extrasRegister = 3;          // b#: uint4[32] mappings (t0-t127), then the root views' offsets
        uint32_t fetchRootIndex = 3;          // the root SRV whose offset goes to the extras (whole-buffer view)
        uint32_t exportRootIndex = 4;         // the root UAV, likewise
    };

    void D3D11SetTableBindings(RenderPipelineLayout *layout, const D3D11TableBinding *bindings, uint32_t count);
    void D3D11SetGuestBinding(RenderPipelineLayout *layout, const D3D11GuestBinding &binding);
    // The register of a root descriptor (default: its shaderRegister).
    void D3D11SetRootRegister(RenderPipelineLayout *layout, uint32_t rootIndex, uint32_t reg);
    // The blend factor for the rest of the command list.
    bool D3D11SetBlendConstant(RenderCommandList *commandList, const float rgba[4]);
    // Diagnostics for the application.
    bool D3D11DeviceRemoved(RenderDevice *device, HRESULT *reason);
    IDXGIAdapter1 *D3D11Adapter(RenderDevice *device);
    bool D3D11TextureCreated(const RenderTexture *texture);
    bool D3D11PipelineCreated(const RenderPipeline *pipeline);
    std::string D3D11FeatureLevelName(RenderDevice *device);

    // What a DXBC shader declares (its SHEX chunk).
    struct D3D11ShaderInfo {
        uint32_t programType = 0xFFFFFFFFu;  // 0 pixel, 1 vertex, 2 geometry, 5 compute
        uint64_t srvMask[2] = {};            // t0-t127
        uint32_t samplerMask = 0;            // s0-s15
        uint32_t cbMask = 0;                 // b0-b13
        uint16_t cbSize[14] = {};            // 16-byte registers
        uint64_t uavMask = 0;                // u0-u63
        bool usesSrv(uint32_t reg) const { return reg < 128 && ((srvMask[reg >> 6] >> (reg & 63)) & 1); }
    };

    bool D3D11ParseShader(const void *data, size_t size, D3D11ShaderInfo &info);

    std::unique_ptr<RenderInterface> CreateD3D11Interface();

    // ---------------------------------------------------------------------------------------------------------------

    struct D3D11Interface : RenderInterface {
        IDXGIFactory1 *dxgiFactory = nullptr;
        RenderInterfaceCapabilities capabilities;
        std::vector<std::string> deviceNames;
        bool allowTearing = false;

        D3D11Interface();
        ~D3D11Interface() override;
        std::unique_ptr<RenderDevice> createDevice(const std::string &preferredDeviceName) override;
        const RenderInterfaceCapabilities &getCapabilities() const override;
        const std::vector<std::string> &getDeviceNames() const override;
        bool isValid() const;
    };

    struct D3D11BufferFormattedView : RenderBufferFormattedView {
        ID3D11ShaderResourceView *srv = nullptr;
        ID3D11UnorderedAccessView *uav = nullptr;
        ~D3D11BufferFormattedView() override;
    };

    struct D3D11Buffer : RenderBuffer {
        D3D11Device *device = nullptr;
        RenderBufferDesc desc;
        // DEFAULT: the buffer. UPLOAD: the GPU twin (made when a command first reads it as a vertex, index or raw
        // buffer). READBACK: a staging buffer.
        ID3D11Buffer *d3d = nullptr;
        // UPLOAD: the data, in write-watched CPU memory (writeWatch), and the twin's stale pages.
        uint8_t *cpu = nullptr;
        bool writeWatch = false;
        std::vector<uint64_t> stalePages;
        bool twinFailed = false;
        // Raw views of the whole buffer (UNORDERED_ACCESS, or read as a byte address buffer).
        ID3D11ShaderResourceView *rawSrv = nullptr;
        ID3D11UnorderedAccessView *rawUav = nullptr;
        // READBACK: mapped by map() (the immediate context, under the device's lock).
        bool mapped = false;
        std::string name;

        D3D11Buffer(D3D11Device *device, const RenderBufferDesc &desc);
        ~D3D11Buffer() override;
        void *map(uint32_t subresource, const RenderRange *readRange) override;
        void unmap(uint32_t subresource, const RenderRange *writtenRange) override;
        std::unique_ptr<RenderBufferFormattedView> createBufferFormattedView(RenderFormat format) override;
        void setName(const std::string &name) override;
        uint64_t getDeviceAddress() const override;
        bool valid() const;
        // The GPU resource commands read (UPLOAD: the twin, with [begin, end) brought up to date). Immediate context.
        ID3D11Buffer *gpuResource(uint64_t begin, uint64_t end);
        ID3D11ShaderResourceView *getRawSrv();
        ID3D11UnorderedAccessView *getRawUav();
        void markWritten(uint64_t begin, uint64_t end);
    };

    struct D3D11TextureView : RenderTextureView {
        D3D11Texture *texture = nullptr;
        RenderTextureViewDesc desc;
        ID3D11ShaderResourceView *srv = nullptr;
        uint32_t swizzle = 0;  // 3 bits a component, 0-3 a channel, 4 zero, 5 one

        D3D11TextureView(D3D11Texture *texture, const RenderTextureViewDesc &desc);
        ~D3D11TextureView() override;
    };

    struct D3D11Texture : RenderTexture {
        D3D11Device *device = nullptr;
        RenderTextureDesc desc;
        ID3D11Resource *d3d = nullptr;
        DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
        ID3D11ShaderResourceView *defaultSrv = nullptr;
        std::mutex viewMutex;
        std::unordered_map<uint64_t, ID3D11RenderTargetView *> rtvs;
        std::unordered_map<uint64_t, ID3D11DepthStencilView *> dsvs;
        bool swapChainBuffer = false;

        D3D11Texture() = default;
        D3D11Texture(D3D11Device *device, const RenderTextureDesc &desc);
        ~D3D11Texture() override;
        std::unique_ptr<RenderTextureView> createTextureView(const RenderTextureViewDesc &desc) const override;
        void setName(const std::string &name) override;
        void releaseViews();
        ID3D11ShaderResourceView *getDefaultSrv();
        ID3D11RenderTargetView *getRtv(uint32_t mip, uint32_t slice, RenderFormat format);
        ID3D11DepthStencilView *getDsv(uint32_t mip, uint32_t slice, bool readOnly);
        uint32_t subresourceIndex(uint32_t mip, uint32_t slice) const;
        void mipSize(uint32_t mip, uint32_t &width, uint32_t &height, uint32_t &depth) const;
    };

    struct D3D11Sampler : RenderSampler {
        ID3D11SamplerState *d3d = nullptr;
        D3D11Sampler(D3D11Device *device, const RenderSamplerDesc &desc);
        ~D3D11Sampler() override;
    };

    struct D3D11Shader : RenderShader {
        D3D11Device *device = nullptr;
        std::vector<uint8_t> bytecode;
        D3D11ShaderInfo info;
        ID3D11VertexShader *vs = nullptr;
        ID3D11PixelShader *ps = nullptr;
        ID3D11GeometryShader *gs = nullptr;
        ID3D11ComputeShader *cs = nullptr;

        D3D11Shader(D3D11Device *device, const void *data, uint64_t size);
        ~D3D11Shader() override;
        void setName(const std::string &name) override;
        bool valid() const;
    };

    struct D3D11Pipeline : RenderPipeline {
        enum class Type { Graphics, Compute };
        D3D11Device *device = nullptr;
        Type type;
        const D3D11PipelineLayout *layout = nullptr;
        bool isValid = false;

        D3D11Pipeline(D3D11Device *device, Type type) : device(device), type(type) {}
        void setName(const std::string &name) override {}
        RenderPipelineProgram getProgram(const std::string &name) const override { return RenderPipelineProgram(); }
    };

    enum D3D11Stage : uint32_t { D3D11StageVertex = 0, D3D11StageGeometry = 1, D3D11StagePixel = 2, D3D11StageCompute = 3, D3D11StageCount = 4 };

    struct D3D11GraphicsPipeline : D3D11Pipeline {
        ID3D11VertexShader *vs = nullptr;
        ID3D11GeometryShader *gs = nullptr;
        ID3D11PixelShader *ps = nullptr;
        ID3D11InputLayout *inputLayout = nullptr;
        ID3D11RasterizerState *rasterizer = nullptr;
        ID3D11BlendState *blend = nullptr;
        ID3D11DepthStencilState *depthStencil = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        uint32_t stencilReference = 0;
        D3D11ShaderInfo stageInfo[3];
        bool hasStage[3] = {};
        uint64_t uavMask = 0;  // the graphics stages' UAVs

        D3D11GraphicsPipeline(D3D11Device *device, const RenderGraphicsPipelineDesc &desc);
        ~D3D11GraphicsPipeline() override;
    };

    struct D3D11ComputePipeline : D3D11Pipeline {
        ID3D11ComputeShader *cs = nullptr;
        D3D11ShaderInfo info;

        D3D11ComputePipeline(D3D11Device *device, const RenderComputePipelineDesc &desc);
        ~D3D11ComputePipeline() override;
    };

    struct D3D11Descriptor {
        RenderDescriptorRangeType type = RenderDescriptorRangeType::UNKNOWN;
        ID3D11ShaderResourceView *srv = nullptr;   // owned by a view, a buffer, or ownedSrv
        ID3D11UnorderedAccessView *uav = nullptr;  // likewise
        ID3D11SamplerState *sampler = nullptr;
        const D3D11Buffer *buffer = nullptr;       // constant buffers
        uint64_t bufferSize = 0;
        uint32_t swizzle = 0x688;                  // identity
        ID3D11ShaderResourceView *ownedSrv = nullptr;
        ID3D11UnorderedAccessView *ownedUav = nullptr;
    };

    struct D3D11DescriptorSet : RenderDescriptorSet {
        D3D11Device *device = nullptr;
        std::vector<D3D11Descriptor> descriptors;
        std::vector<RenderDescriptorRangeType> types;
        std::mutex mutex;

        D3D11DescriptorSet(D3D11Device *device, const RenderDescriptorSetDesc &desc);
        ~D3D11DescriptorSet() override;
        void setBuffer(uint32_t descriptorIndex, const RenderBuffer *buffer, uint64_t bufferSize, const RenderBufferStructuredView *bufferStructuredView, const RenderBufferFormattedView *bufferFormattedView) override;
        void setTexture(uint32_t descriptorIndex, const RenderTexture *texture, RenderTextureLayout textureLayout, const RenderTextureView *textureView) override;
        void setSampler(uint32_t descriptorIndex, const RenderSampler *sampler) override;
        void setAccelerationStructure(uint32_t descriptorIndex, const RenderAccelerationStructure *accelerationStructure) override;
        void clearOwned(D3D11Descriptor &descriptor);
    };

    struct D3D11PipelineLayout : RenderPipelineLayout {
        struct PushRange {
            uint32_t offset = 0;
            uint32_t size = 0;
            uint32_t reg = 0;
            RenderShaderStageFlags stages = 0;
        };
        struct Root {
            RenderRootDescriptorType type = RenderRootDescriptorType::UNKNOWN;
            uint32_t reg = 0;
        };
        struct Set {
            std::vector<RenderDescriptorRange> ranges;
            bool table = false;
        };
        D3D11Device *device = nullptr;
        std::vector<PushRange> pushRanges;
        std::vector<Root> roots;
        std::vector<Set> sets;
        std::vector<D3D11TableBinding> tableBindings;
        bool hasGuest = false;
        D3D11GuestBinding guest;

        D3D11PipelineLayout(D3D11Device *device, const RenderPipelineLayoutDesc &desc);
        ~D3D11PipelineLayout() override;
    };

    struct D3D11Framebuffer : RenderFramebuffer {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<ID3D11RenderTargetView *> rtvs;
        ID3D11DepthStencilView *dsv = nullptr;
        std::vector<const D3D11Texture *> colorTextures;
        const D3D11Texture *depthTexture = nullptr;
        // Each color target's level, layer, view format and size; the depth target's size. Direct3D 11 binds targets
        // of one size only: 'mismatched' framebuffers draw their other color targets into proxies (bindOutputsLocked).
        std::vector<uint32_t> colorMips, colorSlices, colorWidths, colorHeights;
        std::vector<RenderFormat> colorFormats;
        uint32_t depthWidth = 0;
        uint32_t depthHeight = 0;
        bool mismatched = false;

        D3D11Framebuffer(D3D11Device *device, const RenderFramebufferDesc &desc);
        ~D3D11Framebuffer() override;
        uint32_t getWidth() const override;
        uint32_t getHeight() const override;
    };

    struct D3D11CommandFence : RenderCommandFence {
        D3D11Device *device = nullptr;
        ID3D11Fence *fence = nullptr;  // Direct3D 11.3 (Windows 10)
        HANDLE event = nullptr;
        uint64_t value = 0;
        ID3D11Query *query = nullptr;  // otherwise an event query
        bool pending = false;

        D3D11CommandFence(D3D11Device *device);
        ~D3D11CommandFence() override;
        void signalLocked();
        void wait();
    };

    struct D3D11CommandSemaphore : RenderCommandSemaphore {
    };

    struct D3D11QueryPool : RenderQueryPool {
        std::vector<uint64_t> results;
        D3D11QueryPool(uint32_t count) : results(count, 0) {}
        void queryResults() override {}
        const uint64_t *getResults() const override { return results.data(); }
        uint32_t getCount() const override { return uint32_t(results.size()); }
    };

    // A recorded command; variable-sized arguments are in D3D11CommandList::data.
    struct D3D11Command {
        enum class Op : uint8_t {
            SetPipeline, SetGraphicsLayout, SetComputeLayout, GraphicsPush, ComputePush, GraphicsSet, ComputeSet,
            GraphicsRoot, IndexBuffer, VertexBuffers, Viewports, Scissors, Framebuffer, ClearColor, ClearDepth,
            Draw, DrawIndexed, Dispatch, CopyBufferRegion, CopyTextureRegion, CopyBuffer, CopyTexture, Resolve,
            ResolveRegion, Discard, BlendConstant
        };
        Op op;
        const void *p0 = nullptr;
        const void *p1 = nullptr;
        uint64_t u0 = 0;
        uint64_t u1 = 0;
        uint64_t u2 = 0;
        uint32_t a = 0, b = 0, c = 0, d = 0, e = 0;
        uint32_t data = 0;  // offset in D3D11CommandList::data
    };

    struct D3D11CommandList : RenderCommandList {
        D3D11CommandQueue *queue = nullptr;
        std::vector<D3D11Command> commands;
        std::vector<uint8_t> data;
        // The layouts set while recording (a push constant call's size 0 is its range's).
        const D3D11PipelineLayout *recordGraphicsLayout = nullptr;
        const D3D11PipelineLayout *recordComputeLayout = nullptr;

        D3D11CommandList(D3D11CommandQueue *queue);
        ~D3D11CommandList() override;
        void begin() override;
        void end() override;
        void barriers(RenderBarrierStages stages, const RenderBufferBarrier *bufferBarriers, uint32_t bufferBarriersCount, const RenderTextureBarrier *textureBarriers, uint32_t textureBarriersCount) override;
        void dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override;
        void traceRays(uint32_t width, uint32_t height, uint32_t depth, RenderBufferReference shaderBindingTable, const RenderShaderBindingGroupsInfo &shaderBindingGroupsInfo) override;
        void drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation) override;
        void drawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation) override;
        void setPipeline(const RenderPipeline *pipeline) override;
        void setComputePipelineLayout(const RenderPipelineLayout *pipelineLayout) override;
        void setComputePushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override;
        void setComputeDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override;
        void setGraphicsPipelineLayout(const RenderPipelineLayout *pipelineLayout) override;
        void setGraphicsPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override;
        void setGraphicsDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override;
        void setGraphicsRootDescriptor(RenderBufferReference bufferReference, uint32_t rootDescriptorIndex) override;
        void setRaytracingPipelineLayout(const RenderPipelineLayout *pipelineLayout) override;
        void setRaytracingPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override;
        void setRaytracingDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override;
        void setIndexBuffer(const RenderIndexBufferView *view) override;
        void setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView *views, uint32_t viewCount, const RenderInputSlot *inputSlots) override;
        void setViewports(const RenderViewport *viewports, uint32_t count) override;
        void setScissors(const RenderRect *scissorRects, uint32_t count) override;
        void setFramebuffer(const RenderFramebuffer *framebuffer) override;
        void setDepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias) override;
        void clearColor(uint32_t attachmentIndex, RenderColor colorValue, const RenderRect *clearRects, uint32_t clearRectsCount) override;
        void clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue, uint32_t stencilValue, const RenderRect *clearRects, uint32_t clearRectsCount) override;
        void copyBufferRegion(RenderBufferReference dstBuffer, RenderBufferReference srcBuffer, uint64_t size) override;
        void copyTextureRegion(const RenderTextureCopyLocation &dstLocation, const RenderTextureCopyLocation &srcLocation, uint32_t dstX, uint32_t dstY, uint32_t dstZ, const RenderBox *srcBox) override;
        void copyBuffer(const RenderBuffer *dstBuffer, const RenderBuffer *srcBuffer) override;
        void copyTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) override;
        void resolveTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) override;
        void resolveTextureRegion(const RenderTexture *dstTexture, uint32_t dstX, uint32_t dstY, const RenderTexture *srcTexture, const RenderRect *srcRect, RenderResolveMode resolveMode) override;
        void buildBottomLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, const RenderBottomLevelASBuildInfo &buildInfo) override;
        void buildTopLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, RenderBufferReference instancesBuffer, const RenderTopLevelASBuildInfo &buildInfo) override;
        void discardTexture(const RenderTexture *texture) override;
        void resetQueryPool(const RenderQueryPool *queryPool, uint32_t queryFirstIndex, uint32_t queryCount) override;
        void writeTimestamp(const RenderQueryPool *queryPool, uint32_t queryIndex) override;

        D3D11Command &emit(D3D11Command::Op op);
        uint32_t store(const void *bytes, size_t size);
    };

    struct D3D11SwapChain : RenderSwapChain {
        D3D11CommandQueue *queue = nullptr;
        RenderSwapChainDesc desc;
        IDXGISwapChain1 *d3d = nullptr;
        std::vector<std::unique_ptr<D3D11Texture>> textures;
        DXGI_FORMAT nativeFormat = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0;
        uint32_t height = 0;
        bool vsyncEnabled = true;
        bool flipModel = false;
        UINT swapChainFlags = 0;
        uint32_t nextIndex = 0;
        HANDLE waitableObject = nullptr;

        D3D11SwapChain(D3D11CommandQueue *queue, const RenderSwapChainDesc &desc);
        ~D3D11SwapChain() override;
        bool present(uint32_t textureIndex, RenderCommandSemaphore **waitSemaphores, uint32_t waitSemaphoreCount) override;
        void wait() override;
        bool resize() override;
        bool needsResize() const override;
        void setVsyncEnabled(bool vsyncEnabled) override;
        bool isVsyncEnabled() const override;
        uint32_t getWidth() const override;
        uint32_t getHeight() const override;
        RenderTexture *getTexture(uint32_t textureIndex) override;
        uint32_t getTextureCount() const override;
        bool acquireTexture(RenderCommandSemaphore *signalSemaphore, uint32_t *textureIndex) override;
        RenderWindow getWindow() const override;
        bool isEmpty() const override;
        uint32_t getRefreshRate() const override;
        void getWindowSize(uint32_t &dstWidth, uint32_t &dstHeight) const;
        void setTextures();
    };

    struct D3D11CommandQueue : RenderCommandQueue {
        D3D11Device *device = nullptr;
        RenderCommandListType type = RenderCommandListType::UNKNOWN;

        D3D11CommandQueue(D3D11Device *device, RenderCommandListType type);
        ~D3D11CommandQueue() override;
        std::unique_ptr<RenderCommandList> createCommandList() override;
        std::unique_ptr<RenderSwapChain> createSwapChain(const RenderSwapChainDesc &desc) override;
        void executeCommandLists(const RenderCommandList **commandLists, uint32_t commandListCount, RenderCommandSemaphore **waitSemaphores, uint32_t waitSemaphoreCount, RenderCommandSemaphore **signalSemaphores, uint32_t signalSemaphoreCount, RenderCommandFence *signalFence) override;
        void waitForCommandFence(RenderCommandFence *fence) override;
    };

    struct D3D11Pool : RenderPool {
        D3D11Device *device = nullptr;
        D3D11Pool(D3D11Device *device) : device(device) {}
        std::unique_ptr<RenderBuffer> createBuffer(const RenderBufferDesc &desc) override;
        std::unique_ptr<RenderTexture> createTexture(const RenderTextureDesc &desc) override;
    };

    // The replay's state: what the commands set, and what the immediate context has bound.
    struct D3D11Replay {
        // Set by the commands.
        const D3D11PipelineLayout *graphicsLayout = nullptr;
        const D3D11PipelineLayout *computeLayout = nullptr;
        const D3D11GraphicsPipeline *graphicsPipeline = nullptr;
        const D3D11ComputePipeline *computePipeline = nullptr;
        D3D11DescriptorSet *graphicsSets[8] = {};
        D3D11DescriptorSet *computeSets[8] = {};
        RenderBufferReference roots[8];
        uint8_t graphicsPush[4][256] = {};
        uint8_t computePush[4][256] = {};
        bool graphicsPushDirty[4] = {};
        bool computePushDirty[4] = {};
        struct Vertex {
            const D3D11Buffer *buffer = nullptr;
            uint64_t offset = 0;
            uint32_t size = 0;
            uint32_t stride = 0;
        };
        Vertex vertex[16];
        uint32_t vertexDirty = 0;
        const D3D11Buffer *indexBuffer = nullptr;
        uint64_t indexOffset = 0;
        uint32_t indexSize = 0;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT;
        bool indexDirty = false;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        uint32_t viewportCount = 0;
        bool viewportsDirty = false;
        D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        uint32_t scissorCount = 0;
        bool scissorsDirty = false;
        const D3D11Framebuffer *framebuffer = nullptr;
        float blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        bool blendDirty = true;

        // Bound on the immediate context.
        const void *boundShaders[4] = {};
        ID3D11InputLayout *boundInputLayout = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY boundTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ID3D11RasterizerState *boundRasterizer = nullptr;
        ID3D11DepthStencilState *boundDepthStencil = nullptr;
        uint32_t boundStencilRef = 0;
        ID3D11BlendState *boundBlend = nullptr;
        ID3D11RenderTargetView *boundRtvs[8] = {};
        uint32_t boundRtvCount = 0;
        ID3D11DepthStencilView *boundDsv = nullptr;
        ID3D11UnorderedAccessView *boundGraphicsUav = nullptr;
        uint32_t boundGraphicsUavSlot = 0;
        const D3D11Framebuffer *boundFramebuffer = nullptr;
        bool outputsValid = false;
        // Color targets drawn into a proxy of the framebuffer's size (D3D11Framebuffer::mismatched), copied back by
        // flushProxiesLocked.
        struct Proxy {
            ID3D11Resource *real = nullptr;
            UINT subresource = 0;
            ID3D11Texture2D *proxy = nullptr;
            UINT width = 0;
            UINT height = 0;
        };
        std::vector<Proxy> proxies;
        ID3D11ShaderResourceView *boundSrvs[4][128] = {};
        ID3D11SamplerState *boundSamplers[4][16] = {};
        ID3D11Buffer *boundCbs[4][14] = {};
        uint32_t boundCbFirst[4][14] = {};
        uint32_t boundCbCount[4][14] = {};
        ID3D11UnorderedAccessView *boundComputeUavs[8] = {};
        ID3D11Buffer *boundVertexBuffers[16] = {};
        UINT boundVertexOffsets[16] = {};
        UINT boundVertexStrides[16] = {};
        ID3D11Buffer *boundIndexBuffer = nullptr;
        UINT boundIndexOffset = 0;
        DXGI_FORMAT boundIndexFormat = DXGI_FORMAT_UNKNOWN;
        bool computeLast = false;  // the last work was a dispatch (graphics bindings are rebound)

        // The constant buffers copied from UPLOAD buffers: per root descriptor (each stage that declares its register
        // binds the same buffer) and the extras per stage.
        struct RootCopy {
            ID3D11Buffer *buffer = nullptr;
            uint32_t capacity = 0;
            const D3D11Buffer *source = nullptr;
            uint64_t offset = 0;
            uint32_t size = 0;
        };
        RootCopy rootCopies[8];
        ID3D11Buffer *pushBuffers[2][4] = {};  // graphics, compute
        uint32_t pushCapacity[2][4] = {};
        ID3D11Buffer *extrasBuffers[4] = {};
        uint32_t extras[4][132] = {};
        bool extrasValid[4] = {};
    };

    struct D3D11Device : RenderDevice {
        D3D11Interface *renderInterface = nullptr;
        IDXGIAdapter1 *adapter = nullptr;
        ID3D11Device *d3d = nullptr;
        ID3D11Device1 *d3d1 = nullptr;
        ID3D11DeviceContext *context = nullptr;
        ID3D11DeviceContext1 *context1 = nullptr;
        ID3D11Device5 *d3d5 = nullptr;
        ID3D11DeviceContext4 *context4 = nullptr;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        RenderDeviceCapabilities capabilities;
        RenderDeviceDescription description;
        std::recursive_mutex contextMutex;
        D3D11Replay replay;
        // UPLOAD buffers with write-watched memory, read for changed pages before each submission.
        std::mutex uploadMutex;
        std::vector<D3D11Buffer *> uploadBuffers;
        std::vector<void *> watchScratch;
        // Scratch textures for region resolves, by format and size.
        std::map<uint64_t, ID3D11Texture2D *> resolveScratch;
        // Proxies of color targets (D3D11Framebuffer::mismatched), by format, samples, size and slot, and their views.
        std::map<uint64_t, ID3D11Texture2D *> proxyTextures;
        std::map<std::pair<ID3D11Texture2D *, DXGI_FORMAT>, ID3D11RenderTargetView *> proxyRtvs;
        // Depth clears of rectangles: a pipeline of the device's own.
        ID3D11VertexShader *clearVs = nullptr;
        ID3D11PixelShader *clearPs = nullptr;
        ID3D11DepthStencilState *clearDepthStates[4] = {};
        ID3D11RasterizerState *clearRasterizer = nullptr;
        ID3D11BlendState *clearBlend = nullptr;
        ID3D11Buffer *clearConstants = nullptr;
        bool clearShadersTried = false;
        // PLUME_D3D11_CHECKS=1: the replay logs views it could not bind and views of what the outputs write.
        bool checks = false;

        D3D11Device(D3D11Interface *renderInterface, const std::string &preferredDeviceName);
        ~D3D11Device() override;
        std::unique_ptr<RenderDescriptorSet> createDescriptorSet(const RenderDescriptorSetDesc &desc) override;
        std::unique_ptr<RenderShader> createShader(const void *data, uint64_t size, const char *entryPointName, RenderShaderFormat format) override;
        std::unique_ptr<RenderSampler> createSampler(const RenderSamplerDesc &desc) override;
        std::unique_ptr<RenderPipeline> createComputePipeline(const RenderComputePipelineDesc &desc) override;
        std::unique_ptr<RenderPipeline> createGraphicsPipeline(const RenderGraphicsPipelineDesc &desc) override;
        std::unique_ptr<RenderPipeline> createRaytracingPipeline(const RenderRaytracingPipelineDesc &desc, const RenderPipeline *previousPipeline) override;
        std::unique_ptr<RenderCommandQueue> createCommandQueue(RenderCommandListType type) override;
        std::unique_ptr<RenderBuffer> createBuffer(const RenderBufferDesc &desc) override;
        std::unique_ptr<RenderTexture> createTexture(const RenderTextureDesc &desc) override;
        std::unique_ptr<RenderAccelerationStructure> createAccelerationStructure(const RenderAccelerationStructureDesc &desc) override;
        std::unique_ptr<RenderPool> createPool(const RenderPoolDesc &desc) override;
        std::unique_ptr<RenderPipelineLayout> createPipelineLayout(const RenderPipelineLayoutDesc &desc) override;
        std::unique_ptr<RenderCommandFence> createCommandFence() override;
        std::unique_ptr<RenderCommandSemaphore> createCommandSemaphore() override;
        std::unique_ptr<RenderFramebuffer> createFramebuffer(const RenderFramebufferDesc &desc) override;
        std::unique_ptr<RenderQueryPool> createQueryPool(uint32_t queryCount) override;
        void setBottomLevelASBuildInfo(RenderBottomLevelASBuildInfo &buildInfo, const RenderBottomLevelASMesh *meshes, uint32_t meshCount, bool preferFastBuild, bool preferFastTrace) override;
        void setTopLevelASBuildInfo(RenderTopLevelASBuildInfo &buildInfo, const RenderTopLevelASInstance *instances, uint32_t instanceCount, bool preferFastBuild, bool preferFastTrace) override;
        void setShaderBindingTableInfo(RenderShaderBindingTableInfo &tableInfo, const RenderShaderBindingGroups &groups, const RenderPipeline *pipeline, RenderDescriptorSet **descriptorSets, uint32_t descriptorSetCount) override;
        const RenderDeviceCapabilities &getCapabilities() const override;
        const RenderDeviceDescription &getDescription() const override;
        RenderSampleCounts getSampleCountsSupported(RenderFormat format) const override;
        bool beginCapture() override;
        bool endCapture() override;
        bool isValid() const;

        // The replay (immediate context, under contextMutex).
        void collectWrittenPages();
        void execute(const D3D11CommandList *list);
        void resetReplayLocked();
        void flushGraphicsLocked();
        void flushComputeLocked();
        void bindOutputsLocked(ID3D11UnorderedAccessView *uav, uint32_t uavSlot);
        void flushProxiesLocked();
        ID3D11RenderTargetView *proxyTargetLocked(const D3D11Framebuffer *framebuffer, uint32_t index, uint32_t width, uint32_t height, ID3D11Texture2D **proxy);
        void unbindOutputsLocked();
        void bindStageResourcesLocked(uint32_t stage, const D3D11ShaderInfo &info, const D3D11PipelineLayout *layout, D3D11DescriptorSet *const *sets, bool compute);
        const uint8_t *rootCpuData(uint32_t rootIndex, uint32_t bytes) const;
        const D3D11Descriptor *guestTexture(uint32_t reg, const uint32_t *words, const D3D11ShaderInfo &info, D3D11DescriptorSet *const *sets, const D3D11GuestBinding &guest) const;
        const D3D11Descriptor *tableDescriptor(D3D11TableBinding::Kind kind, uint32_t reg, const D3D11PipelineLayout *layout, D3D11DescriptorSet *const *sets, bool compute) const;
        const D3D11Descriptor *directDescriptor(int rangeClass, uint32_t reg, const D3D11PipelineLayout *layout, D3D11DescriptorSet *const *sets) const;
        void invalidateBindingsLocked(bool srvs, bool inputs, bool constants);
        ID3D11Buffer *rootConstantBufferLocked(uint32_t rootIndex, uint32_t bytes);
        ID3D11Buffer *pushConstantBufferLocked(bool compute, uint32_t rangeIndex, const D3D11PipelineLayout *layout);
        void setStageConstantBuffer(uint32_t stage, uint32_t reg, ID3D11Buffer *buffer, uint32_t first, uint32_t count);
        void setStageSrvs(uint32_t stage, ID3D11ShaderResourceView *const *srvs, const uint64_t mask[2]);
        void setStageSamplers(uint32_t stage, ID3D11SamplerState *const *samplers, uint32_t mask);
        void copyBufferRegionLocked(const D3D11Buffer *dst, uint64_t dstOffset, const D3D11Buffer *src, uint64_t srcOffset, uint64_t size);
        void copyTextureRegionLocked(const RenderTextureCopyLocation &dst, const RenderTextureCopyLocation &src, uint32_t dstX, uint32_t dstY, uint32_t dstZ, const RenderBox *srcBox);
        void resolveRegionLocked(const D3D11Texture *dst, uint32_t dstX, uint32_t dstY, const D3D11Texture *src, const RenderRect *rect);
        void clearDepthRectsLocked(bool clearDepth, bool clearStencil, float depth, uint32_t stencil, const RenderRect *rects, uint32_t count);
        bool createClearShadersLocked();
    };
};
