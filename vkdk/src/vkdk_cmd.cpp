// vkdk: command pools and buffers, every vkCmd* bo2 records, and the queue. Copyright 2026 aks796. GPL-3.0.
//
// A Vulkan command buffer is a DkCmdBuf fed from its own 1 MB command memory blocks (kept and reused from the first on
// every begin: the application waited for the buffer's last submission, as Vulkan requires). deko3d writes Maxwell's
// methods as each call is made, so the work here is deciding what changed: draws bind only the shader stages, state
// groups, constant buffers and texture handles that differ from what the buffer already bound (renut-nx's plume deko3d
// rules, docs/deko3d-renderer.md sections 15 and 4.6g there).
#include "vkdk_internal.h"

#include <map>
#include <mutex>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <algorithm>
#include <cmath>

namespace vkdk {

namespace {

constexpr uint32_t kCommandBlock = 1u << 20;
constexpr uint32_t kTransientBlock = 256u << 10;

std::atomic<uint64_t> g_draws{0}, g_dispatches{0}, g_pipelineBinds{0}, g_stateBinds{0}, g_textureBinds{0},
    g_uboBinds{0}, g_barriers{0}, g_submits{0}, g_commandBytes{0};
// Drawn copies, image copies left to the copy engine, barriers made Primitives instead of Full (log only).
std::atomic<uint64_t> g_drawnCopies{0}, g_engineImageCopies{0}, g_leanBarriers{0}, g_volumeCopies{0};

inline void Bump(std::atomic<uint64_t>& c, uint64_t n = 1) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

uint32_t FloatBits(float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof(u));
  return u;
}

void AddMemory(void* user, DkCmdBuf, size_t minimum) {
  VkCommandBuffer cb = static_cast<VkCommandBuffer>(user);
  const uint32_t want = AlignUp(uint32_t(std::max<size_t>(minimum, kCommandBlock)), DK_MEMBLOCK_ALIGNMENT);
  while (cb->memoryUsed < cb->memory.size() && cb->memory[cb->memoryUsed].second < want) {
    cb->memoryUsed++;
  }
  if (cb->memoryUsed == cb->memory.size()) {
    DkMemBlock block =
        cb->device->MakeBlock(want, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "command memory");
    if (!block) {
      // deko3d has no way back from here: it writes past its memory. Logged, then the abort is deko3d's.
      Log("command memory: out of memory");
      return;
    }
    cb->memory.push_back({block, want});
    Bump(g_commandBytes, want);
  }
  dkCmdBufAddMemory(cb->cmd, cb->memory[cb->memoryUsed].first, 0, cb->memory[cb->memoryUsed].second);
  cb->memoryUsed++;
}

bool TransientAlloc(VkCommandBuffer cb, uint32_t size, void** cpu, DkGpuAddr* gpu) {
  size = AlignUp(size, DK_UNIFORM_BUF_ALIGNMENT);
  for (;;) {
    if (cb->transientBlock < cb->transient.size()) {
      auto& t = cb->transient[cb->transientBlock];
      const uint32_t at = AlignUp(cb->transientUsed, DK_UNIFORM_BUF_ALIGNMENT);
      if (at + size <= t.second) {
        *cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(t.first)) + at;
        *gpu = dkMemBlockGetGpuAddr(t.first) + at;
        cb->transientUsed = at + size;
        return true;
      }
      cb->transientBlock++;
      cb->transientUsed = 0;
      continue;
    }
    DkMemBlock block = cb->device->MakeBlock(kTransientBlock, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                             "transient data");
    if (!block) {
      return false;
    }
    cb->transient.push_back({block, kTransientBlock});
  }
}

// Barriers are held until the next command that does work and merged with any recorded meanwhile. A barrier that is
// Full only because of a transfer stage, with no copy-engine or 2D work since the last Full one and none about to be
// recorded (copyEngine), is Primitives: the transfers on its sides were drawn copies (DrawnCopy), 3D work.
inline void FlushBarriers(VkCommandBuffer cb, bool copyEngine = false) {
  // Round m diagnostics (sdmc:/switch/sdt_nx/vkdk_paranoid): before every draw, dispatch, copy and clear, the whole
  // GPU drained and every cache it keeps dropped, whatever the barriers asked for. A glitch that stays is no missing
  // barrier or invalidation inside a command buffer.
  static const bool paranoid = DebugSwitch("vkdk_paranoid", false);
  if (paranoid) {
    cb->pendingBarrier = true;
    cb->pendingMode = cb->pendingLean = DkBarrier_Full;
    cb->pendingInvalidate |= DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors |
                             DkInvalidateFlags_Zcull | DkInvalidateFlags_L2Cache;
  }
  if (cb->pendingBarrier) {
    DkBarrier mode = cb->pendingMode;
    static const bool fullBarriers = DebugSwitch("vkdk_fullbarriers", true);  // round k diagnostics
    if (fullBarriers && mode != DkBarrier_None) {
      mode = DkBarrier_Full;
    }
    if (!fullBarriers && mode == DkBarrier_Full && cb->pendingLean < DkBarrier_Full && !cb->copyEngineSince &&
        !copyEngine) {
      mode = cb->pendingLean;
      Bump(g_leanBarriers);
    }
    dkCmdBufBarrier(cb->cmd, mode, cb->pendingInvalidate);
    if (mode == DkBarrier_Full) {
      cb->copyEngineSince = false;
    }
    cb->pendingBarrier = false;
    cb->pendingMode = cb->pendingLean = DkBarrier_None;
    cb->pendingInvalidate = 0;
    Bump(g_barriers);
  }
}

// Before and after recording copy-engine or 2D-engine work.
inline void BeginEngineCopy(VkCommandBuffer cb) {
  FlushBarriers(cb, true);
}
inline void EndEngineCopy(VkCommandBuffer cb) {
  cb->copyEngineSince = true;
}

// Descriptors written or shader code loaded since this buffer last looked: the GPU's caches of them dropped.
inline void FlushEpochs(VkCommandBuffer cb) {
  VkDevice d = cb->device;
  const uint64_t de = d->descriptorEpoch.load(std::memory_order_acquire);
  const uint64_t ce = d->codeEpoch.load(std::memory_order_acquire);
  if (de != cb->seenDescriptorEpoch || ce != cb->seenCodeEpoch) {
    uint32_t flags = 0;
    if (de != cb->seenDescriptorEpoch) flags |= DkInvalidateFlags_Descriptors;
    if (ce != cb->seenCodeEpoch) flags |= DkInvalidateFlags_Shader;
    FlushBarriers(cb);
    dkCmdBufBarrier(cb->cmd, DkBarrier_None, flags);
    cb->seenDescriptorEpoch = de;
    cb->seenCodeEpoch = ce;
  }
}

void ApplyScissors(VkCommandBuffer cb) {
  if (cb->scissorCount) {
    dkCmdBufSetScissors(cb->cmd, 0, cb->scissors, cb->scissorCount);
  } else {
    const DkScissor all = {0, 0, 16384, 16384};
    dkCmdBufSetScissors(cb->cmd, 0, &all, 1);
  }
}

// Vulkan's viewport (clip space y down) on deko3d's upper-left origin, whose own transform is D3D's (y up): a positive
// height is given to deko3d as it is and its ScaleY (method 0x281 + 8 * i) written as +h/2 afterwards; a negative one
// (VK_KHR_maintenance1's flip) is deko3d's own orientation, given as the positive viewport it covers.
void SetViewports(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport* vps) {
  count = std::min<uint32_t>(count, DK_NUM_VIEWPORTS - first);
  for (uint32_t i = 0; i < count; ++i) {
    const VkViewport& v = vps[i];
    DkViewport& d = cb->viewports[first + i];
    d.x = v.x;
    d.width = v.width;
    d.y = v.height < 0.0f ? v.y + v.height : v.y;
    d.height = std::fabs(v.height);
    d.near = v.minDepth;
    d.far = v.maxDepth;
    cb->vkViewports[first + i] = v;
  }
  cb->viewportCount = std::max(cb->viewportCount, first + count);
  FlushBarriers(cb);
  dkCmdBufSetViewports(cb->cmd, first, cb->viewports + first, count);
  for (uint32_t i = 0; i < count; ++i) {
    if (vps[i].height >= 0.0f) {
      const uint32_t words[2] = {(1u << 29) | (1u << 16) | (0x281u + 8u * (first + i)),
                                 FloatBits(0.5f * cb->viewports[first + i].height)};
      dkCmdBufReplayCmds(cb->cmd, words, 2);
    }
  }
}

void SetScissors(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D* rects) {
  count = std::min<uint32_t>(count, DK_NUM_SCISSORS - first);
  for (uint32_t i = 0; i < count; ++i) {
    const VkRect2D& r = rects[i];
    DkScissor& s = cb->scissors[first + i];
    s.x = uint32_t(std::max(r.offset.x, 0));
    s.y = uint32_t(std::max(r.offset.y, 0));
    s.width = r.extent.width;
    s.height = r.extent.height;
  }
  cb->scissorCount = std::max(cb->scissorCount, first + count);
  dkCmdBufSetScissors(cb->cmd, first, cb->scissors + first, count);
}

void BindPipelineState(VkCommandBuffer cb, const VkPipeline_T* p) {
  DkCmdBuf cmd = cb->cmd;
  // Shaders: only the stages that change.
  const Variant* want[2] = {p->stages[0], p->stages[1]};
  if (want[0] != cb->boundShaders[0] || want[1] != cb->boundShaders[1]) {
    const DkShader* list[2];
    uint32_t n = 0, mask = 0;
    for (int s = 0; s < 2; ++s) {
      if (want[s] != cb->boundShaders[s]) {
        mask |= s ? DkStageFlag_Fragment : DkStageFlag_Vertex;
        if (want[s]) list[n++] = &want[s]->shader;
      }
    }
    dkCmdBufBindShaders(cmd, mask, list, n);
    cb->boundShaders[0] = want[0];
    cb->boundShaders[1] = want[1];
    // A new program reads its stage's bindings, which stay; the texture handles a stage holds stay valid too.
  }
  auto changed = [&](uint32_t g) {
    if (cb->boundGroups[g] == p->groupHash[g]) return false;
    cb->boundGroups[g] = p->groupHash[g];
    return true;
  };
  bool any = false;
  if (changed(0)) {
    dkCmdBufBindRasterizerState(cmd, &p->rasterizer);
    any = true;
  }
  if (changed(1)) {
    dkCmdBufBindMultisampleState(cmd, &p->multisample);
    any = true;
  }
  if (changed(2)) {
    dkCmdBufBindColorState(cmd, &p->color);
    any = true;
  }
  if (changed(3)) {
    dkCmdBufBindColorWriteState(cmd, &p->colorWrite);
    any = true;
  }
  if (p->blendCount && changed(4)) {
    dkCmdBufBindBlendStates(cmd, 0, p->blend, p->blendCount);
    for (uint32_t i = 0; i < p->blendCount; ++i) {
      dkCmdBufReplayCmds(cmd, p->blendWords[i], 7);
    }
    any = true;
  }
  if (changed(5)) {
    dkCmdBufBindDepthStencilState(cmd, &p->depthStencil);
    any = true;
  }
  if (p->depthStencil.stencilTestEnable && !(p->dynamic & ((1u << 6) | (1u << 7) | (1u << 8))) && changed(6)) {
    dkCmdBufSetStencil(cmd, DkFace_Front, p->stencilFront[0], p->stencilFront[1], p->stencilFront[2]);
    dkCmdBufSetStencil(cmd, DkFace_Back, p->stencilBack[0], p->stencilBack[1], p->stencilBack[2]);
  }
  // Always when it differs, also with no input: deko3d makes the attributes after the last one constants and disables
  // the buffers after the last one, so no attribute keeps fetching from a buffer that may be gone.
  if (changed(7)) {
    dkCmdBufBindVtxAttribState(cmd, p->attribs, p->attribCount);
    dkCmdBufBindVtxBufferState(cmd, p->vtxBuffers, p->vtxBufferCount);
    cb->vtxDirty = (1u << DK_MAX_VERTEX_BUFFERS) - 1;
    any = true;
  }
  if (!(p->dynamic & (1u << 3)) && p->rasterizer.depthBiasEnableMask) {
    dkCmdBufSetDepthBias(cmd, p->depthBias[0], p->depthBias[1], p->depthBias[2]);
  }
  if (!(p->dynamic & (1u << 4)) && p->color.blendEnableMask) {
    dkCmdBufSetBlendConst(cmd, p->blendConstants[0], p->blendConstants[1], p->blendConstants[2],
                          p->blendConstants[3]);
  }
  if (p->hasStaticViewport) {
    SetViewports(cb, 0, 1, &p->staticViewport);
  }
  if (p->hasStaticScissor) {
    SetScissors(cb, 0, 1, &p->staticScissor);
  }
  if (p->sampleMask != 0xFFFFFFFF) {
    dkCmdBufSetSampleMask(cmd, p->sampleMask);
  }
  if (p->patchSize) {
    dkCmdBufSetPatchSize(cmd, p->patchSize);
  }
  if (any) {
    Bump(g_stateBinds);
  }
}

// The UBOs, storage buffers and texture handles one stage's shader reads, from the bound descriptor sets.
void BindStageResources(VkCommandBuffer cb, int bindPoint, int stageIndex, DkStage stage, const Variant* v) {
  const ShaderMeta& meta = *v->meta;
  const BoundSet* sets = cb->sets[bindPoint];
  DkCmdBuf cmd = cb->cmd;
  DkBufExtents* boundUbo = cb->boundUbo[stageIndex];
  for (const ShaderMeta::Ubo& u : meta.ubos) {
    if (u.ref.set >= 8 || !sets[u.ref.set].set) continue;
    const VkDescriptorSet_T* s = sets[u.ref.set].set;
    if (u.ref.binding >= s->layout.bindings.size()) continue;
    const auto& b = s->layout.bindings[u.ref.binding];
    DkBufExtents e = s->buffers[b.buffer + u.ref.element];
    if (b.dynamic >= 0) {
      e.addr += sets[u.ref.set].dynamicOffsets[b.dynamic + u.ref.element];
    }
    e.size = std::min<uint32_t>(e.size, DK_UNIFORM_BUF_MAX_SIZE);
    if (stage == DkStage_Compute || boundUbo[u.slot].addr != e.addr || boundUbo[u.slot].size != e.size) {
      dkCmdBufBindUniformBuffer(cmd, stage, u.slot, e.addr, e.size);
      if (stage != DkStage_Compute) boundUbo[u.slot] = e;
      Bump(g_uboBinds);
    }
  }
  DkBufExtents* boundSsbo = cb->boundSsbo[stageIndex];
  for (const ShaderMeta::Ubo& u : meta.ssbos) {
    if (u.ref.set >= 8 || !sets[u.ref.set].set) continue;
    const VkDescriptorSet_T* s = sets[u.ref.set].set;
    if (u.ref.binding >= s->layout.bindings.size()) continue;
    const auto& b = s->layout.bindings[u.ref.binding];
    DkBufExtents e = s->buffers[b.buffer + u.ref.element];
    if (b.dynamic >= 0) {
      e.addr += sets[u.ref.set].dynamicOffsets[b.dynamic + u.ref.element];
    }
    if (stage == DkStage_Compute || boundSsbo[u.slot].addr != e.addr || boundSsbo[u.slot].size != e.size) {
      dkCmdBufBindStorageBuffer(cmd, stage, u.slot, e.addr, e.size);
      if (stage != DkStage_Compute) boundSsbo[u.slot] = e;
    }
  }
  // Storage images (Dekopon-32: the PICA shadow buffer, compute copies): rebound with every draw that reads them.
  for (const ShaderMeta::Ubo& u : meta.images) {
    if (u.ref.set >= 8 || !sets[u.ref.set].set) continue;
    const VkDescriptorSet_T* s = sets[u.ref.set].set;
    if (u.ref.binding >= s->layout.bindings.size()) continue;
    const uint32_t image = s->imageFirst + s->layout.bindings[u.ref.binding].image + u.ref.element;
    dkCmdBufBindImage(cmd, stage, u.slot, dkMakeImageHandle(image));
  }
  if (!meta.textures.empty()) {
    DkResHandle handles[32];
    uint32_t lo = 32, hi = 0;
    DkResHandle* bound = cb->boundTex[stageIndex];
    for (const ShaderMeta::Tex& t : meta.textures) {
      uint32_t image = 0, sampler = 0;
      if (t.image.set < 8 && sets[t.image.set].set) {
        const VkDescriptorSet_T* s = sets[t.image.set].set;
        if (t.image.binding < s->layout.bindings.size()) {
          image = s->imageFirst + s->layout.bindings[t.image.binding].image + t.image.element;
        }
      }
      if (t.sampler.set < 8 && sets[t.sampler.set].set) {
        const VkDescriptorSet_T* s = sets[t.sampler.set].set;
        if (t.sampler.binding < s->layout.bindings.size()) {
          const uint32_t at = s->layout.bindings[t.sampler.binding].sampler + t.sampler.element;
          sampler = at < s->samplerSlots.size() ? s->samplerSlots[at] : cb->device->defaultSamplerSlot;
        }
      }
      const DkResHandle h = dkMakeTextureHandle(image, sampler);
      handles[t.slot] = h;
      if (stage == DkStage_Compute || bound[t.slot] != h) {
        lo = std::min<uint32_t>(lo, t.slot);
        hi = std::max<uint32_t>(hi, t.slot + 1u);
      }
    }
    if (lo < hi) {
      // The slots in between that the shader does not read keep what they had.
      for (uint32_t k = lo; k < hi; ++k) {
        bool used = false;
        for (const ShaderMeta::Tex& t : meta.textures) used |= t.slot == k;
        if (!used) handles[k] = bound[k];
      }
      dkCmdBufBindTextures(cmd, stage, lo, handles + lo, hi - lo);
      if (stage != DkStage_Compute) {
        memcpy(bound + lo, handles + lo, (hi - lo) * sizeof(DkResHandle));
      }
      Bump(g_textureBinds);
    }
  }
}

bool PrepareDraw(VkCommandBuffer cb) {
  const VkPipeline_T* p = cb->graphics;
  if (!p) {
    return false;
  }
  FlushBarriers(cb);
  FlushEpochs(cb);
  if (cb->boundStatePipeline != p) {
    BindPipelineState(cb, p);
    cb->boundStatePipeline = p;
    cb->setsDirty[0] = ~0u;
  }
  // The push constants' UBO, written in the command stream (pipelined) when they changed.
  if (cb->pushDirty && cb->pushSize) {
    dkCmdBufPushConstants(cb->cmd, cb->device->pushGpu, kPushBytes, 0, AlignUp(cb->pushSize, 4), cb->push);
    cb->pushDirty = false;
  }
  // Constant buffers move with every draw's dynamic offsets: resolved each draw, sent only when they differ.
  for (int s = 0; s < 2; ++s) {
    if (const Variant* v = p->stages[s]) {
      BindStageResources(cb, 0, s, s ? DkStage_Fragment : DkStage_Vertex, v);
      if (v->meta->push) {
        DkBufExtents& b = cb->boundUbo[s][kPushUboSlot];
        if (b.addr != cb->device->pushGpu) {
          dkCmdBufBindUniformBuffer(cb->cmd, s ? DkStage_Fragment : DkStage_Vertex, kPushUboSlot,
                                    cb->device->pushGpu, kPushBytes);
          b = {cb->device->pushGpu, kPushBytes};
        }
      }
    }
  }
  cb->setsDirty[0] = 0;
  if (cb->vtxDirty) {
    for (uint32_t i = 0; i < p->vtxBufferCount; ++i) {
      if ((cb->vtxDirty >> i) & 1u) {
        dkCmdBufBindVtxBuffer(cb->cmd, i, cb->vtx[i].addr, cb->vtx[i].size);
      }
    }
    cb->vtxDirty = 0;
  }
  return true;
}

}  // namespace

void ResetCommandBufferState(VkCommandBuffer cb) {
  cb->graphics = cb->compute = nullptr;
  cb->boundStatePipeline = nullptr;
  memset(cb->boundGroups, 0, sizeof(cb->boundGroups));
  cb->boundShaders[0] = cb->boundShaders[1] = nullptr;
  for (auto& bp : cb->sets) {
    for (auto& s : bp) s = BoundSet{};
  }
  cb->setsDirty[0] = cb->setsDirty[1] = ~0u;
  cb->pushSize = 0;
  cb->pushDirty = false;
  memset(cb->boundUbo, 0, sizeof(cb->boundUbo));
  memset(cb->boundSsbo, 0, sizeof(cb->boundSsbo));
  // A handle no binding holds: the first draw sends every texture.
  memset(cb->boundTex, 0xFF, sizeof(cb->boundTex));
  cb->seenDescriptorEpoch = 0;
  cb->seenCodeEpoch = 0;
  memset(cb->vtx, 0, sizeof(cb->vtx));
  cb->vtxDirty = 0;
  cb->indexAddr = 0;
  cb->indexBound = false;
  for (auto& f : cb->stencil) {
    f[0] = 0xFF;
    f[1] = 0;
    f[2] = 0xFF;
  }
  cb->restartKnown = false;
  cb->baseVertexZero = false;
  cb->viewportCount = cb->scissorCount = 0;
  cb->pass = nullptr;
  cb->framebuffer = nullptr;
  cb->pendingBarrier = false;
  cb->pendingMode = cb->pendingLean = DkBarrier_None;
  cb->pendingInvalidate = 0;
  cb->copyEngineSince = true;
  cb->openQuery = nullptr;
}

void DestroyCommandBuffer(VkCommandBuffer cb) {
  if (cb->cmd) {
    dkCmdBufDestroy(cb->cmd);
  }
  for (auto& m : cb->memory) cb->device->DestroyBlock(m.first);
  for (auto& t : cb->transient) cb->device->DestroyBlock(t.first);
  delete cb;
}

}  // namespace vkdk

using namespace vkdk;

// ---- pools and buffers

static VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo*,
                                                        const VkAllocationCallbacks*, VkCommandPool* out) {
  VKDK_TRACE("CreateCommandPool");
  VkCommandPool p = new (std::nothrow) VkCommandPool_T();
  if (!p) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  p->device = d;
  *out = p;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice, VkCommandPool p, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyCommandPool");
  if (!p) return;
  for (VkCommandBuffer cb : p->buffers) DestroyCommandBuffer(cb);
  delete p;
}

static VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags) {
  VKDK_TRACE("ResetCommandPool");
  // Each buffer rewinds its memory on its next begin.
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo* info,
                                                             VkCommandBuffer* out) {
  VKDK_TRACE("AllocateCommandBuffers");
  for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
    VkCommandBuffer cb = new (std::nothrow) VkCommandBuffer_T();
    if (!cb) return VK_ERROR_OUT_OF_HOST_MEMORY;
    cb->device = d;
    cb->pool = info->commandPool;
    DkCmdBufMaker m;
    dkCmdBufMakerDefaults(&m, d->dk);
    m.userData = cb;
    m.cbAddMem = AddMemory;
    cb->cmd = dkCmdBufCreate(&m);
    cb->pool->buffers.push_back(cb);
    out[i] = cb;
  }
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice, VkCommandPool p, uint32_t count,
                                                     const VkCommandBuffer* cbs) {
  VKDK_TRACE("FreeCommandBuffers");
  for (uint32_t i = 0; i < count; ++i) {
    if (!cbs[i]) continue;
    auto it = std::find(p->buffers.begin(), p->buffers.end(), cbs[i]);
    if (it != p->buffers.end()) p->buffers.erase(it);
    DestroyCommandBuffer(cbs[i]);
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
  VKDK_TRACE("BeginCommandBuffer");
  cb->inheritPass = nullptr;
  cb->inheritFramebuffer = nullptr;
  if (info && (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) && info->pInheritanceInfo) {
    cb->inheritPass = info->pInheritanceInfo->renderPass;
    cb->inheritFramebuffer = info->pInheritanceInfo->framebuffer;
  }
  // deko3d 0.5.0's dkCmdBufClear rewinds into the memory fed last (SwitchWakerHD): the first block is fed explicitly.
  dkCmdBufClear(cb->cmd);
  cb->memoryUsed = 0;
  AddMemory(cb, cb->cmd, 0);
  cb->transientBlock = 0;
  cb->transientUsed = 0;
  cb->list = 0;
  cb->recording = true;
  ResetCommandBufferState(cb);
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer cb) {
  VKDK_TRACE("EndCommandBuffer");
  FlushBarriers(cb);
  cb->list = dkCmdBufFinishList(cb->cmd);
  cb->recording = false;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags) {
  VKDK_TRACE("ResetCommandBuffer");
  cb->list = 0;
  return VK_SUCCESS;
}

// ---- binding

static VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline p) {
  VKDK_TRACE("CmdBindPipeline");
  Bump(g_pipelineBinds);
  if (point == VK_PIPELINE_BIND_POINT_COMPUTE) {
    cb->compute = p;
  } else {
    cb->graphics = p;
  }
}

static VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint point,
                                                        VkPipelineLayout layout, uint32_t first, uint32_t count,
                                                        const VkDescriptorSet* sets, uint32_t dynamicCount,
                                                        const uint32_t* dynamicOffsets) {
  VKDK_TRACE("CmdBindDescriptorSets");
  const int bp = point == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0;
  uint32_t d = 0;
  for (uint32_t i = 0; i < count && first + i < 8; ++i) {
    BoundSet& b = cb->sets[bp][first + i];
    b.set = sets[i];
    const uint32_t n = sets[i] ? sets[i]->layout.dynamics : 0;
    for (uint32_t k = 0; k < n && k < 16 && d < dynamicCount; ++k) {
      b.dynamicOffsets[k] = dynamicOffsets[d++];
    }
    cb->setsDirty[bp] |= 1u << (first + i);
  }
  (void)layout;
}

static VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout, VkShaderStageFlags,
                                                   uint32_t offset, uint32_t size, const void* values) {
  VKDK_TRACE("CmdPushConstants");
  if (offset + size > kPushBytes) {
    return;
  }
  memcpy(cb->push + offset, values, size);
  cb->pushSize = std::max(cb->pushSize, offset + size);
  cb->pushDirty = true;
}

static VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                                       const VkBuffer* buffers, const VkDeviceSize* offsets) {
  VKDK_TRACE("CmdBindVertexBuffers");
  for (uint32_t i = 0; i < count && first + i < DK_MAX_VERTEX_BUFFERS; ++i) {
    DkBufExtents e{0, 0};
    if (buffers[i]) {
      e.addr = buffers[i]->gpu + offsets[i];
      e.size = uint32_t(std::min<uint64_t>(buffers[i]->size - offsets[i], UINT32_MAX));
    }
    if (cb->vtx[first + i].addr != e.addr || cb->vtx[first + i].size != e.size) {
      cb->vtx[first + i] = e;
      cb->vtxDirty |= 1u << (first + i);
    }
  }
}

static VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset,
                                                     VkIndexType type) {
  VKDK_TRACE("CmdBindIndexBuffer");
  const DkGpuAddr addr = buffer->gpu + offset;
  const DkIdxFormat f = type == VK_INDEX_TYPE_UINT32 ? DkIdxFormat_Uint32
                        : type == VK_INDEX_TYPE_UINT8_EXT ? DkIdxFormat_Uint8
                                                          : DkIdxFormat_Uint16;
  if (addr != cb->indexAddr || f != cb->indexFormat || !cb->indexBound) {
    cb->indexAddr = addr;
    cb->indexFormat = f;
    FlushBarriers(cb);
    dkCmdBufBindIdxBuffer(cb->cmd, f, addr);
  }
  cb->indexBound = true;
}

// ---- dynamic state

static VKAPI_ATTR void VKAPI_CALL CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                                 const VkViewport* vps) {
  VKDK_TRACE("CmdSetViewport");
  SetViewports(cb, first, count, vps);
}

static VKAPI_ATTR void VKAPI_CALL CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                                const VkRect2D* rects) {
  VKDK_TRACE("CmdSetScissor");
  FlushBarriers(cb);
  SetScissors(cb, first, count, rects);
}

static VKAPI_ATTR void VKAPI_CALL CmdSetDepthBias(VkCommandBuffer cb, float constant, float clamp, float slope) {
  VKDK_TRACE("CmdSetDepthBias");
  dkCmdBufSetDepthBias(cb->cmd, constant, clamp, slope);
}

static VKAPI_ATTR void VKAPI_CALL CmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
  VKDK_TRACE("CmdSetBlendConstants");
  dkCmdBufSetBlendConst(cb->cmd, c[0], c[1], c[2], c[3]);
}

// Stencil values: deko3d sets write mask, reference and compare mask together per face; the command buffer keeps the
// dynamic ones.
static void StencilValue(VkCommandBuffer cb, VkStencilFaceFlags faces, int which, uint32_t value) {
  for (int f = 0; f < 2; ++f) {
    if (!(faces & (f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT))) continue;
    cb->stencil[f][which] = uint8_t(value);
    dkCmdBufSetStencil(cb->cmd, f ? DkFace_Back : DkFace_Front, cb->stencil[f][0], cb->stencil[f][1],
                       cb->stencil[f][2]);
  }
  cb->boundGroups[6] = 0;
}

static VKAPI_ATTR void VKAPI_CALL CmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) {
  VKDK_TRACE("CmdSetStencilCompareMask");
  StencilValue(cb, f, 2, v);
}

static VKAPI_ATTR void VKAPI_CALL CmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) {
  VKDK_TRACE("CmdSetStencilWriteMask");
  StencilValue(cb, f, 0, v);
}

static VKAPI_ATTR void VKAPI_CALL CmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) {
  VKDK_TRACE("CmdSetStencilReference");
  StencilValue(cb, f, 1, v);
}

// ---- render passes

// The framebuffer sRGB register (Maxwell 0x56E), which deko3d 0.5.0 never writes (SwitchWakerHD).
static void WriteFramebufferSrgb(DkCmdBuf cmd, bool srgb) {
  const uint32_t words[2] = {(1u << 29) | (1u << 16) | 0x56Eu, srgb ? 1u : 0u};
  dkCmdBufReplayCmds(cmd, words, 2);
}

// The targets of one subpass (sdt_nx: subpasses other than the first, for Unity's Android player).
static void BindSubpassTargets(VkCommandBuffer cb, uint32_t index) {
  VkRenderPass pass = cb->pass;
  VkFramebuffer fb = cb->framebuffer;
  const VkRenderPass_T::Subpass& sp = pass->subpasses[index];
  const DkImageView* colors[8];
  uint32_t n = 0;
  bool srgb = false;
  for (uint32_t i = 0; i < sp.colorCount; ++i) {
    const uint32_t ref = sp.colorRefs[i];
    colors[n++] = ref != VK_ATTACHMENT_UNUSED && ref < fb->views.size() ? &fb->views[ref]->target : nullptr;
    if (i == 0 && ref != VK_ATTACHMENT_UNUSED) {
      const VkFormat f = pass->attachments[ref].format;
      srgb = f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB;
    }
  }
  const DkImageView* depth =
      sp.depthRef != VK_ATTACHMENT_UNUSED && sp.depthRef < fb->views.size() ? &fb->views[sp.depthRef]->target : nullptr;
  FlushBarriers(cb);
  dkCmdBufBindRenderTargets(cb->cmd, colors, n, depth);
  WriteFramebufferSrgb(cb->cmd, srgb);
}

// A subpass's multisampled colours into their resolve attachments, at its end.
static void ResolveSubpass(VkCommandBuffer cb) {
  VkRenderPass pass = cb->pass;
  VkFramebuffer fb = cb->framebuffer;
  if (!pass || !fb || cb->subpass >= pass->subpasses.size()) {
    return;
  }
  const VkRenderPass_T::Subpass& sp = pass->subpasses[cb->subpass];
  bool any = false;
  for (uint32_t i = 0; i < sp.colorCount; ++i) {
    const uint32_t src = sp.colorRefs[i], dst = sp.resolveRefs[i];
    if (src == VK_ATTACHMENT_UNUSED || dst == VK_ATTACHMENT_UNUSED || src >= fb->views.size() ||
        dst >= fb->views.size()) {
      continue;
    }
    if (!any) {
      dkCmdBufBarrier(cb->cmd, DkBarrier_Fragments, 0);
      any = true;
    }
    dkCmdBufResolveImage(cb->cmd, &fb->views[src]->target, &fb->views[dst]->target);
  }
  if (any) {
    cb->copyEngineSince = true;
  }
}

static VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* info,
                                                     VkSubpassContents) {
  VKDK_TRACE("CmdBeginRenderPass");
  VkRenderPass pass = info->renderPass;
  VkFramebuffer fb = info->framebuffer;
  cb->pass = pass;
  cb->framebuffer = fb;
  cb->subpass = 0;
  BindSubpassTargets(cb, 0);
  cb->targetWidth = fb->width;
  cb->targetHeight = fb->height;

  // Clears of the render area (deko3d's clears go through scissor 0).
  bool scissored = false;
  const VkRect2D& area = info->renderArea;
  auto scissor = [&] {
    if (!scissored) {
      const DkScissor s = {uint32_t(std::max(area.offset.x, 0)), uint32_t(std::max(area.offset.y, 0)),
                           area.extent.width, area.extent.height};
      dkCmdBufSetScissors(cb->cmd, 0, &s, 1);
      scissored = true;
    }
  };
  for (uint32_t i = 0; i < pass->colorCount; ++i) {
    const uint32_t ref = pass->colorRefs[i];
    if (ref == VK_ATTACHMENT_UNUSED || pass->attachments[ref].load != VK_ATTACHMENT_LOAD_OP_CLEAR ||
        ref >= info->clearValueCount) {
      continue;
    }
    scissor();
    dkCmdBufClearColor(cb->cmd, i, DkColorMask_RGBA, &info->pClearValues[ref].color);
  }
  if (pass->depthRef != VK_ATTACHMENT_UNUSED && pass->depthRef < info->clearValueCount) {
    const auto& a = pass->attachments[pass->depthRef];
    const bool clearDepth = a.depth && a.load == VK_ATTACHMENT_LOAD_OP_CLEAR;
    const bool clearStencil = a.stencil && a.stencilLoad == VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (clearDepth || clearStencil) {
      scissor();
      const VkClearDepthStencilValue& v = info->pClearValues[pass->depthRef].depthStencil;
      dkCmdBufClearDepthStencil(cb->cmd, clearDepth, v.depth, clearStencil ? 0xFF : 0, uint8_t(v.stencil));
    }
  }
  if (scissored) {
    ApplyScissors(cb);
  }
}

static VKAPI_ATTR void VKAPI_CALL CmdNextSubpass(VkCommandBuffer cb, VkSubpassContents) {
  VKDK_TRACE("CmdNextSubpass");
  if (!cb->pass || cb->subpass + 1 >= cb->pass->subpasses.size()) {
    return;
  }
  ResolveSubpass(cb);
  cb->subpass++;
  // the next subpass may read what this one drew
  cb->pendingBarrier = true;
  if (cb->pendingMode < DkBarrier_Fragments) {
    cb->pendingMode = DkBarrier_Fragments;
  }
  cb->pendingInvalidate |= DkInvalidateFlags_Image;
  BindSubpassTargets(cb, cb->subpass);
}

static VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer cb) {
  VKDK_TRACE("CmdEndRenderPass");
  ResolveSubpass(cb);
  cb->pass = nullptr;
  cb->framebuffer = nullptr;
  cb->subpass = 0;
}

static VKAPI_ATTR void VKAPI_CALL CmdClearAttachments(VkCommandBuffer cb, uint32_t count,
                                                      const VkClearAttachment* atts, uint32_t rectCount,
                                                      const VkClearRect* rects) {
  VKDK_TRACE("CmdClearAttachments");
  FlushBarriers(cb);
  for (uint32_t r = 0; r < rectCount; ++r) {
    const VkRect2D& rc = rects[r].rect;
    const DkScissor s = {uint32_t(std::max(rc.offset.x, 0)), uint32_t(std::max(rc.offset.y, 0)), rc.extent.width,
                         rc.extent.height};
    dkCmdBufSetScissors(cb->cmd, 0, &s, 1);
    for (uint32_t i = 0; i < count; ++i) {
      const VkClearAttachment& a = atts[i];
      if (a.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
        dkCmdBufClearColor(cb->cmd, a.colorAttachment, DkColorMask_RGBA, &a.clearValue.color);
      } else {
        const bool d = (a.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        const bool st = (a.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        dkCmdBufClearDepthStencil(cb->cmd, d, a.clearValue.depthStencil.depth, st ? 0xFF : 0,
                                  uint8_t(a.clearValue.depthStencil.stencil));
      }
    }
  }
  ApplyScissors(cb);
}

// ---- draws and dispatches

// deko3d's DrawIndexed macro writes its vertex offset to the base vertex (0x50D, SET_GLOBAL_BASE_VERTEX_INDEX: added to
// the vertex fetched for non-indexed draws as well) and to the vertex id base (0x446, part of gl_VertexIndex); its Draw
// macro leaves both alone. A non-indexed draw after an indexed one with an offset (Azahar draws every indexed PICA batch
// with -lowest index) fetched its attributes that many vertices off and got a shifted gl_VertexIndex: the blit
// helper's full-screen triangle (made from gl_VertexIndex) covered half its target, the next game draw took another
// draw's vertices from the stream buffer. MK7 on the console: dialog panels cut into triangles, the bottom screen's rank
// list in the race view, changing with the draw before (2026-11-13). The register survives across command buffers and
// the present thread, so each buffer's first non-indexed draw sets both to 0, as NVK (every draw) and nouveau do.
// Ryujinx ignores both for non-indexed draws, so Ryubing never showed it.
static void ZeroBaseVertex(VkCommandBuffer cb) {
  if (cb->baseVertexZero) {
    return;
  }
  const uint32_t words[4] = {(1u << 29) | (1u << 16) | 0x50Du, 0u, (1u << 29) | (1u << 16) | 0x446u, 0u};
  dkCmdBufReplayCmds(cb->cmd, words, 4);
  cb->baseVertexZero = true;
}

static VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances,
                                          uint32_t firstVertex, uint32_t firstInstance) {
  VKDK_TRACE("CmdDraw");
  if (!PrepareDraw(cb)) return;
  ZeroBaseVertex(cb);
  dkCmdBufDraw(cb->cmd, cb->graphics->primitive, vertices, instances, firstVertex, firstInstance);
  Bump(g_draws);
  if (g_censusFrames.load(std::memory_order_relaxed) > 0) CensusDraw(cb, instances);
}

static VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cb, uint32_t indices, uint32_t instances,
                                                 uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
  VKDK_TRACE("CmdDrawIndexed");
  if (!PrepareDraw(cb)) return;
  const VkPipeline_T* p = cb->graphics;
  const uint32_t restartIndex = cb->indexFormat == DkIdxFormat_Uint32 ? 0xFFFFFFFFu
                                : cb->indexFormat == DkIdxFormat_Uint16 ? 0xFFFFu
                                                                        : 0xFFu;
  if (!cb->restartKnown || cb->restartBound != p->restart || (p->restart && cb->restartIndex != restartIndex)) {
    dkCmdBufSetPrimitiveRestart(cb->cmd, p->restart, restartIndex);
    cb->restartBound = p->restart;
    cb->restartIndex = restartIndex;
    cb->restartKnown = true;
  }
  dkCmdBufDrawIndexed(cb->cmd, p->primitive, indices, instances, firstIndex, vertexOffset, firstInstance);
  cb->baseVertexZero = vertexOffset == 0;
  if (g_censusFrames.load(std::memory_order_relaxed) > 0) CensusDraw(cb, instances);
  Bump(g_draws);
}

static VKAPI_ATTR void VKAPI_CALL CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
  VKDK_TRACE("CmdDispatch");
  const VkPipeline_T* p = cb->compute;
  if (!p || !p->stages[0]) return;
  FlushBarriers(cb);
  FlushEpochs(cb);
  const DkShader* shader = &p->stages[0]->shader;
  dkCmdBufBindShaders(cb->cmd, DkStageFlag_Compute, &shader, 1);
  // Compute jobs read their uniforms when they run: the push constants go into fresh transient memory per dispatch,
  // not the graphics stages' pipelined UBO.
  if (p->stages[0]->meta->push && cb->pushSize) {
    void* cpu;
    DkGpuAddr gpu;
    if (TransientAlloc(cb, kPushBytes, &cpu, &gpu)) {
      memcpy(cpu, cb->push, cb->pushSize);
      dkCmdBufBindUniformBuffer(cb->cmd, DkStage_Compute, kPushUboSlot, gpu, kPushBytes);
    }
  }
  BindStageResources(cb, 1, 0, DkStage_Compute, p->stages[0]);
  dkCmdBufDispatchCompute(cb->cmd, x, y, z);
  Bump(g_dispatches);
}

// ---- barriers

static VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src,
                                                     VkPipelineStageFlags dst, VkDependencyFlags, uint32_t memCount,
                                                     const VkMemoryBarrier* mems, uint32_t bufCount,
                                                     const VkBufferMemoryBarrier* bufs, uint32_t imgCount,
                                                     const VkImageMemoryBarrier* imgs) {
  VKDK_TRACE("CmdPipelineBarrier");
  // Image layouts do not exist in deko3d: a barrier orders the GPU's work and drops its caches. The copy engine on
  // either side: Full (it waits for the copy engine too). Rendering or compute before reading: Fragments / Primitives
  // with the texture cache dropped. A write that only waits on earlier writes: no invalidation (renut-nx's lean
  // barriers).
  VkAccessFlags dstAccess = 0;
  for (uint32_t i = 0; i < memCount; ++i) dstAccess |= mems[i].dstAccessMask;
  for (uint32_t i = 0; i < bufCount; ++i) dstAccess |= bufs[i].dstAccessMask;
  for (uint32_t i = 0; i < imgCount; ++i) dstAccess |= imgs[i].dstAccessMask;
  DkBarrier mode = DkBarrier_Fragments;
  // lean: the mode when the transfers on both sides are drawn copies (FlushBarriers decides).
  DkBarrier lean = DkBarrier_Fragments;
  if ((src | dst) & VK_PIPELINE_STAGE_HOST_BIT) {
    mode = lean = DkBarrier_Full;
  } else if ((src | dst) & VK_PIPELINE_STAGE_TRANSFER_BIT) {
    mode = DkBarrier_Full;
    lean = DkBarrier_Primitives;
  } else if ((src | dst) & (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)) {
    mode = lean = DkBarrier_Primitives;
  }
  if (src & (VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)) {
    mode = lean = DkBarrier_Full;
  }
  uint32_t invalidate = 0;
  if (dstAccess & (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_MEMORY_READ_BIT)) {
    invalidate |= DkInvalidateFlags_Image;
  }
  if (dstAccess & (VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                   VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_MEMORY_READ_BIT)) {
    invalidate |= DkInvalidateFlags_Shader;
  }
  if (dstAccess & VK_ACCESS_HOST_READ_BIT) {
    invalidate |= DkInvalidateFlags_L2Cache;
    mode = lean = DkBarrier_Full;
  }
  // A copy that writes depth makes Zcull's view of it stale.
  for (uint32_t i = 0; i < imgCount; ++i) {
    if ((imgs[i].srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) && imgs[i].image && imgs[i].image->depth) {
      invalidate |= DkInvalidateFlags_Zcull;
    }
  }
  cb->pendingMode = cb->pendingBarrier ? std::max(cb->pendingMode, mode) : mode;
  cb->pendingLean = cb->pendingBarrier ? std::max(cb->pendingLean, lean) : lean;
  cb->pendingInvalidate |= invalidate;
  cb->pendingBarrier = true;
}

// ---- copies and clears outside render passes

static DkImageView MipView(VkImage img, uint32_t mip, uint32_t layer, uint32_t layers) {
  DkImageView v;
  dkImageViewDefaults(&v, &img->image);
  v.mipLevelOffset = uint8_t(mip);
  v.mipLevelCount = 1;
  if (img->dkType == DkImageType_Cubemap || img->dkType == DkImageType_CubemapArray ||
      img->dkType == DkImageType_2DArray) {
    v.type = layers > 1 ? DkImageType_2DArray : DkImageType_2D;
    v.layerOffset = uint16_t(layer);
    v.layerCount = uint16_t(layers);
  }
  return v;
}

// sdt_nx: a GPU write past the end of a buffer lands in the CPU heap (device memory is heap memory): every copy,
// fill and update is checked against its buffer and an out-of-range one is cut to what fits (and logged).
static bool BufferRange(const char* what, VkBuffer buf, uint64_t offset, uint64_t& size) {
  if (!buf || !buf->gpu) {
    VKDK_LOG_LIMITED(16, "%s: buffer %p not bound to memory: skipped", what, (void*)buf);
    return false;
  }
  if (offset >= buf->size) {
    VKDK_LOG_LIMITED(16, "%s: offset %llu past the buffer's %llu bytes: skipped", what, (unsigned long long)offset,
                     (unsigned long long)buf->size);
    return false;
  }
  if (offset + size > buf->size) {
    VKDK_LOG_LIMITED(16, "%s: %llu bytes at %llu run past the buffer's %llu: cut", what, (unsigned long long)size,
                     (unsigned long long)offset, (unsigned long long)buf->size);
    size = buf->size - offset;
  }
  return size != 0;
}

static VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t count,
                                                const VkBufferCopy* regions) {
  VKDK_TRACE("CmdCopyBuffer");
  BeginEngineCopy(cb);
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t size = regions[i].size;
    if (!BufferRange("vkCmdCopyBuffer (destination)", dst, regions[i].dstOffset, size) ||
        !BufferRange("vkCmdCopyBuffer (source)", src, regions[i].srcOffset, size)) {
      continue;
    }
    dkCmdBufCopyBuffer(cb->cmd, src->gpu + regions[i].srcOffset, dst->gpu + regions[i].dstOffset, uint32_t(size));
  }
  EndEngineCopy(cb);
}

static void BufferImageCopy(VkCommandBuffer cb, VkImage img, VkBuffer buf, const VkBufferImageCopy& r, bool toImage) {
  uint32_t bw, bh;
  const uint32_t bytes = FormatBlockBytes(img->format, bw, bh);
  const uint32_t rowTexels = r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width;
  const uint32_t heightTexels = r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height;
  DkCopyBuf copy;
  copy.addr = buf->gpu + r.bufferOffset;
  // deko3d takes the row length in bytes and the image height in rows of blocks.
  copy.rowLength = (rowTexels + bw - 1) / bw * bytes;
  copy.imageHeight = (heightTexels + bh - 1) / bh;
  const uint32_t layers = r.imageSubresource.layerCount;
  {
    const uint64_t span = uint64_t(copy.rowLength) * copy.imageHeight * std::max(r.imageExtent.depth, 1u) * layers;
    uint64_t want = span;
    if (!BufferRange(toImage ? "vkCmdCopyBufferToImage (source)" : "vkCmdCopyImageToBuffer (destination)", buf,
                     r.bufferOffset, want) ||
        (!toImage && want < span)) {
      return;  // (a readback that does not fit its buffer is not done at all)
    }
    const uint32_t mw = std::max(img->extent.width >> r.imageSubresource.mipLevel, 1u);
    const uint32_t mh = std::max(img->extent.height >> r.imageSubresource.mipLevel, 1u);
    if (uint32_t(r.imageOffset.x) + r.imageExtent.width > AlignUp(mw, bw) ||
        uint32_t(r.imageOffset.y) + r.imageExtent.height > AlignUp(mh, bh) ||
        r.imageSubresource.mipLevel >= img->mips ||
        r.imageSubresource.baseArrayLayer + layers > std::max(img->layers, img->extent.depth)) {
      VKDK_LOG_LIMITED(16, "buffer/image copy: region %ux%u at (%d,%d) mip %u layers %u+%u outside image %ux%u (%u mips, "
                       "%u layers): skipped", r.imageExtent.width, r.imageExtent.height, r.imageOffset.x,
                       r.imageOffset.y, r.imageSubresource.mipLevel, r.imageSubresource.baseArrayLayer, layers,
                       img->extent.width, img->extent.height, img->mips, img->layers);
      return;
    }
  }
  for (uint32_t l = 0; l < layers; ++l) {
    DkImageView view = MipView(img, r.imageSubresource.mipLevel, r.imageSubresource.baseArrayLayer + l, 1);
    DkImageRect rect = {uint32_t(r.imageOffset.x), uint32_t(r.imageOffset.y), uint32_t(r.imageOffset.z),
                        r.imageExtent.width, r.imageExtent.height, std::max(r.imageExtent.depth, 1u)};
    if (toImage) {
      dkCmdBufCopyBufferToImage(cb->cmd, &copy, &view, &rect, 0);
    } else {
      dkCmdBufCopyImageToBuffer(cb->cmd, &view, &rect, &copy, 0);
    }
    copy.addr += uint64_t(copy.rowLength) * copy.imageHeight * std::max(r.imageExtent.depth, 1u);
  }
}

static VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout,
                                                       uint32_t count, const VkBufferImageCopy* regions) {
  VKDK_TRACE("CmdCopyBufferToImage");
  BeginEngineCopy(cb);
  for (uint32_t i = 0; i < count; ++i) BufferImageCopy(cb, dst, src, regions[i], true);
  EndEngineCopy(cb);
}

static VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout, VkBuffer dst,
                                                       uint32_t count, const VkBufferImageCopy* regions) {
  VKDK_TRACE("CmdCopyImageToBuffer");
  BeginEngineCopy(cb);
  for (uint32_t i = 0; i < count; ++i) BufferImageCopy(cb, src, dst, regions[i], false);
  EndEngineCopy(cb);
}

// Colour copies drawn on the 3D engine (renut-nx's resolves as draws, its docs/deko3d-renderer.md section 13): deko3d's
// image copy runs on another engine, so it and the Full barriers on its sides drain the whole GPU (3.7-5.1 ms a frame
// of copies at 307 MHz, round 5). A draw that loads each source texel and writes it to the destination as a render
// target stays in the 3D pipe; it copies the bits where a texel load gives back what is stored (DrawnCopyFormat: unorm
// and float, the same format on both sides), one sample, 2D, into a destination made renderable (vkdk_core.cpp).
constexpr uint32_t kCopySlots = 1024;  // image descriptors of the sources, reused in turn

// Why a copy stayed on the copy engine (the census below).
static const char* DrawnCopyRefusal(VkDevice d, VkImage src, VkImage dst, const VkImageCopy& r) {
  if (!d->copyShaders[0] || !d->copyShaders[1]) return "no copy shaders";
  if (src == dst) return "same image";
  if (src->depth || dst->depth) return "depth";
  if (src->format != dst->format) return "formats differ";
  if (!DrawnCopyFormat(src->format)) return "format";
  if (src->dkType == DkImageType_3D || dst->dkType == DkImageType_3D || r.extent.depth > 1) return "3D";
  if (r.srcSubresource.layerCount != 1 || r.dstSubresource.layerCount != 1) return "layers";
  if (!dst->renderable) return "destination not renderable";
  if (r.srcOffset.x < 0 || r.srcOffset.y < 0 || r.dstOffset.x < 0 || r.dstOffset.y < 0 || !r.extent.width ||
      !r.extent.height) {
    return "rectangle";
  }
  return nullptr;
}

// A census of the copies left to the copy engine, by reason, formats and size: the log gets its largest groups now
// and then (round 7's console run had ~500 of them a frame against ~5 drawn).
static void CountEngineCopy(const char* why, VkImage src, VkImage dst, const VkImageCopy& r) {
  static std::mutex mutex;
  static std::unordered_map<std::string, uint64_t> groups;
  char key[160];
  snprintf(key, sizeof(key), "%s: %d -> %d, %ux%u, mip %u, %u layers%s", why, int(src->format), int(dst->format),
           r.extent.width, r.extent.height, r.srcSubresource.mipLevel, r.srcSubresource.layerCount,
           src->swapchain || dst->swapchain ? ", swapchain" : "");
  std::lock_guard<std::mutex> lock(mutex);
  ++groups[key];
  const uint64_t n = g_engineImageCopies.load(std::memory_order_relaxed);
  if (n == 1 || n == 1000 || n % 200000 == 0) {
    std::vector<std::pair<uint64_t, std::string>> top;
    for (const auto& g : groups) top.push_back({g.second, g.first});
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string line;
    for (size_t i = 0; i < top.size() && i < 8; ++i) {
      line += " | " + std::to_string(top[i].first) + "x " + top[i].second;
    }
    Log("copy engine census after %llu image copies (%zu kinds):%s", (unsigned long long)n, groups.size(),
        line.c_str());
  }
}

static bool DrawnCopy(VkCommandBuffer cb, VkImage src, VkImage dst, const VkImageCopy& r) {
  VkDevice d = cb->device;
  if (const char* why = DrawnCopyRefusal(d, src, dst, r)) {
    Bump(g_engineImageCopies);
    CountEngineCopy(why, src, dst, r);
    return false;
  }
  void* cpu;
  DkGpuAddr gpu;
  if (!TransientAlloc(cb, 16, &cpu, &gpu)) {
    return false;
  }
  const int32_t offset[4] = {r.srcOffset.x - r.dstOffset.x, r.srcOffset.y - r.dstOffset.y, 0, 0};
  memcpy(cpu, offset, sizeof(offset));
  // The source's descriptor; the texture cache dropped with the barrier (a transfer read does not ask for it).
  const uint32_t slot = d->copyImageFirst + d->copyNext.fetch_add(1, std::memory_order_relaxed) % kCopySlots;
  const DkImageView sv = MipView(src, r.srcSubresource.mipLevel, r.srcSubresource.baseArrayLayer, 1);
  dkImageDescriptorInitialize(&d->imageCpu[slot], &sv, false, false);
  d->descriptorEpoch.fetch_add(1, std::memory_order_release);
  if (cb->pendingBarrier) {
    cb->pendingInvalidate |= DkInvalidateFlags_Image;
    FlushBarriers(cb);
  } else {
    dkCmdBufBarrier(cb->cmd, DkBarrier_None, DkInvalidateFlags_Image);
  }
  FlushEpochs(cb);
  DkCmdBuf cmd = cb->cmd;
  const DkImageView dv = MipView(dst, r.dstSubresource.mipLevel, r.dstSubresource.baseArrayLayer, 1);
  const DkImageView* target = &dv;
  dkCmdBufBindRenderTargets(cmd, &target, 1, nullptr);
  WriteFramebufferSrgb(cmd, false);
  const DkViewport vp = {float(r.dstOffset.x), float(r.dstOffset.y), float(r.extent.width), float(r.extent.height),
                         0.0f, 1.0f};
  dkCmdBufSetViewports(cmd, 0, &vp, 1);
  const DkScissor sc = {uint32_t(r.dstOffset.x), uint32_t(r.dstOffset.y), r.extent.width, r.extent.height};
  dkCmdBufSetScissors(cmd, 0, &sc, 1);
  const DkShader* shaders[2] = {&d->copyShaders[0]->shader, &d->copyShaders[1]->shader};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  dkCmdBufBindRasterizerState(cmd, &rs);
  DkMultisampleState ms;
  dkMultisampleStateDefaults(&ms);
  dkCmdBufBindMultisampleState(cmd, &ms);
  DkColorState cs;
  dkColorStateDefaults(&cs);
  dkCmdBufBindColorState(cmd, &cs);
  DkColorWriteState cw;
  dkColorWriteStateDefaults(&cw);
  dkCmdBufBindColorWriteState(cmd, &cw);
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = false;
  ds.depthWriteEnable = false;
  ds.stencilTestEnable = false;
  dkCmdBufBindDepthStencilState(cmd, &ds);
  dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
  dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, gpu, 16);
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(slot, d->copySampler));
  ZeroBaseVertex(cb);
  dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
  // What the next draw sends again: everything this one changed.
  cb->boundStatePipeline = nullptr;
  memset(cb->boundGroups, 0, sizeof(cb->boundGroups));
  cb->boundShaders[0] = cb->boundShaders[1] = nullptr;
  cb->boundUbo[1][0] = DkBufExtents{};
  memset(&cb->boundTex[1][0], 0xFF, sizeof(cb->boundTex[1][0]));
  cb->vtxDirty = ~0u;
  if (cb->viewportCount) {
    SetViewports(cb, 0, cb->viewportCount, cb->vkViewports);
  }
  ApplyScissors(cb);
  if (cb->pass && cb->framebuffer) {  // (Vulkan forbids copies inside a render pass; kept safe anyway.)
    const DkImageView* colors[8];
    for (uint32_t i = 0; i < cb->pass->colorCount; ++i) {
      colors[i] = &cb->framebuffer->views[cb->pass->colorRefs[i]]->target;
    }
    const DkImageView* depthView = cb->pass->depthRef != VK_ATTACHMENT_UNUSED
                                       ? &cb->framebuffer->views[cb->pass->depthRef]->target
                                       : nullptr;
    dkCmdBufBindRenderTargets(cmd, colors, cb->pass->colorCount, depthView);
  }
  const uint64_t n = g_drawnCopies.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || n % 20000 == 0) {
    Log("copies: %llu drawn on the 3D engine, %llu image copies on the copy engine; %llu barriers Primitives "
        "instead of Full",
        (unsigned long long)n, (unsigned long long)g_engineImageCopies.load(std::memory_order_relaxed),
        (unsigned long long)g_leanBarriers.load(std::memory_order_relaxed));
  }
  return true;
}

// Many small copies from a 2D image into a 3D one, as one compute pass (round 8's census: the game's lighting volume,
// ~500 8x8 blocks a frame, each its own copy-engine job). A workgroup of 8x8 per block loads the source texels and
// stores them into the volume; the blocks' offsets and sizes are a uniform table (two ivec4 each). RGBA8 only: the load
// gives back the stored bytes and the store writes them again.
constexpr uint32_t kVolumeBlocks = 2048;  // per dispatch: the table fills the 64 KB uniform buffer

static bool VolumeCopy(VkCommandBuffer cb, VkImage src, VkImage dst, uint32_t count, const VkImageCopy* regions) {
  VkDevice d = cb->device;
  if (!d->volumeCopyShader || count < 2 || !dst->storable || src->dkType != DkImageType_2D ||
      src->format != VK_FORMAT_R8G8B8A8_UNORM || dst->format != VK_FORMAT_R8G8B8A8_UNORM) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const VkImageCopy& r = regions[i];
    if (r.srcSubresource.mipLevel || r.dstSubresource.mipLevel || r.srcSubresource.layerCount != 1 ||
        r.srcSubresource.baseArrayLayer || r.extent.depth != 1 || !r.extent.width || !r.extent.height ||
        r.extent.width > 8 || r.extent.height > 8 || r.srcOffset.x < 0 || r.srcOffset.y < 0 || r.dstOffset.x < 0 ||
        r.dstOffset.y < 0 || r.dstOffset.z < 0) {
      return false;
    }
  }
  const uint32_t first = d->copyNext.fetch_add(2, std::memory_order_relaxed);
  const uint32_t srcSlot = d->copyImageFirst + first % kCopySlots;
  const uint32_t dstSlot = d->copyImageFirst + (first + 1) % kCopySlots;
  DkImageView sv = MipView(src, 0, 0, 1);
  dkImageDescriptorInitialize(&d->imageCpu[srcSlot], &sv, false, false);
  DkImageView dv;
  dkImageViewDefaults(&dv, &dst->image);
  dkImageDescriptorInitialize(&d->imageCpu[dstSlot], &dv, true, false);
  d->descriptorEpoch.fetch_add(1, std::memory_order_release);
  if (cb->pendingBarrier) {
    cb->pendingInvalidate |= DkInvalidateFlags_Image;
    FlushBarriers(cb);
  } else {
    dkCmdBufBarrier(cb->cmd, DkBarrier_None, DkInvalidateFlags_Image);
  }
  FlushEpochs(cb);
  DkCmdBuf cmd = cb->cmd;
  const DkShader* shader = &d->volumeCopyShader->shader;
  dkCmdBufBindShaders(cmd, DkStageFlag_Compute, &shader, 1);
  dkCmdBufBindTexture(cmd, DkStage_Compute, 0, dkMakeTextureHandle(srcSlot, d->copySampler));
  dkCmdBufBindImage(cmd, DkStage_Compute, 0, dkMakeImageHandle(dstSlot));
  for (uint32_t at = 0; at < count; at += kVolumeBlocks) {
    const uint32_t n = std::min(count - at, kVolumeBlocks);
    void* cpu;
    DkGpuAddr gpu;
    if (!TransientAlloc(cb, n * 32, &cpu, &gpu)) {
      return at != 0;  // (the first slice: nothing recorded, the copy engine does it all)
    }
    int32_t* t = static_cast<int32_t*>(cpu);
    for (uint32_t i = 0; i < n; ++i) {
      const VkImageCopy& r = regions[at + i];
      const int32_t row[8] = {r.srcOffset.x, r.srcOffset.y, r.dstOffset.x, r.dstOffset.y,
                              r.dstOffset.z, int32_t(r.extent.width), int32_t(r.extent.height), 0};
      memcpy(t + i * 8, row, sizeof(row));
    }
    dkCmdBufBindUniformBuffer(cmd, DkStage_Compute, 0, gpu, AlignUp(n * 32, DK_UNIFORM_BUF_ALIGNMENT));
    dkCmdBufDispatchCompute(cmd, n, 1, 1);
  }
  Bump(g_dispatches);
  const uint64_t calls = g_volumeCopies.fetch_add(1, std::memory_order_relaxed) + 1;
  if (calls == 1 || calls % 20000 == 0) {
    Log("copies: %llu calls of 3D blocks done as one compute pass each (%u blocks in this one)",
        (unsigned long long)calls, count);
  }
  return true;
}

static VKAPI_ATTR void VKAPI_CALL CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout, VkImage dst,
                                               VkImageLayout, uint32_t count, const VkImageCopy* regions) {
  VKDK_TRACE("CmdCopyImage");
  if (VolumeCopy(cb, src, dst, count, regions)) {
    return;
  }
  bool engine = false;
  for (uint32_t i = 0; i < count; ++i) {
    const VkImageCopy& r = regions[i];
    if (DrawnCopy(cb, src, dst, r)) {
      continue;
    }
    if (!engine) {
      BeginEngineCopy(cb);
      engine = true;
    }
    for (uint32_t l = 0; l < r.srcSubresource.layerCount; ++l) {
      DkImageView sv = MipView(src, r.srcSubresource.mipLevel, r.srcSubresource.baseArrayLayer + l, 1);
      DkImageView dv = MipView(dst, r.dstSubresource.mipLevel, r.dstSubresource.baseArrayLayer + l, 1);
      DkImageRect sr = {uint32_t(r.srcOffset.x), uint32_t(r.srcOffset.y), uint32_t(r.srcOffset.z), r.extent.width,
                        r.extent.height, std::max(r.extent.depth, 1u)};
      DkImageRect dr = {uint32_t(r.dstOffset.x), uint32_t(r.dstOffset.y), uint32_t(r.dstOffset.z), r.extent.width,
                        r.extent.height, std::max(r.extent.depth, 1u)};
      dkCmdBufCopyImage(cb->cmd, &sv, &sr, &dv, &dr, 0);
    }
  }
  if (engine) {
    EndEngineCopy(cb);
  }
}

static VKAPI_ATTR void VKAPI_CALL CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout, VkImage dst,
                                               VkImageLayout, uint32_t count, const VkImageBlit* regions,
                                               VkFilter filter) {
  VKDK_TRACE("CmdBlitImage");
  BeginEngineCopy(cb);
  for (uint32_t i = 0; i < count; ++i) {
    const VkImageBlit& r = regions[i];
    DkImageView sv = MipView(src, r.srcSubresource.mipLevel, r.srcSubresource.baseArrayLayer, 1);
    DkImageView dv = MipView(dst, r.dstSubresource.mipLevel, r.dstSubresource.baseArrayLayer, 1);
    // Depth (and stencil) are copied as raw texels: never filtered (Vulkan requires nearest for them too).
    const bool depth = src->depth || dst->depth;
    uint32_t flags = filter == VK_FILTER_LINEAR && !depth ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest;
    auto rect = [&](const VkOffset3D* o, uint32_t& fl, uint32_t flipBit) {
      int32_t x0 = o[0].x, x1 = o[1].x, y0 = o[0].y, y1 = o[1].y;
      if (x0 > x1) {
        std::swap(x0, x1);
        fl |= DkBlitFlag_FlipX;
      }
      if (y0 > y1) {
        std::swap(y0, y1);
        fl |= flipBit;
      }
      return DkImageRect{uint32_t(x0), uint32_t(y0), 0, uint32_t(x1 - x0), uint32_t(y1 - y0), 1};
    };
    uint32_t fs = 0, fd = 0;
    DkImageRect sr = rect(r.srcOffsets, fs, DkBlitFlag_FlipY);
    DkImageRect dr = rect(r.dstOffsets, fd, DkBlitFlag_FlipY);
    flags |= (fs ^ fd);
    dkCmdBufBlitImage(cb->cmd, &sv, &sr, &dv, &dr, flags, 0);
    if (dst->depth) {
      dkCmdBufBarrier(cb->cmd, DkBarrier_None, DkInvalidateFlags_Zcull);  // the 2D engine wrote depth
    }
  }
  EndEngineCopy(cb);
}

static VKAPI_ATTR void VKAPI_CALL CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout,
                                                     const VkClearColorValue* color, uint32_t rangeCount,
                                                     const VkImageSubresourceRange* ranges) {
  VKDK_TRACE("CmdClearColorImage");
  // A clear is a render-target operation in deko3d: the image is bound as the target for it, then the command
  // buffer's own target (its render pass, if any) is bound again.
  FlushBarriers(cb);
  const DkScissor all = {0, 0, 16384, 16384};
  dkCmdBufSetScissors(cb->cmd, 0, &all, 1);
  for (uint32_t i = 0; i < rangeCount; ++i) {
    const VkImageSubresourceRange& r = ranges[i];
    const uint32_t mips = r.levelCount == VK_REMAINING_MIP_LEVELS ? img->mips - r.baseMipLevel : r.levelCount;
    const uint32_t layers = r.layerCount == VK_REMAINING_ARRAY_LAYERS ? img->layers - r.baseArrayLayer : r.layerCount;
    for (uint32_t m = 0; m < mips; ++m) {
      for (uint32_t l = 0; l < layers; ++l) {
        DkImageView v = MipView(img, r.baseMipLevel + m, r.baseArrayLayer + l, 1);
        const DkImageView* target = &v;
        dkCmdBufBindRenderTargets(cb->cmd, &target, 1, nullptr);
        dkCmdBufClearColor(cb->cmd, 0, DkColorMask_RGBA, color);
      }
    }
  }
  if (cb->pass && cb->framebuffer) {
    // (Vulkan forbids this inside a render pass; kept safe anyway.)
    const DkImageView* colors[8];
    for (uint32_t i = 0; i < cb->pass->colorCount; ++i) {
      colors[i] = &cb->framebuffer->views[cb->pass->colorRefs[i]]->target;
    }
    dkCmdBufBindRenderTargets(cb->cmd, colors, cb->pass->colorCount, nullptr);
  }
  ApplyScissors(cb);
}

static VKAPI_ATTR void VKAPI_CALL CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout,
                                                            const VkClearDepthStencilValue* value, uint32_t rangeCount,
                                                            const VkImageSubresourceRange* ranges) {
  VKDK_TRACE("CmdClearDepthStencilImage");
  // As CmdClearColorImage: the image bound as the depth target for the clear, the render pass's targets after.
  FlushBarriers(cb);
  const DkScissor all = {0, 0, 16384, 16384};
  dkCmdBufSetScissors(cb->cmd, 0, &all, 1);
  for (uint32_t i = 0; i < rangeCount; ++i) {
    const VkImageSubresourceRange& r = ranges[i];
    const uint32_t mips = r.levelCount == VK_REMAINING_MIP_LEVELS ? img->mips - r.baseMipLevel : r.levelCount;
    const uint32_t layers = r.layerCount == VK_REMAINING_ARRAY_LAYERS ? img->layers - r.baseArrayLayer : r.layerCount;
    const bool depth = (r.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    const bool stencil = (r.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 && img->stencil;
    for (uint32_t m = 0; m < mips; ++m) {
      for (uint32_t l = 0; l < layers; ++l) {
        DkImageView v = MipView(img, r.baseMipLevel + m, r.baseArrayLayer + l, 1);
        dkCmdBufBindRenderTargets(cb->cmd, nullptr, 0, &v);
        dkCmdBufClearDepthStencil(cb->cmd, depth, value->depth, stencil ? 0xFF : 0, uint8_t(value->stencil));
      }
    }
  }
  // A copy-engine or 3D write of depth: Zcull's view of it is stale.
  dkCmdBufBarrier(cb->cmd, DkBarrier_None, DkInvalidateFlags_Zcull);
  if (cb->pass && cb->framebuffer) {
    const DkImageView* colors[8];
    for (uint32_t i = 0; i < cb->pass->colorCount; ++i) {
      colors[i] = &cb->framebuffer->views[cb->pass->colorRefs[i]]->target;
    }
    const DkImageView* depthView = cb->pass->depthRef != VK_ATTACHMENT_UNUSED
                                       ? &cb->framebuffer->views[cb->pass->depthRef]->target
                                       : nullptr;
    dkCmdBufBindRenderTargets(cb->cmd, colors, cb->pass->colorCount, depthView);
  }
  ApplyScissors(cb);
}

// ---- queries

static VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t) {
  VKDK_TRACE("CmdResetQueryPool");
  // Every query's report is written again by its end; nothing to reset.
}

static DkCounter QueryCounter(const VkQueryPool_T* pool) {
  return pool->type == VK_QUERY_TYPE_OCCLUSION ? DkCounter_SamplesPassed : DkCounter_ClipperInputPrimitives;
}

static VKAPI_ATTR void VKAPI_CALL CmdBeginQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t query,
                                                VkQueryControlFlags) {
  VKDK_TRACE("CmdBeginQuery");
  FlushBarriers(cb);
  // The counter keeps running: a query is its end report minus its begin report (vkGetQueryPoolResults), which needs
  // no counter reset and lets queries of different pools overlap.
  dkCmdBufReportCounter(cb->cmd, QueryCounter(pool), pool->gpu + pool->beginOffset + query * 16);
  cb->openQuery = pool;
  cb->openQueryIndex = query;
}

static VKAPI_ATTR void VKAPI_CALL CmdEndQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t query) {
  VKDK_TRACE("CmdEndQuery");
  FlushBarriers(cb);
  dkCmdBufReportCounter(cb->cmd, QueryCounter(pool), pool->gpu + query * 16);
  cb->openQuery = nullptr;
}

static VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage,
                                                    VkQueryPool pool, uint32_t query) {
  VKDK_TRACE("CmdWriteTimestamp");
  FlushBarriers(cb);
  dkCmdBufReportCounter(cb->cmd,
                        stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT ? DkCounter_TimestampPipelineTop
                                                                   : DkCounter_Timestamp,
                        pool->gpu + query * 16);
}

static VKAPI_ATTR void VKAPI_CALL CmdCopyQueryPoolResults(VkCommandBuffer cb, VkQueryPool pool, uint32_t first,
                                                          uint32_t count, VkBuffer dst, VkDeviceSize offset,
                                                          VkDeviceSize stride, VkQueryResultFlags flags) {
  VKDK_TRACE("CmdCopyQueryPoolResults");
  // Copies cannot subtract the begin report: only timestamps are exact here (bo2 reads occlusion results on the CPU,
  // vkGetQueryPoolResults).
  if (pool->type != VK_QUERY_TYPE_TIMESTAMP) {
    VKDK_LOG_LIMITED(4, "vkCmdCopyQueryPoolResults of an occlusion or statistics query: not exact on vkdk");
  }
  const uint32_t bytes = (flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4;
  const uint32_t word = pool->type == VK_QUERY_TYPE_TIMESTAMP ? 8 : 0;
  {
    uint64_t span = count ? uint64_t(count - 1) * stride + bytes : 0, want = span;
    if (!count || !BufferRange("vkCmdCopyQueryPoolResults", dst, offset, want) || want < span) {
      return;
    }
  }
  BeginEngineCopy(cb);
  for (uint32_t i = 0; i < count; ++i) {
    dkCmdBufCopyBuffer(cb->cmd, pool->gpu + (first + i) * 16 + word, dst->gpu + offset + i * stride, bytes);
  }
  EndEngineCopy(cb);
}

// ---- queue


// the reports written since the last call: their times added up, their slots free again (q->mutex held)
static void HarvestStamps(VkQueue q) {
  if (!q->stampData) {
    return;
  }
  volatile uint64_t* r = static_cast<volatile uint64_t*>(dkMemBlockGetCpuAddr(q->stampData));
  uint64_t ticks = 0;
  for (uint32_t i = 0; i < VkQueue_T::kStampSlots; ++i) {
    if (!q->stampUsed[i]) {
      continue;
    }
    const uint64_t b = r[i * 4 + 1], e = r[i * 4 + 3];
    if (!b || !e) {
      continue;  // (still on its way)
    }
    if (e > b && e - b < 1000000000ull) {
      ticks += e - b;
    }
    r[i * 4 + 1] = 0;
    r[i * 4 + 3] = 0;
    q->stampUsed[i] = false;
  }
  if (ticks) {
    g_gpuBusyNs.fetch_add(ticks * 625 / 384, std::memory_order_relaxed);
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue q, uint32_t count, const VkSubmitInfo* submits,
                                                  VkFence fence) {
  VKDK_TRACE("QueueSubmit");
  std::lock_guard<std::mutex> lock(q->mutex);
  HarvestStamps(q);
  for (uint32_t s = 0; s < count; ++s) {
    const VkSubmitInfo& si = submits[s];
    for (uint32_t i = 0; i < si.waitSemaphoreCount; ++i) {
      VkSemaphore sem = si.pWaitSemaphores[i];
      if (sem && sem->pending) {
        dkQueueWaitFence(q->dk, &sem->fence);
        sem->pending = false;
      }
    }
    // sdt_nx diagnostics (sdmc:/switch/sdt_nx/vkdk_nosubmit): no command buffer reaches the GPU (fences still
    // signal), to tell GPU writes from CPU ones when memory is found overwritten.
    static const bool noSubmit = DebugSwitch("vkdk_nosubmit", false);
    // (a slot still waiting for its reports is skipped: this submission goes untimed)
    const uint32_t slot = q->stampNext;
    const bool stamp = si.commandBufferCount && !noSubmit && q->stampBuf && !q->stampUsed[slot];
    if (stamp) {
      dkQueueSubmitCommands(q->dk, q->stampBegin[slot]);
    }
    if (si.commandBufferCount && !noSubmit) {
      dkQueueSubmitCommands(q->dk, q->prologue);
    }
    for (uint32_t i = 0; i < si.commandBufferCount && !noSubmit; ++i) {
      VkCommandBuffer cb = si.pCommandBuffers[i];
      if (cb->list) {
        dkQueueSubmitCommands(q->dk, cb->list);
      }
    }
    if (stamp) {
      dkQueueSubmitCommands(q->dk, q->stampEnd[slot]);
      q->stampUsed[slot] = true;
      q->stampNext = (slot + 1) % VkQueue_T::kStampSlots;
    }
    // Signal semaphores: one queue orders everything after this already.
  }
  if (fence) {
    dkQueueSignalFence(q->dk, &fence->fence, true);
    fence->state.store(1, std::memory_order_release);
  }
  dkQueueFlush(q->dk);
  // Round m diagnostics (sdmc:/switch/sdt_nx/vkdk_waitidle): every submission done on the GPU before the CPU goes
  // on, so no CPU write (stream buffers, descriptors, command memory) can meet GPU work still in flight.
  static const bool waitIdle = DebugSwitch("vkdk_waitidle", false);
  if (waitIdle) {
    dkQueueWaitIdle(q->dk);
  }
  Bump(g_submits);
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(VkQueue, uint32_t, const VkBindSparseInfo*, VkFence) {
  VKDK_TRACE("QueueBindSparse");
  return VK_ERROR_FEATURE_NOT_PRESENT;
}

// ---- the rest of Vulkan 1.0's commands (sdt_nx: Unity's Android player loads them all)

static VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset,
                                                  uint32_t count, uint32_t stride) {
  VKDK_TRACE("CmdDrawIndirect");
  if (!buffer || !PrepareDraw(cb)) return;
  ZeroBaseVertex(cb);
  for (uint32_t i = 0; i < count; ++i) {
    dkCmdBufDrawIndirect(cb->cmd, cb->graphics->primitive, buffer->gpu + offset + uint64_t(i) * stride);
  }
  Bump(g_draws, count);
}

static VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset,
                                                         uint32_t count, uint32_t stride) {
  VKDK_TRACE("CmdDrawIndexedIndirect");
  if (!buffer || !PrepareDraw(cb)) return;
  const VkPipeline_T* p = cb->graphics;
  const uint32_t restartIndex = cb->indexFormat == DkIdxFormat_Uint32 ? 0xFFFFFFFFu
                                : cb->indexFormat == DkIdxFormat_Uint16 ? 0xFFFFu
                                                                        : 0xFFu;
  if (!cb->restartKnown || cb->restartBound != p->restart || (p->restart && cb->restartIndex != restartIndex)) {
    dkCmdBufSetPrimitiveRestart(cb->cmd, p->restart, restartIndex);
    cb->restartBound = p->restart;
    cb->restartIndex = restartIndex;
    cb->restartKnown = true;
  }
  for (uint32_t i = 0; i < count; ++i) {
    dkCmdBufDrawIndexedIndirect(cb->cmd, p->primitive, buffer->gpu + offset + uint64_t(i) * stride);
  }
  cb->baseVertexZero = false;  // (the vertex offsets come from the buffer)
  Bump(g_draws, count);
}

static VKAPI_ATTR void VKAPI_CALL CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset) {
  VKDK_TRACE("CmdDispatchIndirect");
  const VkPipeline_T* p = cb->compute;
  if (!p || !p->stages[0] || !buffer) return;
  FlushBarriers(cb);
  FlushEpochs(cb);
  const DkShader* shader = &p->stages[0]->shader;
  dkCmdBufBindShaders(cb->cmd, DkStageFlag_Compute, &shader, 1);
  if (p->stages[0]->meta->push && cb->pushSize) {
    void* cpu;
    DkGpuAddr gpu;
    if (TransientAlloc(cb, kPushBytes, &cpu, &gpu)) {
      memcpy(cpu, cb->push, cb->pushSize);
      dkCmdBufBindUniformBuffer(cb->cmd, DkStage_Compute, kPushUboSlot, gpu, kPushBytes);
    }
  }
  BindStageResources(cb, 1, 0, DkStage_Compute, p->stages[0]);
  dkCmdBufDispatchComputeIndirect(cb->cmd, buffer->gpu + offset);
  Bump(g_dispatches);
}

static VKAPI_ATTR void VKAPI_CALL CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset,
                                                  VkDeviceSize size, const void* data) {
  VKDK_TRACE("CmdUpdateBuffer");
  uint64_t n = size;
  if (!size || !BufferRange("vkCmdUpdateBuffer", dst, offset, n)) return;
  size = n;
  FlushBarriers(cb);
  dkCmdBufPushData(cb->cmd, dst->gpu + offset, data, uint32_t(size));
}

static VKAPI_ATTR void VKAPI_CALL CmdFillBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset,
                                                VkDeviceSize size, uint32_t value) {
  VKDK_TRACE("CmdFillBuffer");
  if (!dst) return;
  if (size == VK_WHOLE_SIZE) {
    size = offset < dst->size ? (dst->size - offset) & ~VkDeviceSize(3) : 0;
  }
  uint64_t n = size;
  if (!size || !BufferRange("vkCmdFillBuffer", dst, offset, n)) return;
  size = n & ~VkDeviceSize(3);
  if (!size) return;
  FlushBarriers(cb);
  uint32_t pattern[1024];
  for (uint32_t& w : pattern) w = value;
  // Inline data in the command stream (copied in), 4 KB at a time.
  for (VkDeviceSize done = 0; done < size;) {
    const uint32_t n = uint32_t(std::min<VkDeviceSize>(size - done, sizeof(pattern)));
    dkCmdBufPushData(cb->cmd, dst->gpu + offset + done, pattern, n);
    done += n;
  }
}

static VKAPI_ATTR void VKAPI_CALL CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout, VkImage dst,
                                                  VkImageLayout, uint32_t count, const VkImageResolve* regions) {
  VKDK_TRACE("CmdResolveImage");
  FlushBarriers(cb);
  for (uint32_t i = 0; i < count; ++i) {
    const VkImageResolve& r = regions[i];
    const DkImageView s = MipView(src, r.srcSubresource.mipLevel, r.srcSubresource.baseArrayLayer, 1);
    const DkImageView d = MipView(dst, r.dstSubresource.mipLevel, r.dstSubresource.baseArrayLayer, 1);
    dkCmdBufResolveImage(cb->cmd, &s, &d);
  }
  cb->copyEngineSince = true;
}

static VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer cb, uint32_t count, const VkCommandBuffer* cbs) {
  VKDK_TRACE("CmdExecuteCommands");
  FlushBarriers(cb);
  for (uint32_t i = 0; i < count; ++i) {
    if (cbs[i] && cbs[i]->list) {
      dkCmdBufCallList(cb->cmd, cbs[i]->list);
    }
  }
  // What the secondaries bound is unknown here now: everything is sent again (Vulkan: undefined after this; the
  // application binds again). The render pass and its targets stay.
  VkRenderPass pass = cb->pass;
  VkFramebuffer fb = cb->framebuffer;
  const uint32_t subpass = cb->subpass, w = cb->targetWidth, h = cb->targetHeight;
  ResetCommandBufferState(cb);
  cb->pass = pass;
  cb->framebuffer = fb;
  cb->subpass = subpass;
  cb->targetWidth = w;
  cb->targetHeight = h;
}

static VKAPI_ATTR void VKAPI_CALL CmdSetLineWidth(VkCommandBuffer cb, float width) {
  VKDK_TRACE("CmdSetLineWidth");
  dkCmdBufSetLineWidth(cb->cmd, width);
}

static VKAPI_ATTR void VKAPI_CALL CmdSetDepthBounds(VkCommandBuffer, float, float) {
  VKDK_TRACE("CmdSetDepthBounds");  // depthBounds is not offered (GetPhysicalDeviceFeatures): never enabled
}

static VKAPI_ATTR void VKAPI_CALL CmdSetEvent(VkCommandBuffer, VkEvent e, VkPipelineStageFlags) {
  VKDK_TRACE("CmdSetEvent");
  if (e) e->set.store(1);
}

static VKAPI_ATTR void VKAPI_CALL CmdResetEvent(VkCommandBuffer, VkEvent e, VkPipelineStageFlags) {
  VKDK_TRACE("CmdResetEvent");
  if (e) e->set.store(0);
}

static VKAPI_ATTR void VKAPI_CALL CmdWaitEvents(VkCommandBuffer cb, uint32_t, const VkEvent*,
                                                VkPipelineStageFlags src, VkPipelineStageFlags dst, uint32_t memCount,
                                                const VkMemoryBarrier* mems, uint32_t bufCount,
                                                const VkBufferMemoryBarrier* bufs, uint32_t imgCount,
                                                const VkImageMemoryBarrier* imgs) {
  VKDK_TRACE("CmdWaitEvents");
  // One queue, in order: a wait on events is the barrier between the two stages.
  CmdPipelineBarrier(cb, src, dst, 0, memCount, mems, bufCount, bufs, imgCount, imgs);
}

namespace vkdk {

#define VKDK_ENTRY(name, fn) \
  if (!strcmp(n, #name)) return reinterpret_cast<PFN_vkVoidFunction>(fn)

PFN_vkVoidFunction vkdk_cmd_proc(const char* n) {
  if (n[0] != 'v' || n[1] != 'k') return nullptr;
  VKDK_ENTRY(vkCreateCommandPool, CreateCommandPool);
  VKDK_ENTRY(vkDestroyCommandPool, DestroyCommandPool);
  VKDK_ENTRY(vkResetCommandPool, ResetCommandPool);
  VKDK_ENTRY(vkAllocateCommandBuffers, AllocateCommandBuffers);
  VKDK_ENTRY(vkFreeCommandBuffers, FreeCommandBuffers);
  VKDK_ENTRY(vkBeginCommandBuffer, BeginCommandBuffer);
  VKDK_ENTRY(vkEndCommandBuffer, EndCommandBuffer);
  VKDK_ENTRY(vkResetCommandBuffer, ResetCommandBuffer);
  VKDK_ENTRY(vkCmdBindPipeline, CmdBindPipeline);
  VKDK_ENTRY(vkCmdBindDescriptorSets, CmdBindDescriptorSets);
  VKDK_ENTRY(vkCmdPushConstants, CmdPushConstants);
  VKDK_ENTRY(vkCmdBindVertexBuffers, CmdBindVertexBuffers);
  VKDK_ENTRY(vkCmdBindIndexBuffer, CmdBindIndexBuffer);
  VKDK_ENTRY(vkCmdSetViewport, CmdSetViewport);
  VKDK_ENTRY(vkCmdSetScissor, CmdSetScissor);
  VKDK_ENTRY(vkCmdSetDepthBias, CmdSetDepthBias);
  VKDK_ENTRY(vkCmdSetBlendConstants, CmdSetBlendConstants);
  VKDK_ENTRY(vkCmdSetStencilCompareMask, CmdSetStencilCompareMask);
  VKDK_ENTRY(vkCmdSetStencilWriteMask, CmdSetStencilWriteMask);
  VKDK_ENTRY(vkCmdSetStencilReference, CmdSetStencilReference);
  VKDK_ENTRY(vkCmdBeginRenderPass, CmdBeginRenderPass);
  VKDK_ENTRY(vkCmdEndRenderPass, CmdEndRenderPass);
  VKDK_ENTRY(vkCmdClearAttachments, CmdClearAttachments);
  VKDK_ENTRY(vkCmdDraw, CmdDraw);
  VKDK_ENTRY(vkCmdDrawIndexed, CmdDrawIndexed);
  VKDK_ENTRY(vkCmdDispatch, CmdDispatch);
  VKDK_ENTRY(vkCmdPipelineBarrier, CmdPipelineBarrier);
  VKDK_ENTRY(vkCmdCopyBuffer, CmdCopyBuffer);
  VKDK_ENTRY(vkCmdCopyBufferToImage, CmdCopyBufferToImage);
  VKDK_ENTRY(vkCmdCopyImageToBuffer, CmdCopyImageToBuffer);
  VKDK_ENTRY(vkCmdCopyImage, CmdCopyImage);
  VKDK_ENTRY(vkCmdBlitImage, CmdBlitImage);
  VKDK_ENTRY(vkCmdClearColorImage, CmdClearColorImage);
  VKDK_ENTRY(vkCmdClearDepthStencilImage, CmdClearDepthStencilImage);
  VKDK_ENTRY(vkCmdResetQueryPool, CmdResetQueryPool);
  VKDK_ENTRY(vkCmdBeginQuery, CmdBeginQuery);
  VKDK_ENTRY(vkCmdEndQuery, CmdEndQuery);
  VKDK_ENTRY(vkCmdWriteTimestamp, CmdWriteTimestamp);
  VKDK_ENTRY(vkCmdCopyQueryPoolResults, CmdCopyQueryPoolResults);
  VKDK_ENTRY(vkQueueSubmit, QueueSubmit);
  VKDK_ENTRY(vkQueueBindSparse, QueueBindSparse);
  VKDK_ENTRY(vkCmdNextSubpass, CmdNextSubpass);
  VKDK_ENTRY(vkCmdDrawIndirect, CmdDrawIndirect);
  VKDK_ENTRY(vkCmdDrawIndexedIndirect, CmdDrawIndexedIndirect);
  VKDK_ENTRY(vkCmdDispatchIndirect, CmdDispatchIndirect);
  VKDK_ENTRY(vkCmdUpdateBuffer, CmdUpdateBuffer);
  VKDK_ENTRY(vkCmdFillBuffer, CmdFillBuffer);
  VKDK_ENTRY(vkCmdResolveImage, CmdResolveImage);
  VKDK_ENTRY(vkCmdExecuteCommands, CmdExecuteCommands);
  VKDK_ENTRY(vkCmdSetLineWidth, CmdSetLineWidth);
  VKDK_ENTRY(vkCmdSetDepthBounds, CmdSetDepthBounds);
  VKDK_ENTRY(vkCmdSetEvent, CmdSetEvent);
  VKDK_ENTRY(vkCmdResetEvent, CmdResetEvent);
  VKDK_ENTRY(vkCmdWaitEvents, CmdWaitEvents);
  return nullptr;
}


// The drawn copies' shaders (compiled by uam once per install, then from the cache file), their sources' descriptor
// ring and a sampler (a texel load ignores it; a handle needs one).
bool g_drawnCopiesOn = true, g_volumeCopiesOn = true;  // vkdkSetCopyPaths

void SetUpDrawnCopies(VkDevice d) {
  if (DebugSwitch("vkdk_copyengine", true)) {  // round k diagnostics: copies on the copy / 2D engine
    g_drawnCopiesOn = false;
    g_volumeCopiesOn = false;
  }
  static const char kVs[] =
      "#version 460\n"
      "void main() {\n"
      "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
      "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
      "}\n";
  static const char kPs[] =
      "#version 460\n"
      "layout(location = 0) out vec4 o;\n"
      "layout(binding = 0) uniform sampler2D source;\n"
      "layout(std140, binding = 0) uniform Copy { ivec4 delta; };\n"
      "void main() { o = texelFetch(source, ivec2(gl_FragCoord.xy) + delta.xy, 0); }\n";
  {
    std::lock_guard<std::mutex> lock(d->tableMutex);
    d->copyImageFirst = d->imageRanges.Allocate(kCopySlots);
    d->copySampler = d->samplerRanges.Allocate(1);
  }
  if (d->copyImageFirst == UINT32_MAX || d->copySampler == UINT32_MAX) {
    Log("drawn copies: no descriptors left, every copy on the copy engine");
    return;
  }
  Log("copies: drawn %s, 3D blocks by compute %s (vkdkSetCopyPaths)", g_drawnCopiesOn ? "on" : "off",
      g_volumeCopiesOn ? "on" : "off");
  DkSampler sampler;
  dkSamplerDefaults(&sampler);
  dkSamplerDescriptorInitialize(&d->samplerCpu[d->copySampler], &sampler);
  d->descriptorEpoch.fetch_add(1, std::memory_order_release);
  const Variant* vs = d->shaders->FromGlsl(DkStage_Vertex, kVs, "vs copy");
  const Variant* ps = d->shaders->FromGlsl(DkStage_Fragment, kPs, "ps copy");
  if (!vs || !ps) {
    Log("drawn copies: shaders did not compile, every copy on the copy engine");
    return;
  }
  d->copyShaders[0] = g_drawnCopiesOn ? vs : nullptr;
  d->copyShaders[1] = g_drawnCopiesOn ? ps : nullptr;
  static const char kCs[] =
      "#version 460\n"
      "layout(local_size_x = 8, local_size_y = 8) in;\n"
      "layout(binding = 0) uniform sampler2D source;\n"
      "layout(binding = 0, rgba8) uniform writeonly image3D target;\n"
      "layout(std140, binding = 0) uniform Blocks { ivec4 block[4096]; };\n"
      "void main() {\n"
      "  ivec4 a = block[gl_WorkGroupID.x * 2u];\n"
      "  ivec4 b = block[gl_WorkGroupID.x * 2u + 1u];\n"
      "  ivec2 l = ivec2(gl_LocalInvocationID.xy);\n"
      "  if (l.x >= b.y || l.y >= b.z) return;\n"
      "  imageStore(target, ivec3(a.zw + l, b.x), texelFetch(source, a.xy + l, 0));\n"
      "}\n";
  d->volumeCopyShader = g_volumeCopiesOn ? d->shaders->FromGlsl(DkStage_Compute, kCs, "cs volume copy") : nullptr;
  if (!d->volumeCopyShader) {
    Log("volume copies: shader did not compile, on the copy engine");
  }
}
}  // namespace vkdk

namespace vkdk {
std::atomic<uint64_t> g_fenceWaitNs{0}, g_acquireNs{0}, g_fenceWaitTotalNs{0};
std::atomic<uint64_t> g_gpuBusyNs{0};  // the GPU's busy time (VkQueue_T::stamp*), ns in all

// ---- the draw census (vkdkDrawCensus): draws per render pass over a few frames, logged at the end
std::atomic<int> g_censusFrames{0};
static std::mutex g_censusMutex;
struct CensusRow {
  uint32_t w, h, colors, depth, samples;  // (samples: the first attachment's VkFormat)
  uint64_t draws, instances;
};
static std::map<const void*, CensusRow> g_census;
static int g_censusTotalFrames;

void CensusDraw(VkCommandBuffer cb, uint32_t instances) {
  VkRenderPass pass = cb->pass ? cb->pass : cb->inheritPass;
  VkFramebuffer fb = cb->pass ? cb->framebuffer : cb->inheritFramebuffer;
  std::lock_guard<std::mutex> lock(g_censusMutex);
  CensusRow& r = g_census[pass];
  if (r.draws == 0 && pass) {
    r.colors = pass->colorCount;
    r.depth = pass->depthRef != VK_ATTACHMENT_UNUSED;
    r.samples = pass->attachments.empty() ? 0 : uint32_t(pass->attachments[0].format);  // (the first one's format)
    r.w = fb ? fb->width : 0;
    r.h = fb ? fb->height : 0;
  }
  r.draws++;
  r.instances += instances;
}

void CensusFrameEnd() {
  int left = g_censusFrames.load(std::memory_order_relaxed);
  if (left <= 0) return;
  if (g_censusFrames.fetch_sub(1, std::memory_order_relaxed) != 1) return;
  std::vector<std::pair<const void*, CensusRow>> rows;
  {
    std::lock_guard<std::mutex> lock(g_censusMutex);
    rows.assign(g_census.begin(), g_census.end());
    g_census.clear();
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.draws > b.second.draws; });
  uint64_t total = 0;
  for (auto& r : rows) total += r.second.draws;
  const double f = g_censusTotalFrames > 0 ? g_censusTotalFrames : 1;
  Log("draw census: %.0f draws a frame over %d frames, by render pass:", total / f, g_censusTotalFrames);
  for (auto& r : rows) {
    const CensusRow& c = r.second;
    Log("  %6.1f draws (%5.1f instances) a frame: pass %p, %ux%u, %u colour target(s)%s, format %u%s", c.draws / f,
        c.instances / f, r.first, c.w, c.h, c.colors, c.depth ? " + depth" : "", c.samples,
        c.colors == 0 && c.depth ? " (depth only: a shadow map or a depth prepass)" : "");
  }
}
}  // namespace vkdk

extern "C" void vkdkDrawCensus(int frames) {
  if (frames <= 0) return;
  {
    std::lock_guard<std::mutex> lock(vkdk::g_censusMutex);
    vkdk::g_census.clear();
    vkdk::g_censusTotalFrames = frames;
  }
  vkdk::g_censusFrames.store(frames, std::memory_order_relaxed);
}

extern "C" void vkdkTakeStats(VkdkStats* out) {
  auto take = [](std::atomic<uint64_t>& c) { return c.exchange(0, std::memory_order_relaxed); };
  memset(out, 0, sizeof(*out));
  out->draws = take(g_draws);
  out->dispatches = take(g_dispatches);
  out->pipeline_binds = take(g_pipelineBinds);
  out->state_binds = take(g_stateBinds);
  out->texture_binds = take(g_textureBinds);
  out->ubo_binds = take(g_uboBinds);
  out->barriers = take(g_barriers);
  out->submits = take(g_submits);
  out->command_bytes = g_commandBytes.load(std::memory_order_relaxed);
  out->fence_wait_ns = take(vkdk::g_fenceWaitNs);
  out->acquire_ns = take(vkdk::g_acquireNs);
}

extern "C" uint64_t vkdkFenceWaitTotalNs(void) {
  return vkdk::g_fenceWaitTotalNs.load(std::memory_order_relaxed);
}

extern "C" uint64_t vkdkGpuBusyTotalNs(void) {
  return vkdk::g_gpuBusyNs.load(std::memory_order_relaxed);
}

extern "C" void vkdkSetCopyPaths(bool drawn, bool volume) {
  vkdk::g_drawnCopiesOn = drawn;
  vkdk::g_volumeCopiesOn = volume;
}
