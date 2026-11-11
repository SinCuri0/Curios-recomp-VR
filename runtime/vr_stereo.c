#include "vk.h"

#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vr_stereo_spv.h"

#define SLOTS 8
#define STALE_PRESENTS 8
#define DEFAULT_FAR_PERCENT 1.0f   // of the picture's width, per eye
#define DEFAULT_CONVERGENCE 4.0f   // view distance shown at the screen's own depth, game units
#define DEFAULT_P11 2.1445f        // 1 / tan(half of 50 degrees), until the game's projection was seen
#define MAX_SHIFT 0.04f            // of the picture's width, per eye
#define UNITS_PER_M 1.0f           // game units in one metre, before TOTK_VR_SCALE; as runtime/vr_camera.c

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler linear, nearest;
    VkImage img;
    VmaAllocation alloc;
    VkImageView view;
    uint32_t w, h;
    VkImageView src_view[SLOTS];
    unsigned seen;
    int mode;   // VR_*
    float far, conv, scale, p00, p11;
    uint64_t seq;
    int same, logged;
    uint64_t presents;
} S = {.seen = ~0u};

static int mode(void) {
    unsigned v = cfg_version();
    if (v != S.seen) {
        const char *e = cfg_get("TOTK_VR"), *d = cfg_get("TOTK_VR_DEPTH"), *k = cfg_get("TOTK_VR_CONVERGENCE");
        S.far = (d && *d ? (float)atof(d) : DEFAULT_FAR_PERCENT) / 100.0f;
        S.conv = k && *k ? (float)atof(k) : DEFAULT_CONVERGENCE;
        const char *sc = cfg_get("TOTK_VR_SCALE");
        S.scale = sc && *sc ? (float)atof(sc) : 1.0f;
        S.mode = !e || !*e || !strcmp(e, "0") ? VR_OFF : !strcmp(e, "sbs") ? VR_WINDOW : !strcmp(e, "screen") ? VR_SCREEN : VR_WORLD;
        S.seen = v;
    }
    return S.mode;
}

int vr_mode(void) { return mode(); }
int vr_stereo_wanted(void) { return mode() != VR_OFF; }
int vr_stereo_in_window(void) { return mode() == VR_WINDOW; }
void vr_stereo_fov(float *p00, float *p11) {
    if (!vr_camera_projection(p00, p11)) *p00 = S.p00, *p11 = S.p11;
}

static int init(VkCtx *c) {
    S.tried = 1;
    S.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[3] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 3, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &S.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkSamplerCreateInfo ni = si;
    ni.magFilter = ni.minFilter = VK_FILTER_NEAREST;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vr_stereo_spv,
                                   .pCode = vr_stereo_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .pName = "main"}};
    if (!S.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &S.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &S.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &S.linear) != VK_SUCCESS ||
        vkCreateSampler(c->device, &ni, NULL, &S.nearest) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) {
        rt_log("vr: depth 3D off: its pipeline could not be created");
        return 0;
    }
    ci.stage.module = m, ci.layout = S.layout;
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &S.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        rt_log("vr: depth 3D off: its pipeline could not be created");
        return 0;
    }
    return S.ok = 1;
}

static void free_image(VkCtx *c) {
    if (S.view) vkDestroyImageView(c->device, S.view, NULL);
    if (S.img) vmaDestroyImage(c->vma, S.img, S.alloc);
    S.view = VK_NULL_HANDLE, S.img = VK_NULL_HANDLE, S.w = S.h = 0;
}

static int ensure_image(VkCtx *c, uint32_t w, uint32_t h) {
    if (S.img && S.w == w && S.h == h) return 1;
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    free_image(c);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &S.img, &S.alloc, NULL) != VK_SUCCESS) {
        S.img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = S.img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &S.view) != VK_SUCCESS) {
        S.view = VK_NULL_HANDLE;
        free_image(c);
        return 0;
    }
    S.w = w, S.h = h;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                    VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

VkImage vr_stereo_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, const int crop[4]) {
    if (!mode()) return VK_NULL_HANDLE;
    VkCtx *c = vk_ctx();
    if (!S.tried) init(c);
    if (!S.ok || !ensure_image(c, 2u * (uint32_t)crop[2], (uint32_t)crop[3])) return VK_NULL_HANDLE;
    slot %= SLOTS;
    if (S.src_view[slot]) vkDestroyImageView(c->device, S.src_view[slot], NULL);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = src,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &S.src_view[slot]) != VK_SUCCESS) {
        S.src_view[slot] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    OotMotionFrame mo;
    uint64_t seq = 0;
    int have = oot_motion_peek(&mo, &seq);
    S.same = have && seq != S.seq ? 0 : S.same + 1;
    S.seq = seq;
    int flat = !have || S.same > STALE_PRESENTS || S.far <= 0.0f || mo.near_ <= 0.0f;
    struct { int32_t offset[2], size[2]; float lin[4], par[4]; } pc = {
        {crop[0], crop[1]}, {crop[2], crop[3]}, {0, 0, 1, 1}, {0, 0, 0, 0}};
    S.p11 = have && mo.p11 > 0.0f ? mo.p11 : S.p11 > 0.0f ? S.p11 : DEFAULT_P11;
    S.p00 = have && mo.p00 > 0.0f ? mo.p00 : S.p11 * (float)crop[3] / (float)crop[2];
    pc.par[3] = 1.0f;
    if (!flat) {
        pc.lin[0] = mo.lin_x, pc.lin[1] = mo.lin_y, pc.lin[2] = mo.near_;
        float wp00 = S.p00, wp11;
        vr_camera_projection(&wp00, &wp11);
        if (S.mode == VR_WORLD) pc.par[0] = 0.25f * vr_ipd() * UNITS_PER_M * S.scale * wp00, pc.par[1] = 0.0f, pc.par[2] = MAX_SHIFT;
        else pc.par[0] = S.far * S.conv, pc.par[1] = -S.far, pc.par[2] = S.far;
        pc.par[3] = 0.0f;
    }
    S.presents++;
    if (flat != S.logged - 1) {
        S.logged = flat + 1;
        if (flat) rt_log("vr: depth 3D: no scene depth at present %llu (depth copy %llu, %d presents old), both eyes get the same picture",
                         (unsigned long long)S.presents, (unsigned long long)seq, S.same);
        else rt_log("vr: depth 3D on: far points move %.2f%% of the width per eye, convergence %.2f, near %.4f, lin %.6f %.6f, P00 %.4f P11 %.4f, depth %ux%u, "
                    "picture %dx%d format %d, present %llu", S.far * 100.0f, S.conv, mo.near_, mo.lin_x, mo.lin_y, mo.p00, mo.p11, mo.w, mo.h, crop[2], crop[3], (int)fmt,
                    (unsigned long long)S.presents);
    }

    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, S.img, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    VkDescriptorImageInfo ii = {S.linear, S.src_view[slot], VK_IMAGE_LAYOUT_GENERAL},
                          di = {S.nearest, flat ? S.src_view[slot] : mo.depth, VK_IMAGE_LAYOUT_GENERAL},
                          oi = {VK_NULL_HANDLE, S.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe);
    rec_push(S.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.layout, 0, 3, w);
    vkCmdPushConstants(cb, S.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (S.w + 7) / 8, (S.h + 7) / 8, 1);
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, CS, 0, ALL, 0);
    barrier(cb, S.img, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT);
    return S.img;
}

void vr_stereo_release(void) {
    VkCtx *c = vk_ctx();
    for (int i = 0; i < SLOTS; i++)
        if (S.src_view[i]) vkDestroyImageView(c->device, S.src_view[i], NULL), S.src_view[i] = VK_NULL_HANDLE;
    free_image(c);
    if (S.pipe) vkDestroyPipeline(c->device, S.pipe, NULL);
    if (S.linear) vkDestroySampler(c->device, S.linear, NULL);
    if (S.nearest) vkDestroySampler(c->device, S.nearest, NULL);
    if (S.layout) vkDestroyPipelineLayout(c->device, S.layout, NULL);
    if (S.dsl) vkDestroyDescriptorSetLayout(c->device, S.dsl, NULL);
    S.pipe = VK_NULL_HANDLE, S.linear = S.nearest = VK_NULL_HANDLE, S.layout = VK_NULL_HANDLE, S.dsl = VK_NULL_HANDLE;
    S.tried = S.ok = 0;
}
