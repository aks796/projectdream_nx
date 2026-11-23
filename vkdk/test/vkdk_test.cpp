// vkdk self-test: a few seconds on the console or in an emulator check the paths bo2's renderer depends on, against
// known pixels, through vkdk's Vulkan entry points exactly as the SDK reaches them. Writes sdmc:/switch/vkdk_test.log
// and shows green (all passed) or red (something failed) for three seconds.
//
//   1. a render pass clear                         5. push constants (a std140 UBO in deko3d)
//   2. Vulkan's viewport orientation (y down)      6. an occlusion query
//   3. a texture through a descriptor heap         7. a buffer-to-image upload and an image-to-buffer readback
//      (bindless handle from UBO words)            8. uam compiling at run time, DKSH loaded
//   4. a bound texture (set, binding, element)
//
// Copyright 2026 aks796. GPL-3.0.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>
#include <switch.h>
#include <vkdk.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <string>
#include <vector>

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);

namespace {

FILE* g_log = nullptr;
bool g_consoleGone = false;  // after consoleExit, printf would draw into a console that is gone
int g_failed = 0, g_passed = 0;

void Say(const char* fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (!g_consoleGone) printf("%s\n", line);
  if (g_log) {
    fprintf(g_log, "%s\n", line);
    fflush(g_log);
  }
}

void Check(bool ok, const char* what) {
  Say("%s  %s", ok ? "PASS" : "FAIL", what);
  ok ? ++g_passed : ++g_failed;
}

#define F(name) PFN_##name name = nullptr;
#define DEVICE_FUNCS(X)                                                                                         \
  X(vkCreateDevice) X(vkGetDeviceQueue) X(vkAllocateMemory) X(vkMapMemory) X(vkCreateBuffer)                    \
  X(vkBindBufferMemory) X(vkCreateImage) X(vkBindImageMemory) X(vkGetImageMemoryRequirements)                   \
  X(vkCreateImageView) X(vkCreateSampler) X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool)              \
  X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) X(vkCreateShaderModule)       \
  X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkCreateGraphicsPipelines) X(vkCreateCommandPool)              \
  X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBeginRenderPass)             \
  X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers)               \
  X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) X(vkCmdPipelineBarrier)             \
  X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCreateQueryPool)  \
  X(vkGetQueryPoolResults) X(vkCreateFence) X(vkQueueSubmit) X(vkWaitForFences) X(vkResetFences)                 \
  X(vkInvalidateMappedMemoryRanges) X(vkCreateViSurfaceNN) X(vkCreateSwapchainKHR) X(vkGetSwapchainImagesKHR)   \
  X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCmdBlitImage) X(vkCreateSemaphore) X(vkCreateInstance)       \
  X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFeatures)
DEVICE_FUNCS(F)
#undef F

void Load() {
#define L(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
  DEVICE_FUNCS(L)
#undef L
}

std::vector<uint32_t> Blob(const char* glsl, uint32_t stage) { return vkdk::MakeBlob(glsl, stage); }

const char* kVs = R"(#version 460
// vkdk push=1
layout(location = 0) in vec2 inPos;
layout(location = 0) out vec2 vUV;
layout(std140, binding = 5) uniform Push { vec4 offset; vec4 color; } pc;
void main() { vUV = inPos * 0.5 + 0.5; gl_Position = vec4(inPos + pc.offset.xy, 0.0, 1.0); }
)";

// mode 0: push colour; 1: heap texture (bindless, words from the UBO); 2: bound texture
const char* kFs = R"(#version 460
// vkdk ubo=0:1.0 tex=0:0.0.5/0.1.0 push=1 bindless=1
#extension GL_ARB_bindless_texture : require
#define dkh(t, s) uvec2(uint(t), 0u)
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(std140, binding = 5) uniform Push { vec4 offset; vec4 color; } pc;
layout(binding = 0, std140) uniform Words { uvec4 w; } u;
layout(binding = 0) uniform sampler2D dk_tex_b0;
void main() {
  uint mode = uint(pc.offset.w);
  if (mode == 0u) outColor = pc.color;
  else if (mode == 1u) outColor = texture(sampler2D(dkh(u.w.x, u.w.y)), vUV);
  else outColor = texture(dk_tex_b0, vUV);
}
)";

struct Mem {
  VkDeviceMemory memory;
  uint8_t* cpu;
};

}  // namespace

int main() {
  consoleInit(nullptr);
  g_log = fopen("sdmc:/switch/vkdk_test.log", "w");
  vkdkSetLogCallback([](const char* line) { Say("[vkdk] %s", line); });
  // A fresh cache every run: uam compiles on the console each time, and check 9 sees a pipeline not compiled yet.
  remove("sdmc:/switch/vkdk_test_dksh.bin");
  vkdkSetShaderCachePath("sdmc:/switch/vkdk_test_dksh.bin");
  Say("vkdk self-test");
  Load();

  VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  VkInstance instance;
  vkCreateInstance(&ii, nullptr, &instance);
  uint32_t n = 1;
  VkPhysicalDevice pd;
  vkEnumeratePhysicalDevices(instance, &n, &pd);
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  VkDevice dev;
  Check(vkCreateDevice(pd, &di, nullptr, &dev) == VK_SUCCESS, "device");
  VkQueue queue;
  vkGetDeviceQueue(dev, 0, 0, &queue);

  auto alloc = [&](VkDeviceSize size, uint32_t type) {
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = size;
    ai.memoryTypeIndex = type;
    Mem m{};
    vkAllocateMemory(dev, &ai, nullptr, &m.memory);
    if (type >= 2) vkMapMemory(dev, m.memory, 0, size, 0, reinterpret_cast<void**>(&m.cpu));
    return m;
  };
  auto buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, Mem& m, VkDeviceSize offset) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VkBuffer b;
    vkCreateBuffer(dev, &bi, nullptr, &b);
    vkBindBufferMemory(dev, b, m.memory, offset);
    return b;
  };
  auto image = [&](uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageView& view) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = usage;
    VkImage img;
    vkCreateImage(dev, &ci, nullptr, &img);
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    Mem m = alloc(r.size, 0);
    vkBindImageMemory(dev, img, m.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev, &vi, nullptr, &view);
    return img;
  };

  constexpr uint32_t W = 64, H = 64;
  VkImageView targetView, texView;
  VkImage target = image(W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, targetView);
  VkImage tex = image(2, 2, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, texView);
  Mem upload = alloc(1 << 16, 2), readback = alloc(1 << 16, 3);
  VkBuffer vbuf = buffer(256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, upload, 0);
  VkBuffer ubuf = buffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, upload, 256);
  VkBuffer tbuf = buffer(256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, upload, 512);
  VkBuffer rbuf = buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback, 0);
  // 2x2 texture: red, green / blue, white.
  const uint32_t texels[4] = {0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF};
  memcpy(upload.cpu + 512, texels, sizeof(texels));

  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler;
  vkCreateSampler(dev, &si, nullptr, &sampler);

  // Set 0: a small heap (16 sampled images, 4 samplers), as bo2's; set 1: the words UBO.
  VkDescriptorSetLayoutBinding b0[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16, VK_SHADER_STAGE_ALL, nullptr},
                                        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 4, VK_SHADER_STAGE_ALL, nullptr}};
  VkDescriptorSetLayoutBinding b1 = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL, nullptr};
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  VkDescriptorSetLayout layouts[2];
  li.bindingCount = 2;
  li.pBindings = b0;
  vkCreateDescriptorSetLayout(dev, &li, nullptr, &layouts[0]);
  li.bindingCount = 1;
  li.pBindings = &b1;
  vkCreateDescriptorSetLayout(dev, &li, nullptr, &layouts[1]);
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  VkDescriptorPool pool;
  vkCreateDescriptorPool(dev, &pi, nullptr, &pool);
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool;
  ai.descriptorSetCount = 2;
  ai.pSetLayouts = layouts;
  VkDescriptorSet sets[2];
  vkAllocateDescriptorSets(dev, &ai, sets);
  VkDescriptorImageInfo imgInfo{nullptr, texView, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo smpInfo{sampler, nullptr, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorBufferInfo bufInfo{ubuf, 0, 16};
  VkWriteDescriptorSet w[4] = {};
  for (auto& x : w) x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[0].dstSet = sets[0]; w[0].dstBinding = 0; w[0].dstArrayElement = 3; w[0].descriptorCount = 1;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[0].pImageInfo = &imgInfo;
  w[1] = w[0]; w[1].dstArrayElement = 5;
  w[2].dstSet = sets[0]; w[2].dstBinding = 1; w[2].dstArrayElement = 0; w[2].descriptorCount = 1;
  w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; w[2].pImageInfo = &smpInfo;
  w[3].dstSet = sets[1]; w[3].dstBinding = 0; w[3].descriptorCount = 1;
  w[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[3].pBufferInfo = &bufInfo;
  vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
  // The word a bo2 shader reads: the whole handle, the image's table id (heap element 3) | the sampler's id << 20.
  const uint32_t words[4] = {(vkdkDescriptorSetImageBase(reinterpret_cast<uint64_t>(sets[0])) + 3) |
                                 ((vkdkDescriptorSetSamplerBase(reinterpret_cast<uint64_t>(sets[0])) + 0) << 20),
                             0, 0, 0};
  memcpy(upload.cpu + 256, words, sizeof(words));

  VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 2;
  pli.pSetLayouts = layouts;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &range;
  VkPipelineLayout playout;
  vkCreatePipelineLayout(dev, &pli, nullptr, &playout);

  VkAttachmentDescription att{};
  att.format = VK_FORMAT_R8G8B8A8_UNORM;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_GENERAL};
  VkSubpassDescription sub{};
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1;
  rpi.pAttachments = &att;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sub;
  VkRenderPass pass;
  vkCreateRenderPass(dev, &rpi, nullptr, &pass);
  VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fi.renderPass = pass;
  fi.attachmentCount = 1;
  fi.pAttachments = &targetView;
  fi.width = W;
  fi.height = H;
  fi.layers = 1;
  VkFramebuffer fb;
  vkCreateFramebuffer(dev, &fi, nullptr, &fb);

  const auto vsBlob = Blob(kVs, 0), fsBlob = Blob(kFs, 4);
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  VkShaderModule vs, fs;
  mi.codeSize = vsBlob.size() * 4;
  mi.pCode = vsBlob.data();
  vkCreateShaderModule(dev, &mi, nullptr, &vs);
  mi.codeSize = fsBlob.size() * 4;
  mi.pCode = fsBlob.data();
  vkCreateShaderModule(dev, &mi, nullptr, &fs);
  VkPipelineShaderStageCreateInfo st[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
  VkVertexInputBindingDescription vb{0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription va{0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
  VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vin.vertexBindingDescriptionCount = 1;
  vin.pVertexBindingDescriptions = &vb;
  vin.vertexAttributeDescriptionCount = 1;
  vin.pVertexAttributeDescriptions = &va;
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;
  const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2;
  gp.pStages = st;
  gp.pVertexInputState = &vin;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &ds;
  gp.layout = playout;
  gp.renderPass = pass;
  // 9. VK_EXT_pipeline_creation_cache_control, as bo2's renderer uses it: asked for from the cache only, a pipeline
  // whose shaders are not compiled yet comes back VK_PIPELINE_COMPILE_REQUIRED (the renderer then compiles it on a
  // thread of its own); once made, the same request returns it at once.
  VkPipeline from_cache = VK_NULL_HANDLE;
  gp.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
  const VkResult before = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &from_cache);
  const bool none_before = from_cache == VK_NULL_HANDLE;
  gp.flags = 0;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const uint64_t t0 = armTicksToNs(armGetSystemTick());
  Check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline) == VK_SUCCESS && pipeline,
        "pipeline (uam compiled both stages, DKSH loaded)");
  Say("      pipeline made in %.1f ms", (armTicksToNs(armGetSystemTick()) - t0) / 1e6);
  gp.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
  const uint64_t t1 = armTicksToNs(armGetSystemTick());
  const VkResult after = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &from_cache);
  Say("      before compiling: %d, after: %d in %.2f ms", int(before), int(after),
      (armTicksToNs(armGetSystemTick()) - t1) / 1e6);
  gp.flags = 0;
  Check(before == VK_PIPELINE_COMPILE_REQUIRED_EXT && none_before && after == VK_SUCCESS && from_cache,
        "9. compile required, then from the cache (pipeline creation cache control)");

  // Quads: full screen (two triangles), and the top half in Vulkan's clip space (y -1..0).
  const float verts[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1,  // full
                         -1, -1, 1, -1, -1, 0, -1, 0, 1, -1, 1, 0};  // top half
  memcpy(upload.cpu, verts, sizeof(verts));

  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qi.queryType = VK_QUERY_TYPE_OCCLUSION;
  qi.queryCount = 1;
  VkQueryPool query;
  vkCreateQueryPool(dev, &qi, nullptr, &query);
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  VkCommandPool cpool;
  vkCreateCommandPool(dev, &cpi, nullptr, &cpool);
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = cpool;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(dev, &cai, &cmd);
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  vkCreateFence(dev, &fci, nullptr, &fence);

  // One pass: clear, then draw the given vertices with a mode; read the target back.
  auto run = [&](int mode, uint32_t first, bool withQuery, const float color[4]) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);
    VkBufferImageCopy up{};
    up.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    up.imageExtent = {2, 2, 1};
    vkCmdCopyBufferToImage(cmd, tbuf, tex, VK_IMAGE_LAYOUT_GENERAL, 1, &up);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    VkClearValue clear{};
    clear.color.float32[0] = 0.25f;
    clear.color.float32[1] = 0.5f;
    clear.color.float32[2] = 0.75f;
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = pass;
    rb.framebuffer = fb;
    rb.renderArea = {{0, 0}, {W, H}};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    if (withQuery) vkCmdBeginQuery(cmd, query, 0, 0);
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    if (mode >= 0) {
      const VkViewport v{0, 0, float(W), float(H), 0, 1};
      const VkRect2D s{{0, 0}, {W, H}};
      vkCmdSetViewport(cmd, 0, 1, &v);
      vkCmdSetScissor(cmd, 0, 1, &s);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      const uint32_t dynOffset = 0;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 2, sets, 1, &dynOffset);
      const VkDeviceSize off = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &off);
      const float push[8] = {0, 0, 0, float(mode), color[0], color[1], color[2], color[3]};
      vkCmdPushConstants(cmd, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                         push);
      vkCmdDraw(cmd, 6, 1, first, 0);
    }
    vkCmdEndRenderPass(cmd);
    if (withQuery) vkCmdEndQuery(cmd, query, 0);
    VkMemoryBarrier rb2{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    rb2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    rb2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                         &rb2, 0, nullptr, 0, nullptr);
    VkBufferImageCopy down{};
    down.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    down.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_GENERAL, rbuf, 1, &down);
    VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0,
                         nullptr);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si2{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si2.commandBufferCount = 1;
    si2.pCommandBuffers = &cmd;
    vkResetFences(dev, 1, &fence);
    vkQueueSubmit(queue, 1, &si2, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    mr.memory = readback.memory;
    mr.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(dev, 1, &mr);
  };
  auto pixel = [&](uint32_t x, uint32_t y) {
    uint32_t p;
    memcpy(&p, readback.cpu + (y * W + x) * 4, 4);
    return p;
  };
  auto near = [](uint32_t a, uint32_t b) {
    for (int i = 0; i < 32; i += 8) {
      if (std::abs(int((a >> i) & 0xFF) - int((b >> i) & 0xFF)) > 2) return false;
    }
    return true;
  };
  const float magenta[4] = {1, 0, 1, 1};
  const uint32_t clearPx = 0xFFBF8040;  // (0.25, 0.5, 0.75, 1) as RGBA8 little-endian

  run(-1, 0, false, magenta);
  Say("      cleared pixel %08x", pixel(5, 5));
  Check(near(pixel(5, 5), clearPx), "1. render pass clear");

  run(0, 6, true, magenta);
  Say("      top %08x bottom %08x", pixel(32, 8), pixel(32, 56));
  Check(near(pixel(32, 8), 0xFFFF00FF), "5. push constants (colour)");
  Check(near(pixel(32, 56), clearPx), "2. viewport: clip y -1..0 is the top half (Vulkan's y down)");
  uint64_t samples = 0;
  vkGetQueryPoolResults(dev, query, 0, 1, sizeof(samples), &samples, 8, VK_QUERY_RESULT_64_BIT);
  Say("      occlusion samples %llu (want %u)", (unsigned long long)samples, W * H / 2);
  Check(samples == W * H / 2, "6. occlusion query");

  run(1, 0, false, magenta);
  Say("      heap texture: %08x %08x %08x %08x", pixel(8, 8), pixel(56, 8), pixel(8, 56), pixel(56, 56));
  // Texel (0,0) red at uv (0,0): uv = pos*0.5+0.5, pos y -1 at the top row.
  Check(near(pixel(8, 8), 0xFF0000FF) && near(pixel(56, 8), 0xFF00FF00) && near(pixel(8, 56), 0xFFFF0000) &&
            near(pixel(56, 56), 0xFFFFFFFF),
        "3+7. heap texture through a bindless handle, after an upload");

  run(2, 0, false, magenta);
  Say("      bound texture: %08x %08x", pixel(8, 8), pixel(56, 56));
  Check(near(pixel(8, 8), 0xFF0000FF) && near(pixel(56, 56), 0xFFFFFFFF), "4. bound texture (set 0, element 5)");

  {
    // A colour copy between two images of one format is drawn on the 3D engine (vkdk_cmd.cpp DrawnCopy), into a
    // destination that is no render target: the bits as they were, at the offsets asked for.
    run(1, 0, false, magenta);
    std::vector<uint32_t> before(W * H);
    memcpy(before.data(), readback.cpu, W * H * 4);
    VkImageView copyView;
    VkImage copyDst = image(W, H, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT, copyView);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);
    VkMemoryBarrier b1{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b1.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b1,
                         0, nullptr, 0, nullptr);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.srcOffset = {16, 8, 0};
    c.dstOffset = {4, 12, 0};
    c.extent = {40, 48, 1};
    vkCmdCopyImage(cmd, target, VK_IMAGE_LAYOUT_GENERAL, copyDst, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    VkMemoryBarrier b2{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b2, 0, nullptr, 0,
                         nullptr);
    VkBufferImageCopy down{};
    down.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    down.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, copyDst, VK_IMAGE_LAYOUT_GENERAL, rbuf, 1, &down);
    VkMemoryBarrier b3{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b3.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b3.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b3, 0, nullptr, 0,
                         nullptr);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkResetFences(dev, 1, &fence);
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    mr.memory = readback.memory;
    mr.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(dev, 1, &mr);
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < c.extent.height; ++y) {
      for (uint32_t x = 0; x < c.extent.width; ++x) {
        wrong += pixel(c.dstOffset.x + x, c.dstOffset.y + y) != before[(c.srcOffset.y + y) * W + c.srcOffset.x + x];
      }
    }
    Say("      drawn copy: %u of %u pixels different; (4,12) %08x from (16,8) %08x", wrong,
        c.extent.width * c.extent.height, pixel(4, 12), before[8 * W + 16]);
    Check(wrong == 0, "10. image copy drawn on the 3D engine, exact, at its offsets");
  }

  {
    // Many 8x8 blocks of a 2D image into a 3D one (the game's lighting volume) are one compute pass (vkdk_cmd.cpp
    // VolumeCopy): every texel where its block says, exact.
    std::vector<uint32_t> before(W * H);
    memcpy(before.data(), readback.cpu, W * H * 4);  // check 10's copy of the target, still in the readback
    run(1, 0, false, magenta);
    memcpy(before.data(), readback.cpu, W * H * 4);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {16, 16, 2};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage vol;
    vkCreateImage(dev, &ci, nullptr, &vol);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, vol, &req);
    Mem vm = alloc(req.size, 0);
    vkBindImageMemory(dev, vol, vm.memory, 0);
    std::vector<VkImageCopy> blocks;
    for (int32_t z = 0; z < 2; ++z) {
      for (int32_t by = 0; by < 16; by += 8) {
        for (int32_t bx = 0; bx < 16; bx += 8) {
          VkImageCopy c{};
          c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          const int32_t n = int32_t(blocks.size());
          c.srcOffset = {(n * 13) % 56, (n * 29) % 56, 0};  // scattered over the four colours of the target
          c.dstOffset = {bx, by, z};
          c.extent = {8, 8, 1};
          blocks.push_back(c);
        }
      }
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);
    VkMemoryBarrier b1{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b1.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b1,
                         0, nullptr, 0, nullptr);
    // VKDK_TEST_ENGINE_VOLUME: one block a call, so the copy engine does them (the reference for this check).
    if (getenv("VKDK_TEST_ENGINE_VOLUME") || access("sdmc:/switch/vkdk_test_engine_volume", F_OK) == 0) {
      Say("      (blocks one by one, on the copy engine)");
      for (const VkImageCopy& c : blocks) {
        vkCmdCopyImage(cmd, target, VK_IMAGE_LAYOUT_GENERAL, vol, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
      }
    } else {
      vkCmdCopyImage(cmd, target, VK_IMAGE_LAYOUT_GENERAL, vol, VK_IMAGE_LAYOUT_GENERAL, uint32_t(blocks.size()),
                     blocks.data());
    }
    VkMemoryBarrier b2{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b2, 0, nullptr, 0,
                         nullptr);
    VkBufferImageCopy down{};
    down.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    down.imageExtent = {16, 16, 2};
    vkCmdCopyImageToBuffer(cmd, vol, VK_IMAGE_LAYOUT_GENERAL, rbuf, 1, &down);
    VkMemoryBarrier b3{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b3.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b3.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b3, 0, nullptr, 0,
                         nullptr);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkResetFences(dev, 1, &fence);
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    mr.memory = readback.memory;
    mr.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(dev, 1, &mr);
    uint32_t wrong = 0;
    for (const VkImageCopy& c : blocks) {
      for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
          uint32_t got;
          memcpy(&got, readback.cpu + ((c.dstOffset.z * 16 + c.dstOffset.y + y) * 16 + c.dstOffset.x + x) * 4, 4);
          const uint32_t want = before[(c.srcOffset.y + y) * W + c.srcOffset.x + x];
          if (got != want && wrong < 6) {
            Say("      (%d,%d,%d): %08x, want %08x from (%d,%d)", c.dstOffset.x + int32_t(x), c.dstOffset.y + int32_t(y),
                c.dstOffset.z, got, want, c.srcOffset.x + int32_t(x), c.srcOffset.y + int32_t(y));
          }
          wrong += got != want;
        }
      }
    }
    Say("      volume copy: %u of %u texels different", wrong, uint32_t(blocks.size()) * 64);
    Check(wrong == 0, "11. 8x8 blocks of a 2D image into a 3D one as one compute pass, exact");
  }

  {
    // 12. An ASTC 4x4 texture (Project Dream's Android build transcodes the game's textures to ASTC 4x4): 8x8 texels,
    // 2x2 blocks, each a void-extent block of one colour -- red, green / blue, white, as the RGBA8 texture above --
    // uploaded and sampled through binding element 5.
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    ci.extent = {8, 8, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(pd, ci.format, &fp);
    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceFeatures(pd, &feats);
    Say("      ASTC 4x4: textureCompressionASTC_LDR %u, optimal features %08x", feats.textureCompressionASTC_LDR,
        fp.optimalTilingFeatures);
    VkImage astc = VK_NULL_HANDLE;
    VkImageView astcView = VK_NULL_HANDLE;
    bool made = feats.textureCompressionASTC_LDR && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                vkCreateImage(dev, &ci, nullptr, &astc) == VK_SUCCESS;
    if (made) {
      VkMemoryRequirements r;
      vkGetImageMemoryRequirements(dev, astc, &r);
      Mem m = alloc(r.size, 0);
      vkBindImageMemory(dev, astc, m.memory, 0);
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = astc;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = ci.format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      made = vkCreateImageView(dev, &vi, nullptr, &astcView) == VK_SUCCESS;
    }
    if (made) {
      // void-extent LDR block: the low 64 bits 0xFFFFFFFFFFFFFDFC, then R, G, B, A as UNORM16
      auto block = [](uint8_t* out, uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
        const uint64_t lo = 0xFFFFFFFFFFFFFDFCull;
        const uint64_t hi = uint64_t(r) | (uint64_t(g) << 16) | (uint64_t(b) << 32) | (uint64_t(a) << 48);
        memcpy(out, &lo, 8);
        memcpy(out + 8, &hi, 8);
      };
      uint8_t* blocks = upload.cpu + 512 + 64;  // tbuf + 64
      block(blocks + 0, 0xFFFF, 0, 0, 0xFFFF);
      block(blocks + 16, 0, 0xFFFF, 0, 0xFFFF);
      block(blocks + 32, 0, 0, 0xFFFF, 0xFFFF);
      block(blocks + 48, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF);
      VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vkBeginCommandBuffer(cmd, &bi);
      VkBufferImageCopy up{};
      up.bufferOffset = 64;
      up.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      up.imageExtent = {8, 8, 1};
      vkCmdCopyBufferToImage(cmd, tbuf, astc, VK_IMAGE_LAYOUT_GENERAL, 1, &up);
      vkEndCommandBuffer(cmd);
      VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      s.commandBufferCount = 1;
      s.pCommandBuffers = &cmd;
      vkResetFences(dev, 1, &fence);
      vkQueueSubmit(queue, 1, &s, fence);
      vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
      VkDescriptorImageInfo astcInfo{nullptr, astcView, VK_IMAGE_LAYOUT_GENERAL};
      VkWriteDescriptorSet aw = w[1];
      aw.pImageInfo = &astcInfo;
      vkUpdateDescriptorSets(dev, 1, &aw, 0, nullptr);
      run(2, 0, false, magenta);
      Say("      ASTC texture: %08x %08x %08x %08x", pixel(8, 8), pixel(56, 8), pixel(8, 56), pixel(56, 56));
      made = near(pixel(8, 8), 0xFF0000FF) && near(pixel(56, 8), 0xFF00FF00) && near(pixel(8, 56), 0xFFFF0000) &&
             near(pixel(56, 56), 0xFFFFFFFF);
      vkUpdateDescriptorSets(dev, 1, &w[1], 0, nullptr);  // element 5 back to the RGBA8 texture
    }
    Check(made, "12. ASTC 4x4 texture (advertised, uploaded, sampled)");
  }

  Say("%d passed, %d failed", g_passed, g_failed);

  // The verdict on screen for three seconds: green or red, through the swapchain.
  VkViSurfaceCreateInfoNN vsi{VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
  vsi.window = nwindowGetDefault();
  VkSurfaceKHR surface;
  vkCreateViSurfaceNN(instance, &vsi, nullptr, &surface);
  consoleExit(nullptr);
  g_consoleGone = true;
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface;
  sci.minImageCount = 2;
  sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
  sci.imageExtent = {1280, 720};
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  VkSwapchainKHR swapchain;
  if (vkCreateSwapchainKHR(dev, &sci, nullptr, &swapchain) == VK_SUCCESS) {
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(dev, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(dev, swapchain, &count, images.data());
    const float verdict[4] = {g_failed ? 0.8f : 0.1f, g_failed ? 0.1f : 0.7f, 0.1f, 1.0f};
    run(0, 0, false, verdict);  // the verdict colour in the 64x64 target, then stretched onto the screen
    for (int frame = 0; frame < 180; ++frame) {
      uint32_t index;
      VkSemaphore ready = VK_NULL_HANDLE;
      vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, ready, VK_NULL_HANDLE, &index);
      VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vkBeginCommandBuffer(cmd, &bi);
      VkImageBlit blit{};
      blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[1] = {int32_t(W), int32_t(H), 1};
      blit.dstOffsets[1] = {1280, 720, 1};
      vkCmdBlitImage(cmd, target, VK_IMAGE_LAYOUT_GENERAL, images[index], VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                     VK_FILTER_NEAREST);
      vkEndCommandBuffer(cmd);
      VkSubmitInfo si3{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si3.commandBufferCount = 1;
      si3.pCommandBuffers = &cmd;
      vkResetFences(dev, 1, &fence);
      vkQueueSubmit(queue, 1, &si3, fence);
      VkPresentInfoKHR pinfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
      pinfo.swapchainCount = 1;
      pinfo.pSwapchains = &swapchain;
      pinfo.pImageIndices = &index;
      vkQueuePresentKHR(queue, &pinfo);
      vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    }
  }
  if (g_log) fclose(g_log);
  return g_failed ? 1 : 0;
}
