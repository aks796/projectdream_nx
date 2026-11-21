// vkdk: the Vulkan subset bo2-360-nx calls, implemented on deko3d (the Switch's own GPU API). It replaces Mesa's static
// NVK (libvulkan.a) in the Switch build: the SDK and the native renderer reach every call through their function
// tables, so nothing above this library changes.
//
// The mappings and the deko3d 0.5.0 work-arounds follow renut-nx's plume deko3d back end (ChanseyIsTheBest, MIT, reused
// with permission), docs/deko3d-renderer.md there; what differs is that this is Vulkan's API, so descriptor sets, render
// passes and pipelines keep Vulkan's meaning.
//
// Copyright 2026 aks796. GPL-3.0, like the rest of bo2-360-nx.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_VI_NN
#define VK_USE_PLATFORM_VI_NN
#endif
#include <vulkan/vulkan.h>

#include <deko3d.h>
#include <switch.h>

#include <array>
#include <deque>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "vkdk.h"

namespace vkdk {
extern std::atomic<uint64_t> g_fenceWaitNs, g_acquireNs;  // time waited (vkdkTakeStats)
extern std::atomic<uint64_t> g_fenceWaitTotalNs;  // the same, never reset (vkdkFenceWaitTotalNs)
extern std::atomic<uint64_t> g_gpuBusyNs;
// vkdkDrawCensus: frames still to count (0 = off); CensusDraw notes one draw of cb, CensusFrameEnd closes a frame
extern std::atomic<int> g_censusFrames;
void CensusDraw(VkCommandBuffer cb, uint32_t instances);
void CensusFrameEnd();

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Dekopon-32: every entry point named in the log when sdmc:/switch/dekopon32/vkdk_trace exists (crash hunting:
// the log goes to boot32.log line by line).
extern bool g_trace;
#define VKDK_TRACE(fn)            \
  do {                            \
    if (vkdk::g_trace) {          \
      vkdk::Log("> %s", fn);      \
    }                             \
  } while (0)

// The first `limit` times only, for messages a broken stream could repeat at every draw.
#define VKDK_LOG_LIMITED(limit, ...)                         \
  do {                                                       \
    static std::atomic<uint32_t> vkdk_said_{0};              \
    if (vkdk_said_.fetch_add(1, std::memory_order_relaxed) < (limit)) ::vkdk::Log(__VA_ARGS__); \
  } while (0)

constexpr uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
constexpr uint64_t AlignUp64(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// One image descriptor table and one sampler descriptor table for the whole device: deko3d binds one of each, and a
// texture handle is (image id | sampler id << 20) into them. Descriptor sets take ranges of both.
// 2^17 image descriptors (4 MB; a handle holds 20 bits): Azahar keeps a texture set of 3 for every draw in flight.
constexpr uint32_t kImageDescriptors = 1u << 17;
constexpr uint32_t kSamplerDescriptors = 4096;  // the handle's 12 bits

// Fixed per-stage slots (tools/deko3d/bo2_dk_glsl.py writes the shaders to them). Push constants are a std140 UBO
// updated in the command stream with dkCmdBufPushConstants, pipelined like Vulkan's.
constexpr uint32_t kPushUboSlot = 5;
constexpr uint32_t kPushBytes = 256;

// Memory types (see GetPhysicalDeviceMemoryProperties).
enum MemoryType : uint32_t {
  kMemImage = 0,       // device local, images (DkMemBlockFlags_Image)
  kMemDevice = 1,      // device local buffers
  kMemUpload = 2,      // host visible, coherent (CPU uncached)
  kMemReadback = 3,    // host visible, cached, not coherent
  kMemTypeCount = 4,
};

// A first-fit allocator of index ranges (descriptor tables).
class RangeAllocator {
 public:
  explicit RangeAllocator(uint32_t size) { free_.push_back({0, size}); }
  // UINT32_MAX when full.
  uint32_t Allocate(uint32_t count);
  void Free(uint32_t first, uint32_t count);

 private:
  struct Range {
    uint32_t first, count;
  };
  std::vector<Range> free_;
};

// What a shader binds, from the "// vkdk" line the converter writes first (docs/deko3d.md).
struct BindRef {
  uint8_t set = 0xFF;
  uint8_t binding = 0;
  uint16_t element = 0;
};
struct ShaderMeta {
  struct Ubo {
    uint8_t slot;
    BindRef ref;
  };
  struct Tex {
    uint8_t slot;
    BindRef image;
    BindRef sampler;  // set 0xFF: none (texelFetch); the same as image for a combined image sampler
  };
  std::vector<Ubo> ubos;
  std::vector<Ubo> ssbos;
  std::vector<Ubo> images;  // storage images: img=SLOT:SET.BINDING.ELEM (Dekopon-32)
  std::vector<Tex> textures;
  bool push = false;       // reads the push constants' UBO (kPushUboSlot)
  bool bindless = false;   // makes texture handles itself (descriptor heaps)
  uint32_t vin[16];        // vertex inputs renumbered: vin[i] = the Vulkan location of input i
  uint32_t vinCount = 0;
  // Specialization constants: id -> type ('u' uint, 'i' int, 'f' float, 'b' bool).
  std::vector<std::pair<uint32_t, char>> spec;
  // Specialization constant id -> the bits the shader reads (absent: all of them).
  std::vector<std::pair<uint32_t, uint32_t>> specMask;
  bool Parse(const char* line, std::string& error);
};

struct ShaderSource {
  DkStage stage;
  std::string glsl;  // with the "// vkdk" line
  ShaderMeta meta;
  std::string name;  // for logs
};

// Dekopon-32: SPIR-V to the GLSL uam takes, with the vkdk line (vkdk_spirv.cpp). Null and `error` on failure.
std::shared_ptr<const ShaderSource> ConvertSpirv(const uint32_t* code, size_t bytes, std::string& error);
// Runs fn on vkdk's one 8 MB-stack worker thread (uam and SPIRV-Cross) and waits; false if that thread is missing.
bool RunOnBigStack(const std::function<void()>& fn);

// Compiled stage variants: DKSH loaded into code memory. Thread-safe.
struct Variant {
  DkShader shader;
  uint64_t key;
  ShaderMeta const* meta;
  std::shared_ptr<const struct ShaderSource> source;  // keeps meta alive (variants live as long as the device)
};

class ShaderCache;

// Formats a texel load gives back exactly as stored (unorm and float), so a draw that loads each source texel and writes
// it copies the bits: the formats of drawn copies (vkdk_cmd.cpp DrawnCopy).
inline bool DrawnCopyFormat(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
      return true;
    default:
      return false;
  }
}

}  // namespace vkdk

// ---- dispatchable objects

struct VkInstance_T {
  uint32_t api = VK_API_VERSION_1_2;
};

struct VkPhysicalDevice_T {
  VkInstance instance = nullptr;
};

struct VkQueue_T {
  VkDevice device = nullptr;
  DkQueue dk = nullptr;
  // Run before every submission: the GPU's descriptor, shader and texture caches and its L2 dropped, because the CPU
  // writes descriptors, shader code and upload memory between submissions without telling it.
  DkCmdBuf prologueBuf = nullptr;
  DkMemBlock prologueMem = nullptr;
  DkCmdList prologue = 0;
  std::mutex mutex;
  // The GPU's time on each submission (sdt_nx's dynamic resolution): a timestamp report before its work and one
  // after, into slot i of stampData (16-byte reports, the time at +8); read back once both are written (QueueSubmit
  // harvests them), the busy time added up (vkdkGpuBusyTotalNs). The begin report is written when the work before it
  // is done, so the time between is this submission's alone, without the GPU's idle gaps.
  static constexpr uint32_t kStampSlots = 64;
  DkCmdBuf stampBuf = nullptr;
  DkMemBlock stampMem = nullptr, stampData = nullptr;
  DkCmdList stampBegin[kStampSlots] = {}, stampEnd[kStampSlots] = {};
  bool stampUsed[kStampSlots] = {};
  uint32_t stampNext = 0;
};

struct VkDevice_T {
  VkPhysicalDevice physical = nullptr;
  DkDevice dk = nullptr;
  VkQueue_T queue;

  std::mutex blockMutex;  // dkMemBlockCreate / Destroy
  // Blocks let go, with the present count then (DestroyBlock): destroyed kRetireFrames presents later (RetireBlocks).
  static constexpr uint64_t kRetireFrames = 6;
  std::deque<std::pair<DkMemBlock, uint64_t>> retired;
  std::atomic<uint64_t> presents{0};
  void RetireBlocks(bool all);

  // Descriptor tables (vkdk_internal.h).
  DkMemBlock imageTable = nullptr;
  DkMemBlock samplerTable = nullptr;
  DkImageDescriptor* imageCpu = nullptr;
  DkSamplerDescriptor* samplerCpu = nullptr;
  DkGpuAddr imageGpu = 0, samplerGpu = 0;
  std::mutex tableMutex;
  vkdk::RangeAllocator imageRanges{vkdk::kImageDescriptors};
  vkdk::RangeAllocator samplerRanges{vkdk::kSamplerDescriptors};
  // One sampler table entry per distinct sampler state (the descriptor's bytes), with the samplers using it.
  std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> samplerStates;
  uint32_t defaultSamplerSlot = 0;
  uint32_t imagesInSets = 0, imagesInSetsLogged = 0;  // image entries held by descriptor sets (high-water log)
  // Raised by every descriptor write: a command buffer that sees it move drops the GPU's descriptor cache before its
  // next draw (the queue's prologue covers the start of each submission).
  std::atomic<uint64_t> descriptorEpoch{1};

  // The push constants' UBO (graphics stages; updated in the command stream).
  DkMemBlock pushBlock = nullptr;
  DkGpuAddr pushGpu = 0;

  // Shader code: bump-allocated 16 MB blocks.
  std::mutex codeMutex;
  std::vector<std::pair<DkMemBlock, uint32_t>> codeBlocks;
  std::atomic<uint64_t> codeEpoch{1};

  vkdk::ShaderCache* shaders = nullptr;
  // Colour copies drawn on the 3D engine (vkdk_cmd.cpp DrawnCopy): their two shaders, a ring of image descriptors for
  // their sources and one sampler.
  const vkdk::Variant* copyShaders[2] = {nullptr, nullptr};
  const vkdk::Variant* volumeCopyShader = nullptr;  // vkdk_cmd.cpp VolumeCopy
  uint32_t copyImageFirst = 0, copySampler = 0;
  std::atomic<uint32_t> copyNext{0};

  DkMemBlock MakeBlock(uint32_t size, uint32_t flags, const char* what);
  void DestroyBlock(DkMemBlock block);
  // Loads a DKSH into code memory. False on a malformed file or no memory.
  bool LoadDksh(const uint8_t* data, size_t size, DkShader& out);
};

// ---- non-dispatchable objects

struct VkDeviceMemory_T {
  DkMemBlock block = nullptr;
  uint32_t size = 0;
  uint32_t type = 0;
  uint8_t* cpu = nullptr;
  DkGpuAddr gpu = 0;
};

struct VkBuffer_T {
  uint64_t size = 0;
  VkBufferUsageFlags usage = 0;
  VkDeviceMemory memory = nullptr;
  uint64_t offset = 0;
  DkGpuAddr gpu = 0;
};

struct VkBufferView_T {
  VkBuffer buffer = nullptr;
  // Texel buffers (Dekopon-32: the PICA lighting, fog and procedural-texture LUTs): a deko3d buffer image over the
  // buffer's own memory, sampled with texelFetch from the image table like any texture.
  DkImageLayout layout{};
  DkImage image{};
  DkImageDescriptor descriptor{};
  bool valid = false;
};

struct VkImage_T {
  VkImageType type = VK_IMAGE_TYPE_2D;
  VkFormat format = VK_FORMAT_UNDEFINED;
  DkImageFormat dkFormat = DkImageFormat_None;
  DkImageType dkType = DkImageType_2D;
  VkExtent3D extent{};
  uint32_t mips = 1, layers = 1;
  VkImageCreateFlags flags = 0;
  VkImageUsageFlags usage = 0;
  DkImageLayout layout{};
  DkImage image{};
  uint64_t size = 0;
  uint32_t alignment = 0;
  bool bound = false;
  bool swapchain = false;  // owned by a VkSwapchainKHR_T
  bool depth = false, stencil = false;
  bool renderable = false;  // DkImageFlags_UsageRender: a drawn copy can write it
  bool storable = false;    // DkImageFlags_UsageLoadStore: a 3D copy target written by a compute pass (VolumeCopy)
};

struct VkImageView_T {
  VkImage image = nullptr;
  VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
  VkFormat format = VK_FORMAT_UNDEFINED;
  DkImageView view{};          // for sampling (the view's mips and layers)
  DkImageView target{};        // for rendering and copies (one mip, its layers)
  DkImageDescriptor descriptor{};
  uint32_t baseMip = 0, mipCount = 1, baseLayer = 0, layerCount = 1;
};

struct VkSampler_T {
  DkSamplerDescriptor descriptor{};
  uint32_t slot = 0;  // its entry in the sampler table, shared by every sampler with the same state
};

struct VkFence_T {
  DkFence fence{};
  // 0 unsignaled and not submitted, 1 submitted (the DkFence decides), 2 signaled.
  std::atomic<int> state{0};
};

struct VkSemaphore_T {
  DkFence fence{};
  bool pending = false;  // an acquire left its fence here for the next submission to wait on
};

struct VkEvent_T {
  std::atomic<int> set{0};  // host-side state only (vkSetEvent / vkGetEventStatus); the GPU's waits are barriers
};

struct VkQueryPool_T {
  VkQueryType type = VK_QUERY_TYPE_OCCLUSION;
  uint32_t count = 0;
  VkQueryPipelineStatisticFlags statistics = 0;
  DkMemBlock block = nullptr;
  uint8_t* cpu = nullptr;  // 16 bytes a query: the counter's report (value, timestamp)
  DkGpuAddr gpu = 0;
  // Begin reports for occlusion and statistics queries: the end report minus the begin report.
  uint32_t beginOffset = 0;
};

struct VkShaderModule_T {
  std::shared_ptr<const vkdk::ShaderSource> source;
};

struct VkPipelineCache_T {};

struct VkDescriptorSetLayout_T {
  struct Binding {
    bool valid = false;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLER;
    uint32_t count = 0;
    uint32_t image = 0;    // first index in the set's image range
    uint32_t sampler = 0;  // first index in the set's sampler range
    uint32_t buffer = 0;   // first index in the set's buffers
    int32_t dynamic = -1;  // index among the set's dynamic buffers
  };
  std::vector<Binding> bindings;  // by binding number
  uint32_t images = 0, samplers = 0, buffers = 0, dynamics = 0;
  bool variableLast = false;      // VARIABLE_DESCRIPTOR_COUNT on the last binding (not used by bo2: refused)
};

struct VkPipelineLayout_T {
  std::vector<VkDescriptorSetLayout_T> sets;  // copies: a layout may be destroyed while the pipeline layout lives
  uint32_t pushBytes = 0;
};

struct VkDescriptorPool_T {
  std::vector<VkDescriptorSet> sets;
};

struct VkDescriptorSet_T {
  VkDescriptorPool pool = nullptr;
  VkDescriptorSetLayout_T layout;
  uint32_t imageFirst = UINT32_MAX, samplerFirst = UINT32_MAX;
  // The sampler table entry of each sampler binding (VkSampler_T::slot): sets hold no sampler entries of their own,
  // the table's 4096 (the handle's 12 bits) ran out at ~1365 live 3-texture sets (MK7, round i: the freeze).
  std::vector<uint16_t> samplerSlots;
  std::vector<DkBufExtents> buffers;
  uint64_t generation = 0;  // raised when a buffer binding is written (command buffers compare it)
};

struct VkRenderPass_T {
  struct Attachment {
    VkFormat format;
    VkAttachmentLoadOp load, stencilLoad;
    bool depth, stencil;
  };
  std::vector<Attachment> attachments;
  // Subpass 0's targets (what CmdBeginRenderPass binds).
  uint32_t colorRefs[8];  // VK_ATTACHMENT_UNUSED for a gap
  uint32_t colorCount = 0;
  uint32_t depthRef = VK_ATTACHMENT_UNUSED;
  // Every subpass (sdt_nx: Unity's Android player): its targets and the attachments its colours resolve into (MSAA)
  // at its end, VK_ATTACHMENT_UNUSED for none.
  struct Subpass {
    uint32_t colorRefs[8];
    uint32_t resolveRefs[8];
    uint32_t colorCount = 0;
    uint32_t depthRef = VK_ATTACHMENT_UNUSED;
  };
  std::vector<Subpass> subpasses;
};

struct VkFramebuffer_T {
  std::vector<VkImageView> views;
  uint32_t width = 0, height = 0, layers = 1;
};

struct VkPipeline_T {
  bool compute = false;
  VkPipelineLayout_T layout;  // copy
  // Graphics: vertex and fragment (others are refused). Compute: [0].
  const vkdk::Variant* stages[2] = {nullptr, nullptr};
  uint32_t stageMask = 0;
  uint32_t uboStageMask[2] = {0, 0};  // for each of the two stages, the UBO slots its meta binds
  // State.
  DkPrimitive primitive = DkPrimitive_Triangles;
  bool restart = false;
  uint32_t patchSize = 0;
  DkRasterizerState rasterizer{};
  DkMultisampleState multisample{};
  DkColorState color{};
  DkColorWriteState colorWrite{};
  DkBlendState blend[8]{};
  uint32_t blendWords[8][7]{};
  uint32_t blendCount = 0;
  DkDepthStencilState depthStencil{};
  uint8_t stencilFront[3]{}, stencilBack[3]{};  // write mask, reference, compare mask
  float depthBias[3]{};                         // constant, clamp, slope
  float blendConstants[4]{};
  DkVtxAttribState attribs[DK_MAX_VERTEX_ATTRIBS]{};
  uint32_t attribCount = 0;
  DkVtxBufferState vtxBuffers[DK_MAX_VERTEX_BUFFERS]{};
  uint32_t vtxBufferCount = 0;
  uint32_t sampleMask = 0xFFFFFFFF;
  // Dynamic state flags (bit per VkDynamicState value below 32, see vkdk_pipeline.cpp).
  uint32_t dynamic = 0;
  VkViewport staticViewport{};
  VkRect2D staticScissor{};
  bool hasStaticViewport = false, hasStaticScissor = false;
  // For skipping groups already bound (vkdk_cmd.cpp).
  uint64_t groupHash[8]{};
};

struct VkCommandPool_T {
  VkDevice device = nullptr;
  std::vector<VkCommandBuffer> buffers;
};

struct VkSurfaceKHR_T {
  NWindow* window = nullptr;
};

struct VkSwapchainKHR_T {
  VkDevice device = nullptr;
  DkSwapchain dk = nullptr;
  DkMemBlock memory = nullptr;
  std::vector<VkImage> images;
  uint32_t width = 0, height = 0;
  VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
};

struct VkDebugUtilsMessengerEXT_T {};

namespace vkdk {

// Per command buffer state (vkdk_cmd.cpp).
struct BoundSet {
  VkDescriptorSet set = nullptr;
  uint32_t dynamicOffsets[16]{};
};

}  // namespace vkdk

struct VkCommandBuffer_T {
  // a secondary's render pass and framebuffer as begun (pInheritanceInfo): for the draw census only
  VkRenderPass inheritPass = nullptr;
  VkFramebuffer inheritFramebuffer = nullptr;
  VkDevice device = nullptr;
  VkCommandPool pool = nullptr;
  DkCmdBuf cmd = nullptr;
  std::vector<std::pair<DkMemBlock, uint32_t>> memory;  // command memory blocks, reused from the first
  size_t memoryUsed = 0;
  std::vector<std::pair<DkMemBlock, uint32_t>> transient;  // per-dispatch uniform data
  size_t transientBlock = 0;
  uint32_t transientUsed = 0;
  DkCmdList list = 0;
  bool recording = false;

  // State.
  VkPipeline graphics = nullptr, compute = nullptr;
  const VkPipeline_T* boundStatePipeline = nullptr;
  uint64_t boundGroups[8]{};
  const vkdk::Variant* boundShaders[2] = {nullptr, nullptr};
  vkdk::BoundSet sets[2][8];  // [graphics, compute][set]
  uint32_t setsDirty[2] = {0, 0};
  uint8_t push[vkdk::kPushBytes]{};
  uint32_t pushSize = 0;
  bool pushDirty = false;
  // What each graphics stage has bound (vertex, fragment): UBO extents and texture handles, to send only changes.
  DkBufExtents boundUbo[2][16]{};
  DkBufExtents boundSsbo[2][16]{};
  DkResHandle boundTex[2][32]{};
  uint32_t boundTexCount[2] = {0, 0};
  uint64_t seenDescriptorEpoch = 0;
  uint64_t seenCodeEpoch = 0;
  // Vertex and index buffers.
  DkBufExtents vtx[DK_MAX_VERTEX_BUFFERS]{};
  uint32_t vtxDirty = 0;
  DkGpuAddr indexAddr = 0;
  DkIdxFormat indexFormat = DkIdxFormat_Uint16;
  bool indexBound = false;
  uint8_t stencil[2][3]{};  // front, back: write mask, reference, compare mask
  bool restartBound = false;
  uint32_t restartIndex = 0;
  bool restartKnown = false;
  // Whether the 3D engine's base vertex (0x50D) and vertex id base (0x446) are known to be 0 in this command buffer:
  // deko3d's indexed draw sets both to its vertex offset and its plain draw leaves them (ZeroBaseVertex).
  bool baseVertexZero = false;
  // Dynamic state as last set (re-applied around clears).
  DkViewport viewports[DK_NUM_VIEWPORTS]{};
  VkViewport vkViewports[DK_NUM_VIEWPORTS]{};
  uint32_t viewportCount = 0;
  DkScissor scissors[DK_NUM_SCISSORS]{};
  uint32_t scissorCount = 0;
  // Render pass.
  VkRenderPass pass = nullptr;
  uint32_t subpass = 0;
  VkFramebuffer framebuffer = nullptr;
  uint32_t targetWidth = 0, targetHeight = 0;
  // Barriers held until the next command that does work, merged (plume_deko3d's mergeBarriers).
  DkBarrier pendingMode = DkBarrier_None;
  uint32_t pendingInvalidate = 0;
  bool pendingBarrier = false;
  // What the held barriers need when no copy-engine work is on either side of them: Full only because of a transfer
  // stage becomes Primitives (the 3D pipe idle, no wait for the other engines). copyEngineSince: copy-engine or 2D
  // work recorded since the last Full barrier (unknown at the start of a command buffer).
  DkBarrier pendingLean = DkBarrier_None;
  bool copyEngineSince = true;
  // Occlusion / statistics query open in this command buffer.
  VkQueryPool openQuery = nullptr;
  uint32_t openQueryIndex = 0;
};

namespace vkdk {

// vkdk_formats.cpp
DkImageFormat ToDkFormat(VkFormat format);
bool IsDepthFormat(VkFormat format);
bool HasStencil(VkFormat format);
bool IsCompressed(DkImageFormat format);
bool CanUse2DEngine(DkImageFormat format);
uint32_t FormatBlockBytes(VkFormat format, uint32_t& blockW, uint32_t& blockH);
bool VertexFormat(VkFormat format, DkVtxAttribSize& size, DkVtxAttribType& type, bool& bgra);
DkCompareOp ToDk(VkCompareOp op);
DkStencilOp ToDk(VkStencilOp op);
DkBlendFactor ToDk(VkBlendFactor f);
DkBlendOp ToDk(VkBlendOp op);
DkWrapMode ToDk(VkSamplerAddressMode m);
DkPrimitive ToDk(VkPrimitiveTopology t);
DkImageSwizzle ToDk(VkComponentSwizzle s, DkImageSwizzle identity);
uint32_t FormatFeatures(VkFormat format, bool optimal);
bool Renderable(DkImageFormat format);
// Diagnostic switches (Dekopon-32 round k): a file sdmc:/switch/dekopon32/<name> turns one on; vkdk_safe turns on
// every switch marked safe. Read once, logged.
bool DebugSwitch(const char* name, bool partOfSafe);
bool FormatSwizzle(VkFormat format, DkImageSwizzle out[4]);

// vkdk_pipeline.cpp
class ShaderCache {
 public:
  explicit ShaderCache(VkDevice device);
  ~ShaderCache();
  // The variant of a source for these specialization values; compiled by uam when not in memory or the file cache.
  // Blocks the calling thread for a compile. nullptr on failure (logged). With compile = false, nullptr and
  // *required = true instead of compiling (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT).
  const Variant* Get(const std::shared_ptr<const ShaderSource>& source, const VkSpecializationInfo* spec,
                     bool compile = true, bool* required = nullptr);
  // The built-in shader converted from this SPIR-V (vkdk_builtin_shaders.inc), or nullptr.
  std::shared_ptr<const ShaderSource> Builtin(const uint32_t* spirv, size_t bytes);
  void SetCachePath(const char* path);
  void SetPrebuiltPath(const char* path);  // vkdkSetPrebuiltShaderCachePath
  // A stage compiled from GLSL vkdk carries itself (drawn copies); blocks for the compile the first time.
  const Variant* FromGlsl(DkStage stage, const char* glsl, const char* name);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::shared_ptr<const ShaderSource> ParseShaderBlob(const void* data, size_t size, std::string& error);

void ApplyPendingCachePath(VkDevice device);
// vkdk_cmd.cpp: the drawn copies' shaders and descriptors (after the shader cache's files are open).
void SetUpDrawnCopies(VkDevice device);

// vkdk_cmd.cpp
void ResetCommandBufferState(VkCommandBuffer cb);
void DestroyCommandBuffer(VkCommandBuffer cb);

// vkdk_wsi.cpp
PFN_vkVoidFunction WsiProcAddr(const char* name);
// vkdk_android.cpp (sdt_nx): VK_KHR_android_surface and the rest of Vulkan 1.0 Unity's Android player loads
PFN_vkVoidFunction AndroidProcAddr(const char* name);

PFN_vkVoidFunction vkdk_cmd_proc(const char* name);
PFN_vkVoidFunction vkdk_pipeline_proc(const char* name);

// vkdk_core.cpp: the device's proc table (every vkCmd*, vkCreate*... by name).
PFN_vkVoidFunction DeviceProcAddr(const char* name);

uint64_t Hash64(const void* data, size_t size, uint64_t seed = 0);

}  // namespace vkdk
