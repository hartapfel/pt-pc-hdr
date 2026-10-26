#include "engine/render/exposure_math.h"
#include <SDL3/SDL.h>
#include <glm/gtc/packing.hpp>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "engine/core/log.h"
#include "engine/core/resource_path.h"
#include "engine/render/d3d12/backend/agility_exports.h"
#include "engine/render/d3d12/backend/d3d12_backend.h"
#include "engine/render/gpu_types.h"
#include "engine/render/mesh.h"
#include "engine/render/render_util.h"
#include "engine/render/subsurface_pass.h"
#include "engine/render/texture_manager.h"
#include "engine/render/vk/vk_device.h"
#include "engine/render/vk_context.h"

PT_D3D12_AGILITY_EXPORTS()

namespace {

using namespace pt;
namespace fs = std::filesystem;

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    pt::LogInfo("{} {}", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

struct Texture {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips = 1;
    gpu::Format format = gpu::Format::R8G8B8A8Unorm;
    std::vector<std::vector<uint8_t>> levels;
};

uint32_t Hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

Texture MakeRgba(uint32_t w, uint32_t h, uint32_t mips, const std::function<std::array<uint8_t, 4>(uint32_t, uint32_t)>& texel) {
    Texture t;
    t.width = w;
    t.height = h;
    t.mips = mips;
    std::vector<uint8_t> base(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const auto c = texel(x, y);
            std::memcpy(&base[(static_cast<size_t>(y) * w + x) * 4], c.data(), 4);
        }
    }
    t.levels.push_back(std::move(base));
    for (uint32_t m = 1; m < mips; ++m) {
        const uint32_t pw = std::max(w >> (m - 1), 1u);
        const uint32_t ph = std::max(h >> (m - 1), 1u);
        const uint32_t lw = std::max(w >> m, 1u);
        const uint32_t lh = std::max(h >> m, 1u);
        const std::vector<uint8_t>& prev = t.levels.back();
        std::vector<uint8_t> level(static_cast<size_t>(lw) * lh * 4);
        for (uint32_t y = 0; y < lh; ++y) {
            for (uint32_t x = 0; x < lw; ++x) {
                for (int c = 0; c < 4; ++c) {
                    uint32_t sum = 0;
                    for (uint32_t dy = 0; dy < 2; ++dy) {
                        for (uint32_t dx = 0; dx < 2; ++dx) {
                            const uint32_t sx = std::min(x * 2 + dx, pw - 1);
                            const uint32_t sy = std::min(y * 2 + dy, ph - 1);
                            sum += prev[(static_cast<size_t>(sy) * pw + sx) * 4 + c];
                        }
                    }
                    level[(static_cast<size_t>(y) * lw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
        t.levels.push_back(std::move(level));
    }
    return t;
}

Texture MakeR8(uint32_t w, uint32_t h, const std::function<uint8_t(uint32_t, uint32_t)>& texel) {
    Texture t;
    t.width = w;
    t.height = h;
    t.format = gpu::Format::R8Unorm;
    std::vector<uint8_t> base(static_cast<size_t>(w) * h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            base[static_cast<size_t>(y) * w + x] = texel(x, y);
        }
    }
    t.levels.push_back(std::move(base));
    return t;
}

uint8_t U8(float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

Texture SceneTexture() {
    return MakeRgba(256, 144, 1, [](uint32_t x, uint32_t y) {
        const float u = x / 255.0f;
        const float v = y / 143.0f;
        const bool checker = ((x / 8) + (y / 8)) % 2 == 0;
        const float lamp = std::exp(-((u - 0.7f) * (u - 0.7f) + (v - 0.3f) * (v - 0.3f)) * 30.0f);
        return std::array<uint8_t, 4>{U8(0.15f + 0.6f * u + lamp), U8(0.1f + 0.5f * v + (checker ? 0.08f : 0.0f) + 0.7f * lamp),
                                      U8(0.3f * (1.0f - u) + 0.4f * lamp), 255};
    });
}

Texture GrainTexture() {
    return MakeRgba(512, 512, 8, [](uint32_t x, uint32_t y) {
        const uint32_t h = Hash(x * 73856093u ^ y * 19349663u);
        return std::array<uint8_t, 4>{static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h >> 16), 255};
    });
}

Texture UiTopTexture() {
    return MakeRgba(256, 144, 1, [](uint32_t x, uint32_t y) {
        const float dx = (x - 60.0f) / 30.0f;
        const float dy = (y - 70.0f) / 30.0f;
        float a = std::clamp(1.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
        if (y > 120 && y < 130 && x > 20 && x < 230) {
            a = 0.8f;
        }
        return std::array<uint8_t, 4>{U8(0.9f * a), U8(0.85f * a), U8(0.7f * a), U8(a)};
    });
}

Texture UiImageTexture() {
    return MakeRgba(128, 128, 1, [](uint32_t x, uint32_t y) {
        const bool checker = ((x / 16) + (y / 16)) % 2 == 0;
        return std::array<uint8_t, 4>{U8(x / 127.0f), U8(checker ? 0.9f : 0.2f), U8(y / 127.0f), U8(0.5f + 0.5f * (x / 127.0f))};
    });
}

Texture UiCoverageTexture() {
    return MakeR8(64, 32, [](uint32_t x, uint32_t y) {
        const bool stroke = (x % 9 < 1 + (y % 4)) || (y > 14 && y < 17);
        return static_cast<uint8_t>(stroke ? 255 : (x % 9 == 4 ? 90 : 0));
    });
}

Texture UiNoiseTexture() {
    return MakeRgba(64, 64, 1, [](uint32_t x, uint32_t y) {
        const uint32_t h = Hash(x * 2654435761u + y * 40503u);
        return std::array<uint8_t, 4>{static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h >> 16), static_cast<uint8_t>(h >> 24)};
    });
}

enum class Bind { Combined, TextureArray, TexSampler, StorageBuffer };

struct Binding {
    uint32_t set = 0;
    uint32_t binding = 0;
    Bind kind = Bind::Combined;
    std::vector<uint32_t> textures;
    uint32_t sampler = 0;
    std::vector<uint8_t> data;
};

struct Draw {
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 3;
    std::vector<uint8_t> push;
};

struct Case {
    std::string name;
    const char* vertex = "fullscreen.vert";
    const char* fragment = nullptr;
    gpu::Format color = gpu::Format::R8G8B8A8Unorm;
    uint32_t width = 320;
    uint32_t height = 180;
    std::vector<Texture> textures;
    std::vector<gpu::SamplerDesc> samplers;
    std::vector<Binding> bindings;
    uint32_t push_bytes = 0;
    std::vector<Draw> draws;
    std::vector<float> vertices;
    std::vector<gpu::Format> attributes;
    uint32_t stride = 0;
    gpu::CullMode cull = gpu::CullMode::None;
    gpu::BlendMode blend = gpu::BlendMode::None;
    std::array<float, 4> clear = {0.0f, 0.0f, 0.0f, 0.0f};
};

uint32_t SetCount(const Case& c) {
    uint32_t n = 0;
    for (const Binding& b : c.bindings) {
        n = std::max(n, b.set + 1);
    }
    return n;
}

template <class T>
void Put(std::vector<uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

std::vector<uint8_t> Floats(std::initializer_list<float> values) {
    std::vector<uint8_t> out;
    for (float f : values) {
        Put(out, f);
    }
    return out;
}

gpu::SamplerDesc LinearClamp() {
    gpu::SamplerDesc s;
    s.address_u = s.address_v = s.address_w = gpu::AddressMode::ClampToEdge;
    return s;
}

gpu::SamplerDesc LinearRepeat() {
    return gpu::SamplerDesc{};
}

Case CompositeCase(bool mode1) {
    Case c;
    c.name = mode1 ? "composite_grain_ui" : "composite_grade_fade";
    c.fragment = "composite.frag";
    c.textures = {SceneTexture(), GrainTexture(), UiTopTexture()};
    c.samplers = {LinearClamp(), LinearRepeat()};
    c.bindings = {{0, 0, Bind::Combined, {0}, 0}, {0, 1, Bind::Combined, {1}, 1}, {0, 2, Bind::Combined, {2}, 0}};
    c.push_bytes = 128;
    if (mode1) {
        c.draws = {{0, 3, Floats({1.0f, 1.15f, 1.0f, 0.0f, 0, 0, 0, 0, 1.0f, 0.0f, 0.7f, 0.0f, 0.137f, 0.291f, 1.2f, 0.0f, 0, 0, 0, 1.0f})}};
    } else {
        c.draws = {{0, 3, Floats({1.0f, 1.0f, 0.0f, 2.0f, 0.05f, 0.05f, 0.2f, 0.5f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})}};
    }
    for (auto& draw : c.draws) draw.push.resize(128, 0);
    return c;
}

Case XrCopyCase() {
    Case c;
    c.name = "xr_copy_srgb";
    c.fragment = "xr_copy.frag";
    c.color = gpu::Format::R8G8B8A8Srgb;
    c.textures = {SceneTexture(), GrainTexture()};
    c.samplers = {LinearClamp(), LinearRepeat()};
    c.bindings = {{0, 0, Bind::Combined, {0}, 0}, {0, 1, Bind::Combined, {1}, 1}};
    c.push_bytes = 64;
    c.draws = {{0, 3, Floats({0.1f, 0.05f, 0.8f, 0.9f, 0.0f, 1.2f, 0, 0, 1.0f, 1.0f, 0.5f, 0, 0.31f, 0.77f, 1.0f, 0})}};
    return c;
}

Case CullCase(gpu::CullMode cull) {
    Case c = CompositeCase(false);
    c.name = cull == gpu::CullMode::Back ? "cull_back" : "cull_front";
    c.cull = cull;
    c.clear = {1.0f, 0.0f, 1.0f, 1.0f};
    return c;
}

struct UiDraw {
    uint32_t textures[4] = {};
    float m[7][4] = {};
    float screen[4] = {};
    float extra[4] = {};
    float extra2[4] = {};
};
static_assert(sizeof(UiDraw) == 176);

Case UiCase(gpu::Format color = gpu::Format::R8G8B8A8Unorm, uint32_t width = 320, uint32_t height = 180) {
    Case c;
    c.name = "ui_sprite";
    c.vertex = "ui_sprite.vert";
    c.fragment = "ui_sprite.frag";
    c.color = color;
    c.width = width;
    c.height = height;
    c.textures = {UiImageTexture(), UiCoverageTexture(), UiNoiseTexture(), SceneTexture()};
    c.samplers = {LinearRepeat(), LinearClamp()};
    c.blend = gpu::BlendMode::Alpha;
    c.clear = {0.08f, 0.1f, 0.14f, 1.0f};
    c.attributes = {gpu::Format::R32G32Sfloat, gpu::Format::R32G32Sfloat, gpu::Format::R32G32B32A32Sfloat};
    c.stride = 32;
    std::vector<UiDraw> draws;
    const float sx = width / 320.0f;
    const float sy = height / 180.0f;
    auto quad = [&](float x0, float y0, float x1, float y1, std::array<float, 4> col) {
        const float px[6][4] = {{x0, y0, 0, 0}, {x1, y0, 1, 0}, {x1, y1, 1, 1}, {x0, y0, 0, 0}, {x1, y1, 1, 1}, {x0, y1, 0, 1}};
        for (const auto& v : px) {
            c.vertices.insert(c.vertices.end(), {v[0] * sx, v[1] * sy, v[2], v[3], col[0], col[1], col[2], col[3]});
        }
    };
    auto add = [&](float x0, float y0, float x1, float y1, std::array<float, 4> col, const UiDraw& d) {
        Draw draw;
        draw.first_vertex = static_cast<uint32_t>(c.vertices.size() / 8);
        quad(x0, y0, x1, y1, col);
        draw.vertex_count = 6;
        Put(draw.push, 1.0f / width);
        Put(draw.push, 1.0f / height);
        Put(draw.push, static_cast<uint32_t>(draws.size()));
        Put(draw.push, 0u);
        draw.push.resize(64, 0);
        Put(draw.push, 1.0f); // UI brightness ratio, inactive in this SDR fixture.
        draw.push.resize(80, 0);
        draws.push_back(d);
        c.draws.push_back(std::move(draw));
    };
    UiDraw base;
    base.m[1][0] = base.m[1][1] = 1.0f;
    base.m[2][2] = base.m[2][3] = 1.0f;
    base.m[4][0] = base.m[4][1] = 1.0f;
    base.m[5][2] = base.m[5][3] = 1.0f;
    base.screen[2] = 1.0f;
    UiDraw material = base;
    material.textures[0] = 0;
    material.textures[1] = 2;
    material.textures[2] = 1;
    material.textures[3] = 2;
    material.m[6][1] = 0.35f;
    material.m[6][2] = 0.6f;
    material.screen[3] = 0.0f;
    add(10, 10, 110, 80, {1.0f, 0.95f, 0.9f, 1.0f}, material);
    UiDraw text = base;
    text.textures[0] = 1;
    text.screen[3] = 1.0f;
    add(120, 10, 310, 50, {0.95f, 0.9f, 0.6f, 1.0f}, text);
    UiDraw border = base;
    border.textures[0] = 1;
    border.screen[3] = 5.0f;
    border.extra[0] = border.extra[1] = 4.0f / 64.0f;
    border.extra[2] = 1.0f;
    add(120, 60, 310, 100, {1, 1, 1, 1}, border);
    UiDraw noise = base;
    noise.textures[0] = 2;
    noise.textures[1] = 2;
    noise.screen[3] = 2.0f;
    noise.extra[0] = 0.21f;
    noise.extra[1] = 0.43f;
    noise.extra[2] = 0.3f;
    noise.extra[3] = 0.9f;
    noise.extra2[0] = 1.0f;
    noise.extra2[1] = 1.0f;
    noise.extra2[2] = 1.0f;
    add(10, 90, 110, 170, {1, 1, 1, 1}, noise);
    UiDraw string = base;
    string.textures[0] = 0;
    string.screen[3] = 3.0f;
    string.extra[3] = 1.0f;
    add(120, 110, 220, 170, {1, 0.6f, 0.6f, 1}, string);
    UiDraw flat = base;
    flat.screen[3] = 4.0f;
    add(200, 30, 300, 160, {0.2f, 0.6f, 1.0f, 0.4f}, flat);
    std::vector<uint8_t> bytes(draws.size() * sizeof(UiDraw));
    std::memcpy(bytes.data(), draws.data(), bytes.size());
    c.bindings = {{0, 0, Bind::TextureArray, {0, 1, 2}, 0},
                  {0, 3, Bind::TexSampler, {}, 0},
                  {1, 0, Bind::StorageBuffer, {}, 0, bytes},
                  {1, 1, Bind::Combined, {3}, 1}};
    c.push_bytes = 80;
    return c;
}

struct Picture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

class VulkanRunner {
public:
    bool Init() {
        if (!ctx_.Init(nullptr, false)) {
            return false;
        }
        return true;
    }
    ~VulkanRunner() {
        if (ctx_.device) {
            ctx_.Shutdown();
        }
    }
    std::string Adapter() const { return ctx_.properties.deviceName; }
    vk::Context& Context() { return ctx_; }

    bool Run(const Case& c, Picture& out) {
        VkDevice dev = ctx_.device;
        std::vector<vk::Image> textures(c.textures.size());
        for (size_t i = 0; i < c.textures.size(); ++i) {
            if (!Upload(c.textures[i], textures[i])) {
                return false;
            }
        }
        std::vector<VkSampler> samplers;
        for (const gpu::SamplerDesc& s : c.samplers) {
            VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            info.magFilter = static_cast<VkFilter>(s.mag);
            info.minFilter = static_cast<VkFilter>(s.min);
            info.mipmapMode = static_cast<VkSamplerMipmapMode>(s.mipmap);
            info.addressModeU = static_cast<VkSamplerAddressMode>(s.address_u);
            info.addressModeV = static_cast<VkSamplerAddressMode>(s.address_v);
            info.addressModeW = static_cast<VkSamplerAddressMode>(s.address_w);
            info.maxLod = VK_LOD_CLAMP_NONE;
            VkSampler sampler = VK_NULL_HANDLE;
            vkCreateSampler(dev, &info, nullptr, &sampler);
            samplers.push_back(sampler);
        }
        vk::Buffer storage;
        const uint32_t sets = SetCount(c);
        std::vector<VkDescriptorSetLayout> layouts(sets);
        for (uint32_t s = 0; s < sets; ++s) {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            for (const Binding& b : c.bindings) {
                if (b.set != s || b.kind == Bind::TexSampler) {
                    continue;
                }
                VkDescriptorSetLayoutBinding lb{b.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr};
                if (b.kind == Bind::TextureArray) {
                    lb.descriptorCount = static_cast<uint32_t>(b.textures.size());
                } else if (b.kind == Bind::StorageBuffer) {
                    lb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                }
                bindings.push_back(lb);
            }
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = static_cast<uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            vkCreateDescriptorSetLayout(dev, &info, nullptr, &layouts[s]);
        }
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8}};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = 8;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(dev, &pool_info, nullptr, &pool);
        std::vector<VkDescriptorSet> set_handles(sets);
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool;
        alloc.descriptorSetCount = sets;
        alloc.pSetLayouts = layouts.data();
        vkAllocateDescriptorSets(dev, &alloc, set_handles.data());
        for (const Binding& b : c.bindings) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set_handles[b.set];
            w.dstBinding = b.binding;
            std::vector<VkDescriptorImageInfo> images;
            VkDescriptorBufferInfo buffer_info{};
            if (b.kind == Bind::TexSampler) {
                continue;
            }
            if (b.kind == Bind::StorageBuffer) {
                ctx_.CreateBuffer(storage, b.data.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
                std::memcpy(storage.mapped, b.data.data(), b.data.size());
                buffer_info = {storage.buffer, 0, VK_WHOLE_SIZE};
                w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w.descriptorCount = 1;
                w.pBufferInfo = &buffer_info;
            } else {
                for (uint32_t t : b.textures) {
                    images.push_back({samplers[b.sampler], textures[t].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                }
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.descriptorCount = static_cast<uint32_t>(images.size());
                w.pImageInfo = images.data();
            }
            vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        }
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, std::max(c.push_bytes, 4u)};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.setLayoutCount = sets;
        layout_info.pSetLayouts = layouts.data();
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev, &layout_info, nullptr, &layout);
        VkPipeline pipeline = CreatePipeline(c, layout);
        vk::Buffer vertices;
        if (!c.vertices.empty()) {
            ctx_.CreateBuffer(vertices, c.vertices.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
            std::memcpy(vertices.mapped, c.vertices.data(), c.vertices.size() * sizeof(float));
        }
        RenderTarget target;
        ctx_.CreateImage(target.image, static_cast<VkFormat>(c.color), {c.width, c.height, 1},
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        vk::Buffer readback;
        const uint32_t bytes = c.width * c.height * 4;
        ctx_.CreateBuffer(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        bool ok = pipeline != VK_NULL_HANDLE;
        if (ok) {
            ctx_.Submit([&](VkCommandBuffer cmd) {
                UseTargets(cmd, {{&target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
                ColorOutput out{&target, true, {}};
                std::memcpy(out.clear_value.float32, c.clear.data(), sizeof(float) * 4);
                BeginPass(cmd, VkExtent2D{c.width, c.height}, {out});
                vkCmdSetCullMode(cmd, static_cast<VkCullModeFlags>(c.cull));
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, sets, set_handles.data(), 0, nullptr);
                if (vertices.buffer) {
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &offset);
                }
                for (const Draw& d : c.draws) {
                    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       static_cast<uint32_t>(d.push.size()), d.push.data());
                    vkCmdDraw(cmd, d.vertex_count, 1, d.first_vertex, 0);
                }
                vkCmdEndRendering(cmd);
                UseTargets(cmd, {{&target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}});
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {c.width, c.height, 1};
                vkCmdCopyImageToBuffer(cmd, gpu::vk::Native(target.image.image), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
            });
            out.width = c.width;
            out.height = c.height;
            out.rgba.assign(static_cast<uint8_t*>(readback.mapped), static_cast<uint8_t*>(readback.mapped) + bytes);
        }
        vkDeviceWaitIdle(dev);
        ctx_.DestroyBuffer(readback);
        ctx_.DestroyImage(target.image);
        if (vertices.buffer) {
            ctx_.DestroyBuffer(vertices);
        }
        if (storage.buffer) {
            ctx_.DestroyBuffer(storage);
        }
        if (pipeline) {
            vkDestroyPipeline(dev, pipeline, nullptr);
        }
        vkDestroyPipelineLayout(dev, layout, nullptr);
        vkDestroyDescriptorPool(dev, pool, nullptr);
        for (VkDescriptorSetLayout l : layouts) {
            vkDestroyDescriptorSetLayout(dev, l, nullptr);
        }
        for (VkSampler s : samplers) {
            vkDestroySampler(dev, s, nullptr);
        }
        for (vk::Image& t : textures) {
            ctx_.DestroyImage(t);
        }
        return ok;
    }

private:
    bool Upload(const Texture& t, vk::Image& image) {
        if (!ctx_.CreateImage(image, static_cast<VkFormat>(t.format), {t.width, t.height, 1},
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, t.mips)) {
            return false;
        }
        size_t total = 0;
        for (const auto& l : t.levels) {
            total += l.size();
        }
        vk::Buffer staging;
        ctx_.CreateBuffer(staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::vector<VkBufferImageCopy> regions;
        size_t offset = 0;
        for (uint32_t m = 0; m < t.mips; ++m) {
            std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, t.levels[m].data(), t.levels[m].size());
            VkBufferImageCopy r{};
            r.bufferOffset = offset;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            r.imageExtent = {std::max(t.width >> m, 1u), std::max(t.height >> m, 1u), 1};
            regions.push_back(r);
            offset += t.levels[m].size();
        }
        ctx_.Submit([&](VkCommandBuffer cmd) {
            vk::ImageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()),
                                   regions.data());
            vk::ImageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        ctx_.DestroyBuffer(staging);
        return true;
    }

    VkPipeline CreatePipeline(const Case& c, VkPipelineLayout layout) {
        VkShaderModule vert = vk::LoadShaderModule(ctx_.device, c.vertex);
        VkShaderModule frag = vk::LoadShaderModule(ctx_.device, c.fragment);
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vert && frag) {
            VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                         {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vert;
            stages[0].pName = "main";
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = frag;
            stages[1].pName = "main";
            VkVertexInputBindingDescription binding{0, c.stride, VK_VERTEX_INPUT_RATE_VERTEX};
            std::vector<VkVertexInputAttributeDescription> attributes;
            uint32_t offset = 0;
            for (size_t i = 0; i < c.attributes.size(); ++i) {
                attributes.push_back({static_cast<uint32_t>(i), 0, static_cast<VkFormat>(c.attributes[i]), offset});
                offset += gpu::Describe(c.attributes[i]).bytes;
            }
            VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            if (!attributes.empty()) {
                vertex_input.vertexBindingDescriptionCount = 1;
                vertex_input.pVertexBindingDescriptions = &binding;
                vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
                vertex_input.pVertexAttributeDescriptions = attributes.data();
            }
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = 1;
            viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            VkPipelineColorBlendAttachmentState blend_attachment{};
            blend_attachment.colorWriteMask = 0xF;
            if (c.blend == gpu::BlendMode::Alpha) {
                blend_attachment.blendEnable = VK_TRUE;
                blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            }
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1;
            blend.pAttachments = &blend_attachment;
            VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = 4;
            dynamic.pDynamicStates = dynamic_states;
            const VkFormat format = static_cast<VkFormat>(c.color);
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &format;
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.pNext = &rendering;
            info.stageCount = 2;
            info.pStages = stages;
            info.pVertexInputState = &vertex_input;
            info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewport;
            info.pRasterizationState = &raster;
            info.pMultisampleState = &multisample;
            info.pDepthStencilState = &depth;
            info.pColorBlendState = &blend;
            info.pDynamicState = &dynamic;
            info.layout = layout;
            vkCreateGraphicsPipelines(ctx_.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        }
        if (vert) {
            vkDestroyShaderModule(ctx_.device, vert, nullptr);
        }
        if (frag) {
            vkDestroyShaderModule(ctx_.device, frag, nullptr);
        }
        return pipeline;
    }

    vk::Context ctx_;
};

class GpuRunner {
public:
    explicit GpuRunner(gpu::Device& device) : dev_(device) {}

    struct Prepared {
        std::vector<gpu::ImageResource> textures;
        std::vector<gpu::SamplerHandle> samplers;
        std::vector<gpu::BindingLayoutHandle> layouts;
        gpu::BindingPoolHandle pool;
        std::vector<gpu::BindingSetHandle> sets;
        gpu::PipelineLayoutHandle layout;
        gpu::PipelineHandle pipeline;
        gpu::BufferResource vertices;
        gpu::BufferResource storage;
    };

    bool Prepare(const Case& c, Prepared& p) {
        for (const Texture& t : c.textures) {
            gpu::ImageResource image;
            if (!Upload(t, image)) {
                return false;
            }
            p.textures.push_back(image);
        }
        for (const gpu::SamplerDesc& s : c.samplers) {
            p.samplers.push_back(dev_.CreateSampler(s));
        }
        const uint32_t sets = SetCount(c);
        for (uint32_t s = 0; s < sets; ++s) {
            gpu::BindingLayoutDesc desc;
            for (const Binding& b : c.bindings) {
                if (b.set != s) {
                    continue;
                }
                gpu::BindingDesc d;
                d.binding = b.binding;
                switch (b.kind) {
                case Bind::Combined: d.type = gpu::BindingType::CombinedImageSampler; break;
                case Bind::TextureArray:
                    d.type = gpu::BindingType::CombinedImageSampler;
                    d.count = static_cast<uint32_t>(b.textures.size());
                    d.flags = gpu::BindingFlag::PartiallyBound;
                    break;
                case Bind::TexSampler: d.type = gpu::BindingType::Sampler; break;
                case Bind::StorageBuffer: d.type = gpu::BindingType::StorageBuffer; break;
                }
                desc.bindings.push_back(d);
            }
            p.layouts.push_back(dev_.CreateBindingLayout(desc));
        }
        gpu::BindingPoolDesc pool;
        pool.sizes = {{gpu::BindingType::CombinedImageSampler, 64}, {gpu::BindingType::StorageBuffer, 8}, {gpu::BindingType::Sampler, 8}};
        pool.max_sets = 8;
        p.pool = dev_.CreateBindingPool(pool);
        for (uint32_t s = 0; s < sets; ++s) {
            p.sets.push_back(dev_.AllocateBindingSet(p.pool, p.layouts[s]));
            if (!p.sets.back()) {
                return false;
            }
        }
        std::vector<gpu::BindingWrite> writes;
        std::vector<std::vector<gpu::ImageBinding>> images(c.bindings.size());
        std::vector<gpu::BufferBinding> buffers(c.bindings.size());
        for (size_t i = 0; i < c.bindings.size(); ++i) {
            const Binding& b = c.bindings[i];
            gpu::BindingWrite w;
            w.set = p.sets[b.set];
            w.binding = b.binding;
            if (b.kind == Bind::TexSampler) {
                w.type = gpu::BindingType::Sampler;
                w.samplers = std::span(&p.samplers[b.sampler], 1);
            } else if (b.kind == Bind::StorageBuffer) {
                gpu::BufferDesc bd;
                bd.size = b.data.size();
                bd.usage = gpu::BufferUsage::StorageBuffer;
                bd.host_visible = true;
                bd.name = "ui draws";
                bd.gpu_read_only = true;
                dev_.CreateBuffer(bd, p.storage);
                std::memcpy(p.storage.mapped, b.data.data(), b.data.size());
                dev_.FlushMapped(p.storage, 0, b.data.size());
                buffers[i] = {p.storage.buffer, 0, ~0ull};
                w.type = gpu::BindingType::StorageBuffer;
                w.buffers = std::span(&buffers[i], 1);
            } else {
                for (uint32_t t : b.textures) {
                    gpu::ImageBinding ib;
                    ib.view = p.textures[t].view;
                    ib.image = &p.textures[t];
                    ib.sampler = p.samplers[b.sampler];
                    images[i].push_back(ib);
                }
                w.type = gpu::BindingType::CombinedImageSampler;
                w.images = images[i];
            }
            writes.push_back(w);
        }
        dev_.WriteBindings(writes);
        gpu::PipelineLayoutDesc ld;
        ld.sets = p.layouts;
        ld.push_bytes = std::max(c.push_bytes, 4u);
        ld.push_stages = gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment;
        p.layout = dev_.CreatePipelineLayout(ld);
        gpu::GraphicsPipelineDesc pd;
        pd.vertex = c.vertex;
        pd.fragment = c.fragment;
        pd.layout = p.layout;
        pd.colors = {c.color};
        pd.blend = c.blend;
        uint32_t offset = 0;
        for (gpu::Format f : c.attributes) {
            pd.vertex_attributes.push_back({f, offset});
            offset += gpu::Describe(f).bytes;
        }
        pd.vertex_stride = c.stride;
        p.pipeline = dev_.CreateGraphicsPipeline(pd);
        if (!c.vertices.empty()) {
            gpu::BufferDesc bd;
            bd.size = c.vertices.size() * sizeof(float);
            bd.usage = gpu::BufferUsage::VertexBuffer;
            bd.host_visible = true;
            bd.name = "test vertices";
            dev_.CreateBuffer(bd, p.vertices);
            std::memcpy(p.vertices.mapped, c.vertices.data(), bd.size);
        }
        return static_cast<bool>(p.pipeline);
    }

    void Record(gpu::CommandList& list, const Case& c, const Prepared& p, gpu::RenderTarget& target, const std::array<float, 4>& clear) {
        list.UseTargets({{&target, gpu::Layout::ColorAttachment}});
        gpu::ColorOutput out{&target, true, {}};
        std::memcpy(out.clear_value.float32, clear.data(), sizeof(float) * 4);
        list.BeginPass(gpu::Extent2D{c.width, c.height}, {out});
        list.SetCullMode(c.cull);
        list.BindPipeline(gpu::BindPoint::Graphics, p.pipeline);
        list.BindSets(gpu::BindPoint::Graphics, p.layout, 0, p.sets);
        if (p.vertices.Valid()) {
            list.BindVertexBuffer(p.vertices.buffer, 0);
        }
        for (const Draw& d : c.draws) {
            list.PushConstants(p.layout, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment, 0, static_cast<uint32_t>(d.push.size()), d.push.data());
            list.Draw(d.vertex_count, 1, d.first_vertex, 0);
        }
        list.EndPass();
    }

    void Release(Prepared& p) {
        dev_.WaitIdle();
        for (gpu::ImageResource& t : p.textures) {
            dev_.DestroyImage(t);
        }
        for (gpu::SamplerHandle s : p.samplers) {
            dev_.DestroySampler(s);
        }
        dev_.DestroyBindingPool(p.pool);
        for (gpu::BindingLayoutHandle l : p.layouts) {
            dev_.DestroyBindingLayout(l);
        }
        dev_.DestroyPipeline(p.pipeline);
        dev_.DestroyPipelineLayout(p.layout);
        dev_.DestroyBuffer(p.vertices);
        dev_.DestroyBuffer(p.storage);
    }

    bool Run(const Case& c, Picture& out) {
        Prepared p;
        if (!Prepare(c, p)) {
            Release(p);
            return false;
        }
        gpu::RenderTarget target;
        gpu::ImageDesc td;
        td.format = c.color;
        td.extent = {c.width, c.height, 1};
        td.usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::TransferSrc;
        td.name = "test target";
        dev_.CreateImage(td, target.image);
        dev_.Submit([&](gpu::CommandList& list) {
            Record(list, c, p, target, c.clear);
            list.UseTargets({{&target, gpu::Layout::TransferSrc}});
        });
        std::vector<uint8_t> pixels;
        const bool ok = dev_.ReadImage(target, pixels);
        out.width = c.width;
        out.height = c.height;
        out.rgba = std::move(pixels);
        dev_.DestroyImage(target.image);
        Release(p);
        return ok && out.rgba.size() == static_cast<size_t>(c.width) * c.height * 4;
    }

    bool Upload(const Texture& t, gpu::ImageResource& image, gpu::ImageUsageFlags usage = {}) {
        gpu::ImageDesc d;
        d.format = t.format;
        d.extent = {t.width, t.height, 1};
        d.usage = gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferDst | usage;
        d.mip_levels = t.mips;
        d.name = "test texture";
        if (!dev_.CreateImage(d, image)) {
            return false;
        }
        size_t total = 0;
        for (const auto& l : t.levels) {
            total += l.size();
        }
        gpu::BufferResource staging;
        gpu::BufferDesc bd;
        bd.size = total;
        bd.usage = gpu::BufferUsage::TransferSrc;
        bd.host_visible = true;
        bd.name = "test staging";
        if (!dev_.CreateBuffer(bd, staging)) {
            return false;
        }
        std::vector<gpu::BufferImageCopy> regions;
        size_t offset = 0;
        for (uint32_t m = 0; m < t.mips; ++m) {
            std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, t.levels[m].data(), t.levels[m].size());
            gpu::BufferImageCopy r;
            r.buffer_offset = offset;
            r.image.mip = m;
            r.extent = {std::max(t.width >> m, 1u), std::max(t.height >> m, 1u), 1};
            regions.push_back(r);
            offset += t.levels[m].size();
        }
        dev_.Submit([&](gpu::CommandList& list) {
            gpu::ImageBarrier b;
            b.image = image.image;
            b.new_layout = gpu::Layout::TransferDst;
            list.Barrier(b);
            list.CopyBufferToImage(staging, image, gpu::Layout::TransferDst, regions);
            b.old_layout = gpu::Layout::TransferDst;
            b.new_layout = gpu::Layout::ShaderReadOnly;
            list.Barrier(b);
        });
        dev_.DestroyBuffer(staging);
        return true;
    }

private:
    gpu::Device& dev_;
};

constexpr uint32_t kSssWidth = 320;
constexpr uint32_t kSssHeight = 180;

Texture SssLight() {
    return MakeRgba(kSssWidth, kSssHeight, 1, [](uint32_t x, uint32_t y) {
        const uint32_t h = Hash(x * 2246822519u ^ y * 3266489917u);
        const float u = x / float(kSssWidth - 1);
        const float v = y / float(kSssHeight - 1);
        const float stripe = ((x / 3) % 2) ? 0.85f : 0.25f;
        return std::array<uint8_t, 4>{U8(stripe * (0.6f + 0.4f * u)), U8(0.3f + 0.4f * v + (h & 63) / 255.0f), U8(((y / 4) % 2) ? 0.7f : 0.2f), 255};
    });
}

Texture SssNormal() {
    return MakeRgba(kSssWidth, kSssHeight, 1, [](uint32_t x, uint32_t y) {
        const float nx = 0.6f * std::sin(x * 0.05f);
        const float ny = 0.5f * std::cos(y * 0.07f);
        const float nz = -std::sqrt(std::max(0.05f, 1.0f - nx * nx - ny * ny));
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        const float ex = nx / len * 0.5f + 0.5f;
        const float ey = ny / len * 0.5f + 0.5f;
        const float ez = std::sqrt(std::max(nz / len * 0.5f + 0.5f, 0.0f));
        return std::array<uint8_t, 4>{U8(ex), U8(ey), U8(ez), 255};
    });
}

Texture SssMaterial() {
    return MakeRgba(kSssWidth, kSssHeight, 1, [](uint32_t x, uint32_t y) {
        const float dx = (x - 160.0f) / 90.0f;
        const float dy = (y - 90.0f) / 70.0f;
        const bool skin = dx * dx + dy * dy < 1.0f && (x % 37) != 0;
        return std::array<uint8_t, 4>{40, 80, static_cast<uint8_t>(skin ? 10 : 3), 255};
    });
}

Texture SssDepth() {
    Texture t;
    t.width = kSssWidth;
    t.height = kSssHeight;
    t.format = gpu::Format::R32Sfloat;
    std::vector<uint8_t> base(static_cast<size_t>(kSssWidth) * kSssHeight * 4);
    for (uint32_t y = 0; y < kSssHeight; ++y) {
        for (uint32_t x = 0; x < kSssWidth; ++x) {
            const float z = 0.4f + 0.2f * (x / float(kSssWidth)) + 0.002f * float(Hash(x * 7u + y * 131u) & 15u);
            const float d = 0.05f / z;
            std::memcpy(&base[(static_cast<size_t>(y) * kSssWidth + x) * 4], &d, 4);
        }
    }
    t.levels.push_back(std::move(base));
    return t;
}

bool SubsurfaceRun(gpu::Device& dev, Picture& out) {
    const bool d3d12 = dev.Info().backend == gpu::Backend::D3D12;
    GpuRunner runner(dev);
    gpu::ImageResource normal, material, depth;
    gpu::RenderTarget light;
    bool ok = runner.Upload(SssNormal(), normal) && runner.Upload(SssMaterial(), material) && runner.Upload(SssDepth(), depth) &&
              runner.Upload(SssLight(), light.image, gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::TransferSrc);
    light.layout = gpu::Layout::ShaderReadOnly;
    gpu::SamplerDesc point;
    point.mag = point.min = gpu::Filter::Nearest;
    point.mipmap = gpu::MipmapMode::Nearest;
    point.address_u = point.address_v = point.address_w = gpu::AddressMode::ClampToEdge;
    const gpu::SamplerHandle sampler = dev.CreateSampler(point);
    gpu::BindingLayoutDesc textures_desc;
    textures_desc.bindings = {{0, gpu::BindingType::CombinedImageSampler, 1, gpu::ShaderStage::All, gpu::BindingFlag::PartiallyBound}};
    const gpu::BindingLayoutHandle textures_layout = dev.CreateBindingLayout(textures_desc);
    gpu::BindingLayoutDesc frame_desc;
    frame_desc.bindings = {{0, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::All},
                           {2, gpu::BindingType::SampledImage, 62, gpu::ShaderStage::All, gpu::BindingFlag::PartiallyBound},
                           {3, gpu::BindingType::Sampler, d3d12 ? 4u : 5u, gpu::ShaderStage::All, gpu::BindingFlag::PartiallyBound}};
    const gpu::BindingLayoutHandle frame_layout = dev.CreateBindingLayout(frame_desc);
    gpu::BindingPoolDesc pool_desc;
    pool_desc.sizes = {{gpu::BindingType::CombinedImageSampler, 4}, {gpu::BindingType::StorageBuffer, 4}, {gpu::BindingType::SampledImage, 64},
                       {gpu::BindingType::Sampler, 8}};
    pool_desc.max_sets = 2;
    const gpu::BindingPoolHandle pool = dev.CreateBindingPool(pool_desc);
    const gpu::BindingLayoutHandle layouts[2] = {textures_layout, frame_layout};
    gpu::BindingSetHandle sets[2];
    ok = ok && dev.AllocateBindingSets(pool, layouts, sets);
    gpu::BufferResource frame;
    gpu::BufferDesc frame_buffer;
    frame_buffer.size = sizeof(gpu::FrameData);
    frame_buffer.usage = gpu::BufferUsage::StorageBuffer;
    frame_buffer.host_visible = true;
    frame_buffer.name = "test frame data";
    ok = ok && dev.CreateBuffer(frame_buffer, frame);
    if (ok) {
        std::memset(frame.mapped, 0, sizeof(gpu::FrameData));
        gpu::View view;
        view.projection_param = glm::vec4(0.7f * kSssWidth / kSssHeight, 0.7f, 0.05f, 0.0f);
        view.viewport = glm::vec4(kSssWidth, kSssHeight, 1.0f / kSssWidth, 1.0f / kSssHeight);
        std::memcpy(frame.mapped, &view, sizeof(view));
        const gpu::BufferBinding frame_binding{frame.buffer, 0, ~0ull};
        const gpu::ImageBinding images[3] = {{normal.view, gpu::Layout::ShaderReadOnly, {}, &normal},
                                             {material.view, gpu::Layout::ShaderReadOnly, {}, &material},
                                             {depth.view, gpu::Layout::ShaderReadOnly, {}, &depth}};
        static_assert(gpu::kImgMaterial == gpu::kImgNormal + 1 && gpu::kImgDepth == gpu::kImgNormal + 2);
        gpu::BindingWrite writes[3];
        writes[0].set = sets[1];
        writes[0].binding = 0;
        writes[0].type = gpu::BindingType::StorageBuffer;
        writes[0].buffers = std::span(&frame_binding, 1);
        writes[1].set = sets[1];
        writes[1].binding = 2;
        writes[1].first = gpu::kImgNormal;
        writes[1].type = gpu::BindingType::SampledImage;
        writes[1].images = std::span(images, 3);
        writes[2].set = sets[1];
        writes[2].binding = 3;
        writes[2].first = gpu::kSmpPointClamp;
        writes[2].type = gpu::BindingType::Sampler;
        writes[2].samplers = std::span(&sampler, 1);
        dev.WriteBindings(writes);
    }
    gpu::PipelineLayoutDesc scene_desc;
    scene_desc.sets = {textures_layout, frame_layout};
    scene_desc.push_bytes = 128;
    const gpu::PipelineLayoutHandle scene_layout = dev.CreatePipelineLayout(scene_desc);
    SubsurfacePass pass;
    ok = ok && pass.Init(dev, textures_layout, frame_layout, gpu::Format::R8G8B8A8Unorm, sampler) && pass.CreateTargets(gpu::Extent2D{kSssWidth, kSssHeight});
    if (ok) {
        dev.Submit([&](gpu::CommandList& list) {
            pass.Record(list, light, 0, [&] { list.BindSets(gpu::BindPoint::Graphics, scene_layout, 0, sets); });
            list.UseTargets({{&light, gpu::Layout::TransferSrc}});
        });
        std::vector<uint8_t> pixels;
        ok = dev.ReadImage(light, pixels);
        out.width = kSssWidth;
        out.height = kSssHeight;
        out.rgba = std::move(pixels);
    }
    dev.WaitIdle();
    pass.Shutdown();
    dev.DestroyPipelineLayout(scene_layout);
    dev.DestroyBindingPool(pool);
    dev.DestroyBindingLayout(textures_layout);
    dev.DestroyBindingLayout(frame_layout);
    dev.DestroyBuffer(frame);
    dev.DestroySampler(sampler);
    dev.DestroyImage(normal);
    dev.DestroyImage(material);
    dev.DestroyImage(depth);
    dev.DestroyImage(light.image);
    return ok && out.rgba.size() == static_cast<size_t>(kSssWidth) * kSssHeight * 4;
}

Picture SssInput() {
    const Texture t = SssLight();
    return Picture{t.width, t.height, t.levels[0]};
}

std::vector<uint8_t> BlockBytes(uint32_t blocks, uint32_t block_bytes, uint32_t seed, bool bc7) {
    std::vector<uint8_t> out(static_cast<size_t>(blocks) * block_bytes);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>(Hash(static_cast<uint32_t>(i) * 2654435761u + seed));
    }
    if (bc7) {
        for (size_t b = 0; b < out.size(); b += 16) {
            out[b] = 0x40;
        }
    }
    return out;
}

bool TextureManagerRun(gpu::Device& dev, Picture& out) {
    TextureManager textures;
    if (!textures.Init(dev)) {
        return false;
    }
    std::vector<TextureMip> rgba_mips;
    const Texture rgba_full = MakeRgba(128, 128, 8, [](uint32_t x, uint32_t y) {
        return std::array<uint8_t, 4>{U8(x / 127.0f), U8(((x / 8 + y / 8) % 2) ? 0.9f : 0.15f), U8(y / 127.0f), 255};
    });
    for (uint32_t m = 0; m < rgba_full.mips; ++m) {
        rgba_mips.push_back({std::max(128u >> m, 1u), std::max(128u >> m, 1u), rgba_full.levels[m]});
    }
    const uint32_t t_rgba = textures.Create("test:rgba_mips", gpu::Format::R8G8B8A8Unorm, rgba_mips);
    const std::vector<uint8_t> bc1 = BlockBytes(16 * 16, 8, 11, false);
    const TextureMip bc1_mip{64, 64, bc1};
    const uint32_t t_bc1 = textures.Create("test:bc1", gpu::Format::Bc1RgbaUnormBlock, std::span(&bc1_mip, 1));
    const std::vector<uint8_t> bc3 = BlockBytes(16 * 8, 16, 23, false);
    const TextureMip bc3_mip{64, 32, bc3};
    const uint32_t t_bc3 = textures.Create("test:bc3", gpu::Format::Bc3UnormBlock, std::span(&bc3_mip, 1));
    const std::vector<uint8_t> bc7 = BlockBytes(8 * 8, 16, 37, true);
    const TextureMip bc7_mip{32, 32, bc7};
    const uint32_t t_bc7 = textures.Create("test:bc7", gpu::Format::Bc7UnormBlock, std::span(&bc7_mip, 1));
    const std::vector<uint8_t> odd = BlockBytes(18 * 10, 8, 41, false);
    const TextureMip odd_mip{70, 38, odd};
    const uint32_t t_odd = textures.Create("test:bc1_70x38", gpu::Format::Bc1RgbaUnormBlock, std::span(&odd_mip, 1));
    const Texture coverage = UiCoverageTexture();
    const TextureMip coverage_mip{coverage.width, coverage.height, coverage.levels[0]};
    const uint32_t t_coverage = textures.Create("test:coverage", gpu::Format::R8Unorm, std::span(&coverage_mip, 1));
    const uint8_t face[6][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}, {0, 255, 255, 255}, {255, 0, 255, 255}};
    std::vector<TextureMip> faces;
    for (const auto& f : face) {
        faces.push_back({1, 1, std::span<const uint8_t>(f, 4)});
    }
    textures.Create("test:cube", gpu::Format::R8G8B8A8Unorm, faces, 6, true);
    GpuRunner runner(dev);
    gpu::ImageResource scene;
    bool ok = runner.Upload(SceneTexture(), scene);
    gpu::SamplerDesc clamp = LinearClamp();
    const gpu::SamplerHandle scene_sampler = dev.CreateSampler(clamp);
    gpu::BindingLayoutDesc set1;
    set1.bindings = {{0, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::All}, {1, gpu::BindingType::CombinedImageSampler, 1, gpu::ShaderStage::All}};
    const gpu::BindingLayoutHandle set1_layout = dev.CreateBindingLayout(set1);
    gpu::BindingPoolDesc pool_desc;
    pool_desc.sizes = {{gpu::BindingType::StorageBuffer, 1}, {gpu::BindingType::CombinedImageSampler, 1}};
    pool_desc.max_sets = 1;
    const gpu::BindingPoolHandle pool = dev.CreateBindingPool(pool_desc);
    const gpu::BindingSetHandle set1_set = dev.AllocateBindingSet(pool, set1_layout);
    std::vector<UiDraw> draws;
    std::vector<float> vertices;
    std::vector<Draw> calls;
    constexpr uint32_t kW = 320;
    constexpr uint32_t kH = 180;
    auto quad = [&](float x0, float y0, float x1, float y1, uint32_t texture, float mode) {
        UiDraw d;
        d.textures[0] = texture;
        d.m[1][0] = d.m[1][1] = 1.0f;
        d.m[2][2] = d.m[2][3] = 1.0f;
        d.m[4][0] = d.m[4][1] = 1.0f;
        d.m[5][2] = d.m[5][3] = 1.0f;
        d.screen[2] = 1.0f;
        d.screen[3] = mode;
        Draw call;
        call.first_vertex = static_cast<uint32_t>(vertices.size() / 8);
        call.vertex_count = 6;
        const float px[6][4] = {{x0, y0, 0, 0}, {x1, y0, 1, 0}, {x1, y1, 1, 1}, {x0, y0, 0, 0}, {x1, y1, 1, 1}, {x0, y1, 0, 1}};
        for (const auto& v : px) {
            vertices.insert(vertices.end(), {v[0], v[1], v[2], v[3], 1.0f, 1.0f, 1.0f, 1.0f});
        }
        Put(call.push, 1.0f / kW);
        Put(call.push, 1.0f / kH);
        Put(call.push, static_cast<uint32_t>(draws.size()));
        Put(call.push, 0u);
        call.push.resize(64, 0);
        Put(call.push, 1.0f); // UI brightness ratio, inactive in this SDR fixture.
        call.push.resize(80, 0);
        draws.push_back(d);
        calls.push_back(std::move(call));
    };
    quad(5, 5, 105, 85, t_rgba, 0.0f);
    quad(110, 5, 210, 85, t_bc1, 0.0f);
    quad(215, 5, 315, 85, t_bc3, 0.0f);
    quad(5, 90, 105, 175, t_bc7, 0.0f);
    quad(110, 90, 210, 175, t_odd, 0.0f);
    quad(215, 90, 315, 130, t_coverage, 1.0f);
    quad(215, 135, 245, 165, t_rgba, 0.0f);
    gpu::BufferResource draw_buffer;
    gpu::BufferDesc bd;
    bd.size = draws.size() * sizeof(UiDraw);
    bd.usage = gpu::BufferUsage::StorageBuffer;
    bd.host_visible = true;
    bd.name = "test ui draws";
    ok = ok && dev.CreateBuffer(bd, draw_buffer);
    gpu::BufferResource vertex_buffer;
    bd.size = vertices.size() * sizeof(float);
    bd.usage = gpu::BufferUsage::VertexBuffer;
    bd.name = "test vertices";
    ok = ok && dev.CreateBuffer(bd, vertex_buffer);
    if (ok) {
        std::memcpy(draw_buffer.mapped, draws.data(), draws.size() * sizeof(UiDraw));
        std::memcpy(vertex_buffer.mapped, vertices.data(), vertices.size() * sizeof(float));
        const gpu::BufferBinding draw_binding{draw_buffer.buffer, 0, ~0ull};
        const gpu::ImageBinding scene_binding{scene.view, gpu::Layout::ShaderReadOnly, scene_sampler, &scene};
        gpu::BindingWrite writes[2];
        writes[0].set = set1_set;
        writes[0].binding = 0;
        writes[0].type = gpu::BindingType::StorageBuffer;
        writes[0].buffers = std::span(&draw_binding, 1);
        writes[1].set = set1_set;
        writes[1].binding = 1;
        writes[1].type = gpu::BindingType::CombinedImageSampler;
        writes[1].images = std::span(&scene_binding, 1);
        dev.WriteBindings(writes);
    }
    gpu::PipelineLayoutDesc ld;
    ld.sets = {textures.BindingLayout(), set1_layout};
    ld.push_bytes = 80;
    ld.push_stages = gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment;
    const gpu::PipelineLayoutHandle layout = dev.CreatePipelineLayout(ld);
    gpu::GraphicsPipelineDesc pd;
    pd.vertex = "ui_sprite.vert";
    pd.fragment = "ui_sprite.frag";
    pd.layout = layout;
    pd.colors = {gpu::Format::R8G8B8A8Unorm};
    pd.blend = gpu::BlendMode::Alpha;
    pd.vertex_attributes = {{gpu::Format::R32G32Sfloat, 0}, {gpu::Format::R32G32Sfloat, 8}, {gpu::Format::R32G32B32A32Sfloat, 16}};
    pd.vertex_stride = 32;
    const gpu::PipelineHandle pipeline = dev.CreateGraphicsPipeline(pd);
    ok = ok && pipeline;
    gpu::RenderTarget target;
    gpu::ImageDesc td;
    td.format = gpu::Format::R8G8B8A8Unorm;
    td.extent = {kW, kH, 1};
    td.usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::TransferSrc;
    td.name = "test target";
    ok = ok && dev.CreateImage(td, target.image);
    if (ok) {
        dev.Submit([&](gpu::CommandList& list) {
            list.UseTargets({{&target, gpu::Layout::ColorAttachment}});
            gpu::ColorOutput color{&target, true, {}};
            color.clear_value.float32[0] = 0.05f;
            color.clear_value.float32[1] = 0.05f;
            color.clear_value.float32[2] = 0.08f;
            color.clear_value.float32[3] = 1.0f;
            list.BeginPass(gpu::Extent2D{kW, kH}, {color});
            list.BindPipeline(gpu::BindPoint::Graphics, pipeline);
            const gpu::BindingSetHandle sets[2] = {textures.BindingSet(), set1_set};
            list.BindSets(gpu::BindPoint::Graphics, layout, 0, sets);
            list.BindVertexBuffer(vertex_buffer.buffer, 0);
            for (const Draw& d : calls) {
                list.PushConstants(layout, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment, 0, static_cast<uint32_t>(d.push.size()), d.push.data());
                list.Draw(d.vertex_count, 1, d.first_vertex, 0);
            }
            list.EndPass();
            list.UseTargets({{&target, gpu::Layout::TransferSrc}});
        });
        std::vector<uint8_t> pixels;
        ok = dev.ReadImage(target, pixels);
        out = Picture{kW, kH, std::move(pixels)};
    }
    dev.WaitIdle();
    dev.ReapDeferred(true);
    dev.DestroyImage(target.image);
    dev.DestroyPipeline(pipeline);
    dev.DestroyPipelineLayout(layout);
    dev.DestroyBuffer(vertex_buffer);
    dev.DestroyBuffer(draw_buffer);
    dev.DestroyBindingPool(pool);
    dev.DestroyBindingLayout(set1_layout);
    dev.DestroySampler(scene_sampler);
    dev.DestroyImage(scene);
    textures.Shutdown();
    return ok && out.rgba.size() == static_cast<size_t>(kW) * kH * 4;
}

struct ComputeResult {
    float metered = -1;
    float values[4] = {};
    bool timestamps = false;
    uint64_t ticks = 0;
};

bool ComputeRun(gpu::Device& dev, ComputeResult& out, bool meter = false, float level = 1.0f) {
    const bool d3d12 = dev.Info().backend == gpu::Backend::D3D12;
    GpuRunner runner(dev);
    gpu::ImageResource scene;
    gpu::ImageResource depth;
    Texture input = SceneTexture();
    if (meter) {
        input.width=128;input.height=128;input.format=gpu::Format::R32G32B32A32Sfloat;
        std::vector<glm::vec4> data(128*128,glm::vec4(level,level,level,1));
        input.levels={std::vector<uint8_t>(data.size()*sizeof(glm::vec4))};
        std::memcpy(input.levels[0].data(),data.data(),input.levels[0].size());
    }
    bool ok = runner.Upload(input, scene);
    if (meter) ok = ok && runner.Upload(MakeRgba(128,128,1,[](uint32_t x,uint32_t) {
        return std::array<uint8_t,4>{static_cast<uint8_t>(x<64?255:0),0,0,255};
    }),depth);
    const gpu::SamplerHandle linear = dev.CreateSampler(LinearClamp());
    gpu::BindingLayoutDesc frame_desc;
    frame_desc.bindings = {{2, gpu::BindingType::SampledImage, 62, gpu::ShaderStage::All, gpu::BindingFlag::PartiallyBound},
                           {3, gpu::BindingType::Sampler, d3d12 ? 4u : 5u, gpu::ShaderStage::All, gpu::BindingFlag::PartiallyBound},
                           {5, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::All}};
    const gpu::BindingLayoutHandle frame_layout = dev.CreateBindingLayout(frame_desc);
    gpu::BindingLayoutDesc empty_desc;
    const gpu::BindingLayoutHandle empty_layout = dev.CreateBindingLayout(empty_desc);
    gpu::BindingPoolDesc pool_desc;
    pool_desc.sizes = {{gpu::BindingType::SampledImage, 64}, {gpu::BindingType::Sampler, 8}, {gpu::BindingType::StorageBuffer, 2}};
    pool_desc.max_sets = 2;
    const gpu::BindingPoolHandle pool = dev.CreateBindingPool(pool_desc);
    const gpu::BindingLayoutHandle layouts[2] = {empty_layout, frame_layout};
    gpu::BindingSetHandle sets[2];
    ok = ok && dev.AllocateBindingSets(pool, layouts, sets);
    gpu::BufferResource result;
    gpu::BufferDesc bd;
    bd.size = 8192;
    bd.usage = gpu::BufferUsage::StorageBuffer;
    bd.host_visible = true;
    bd.name = "test luminance";
    ok = ok && dev.CreateBuffer(bd, result);
    if (ok) {
        std::memset(result.mapped, 0, 8192);
        constexpr uint32_t kSceneSlot = 7;
        const gpu::ImageBinding image{scene.view, gpu::Layout::ShaderReadOnly, {}, &scene};
        const gpu::BufferBinding buffer{result.buffer, 0, ~0ull};
        gpu::BindingWrite writes[4];
        const gpu::ImageBinding depth_binding{depth.view, gpu::Layout::ShaderReadOnly, {}, &depth};
        writes[0].set = sets[1];
        writes[0].binding = 2;
        writes[0].first = meter ? 4u : kSceneSlot;
        writes[0].type = gpu::BindingType::SampledImage;
        writes[0].images = std::span(&image, 1);
        writes[1].set = sets[1];
        writes[1].binding = 3;
        writes[1].first = gpu::kSmpLinearClamp;
        writes[1].type = gpu::BindingType::Sampler;
        writes[1].samplers = std::span(&linear, 1);
        writes[2].set = sets[1];
        writes[2].binding = 5;
        writes[2].type = gpu::BindingType::StorageBuffer;
        writes[2].buffers = std::span(&buffer, 1);
        if (meter) {
            writes[3].set=sets[1];writes[3].binding=2;writes[3].first=3;
            writes[3].type=gpu::BindingType::SampledImage;writes[3].images=std::span(&depth_binding,1);
        }
        dev.WriteBindings(std::span(writes,meter?4:3));
    }
    gpu::PipelineLayoutDesc ld;
    ld.sets = {empty_layout, frame_layout};
    ld.push_bytes = 128;
    const gpu::PipelineLayoutHandle layout = dev.CreatePipelineLayout(ld);
    gpu::ComputePipelineDesc cd;
    cd.layout = layout;
    cd.shader = meter ? "luminance.comp" : "reflect_colour.comp";
    const gpu::PipelineHandle pipeline = dev.CreateComputePipeline(cd);
    const gpu::QueryPoolHandle queries = dev.CreateTimestampPool(2);
    ok = ok && pipeline && queries;
    if (ok) {
        dev.Submit([&](gpu::CommandList& list) {
            list.ResetQueries(queries, 0, 2);
            list.WriteTimestamp(queries, 0);
            list.BindPipeline(gpu::BindPoint::Compute, pipeline);
            list.BindSets(gpu::BindPoint::Compute, layout, 0, sets);
            gpu::PassPush push;
            push.ids = glm::uvec4(7, 0, 0, 0);
            push.f0 = glm::vec4(0.55f, 0.15f, 0.85f, 0.2f);
            push.f1 = glm::vec4(0.5f, 0.45f, 0.9f, 0.5f);
            list.PushConstants(layout, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment | gpu::ShaderStage::Compute, 0, sizeof(push), &push);
            list.Dispatch(meter?2:1, meter?2:1, 1);
            list.BufferBarrier(result.buffer, gpu::PipelineStage::ComputeShader, static_cast<uint64_t>(gpu::Access::ShaderWrite), gpu::PipelineStage::Host,
                               static_cast<uint64_t>(gpu::Access::HostRead));
            list.WriteTimestamp(queries, 1);
        });
        dev.InvalidateMapped(result,0,~0ull);
        std::memcpy(out.values, result.mapped, sizeof(out.values));
        if (meter) out.metered=exposure_math::Meter({static_cast<const glm::vec2*>(result.mapped),1024});
        uint64_t ticks[2] = {};
        out.timestamps = dev.ReadTimestamps(queries, 0, 2, ticks) && ticks[1] >= ticks[0];
        out.ticks = ticks[1] - ticks[0];
    }
    dev.WaitIdle();
    dev.DestroyQueryPool(queries);
    dev.DestroyPipeline(pipeline);
    dev.DestroyPipelineLayout(layout);
    dev.DestroyBuffer(result);
    dev.DestroyBindingPool(pool);
    dev.DestroyBindingLayout(frame_layout);
    dev.DestroyBindingLayout(empty_layout);
    dev.DestroySampler(linear);
    dev.DestroyImage(scene);
    if (meter) dev.DestroyImage(depth);
    return ok;
}

struct DepthPicture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> depth;
};

// Exercise the actual lighting composite, before the post/LUT/output tests.
// The former TPP shoulder hid HDR lighting here even with float render targets.
bool HdrComposeRun(gpu::Device& dev) {
    constexpr uint32_t width = 256;
    GpuRunner runner(dev);
    gpu::ImageResource inputs[4];
    const uint32_t slots[] = {gpu::kImgAlbedo, gpu::kImgDepth, gpu::kImgDiffuse, gpu::kImgSpecular};
    std::array<glm::vec4, width> source[4];
    for (uint32_t x = 0; x < width; ++x) {
        const float radiance = x < 128 ? x / 127.0f : std::exp2((x - 128) / 127.0f * 6.0f);
        source[0][x] = glm::vec4(1.0f);
        source[1][x] = glm::vec4(1.0f);
        source[2][x] = glm::vec4(glm::vec3(radiance * .75f, radiance * .375f, radiance * .1875f), 1.0f);
        source[3][x] = glm::vec4(glm::vec3(radiance * .25f, radiance * .125f, radiance * .0625f), 1.0f);
    }
    bool ok = true;
    for (uint32_t i = 0; i < 4; ++i) {
        Texture t;
        t.width = width; t.height = 1; t.format = gpu::Format::R32G32B32A32Sfloat;
        t.levels.emplace_back(sizeof(source[i]));
        std::memcpy(t.levels[0].data(), source[i].data(), sizeof(source[i]));
        ok = runner.Upload(t, inputs[i]) && ok;
    }
    gpu::BindingLayoutDesc empty_desc;
    const auto empty_layout = dev.CreateBindingLayout(empty_desc);
    gpu::BindingLayoutDesc frame_desc;
    frame_desc.bindings = {{0, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::All},
        {2, gpu::BindingType::SampledImage, 62, gpu::ShaderStage::All, gpu::BindingFlag::PartiallyBound},
        {3, gpu::BindingType::Sampler, 5, gpu::ShaderStage::All, gpu::BindingFlag::PartiallyBound}};
    const auto frame_layout = dev.CreateBindingLayout(frame_desc);
    gpu::BindingPoolDesc pool_desc;
    pool_desc.max_sets = 2;
    pool_desc.sizes = {{gpu::BindingType::StorageBuffer, 1}, {gpu::BindingType::SampledImage, 62}, {gpu::BindingType::Sampler, 5}};
    const auto pool = dev.CreateBindingPool(pool_desc);
    const gpu::BindingLayoutHandle layouts[] = {empty_layout, frame_layout};
    gpu::BindingSetHandle sets[2];
    ok = dev.AllocateBindingSets(pool, layouts, sets) && ok;
    gpu::SamplerDesc sampler_desc;
    sampler_desc.mag = sampler_desc.min = gpu::Filter::Nearest;
    const auto sampler = dev.CreateSampler(sampler_desc);
    sampler_desc.mag = sampler_desc.min = gpu::Filter::Linear;
    const auto linear_sampler = dev.CreateSampler(sampler_desc);
    gpu::BufferResource frame;
    ok = dev.CreateBuffer({sizeof(gpu::FrameData), gpu::BufferUsage::StorageBuffer, true}, frame) && ok;
    if (ok) {
        std::memset(frame.mapped, 0, sizeof(gpu::FrameData));
        const gpu::BufferBinding buffer{frame.buffer, 0, ~0ull};
        gpu::BindingWrite write;
        write.set = sets[1]; write.binding = 0; write.type = gpu::BindingType::StorageBuffer;
        write.buffers = std::span(&buffer, 1);
        dev.WriteBindings({&write, 1});
        write = {};
        write.set = sets[1]; write.binding = 3; write.first = gpu::kSmpPointClamp;
        write.type = gpu::BindingType::Sampler; write.samplers = std::span(&sampler, 1);
        dev.WriteBindings({&write, 1});
        write.first = gpu::kSmpLinearClamp; write.samplers = std::span(&linear_sampler, 1);
        dev.WriteBindings({&write, 1});
        for (uint32_t i = 0; i < 4; ++i) {
            const gpu::ImageBinding image{inputs[i].view, gpu::Layout::ShaderReadOnly, {}, &inputs[i]};
            write = {};
            write.set = sets[1]; write.binding = 2; write.first = slots[i];
            write.type = gpu::BindingType::SampledImage; write.images = std::span(&image, 1);
            dev.WriteBindings({&write, 1});
        }
    }
    gpu::PipelineLayoutDesc layout_desc;
    layout_desc.sets = {empty_layout, frame_layout}; layout_desc.push_bytes = sizeof(gpu::PassPush);
    const auto layout = dev.CreatePipelineLayout(layout_desc);
    gpu::GraphicsPipelineDesc pipeline_desc;
    pipeline_desc.vertex = "fullscreen.vert"; pipeline_desc.fragment = "compose.frag";
    pipeline_desc.layout = layout; pipeline_desc.colors = {gpu::Format::R32G32B32A32Sfloat};
    const auto pipeline = dev.CreateGraphicsPipeline(pipeline_desc);
    gpu::RenderTarget output;
    gpu::ImageDesc image_desc;
    image_desc.format = gpu::Format::R32G32B32A32Sfloat; image_desc.extent = {width, 1, 1};
    image_desc.usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::TransferSrc;
    ok = pipeline && dev.CreateImage(image_desc, output.image) && ok;
    for (int hdr = 0; hdr < 2 && ok; ++hdr) {
        static_cast<gpu::FrameData*>(frame.mapped)->hdr_output.x = float(hdr);
        dev.FlushMapped(frame, 0, ~0ull);
        dev.Submit([&](gpu::CommandList& cmd) {
            cmd.UseTargets({{&output, gpu::Layout::ColorAttachment}});
            cmd.BeginPass({width, 1}, {{&output, false, {}}});
            cmd.BindPipeline(gpu::BindPoint::Graphics, pipeline);
            cmd.BindSets(gpu::BindPoint::Graphics, layout, 0, sets);
            gpu::PassPush push;
            push.ids.z = 2; // TPP tonemap enabled, as in P.T.'s resident settings.
            const float c = 7.7f * .7f / 7.0f;
            push.f0 = glm::vec4(1.0f / (c * c), .3f, c, 0.0f);
            cmd.PushConstants(layout, gpu::ShaderStage::All, 0, sizeof(push), &push);
            cmd.Draw(3, 1, 0, 0); cmd.EndPass();
            cmd.UseTargets({{&output, gpu::Layout::TransferSrc}});
        });
        std::vector<uint8_t> pixels;
        ok = dev.ReadImage(output, pixels) && pixels.size() == sizeof(source[0]);
        for (uint32_t x = 0; x < width && ok; ++x) {
            glm::vec4 actual;
            std::memcpy(&actual, pixels.data() + x * sizeof(actual), sizeof(actual));
            for (int channel = 0; channel < 3; ++channel) {
                float expected = source[2][x][channel] + source[3][x][channel];
                if (!hdr && expected > .3f) expected = .3f + .77f - .77f * .77f / (expected - .3f + .77f);
                ok = ok && std::isfinite(actual[channel]) && std::abs(actual[channel] - expected) < std::max(.00001f, expected * .0001f);
            }
        }
    }
    // The same real shader/descriptors exercise early bloom, the encoded LUT
    // bridge (identity and deliberately clipping grade), and bloom composition.
    auto bind_image = [&](uint32_t slot, const gpu::ImageResource& input) {
        const gpu::ImageBinding image{input.view, gpu::Layout::ShaderReadOnly, {}, &input};
        gpu::BindingWrite write;
        write.set = sets[1]; write.binding = 2; write.first = slot;
        write.type = gpu::BindingType::SampledImage; write.images = std::span(&image, 1);
        dev.WriteBindings({&write, 1});
    };
    auto draw_post = [&](const char* shader, const gpu::PassPush& push, int hdr, std::vector<glm::vec4>* result) {
        pipeline_desc.fragment = shader;
        const auto post = dev.CreateGraphicsPipeline(pipeline_desc);
        if (!post) return false;
        static_cast<gpu::FrameData*>(frame.mapped)->hdr_output.x = float(hdr);
        dev.FlushMapped(frame, 0, ~0ull);
        dev.Submit([&](gpu::CommandList& cmd) {
            cmd.UseTargets({{&output, gpu::Layout::ColorAttachment}});
            cmd.BeginPass({width, 1}, {{&output, false, {}}});
            cmd.BindPipeline(gpu::BindPoint::Graphics, post);
            cmd.BindSets(gpu::BindPoint::Graphics, layout, 0, sets);
            cmd.PushConstants(layout, gpu::ShaderStage::All, 0, sizeof(push), &push);
            cmd.Draw(3, 1, 0, 0); cmd.EndPass();
            cmd.UseTargets({{&output, gpu::Layout::TransferSrc}});
        });
        std::vector<uint8_t> bytes;
        const bool read = dev.ReadImage(output, bytes) && bytes.size() == width * sizeof(glm::vec4);
        if (read) { result->resize(width); std::memcpy(result->data(), bytes.data(), bytes.size()); }
        dev.DestroyPipeline(post);
        return read;
    };
    bind_image(gpu::kImgHdr, inputs[2]);
    bind_image(gpu::kImgBloomA, inputs[2]);
    std::vector<glm::vec4> post_result;
    for (int hdr = 0; hdr < 2 && ok; ++hdr) {
        gpu::PassPush push;
        push.f0.x = 1.0f; push.f1.x = 1.0f;
        ok = draw_post("bright.frag", push, hdr, &post_result);
        float previous = -1.0f;
        for (uint32_t x = 0; x < width && ok; ++x) {
            ok = std::isfinite(post_result[x].x) && post_result[x].x + 1e-5f >= previous;
            if (!hdr) ok = ok && post_result[x].x <= 1.0f;
            previous = post_result[x].x;
        }
        if (hdr) ok = ok && previous > 16.0f;
        push.ids.x = gpu::kImgBloomA; push.f0 = glm::vec4(0, 1, 1, 0);
        ok = ok && draw_post("kawase.frag", push, hdr, &post_result);
        for (uint32_t x = 0; x < width && ok; ++x) {
            const float expected = hdr ? source[2][x].x : std::min(source[2][x].x, 1.0f);
            ok = std::abs(post_result[x].x - expected) < 1e-4f;
        }
    }
    auto encode = [](float x) { return x <= .0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - .055f; };
    Texture codes;
    codes.width = width; codes.height = 1; codes.format = gpu::Format::R32G32B32A32Sfloat;
    std::array<glm::vec4, width> encoded;
    for (uint32_t x = 0; x < width; ++x) {
        encoded[x] = glm::vec4(encode(source[2][x].x), encode(source[2][x].y), encode(source[2][x].z), 1);
    }
    codes.levels.emplace_back(sizeof(encoded));
    std::memcpy(codes.levels[0].data(), encoded.data(), sizeof(encoded));
    gpu::ImageResource code_image, lut_image;
    ok = runner.Upload(codes, code_image) && ok;
    bind_image(gpu::kImgLdrA, code_image);
    for (int clipping = 0; clipping < 2 && ok; ++clipping) {
        Texture lut;
        lut.width = 256; lut.height = 16; lut.format = gpu::Format::R32G32B32A32Sfloat;
        lut.levels.emplace_back(256 * 16 * sizeof(glm::vec4));
        for (int g = 0; g < 16; ++g) for (int b = 0; b < 16; ++b) for (int r = 0; r < 16; ++r) {
            glm::vec4 v(r / 15.f, g / 15.f, b / 15.f, 1);
            if (clipping) v = glm::min(v * 1.3f, glm::vec4(1));
            std::memcpy(lut.levels[0].data() + (g * 256 + b * 16 + r) * sizeof(v), &v, sizeof(v));
        }
        ok = runner.Upload(lut, lut_image) && ok;
        bind_image(gpu::kResColorLut, lut_image);
        for (int sampling=0;sampling<2 && ok;++sampling) for (int hdr = 0; hdr < 2 && ok; ++hdr) {
            gpu::PassPush push;
            push.ids = glm::uvec4(gpu::kImgLdrA, 1, 0, 0);
            push.f0 = glm::vec4(0, 1, 1, 1); push.f1.y = float(hdr); push.f1.z=float(sampling);
            ok = draw_post("tonemap.frag", push, hdr, &post_result);
            float previous = -1.0f;
            for (uint32_t x = 0; x < width && ok; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float actual = post_result[x][c];
                    ok = ok && std::isfinite(actual);
                    if (!clipping) {
                        const float expected = hdr ? encoded[x][c] : std::min(encoded[x][c], 1.0f);
                        ok = ok && std::abs(actual - expected) < .001f;
                    }
                    if (!hdr) ok = ok && actual <= 1.0001f;
                }
                if (clipping && hdr) {
                    // Protect against the old grade plateau BEFORE white as well as above it.
                    if (x > 0 && x != 128) ok = ok && post_result[x].x > previous;
                    if (source[2][x].x <= .18f) ok = ok && std::abs(post_result[x].x - encoded[x].x * 1.3f) < .001f;
                }
                previous = post_result[x].x;
            }
        }
        dev.DestroyImage(lut_image);
    }
    if (!ok) std::printf("HDR failed stage: LUT\n");
    // Linear bloom must add radiance continuously, including across .5 per
    // channel and above SDR white. Use the production tonemap, not a test copy.
    Texture zero;
    zero.width = zero.height = 1; zero.format = gpu::Format::R32G32B32A32Sfloat;
    zero.levels.emplace_back(sizeof(glm::vec4), 0);
    gpu::ImageResource black;
    ok = runner.Upload(zero, black) && ok;
    bind_image(gpu::kImgFlare, black);
    bind_image(gpu::kImgBloomSum, inputs[3]);
    for (int bloom = 0; bloom < 2 && ok; ++bloom) {
        gpu::PassPush push;
        push.f0 = glm::vec4(float(bloom), 1, 1, 0); push.f1.y = 1;
        ok = draw_post("tonemap.frag", push, 1, &post_result);
        for (uint32_t x = 0; x < width && ok; ++x) for (int c = 0; c < 3; ++c) {
            float expected = encode(source[2][x][c] + bloom * source[3][x][c]);
            expected = expected * expected / std::sqrt(std::max(1.f / 512.f, expected * expected));
            ok = ok && std::isfinite(post_result[x][c]) && std::abs(post_result[x][c] - expected) < .002f;
        }
    }
    // DOF's first downsample decodes before filtering. At a 2:1 coordinate
    // footprint midpoint the answer is mean LIGHT, not mean encoded RGB.
    gpu::PassPush down;
    down.ids.x = gpu::kImgLdrA; down.f2 = glm::vec4(width / 2, 1, 2.f / width, 1);
    ok = ok && draw_post("dof_down.frag", down, 1, &post_result);
    for (uint32_t x = 0; x < width / 2 && ok; ++x) for (int c = 0; c < 3; ++c) {
        const float expected = .5f * (source[2][2*x][c] + source[2][2*x+1][c]);
        ok = ok && std::abs(post_result[x][c] - encode(expected)) < std::max(.001f, encode(expected) * .001f);
    }
    // Motion-blur input round-trip and final alpha blend must preserve energy.
    gpu::PassPush motion;
    motion.ids = glm::uvec4(gpu::kImgLdrA, 0, 0, 0);
    motion.f0 = glm::vec4(0, 0, 1, 1); motion.f1.z = 1;
    ok = ok && draw_post("mb_composite.frag", motion, 1, &post_result);
    for (uint32_t x = 0; x < width && ok; ++x) for (int c = 0; c < 3; ++c)
        ok = ok && std::abs(post_result[x][c] - source[2][x][c]) < std::max(.001f, source[2][x][c] * .001f);
    if (!ok) std::printf("HDR failed stage: post filtering\n");
    // Render the same scene-linear shadow ramp through the actual SDR and HDR
    // tonemap branches. Both must use the same toe; only highlight range differs.
    Texture dark; dark.width=width; dark.height=1; dark.format=gpu::Format::R32G32B32A32Sfloat;
    dark.levels.emplace_back(width*sizeof(glm::vec4));
    auto decode=[](float x) { return x<=.04045f ? x/12.92f : std::pow((x+.055f)/1.055f,2.4f); };
    for (uint32_t x=0;x<width;++x) {
        const glm::vec4 v(glm::vec3(decode(.1f*x/(width-1))),1);
        std::memcpy(dark.levels[0].data()+x*sizeof(v),&v,sizeof(v));
    }
    gpu::ImageResource dark_image;
    ok=runner.Upload(dark,dark_image) && ok; bind_image(gpu::kImgHdr,dark_image);
    for (int reserved=0;reserved<2 && ok;++reserved) {
        static_cast<gpu::FrameData*>(frame.mapped)->hdr_output.z=float(reserved); // Old smooth flag must have no effect.
        std::vector<glm::vec4> sdr_shadow;
        float max_difference=0;
        for (int hdr=0;hdr<2 && ok;++hdr) {
            gpu::PassPush p; p.f0=glm::vec4(0,1,1,0); p.f1.y=float(hdr);
            ok=draw_post("tonemap.frag",p,hdr,&post_result) && ok;
            float previous=-1;
            for(uint32_t x=0;x<width && ok;++x) {
                const float c=.1f*x/(width-1), r=c*c;
                // Independent reference: upstream v1.0.8 ScreenTonemap, zero bloom.
                const float expected=r/std::sqrt(std::max(1.f/512.f,r));
                for(int channel=0;channel<3;++channel) {
                    ok=ok && std::abs(post_result[x][channel]-expected)<2e-6f;
                    if(hdr) max_difference=std::max(max_difference,std::abs(post_result[x][channel]-sdr_shadow[x][channel]));
                }
                ok=ok && post_result[x].x>=previous;
                previous=post_result[x].x;
            }
            ok=ok && post_result[0].x==0;
            if(!hdr) sdr_shadow=post_result;
        }
        ok=ok && max_difference<2e-6f;
        std::printf("SDR/HDR vanilla shadow parity (reserved flag %d): max encoded RGB difference %.9g (%u samples)\n",
                    reserved,max_difference,width);
    }
    static_cast<gpu::FrameData*>(frame.mapped)->hdr_output.z=0;
    dev.DestroyImage(dark_image);
    if (!ok) std::printf("HDR failed stage: shadow\n");
    // All tetrahedral orderings, boundaries and HDR-representable grades.
    // An affine cross-channel grade has an exact interpolation-independent answer.
    Texture probes=dark;
    std::array<glm::vec4,width> coordinates;
    for(uint32_t x=0;x<width;++x) {
        coordinates[x]=glm::vec4(float((x*67)%256)/255,float((x*131)%256)/255,float((x*199)%256)/255,1);
        std::memcpy(probes.levels[0].data()+x*sizeof(glm::vec4),&coordinates[x],sizeof(glm::vec4));
    }
    gpu::ImageResource probe_image;
    ok=runner.Upload(probes,probe_image) && ok; bind_image(gpu::kImgLdrA,probe_image);
    for(int size : {16,64}) {
        Texture lut; lut.width=size*size; lut.height=size; lut.format=gpu::Format::R32G32B32A32Sfloat;
        lut.levels.emplace_back(size*size*size*sizeof(glm::vec4));
        for(int b=0;b<size;++b) for(int g=0;g<size;++g) for(int r=0;r<size;++r) {
            glm::vec4 v((.6f*r+.3f*g+.1f*b)/(size-1),(.1f*r+.7f*g+.2f*b)/(size-1),(.2f*r+.1f*g+.7f*b)/(size-1),1);
            std::memcpy(lut.levels[0].data()+(g*size*size+b*size+r)*sizeof(v),&v,sizeof(v));
        }
        ok=runner.Upload(lut,lut_image) && ok; bind_image(gpu::kResColorLut,lut_image);
        for(int sampling=0;sampling<2 && ok;++sampling) {
            gpu::PassPush p; p.ids=glm::uvec4(gpu::kImgLdrA,1,0,0); p.f0=glm::vec4(0,1,1,1); p.f1.z=float(sampling);
            ok=draw_post("tonemap.frag",p,0,&post_result) && ok;
            for(uint32_t x=0;x<width && ok;++x) {
                glm::vec3 c(coordinates[x]);
                glm::vec3 expected(.6f*c.r+.3f*c.g+.1f*c.b,.1f*c.r+.7f*c.g+.2f*c.b,.2f*c.r+.1f*c.g+.7f*c.b);
                ok=glm::all(glm::lessThan(glm::abs(glm::vec3(post_result[x])-expected),glm::vec3(sampling ? 2e-6f : 3e-4f)));
                if(!ok) std::printf("LUT size %d mode %d x %u actual %.7f %.7f %.7f expected %.7f %.7f %.7f\n",size,sampling,x,post_result[x].x,post_result[x].y,post_result[x].z,expected.x,expected.y,expected.z);
            }
        }
        dev.DestroyImage(lut_image);
    }
    if (!ok) std::printf("HDR failed stage: affine LUT\n");

    // Production upstream optical kernels with HDR colour-domain fixes.
    // Test energy preservation, original warp coordinates and no white clipping.
    {
        constexpr uint32_t w=192,h=108;
        gpu::RenderTarget optics;
        auto desc=image_desc; desc.extent={w,h,1};
        ok=dev.CreateImage(desc,optics.image) && ok;
        gpu::ImageResource opt_input[3];
        std::vector<glm::vec4> pixels(w*h), result, reference;
        auto upload=[&](int index,uint32_t slot,const std::vector<glm::vec4>& data, bool half=false) {
            dev.DestroyImage(opt_input[index]);
            Texture t;t.width=w;t.height=h;t.format=gpu::Format::R32G32B32A32Sfloat;
            t.levels.emplace_back(data.size()*sizeof(glm::vec4));
            std::memcpy(t.levels[0].data(),data.data(),t.levels[0].size());
            if(half) {
                t.format=gpu::Format::R16G16B16A16Sfloat;t.levels[0].resize(data.size()*8);
                for(size_t i=0;i<data.size();++i) {
                    const uint32_t packed[]={glm::packHalf2x16(glm::vec2(data[i])),glm::packHalf2x16(glm::vec2(data[i].z,data[i].w))};
                    std::memcpy(t.levels[0].data()+i*8,packed,8);
                }
            }
            const bool uploaded=runner.Upload(t,opt_input[index]);
            if(uploaded) bind_image(slot,opt_input[index]);
            return uploaded;
        };
        auto draw=[&](const char* shader,const gpu::PassPush& push,int hdr) {
            pipeline_desc.fragment=shader;
            const auto pipe=dev.CreateGraphicsPipeline(pipeline_desc);
            if(!pipe) return false;
            static_cast<gpu::FrameData*>(frame.mapped)->hdr_output=glm::vec4(float(hdr),0,1,0);
            dev.FlushMapped(frame,0,~0ull);
            dev.Submit([&](gpu::CommandList& cmd) {
                cmd.UseTargets({{&optics,gpu::Layout::ColorAttachment}});
                cmd.BeginPass({w,h},{{&optics,false,{}}});
                cmd.BindPipeline(gpu::BindPoint::Graphics,pipe);
                cmd.BindSets(gpu::BindPoint::Graphics,layout,0,sets);
                cmd.PushConstants(layout,gpu::ShaderStage::All,0,sizeof(push),&push);
                cmd.Draw(3,1,0,0);cmd.EndPass();
                cmd.UseTargets({{&optics,gpu::Layout::TransferSrc}});
            });
            std::vector<uint8_t> bytes;
            bool valid=dev.ReadImage(optics,bytes) && bytes.size()==w*h*sizeof(glm::vec4);
            if(valid) {
                result.resize(w*h);std::memcpy(result.data(),bytes.data(),bytes.size());
                for(const auto& v:result) for(int c=0;c<4;++c) valid=valid && std::isfinite(v[c]);
            }
            dev.DestroyPipeline(pipe);return valid;
        };
        auto report=[&](const char* name,bool pass) {Check(pass,name);ok=pass && ok;};
        gpu::PassPush lens;lens.ids.x=gpu::kImgLdrA;lens.f0.x=1;
        pixels.assign(w*h,glm::vec4(glm::vec3(encode(16)),1));
        bool valid=upload(0,gpu::kImgLdrA,pixels);
        for(int hdr:{0,1}) {
            valid=draw("screen_fx.frag",lens,hdr) && valid;
            for(const auto& v:result) valid=valid && glm::all(glm::lessThan(glm::abs(glm::vec3(v)-glm::vec3(encode(16))),glm::vec3(.0001f)));
        }
        report("HDR lens preserves constant HDR radiance in SDR/HDR modes",valid);
        for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x)
            pixels[y*w+x]=glm::vec4(encode(4*(x+.5f)/w),encode(4*(y+.5f)/h),encode(4*(x+.5f)/w),1);
        valid=upload(0,gpu::kImgLdrA,pixels) && draw("screen_fx.frag",lens,1);
        for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) {
            const glm::vec2 uv((x+.5f)/w,(y+.5f)/h),q=uv*2.f-1.f;
            const float radius=std::clamp(glm::length(q)*.52240777f,0.f,1.f),dome=1-std::sin(1.5707964f*radius);
            const glm::vec2 offset=std::pow(radius,1.75f)*84.f*glm::vec2(1.f/1920,1.f/1080)*q/std::sqrt(glm::dot(q,q)+dome*dome);
            const float dispersion[]={1.42f,1.275f,1.12f};
            for(int c=0;c<3;++c) {
                const int axis=c==1?1:0;const float size=c==1?float(h):float(w);
                const float coordinate=std::clamp((uv*.9f+.050000012f+offset*dispersion[c])[axis],.5f/size,1-.5f/size);
                valid=valid && std::abs(result[y*w+x][c]-encode(4*coordinate))<.006f;
            }
        }
        report("HDR lens retains authored warp / RGB fringe coordinates",valid);
        lens.f0.x=0;valid=draw("screen_fx.frag",lens,1);
        for(size_t i=0;i<pixels.size();++i) valid=valid && glm::all(glm::lessThan(glm::abs(result[i]-pixels[i]),glm::vec4(1e-6f)));
        report("HDR lens disabled is exact identity",valid);lens.f0.x=1;
        for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x)
            pixels[y*w+x]=glm::vec4(glm::vec3(encode(((x/2+y/2)&1)?16.f:0.f)),1);
        valid=upload(0,gpu::kImgLdrA,pixels) && draw("screen_fx.frag",lens,1);
        for(const auto& v:result) for(int c=0;c<3;++c) valid=valid && v[c]>=-1e-6f && v[c]<=encode(16)+1e-5f;
        report("HDR lens checker has no negative or super-white ringing",valid);
        gpu::PassPush dof;dof.ids=glm::uvec4(gpu::kImgLdrA,gpu::kImgLdrA,0,0);
        dof.f0=glm::vec4(1.f/w,1.f/h,0,0);dof.f1=glm::vec4(1,0,1,0);dof.f2=glm::vec4(w,h,1.f/w,1.f/h);
        pixels.assign(w*h,glm::vec4(4,2,1,1));
        valid=upload(0,gpu::kImgLdrA,pixels) && draw("dof_blur.frag",dof,1);
        for(const auto& v:result) valid=valid && glm::all(glm::lessThan(glm::abs(glm::vec3(v)-glm::vec3(4,2,1)),glm::vec3(1e-4f)));
        report("HDR far DOF conserves uniform HDR light",valid);
        // Original near blur uses straight RGB plus CoC, not premultiplied layers.
        dof.ids.z=1;pixels.assign(w*h,glm::vec4(4,2,1,.2f));
        valid=upload(0,gpu::kImgLdrA,pixels) && draw("dof_blur.frag",dof,1);
        for(const auto& v:result) valid=valid && glm::all(glm::lessThan(glm::abs(v-glm::vec4(4,2,1,.2f)),glm::vec4(1e-4f)));
        report("HDR near DOF preserves uniform radiance and original CoC",valid);
        gpu::PassPush mb;mb.ids=glm::uvec4(gpu::kImgLdrA,gpu::kImgMbBake,1,0);
        mb.f0=glm::vec4(.5f,.5f,1,1);
        pixels.assign(w*h,glm::vec4(4,2,1,1));valid=upload(0,gpu::kImgLdrA,pixels);
        pixels.assign(w*h,glm::vec4(0,0.5f,0.5f,0.5f));valid=upload(1,gpu::kImgMbBake,pixels,true) && draw("mb_mcguire.frag",mb,1) && valid;
        for(const auto& v:result) valid=valid && glm::vec3(v)==glm::vec3(4,2,1) && v.a==0;
        report("HDR zero motion is exact RGB identity",valid);
        pixels.assign(w*h,glm::vec4(.5f,.5f,.75f,.5f));
        valid=upload(1,gpu::kImgMbBake,pixels,true);
        pixels.assign(w*h,glm::vec4(16,8,4,1));
        valid=upload(0,gpu::kImgLdrA,pixels) && draw("mb_mcguire.frag",mb,1);
        for(const auto& v:result) valid=valid && glm::all(glm::lessThan(glm::abs(glm::vec3(v)-glm::vec3(16,8,4)),glm::vec3(1e-4f)));
        report("HDR motion filter preserves HDR light with no white clamp",valid);
        for(auto& input:opt_input) dev.DestroyImage(input);
        dev.DestroyImage(optics.image);
    }
    dev.DestroyImage(probe_image);
    dev.DestroyImage(black);
    dev.DestroyImage(code_image);
    dev.WaitIdle();
    dev.DestroyPipeline(pipeline); dev.DestroyPipelineLayout(layout); dev.DestroyBindingPool(pool);
    dev.DestroyBindingLayout(frame_layout); dev.DestroyBindingLayout(empty_layout);
    dev.DestroySampler(linear_sampler); dev.DestroySampler(sampler); dev.DestroyBuffer(frame); dev.DestroyImage(output.image);
    for (auto& input : inputs) dev.DestroyImage(input);
    return ok;
}

bool DepthRun(gpu::Device& dev, DepthPicture& out) {
    constexpr uint32_t kW = 160;
    constexpr uint32_t kH = 90;
    gpu::BindingLayoutDesc empty_desc;
    const gpu::BindingLayoutHandle empty_layout = dev.CreateBindingLayout(empty_desc);
    gpu::BindingLayoutDesc frame_desc;
    frame_desc.bindings = {{0, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::All}, {1, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::All}};
    const gpu::BindingLayoutHandle frame_layout = dev.CreateBindingLayout(frame_desc);
    gpu::BindingPoolDesc pool_desc;
    pool_desc.sizes = {{gpu::BindingType::StorageBuffer, 4}};
    pool_desc.max_sets = 2;
    const gpu::BindingPoolHandle pool = dev.CreateBindingPool(pool_desc);
    const gpu::BindingLayoutHandle layouts[2] = {empty_layout, frame_layout};
    gpu::BindingSetHandle sets[2];
    bool ok = dev.AllocateBindingSets(pool, layouts, sets);
    gpu::BufferResource frame;
    gpu::BufferResource skin;
    gpu::BufferDesc bd;
    bd.size = sizeof(gpu::FrameData);
    bd.usage = gpu::BufferUsage::StorageBuffer;
    bd.host_visible = true;
    bd.name = "test frame data";
    ok = ok && dev.CreateBuffer(bd, frame);
    bd.size = 256;
    bd.name = "test skin";
    ok = ok && dev.CreateBuffer(bd, skin);
    std::vector<Vertex> vertices;
    auto add = [&](float x, float y, float z) {
        Vertex v;
        v.position = glm::vec3(x, y, z);
        vertices.push_back(v);
    };
    add(-0.9f, -0.8f, 0.3f), add(-0.9f, 0.8f, 0.3f), add(0.4f, 0.0f, 0.3f);
    add(-0.3f, -0.9f, 0.1f), add(-0.3f, 0.9f, 0.1f), add(0.9f, 0.0f, 0.6f);
    add(0.5f, -0.9f, 0.9f), add(0.95f, -0.2f, 0.9f), add(0.5f, -0.2f, 0.9f);
    gpu::BufferResource vertex_buffer;
    bd.size = vertices.size() * sizeof(Vertex);
    bd.usage = gpu::BufferUsage::VertexBuffer;
    bd.name = "test vertices";
    ok = ok && dev.CreateBuffer(bd, vertex_buffer);
    if (ok) {
        std::memset(frame.mapped, 0, sizeof(gpu::FrameData));
        std::memset(skin.mapped, 0, 256);
        gpu::View view;
        view.projection_param.w = 0.0f;
        std::memcpy(frame.mapped, &view, sizeof(view));
        std::memcpy(vertex_buffer.mapped, vertices.data(), vertices.size() * sizeof(Vertex));
        const gpu::BufferBinding buffers[2] = {{frame.buffer, 0, ~0ull}, {skin.buffer, 0, ~0ull}};
        gpu::BindingWrite writes[2];
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].set = sets[1];
            writes[i].binding = i;
            writes[i].type = gpu::BindingType::StorageBuffer;
            writes[i].buffers = std::span(&buffers[i], 1);
        }
        dev.WriteBindings(writes);
    }
    gpu::PipelineLayoutDesc ld;
    ld.sets = {empty_layout, frame_layout};
    ld.push_bytes = 128;
    const gpu::PipelineLayoutHandle layout = dev.CreatePipelineLayout(ld);
    gpu::GraphicsPipelineDesc pd;
    pd.vertex = "shadow.vert";
    pd.layout = layout;
    pd.depth = gpu::Format::D32Sfloat;
    pd.mesh_input = true;
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_compare = gpu::CompareOp::GreaterOrEqual;
    pd.cull = gpu::CullMode::Back;
    pd.depth_bias = true;
    const gpu::PipelineHandle pipeline = dev.CreateGraphicsPipeline(pd);
    ok = ok && pipeline;
    gpu::RenderTarget depth;
    depth.aspect = gpu::Aspect::Depth;
    gpu::ImageDesc td;
    td.format = gpu::Format::D32Sfloat;
    td.extent = {kW, kH, 1};
    td.usage = gpu::ImageUsage::DepthStencilAttachment | gpu::ImageUsage::TransferSrc;
    td.aspect = gpu::Aspect::Depth;
    td.name = "test depth";
    ok = ok && dev.CreateImage(td, depth.image);
    if (ok) {
        dev.Submit([&](gpu::CommandList& list) {
            list.UseTargets({{&depth, gpu::Layout::DepthAttachment}});
            list.BeginPass(gpu::Extent2D{kW, kH}, {}, &depth, false, true);
            list.SetCullMode(gpu::CullMode::Back);
            list.SetDepthBias(4.0f, 0.0f, 1.5f);
            list.BindPipeline(gpu::BindPoint::Graphics, pipeline);
            list.BindSets(gpu::BindPoint::Graphics, layout, 0, sets);
            list.BindVertexBuffer(vertex_buffer.buffer, 0);
            struct {
                glm::mat4 model{1.0f};
                glm::uvec4 ids{0, 0, 0, 0xFFFFFFFFu};
                glm::vec4 tint{1.0f};
            } push;
            list.PushConstants(layout, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment | gpu::ShaderStage::Compute, 0, sizeof(push), &push);
            list.Draw(static_cast<uint32_t>(vertices.size()), 1, 0, 0);
            list.EndPass();
            list.UseTargets({{&depth, gpu::Layout::TransferSrc}});
        });
        std::vector<uint8_t> bytes;
        ok = dev.ReadImage(depth, bytes) && bytes.size() == static_cast<size_t>(kW) * kH * 4;
        out.width = kW;
        out.height = kH;
        out.depth.resize(static_cast<size_t>(kW) * kH);
        if (ok) {
            std::memcpy(out.depth.data(), bytes.data(), bytes.size());
        }
    }
    dev.WaitIdle();
    dev.DestroyImage(depth.image);
    dev.DestroyPipeline(pipeline);
    dev.DestroyPipelineLayout(layout);
    dev.DestroyBuffer(vertex_buffer);
    dev.DestroyBuffer(skin);
    dev.DestroyBuffer(frame);
    dev.DestroyBindingPool(pool);
    dev.DestroyBindingLayout(frame_layout);
    dev.DestroyBindingLayout(empty_layout);
    return ok;
}

std::vector<uint8_t> DepthPng(const DepthPicture& d) {
    std::vector<uint8_t> out(d.depth.size() * 4, 255);
    for (size_t i = 0; i < d.depth.size(); ++i) {
        const uint8_t v = U8(d.depth[i]);
        out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = v;
    }
    return out;
}

struct Diff {
    double mean[4] = {};
    int max[4] = {};
    double psnr = 0.0;
};

Diff Compare(const Picture& a, const Picture& b, std::vector<uint8_t>& heat) {
    Diff d;
    const size_t n = static_cast<size_t>(a.width) * a.height;
    double sq = 0.0;
    heat.assign(n * 4, 255);
    for (size_t i = 0; i < n; ++i) {
        int strongest = 0;
        for (int c = 0; c < 4; ++c) {
            const int e = std::abs(static_cast<int>(a.rgba[i * 4 + c]) - static_cast<int>(b.rgba[i * 4 + c]));
            d.mean[c] += e;
            d.max[c] = std::max(d.max[c], e);
            if (c < 3) {
                sq += static_cast<double>(e) * e;
            }
            strongest = std::max(strongest, e);
        }
        const uint8_t v = static_cast<uint8_t>(std::min(255, strongest * 16));
        heat[i * 4 + 0] = v;
        heat[i * 4 + 1] = static_cast<uint8_t>(v / 4);
        heat[i * 4 + 2] = static_cast<uint8_t>(v / 4);
    }
    for (double& m : d.mean) {
        m /= static_cast<double>(n);
    }
    const double mse = sq / (static_cast<double>(n) * 3.0);
    d.psnr = mse == 0.0 ? 999.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    return d;
}

void WritePng(const fs::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    stbi_write_png(path.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba.data(), static_cast<int>(w * 4));
}

double Coverage(const Picture& img, const std::array<float, 4>& clear) {
    const uint8_t r = U8(clear[0]);
    const uint8_t g = U8(clear[1]);
    const uint8_t b = U8(clear[2]);
    size_t drawn = 0;
    const size_t n = static_cast<size_t>(img.width) * img.height;
    for (size_t i = 0; i < n; ++i) {
        if (img.rgba[i * 4] != r || img.rgba[i * 4 + 1] != g || img.rgba[i * 4 + 2] != b) {
            ++drawn;
        }
    }
    return static_cast<double>(drawn) / static_cast<double>(n);
}

bool WindowTest(gpu::d3d12::D3D12Device& device, const fs::path& out_dir, int frames) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        Check(false, std::string("window: SDL video: ") + SDL_GetError());
        return false;
    }
    constexpr uint32_t kW = 640;
    constexpr uint32_t kH = 360;
    SDL_Window* window = SDL_CreateWindow("pt D3D12 backend smoke test", kW, kH, SDL_WINDOW_NOT_FOCUSABLE);
    if (!window) {
        Check(false, std::string("window: ") + SDL_GetError());
        return false;
    }
    auto* hwnd = static_cast<HWND__*>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    std::string error;
    bool ok = device.CreateSwapchain(hwnd, kW, kH, gpu::Format::B8G8R8A8Unorm, true, error);
    Check(ok, "window: DXGI swapchain through the plain owner" + (error.empty() ? std::string() : " (" + error + ")"));
    GpuRunner runner(device);
    Case ui = UiCase(gpu::Format::B8G8R8A8Unorm, kW, kH);
    GpuRunner::Prepared p;
    ok = ok && runner.Prepare(ui, p);
    gpu::RenderTarget final_image;
    gpu::ImageDesc td;
    td.format = gpu::Format::B8G8R8A8Unorm;
    td.extent = {kW, kH, 1};
    td.usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::TransferSrc;
    td.name = "final image";
    ok = ok && device.CreateImage(td, final_image.image);
    int presented = 0;
    std::array<float, 4> clear{};
    for (int frame = 0; ok && frame < frames; ++frame) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
        }
        const float t = frame / 60.0f;
        clear = {0.1f + 0.1f * std::sin(t * 3.0f), 0.12f, 0.2f + 0.1f * std::cos(t * 2.0f), 1.0f};
        gpu::CommandList* list = device.BeginFrame(static_cast<uint32_t>(frame));
        runner.Record(*list, ui, p, final_image, clear);
        if (device.EndFrame(static_cast<uint32_t>(frame), &final_image)) {
            ++presented;
        }
    }
    device.WaitIdle();
    Check(ok && presented == frames, std::format("window: {} of {} frames presented", presented, frames));
    if (ok) {
        std::vector<uint8_t> bgra;
        final_image.layout = gpu::Layout::Undefined;
        ok = device.ReadImage(final_image, bgra);
        Picture img{kW, kH, bgra};
        for (size_t i = 0; i < img.rgba.size(); i += 4) {
            std::swap(img.rgba[i], img.rgba[i + 2]);
        }
        WritePng(out_dir / "window_last_frame.png", kW, kH, img.rgba);
        const size_t corner = (static_cast<size_t>(kH - 2) * kW + (kW - 2)) * 4;
        const bool clear_seen = std::abs(img.rgba[corner] - U8(clear[0])) <= 1 && std::abs(img.rgba[corner + 2] - U8(clear[2])) <= 1;
        const double covered = Coverage(img, clear);
        Check(ok && clear_seen && covered > 0.3 && covered < 0.9,
              std::format("window: last frame read back (clear colour in the free corner {}, UI covers {:.0f}%)", clear_seen ? "yes" : "no", covered * 100.0));
    }
    device.DestroyImage(final_image.image);
    runner.Release(p);
    device.DestroySwapchain();
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return ok;
}

}

bool MappedInputRun(gpu::Device& dev, bool snapshots) {
    gpu::BufferResource input;
    gpu::BufferResource output;
    constexpr uint32_t initial[4] = {11, 22, 33, 44};
    bool ok = dev.CreateBuffer({sizeof(initial), gpu::BufferUsage::StorageBuffer, true, false, "mapped input test", true}, input) &&
              dev.CreateBuffer({2 * sizeof(initial), gpu::BufferUsage::TransferDst, true, true, "mapped input readback"}, output);
    if (ok) {
        std::memcpy(input.mapped, initial, sizeof(initial));
        dev.FlushMapped(input, 0, ~0ull);
        auto update = [&] {
            static_cast<uint32_t*>(input.mapped)[1] = 99;
            dev.FlushMapped(input, sizeof(uint32_t), sizeof(uint32_t));
        };
        if (snapshots) {
            dev.Submit([&](gpu::CommandList& cmd) {
                cmd.CopyBuffer(input, 0, output, 0, sizeof(initial));
                update();
                cmd.CopyBuffer(input, 0, output, sizeof(initial), sizeof(initial));
            });
        } else {
            dev.Submit([&](gpu::CommandList& cmd) { cmd.CopyBuffer(input, 0, output, 0, sizeof(initial)); });
            update();
            dev.Submit([&](gpu::CommandList& cmd) { cmd.CopyBuffer(input, 0, output, sizeof(initial), sizeof(initial)); });
        }
        const uint32_t expected[8] = {11, 22, 33, 44, 11, 99, 33, 44};
        dev.InvalidateMapped(output, 0, ~0ull);
        ok = std::memcmp(output.mapped, expected, sizeof(expected)) == 0;
    }
    dev.DestroyBuffer(output);
    dev.DestroyBuffer(input);
    return ok;
}

int main(int argc, char** argv) {
    fs::path out_dir = "d3d12_backend_test";
    bool window = false;
    int frames = 120;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (a == "--window") {
            window = true;
        } else if (a == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        }
    }
    fs::create_directories(out_dir);
    pt::LogSetFile(out_dir / "d3d12_backend_test.log");

    std::string error;
    gpu::d3d12::CreateOptions options;
    options.debug = std::getenv("PT_D3D12_DEBUG") != nullptr;
    {
        _putenv_s("PT_D3D12_TEST_LACK", "sm66,typeduav");
        std::string lack_error;
        const bool refused = gpu::d3d12::Create(options, lack_error) == nullptr;
        _putenv_s("PT_D3D12_TEST_LACK", "");
        Check(refused && lack_error.find("shader model 6.6") != std::string::npos && lack_error.find("typed UAV loads") != std::string::npos,
              "a device lacking shader model 6.6 and typed UAV loads is refused without a crash: " + lack_error);
    }
    std::unique_ptr<gpu::d3d12::D3D12Device> device = gpu::d3d12::Create(options, error);
    Check(device != nullptr, "D3D12 device" + (device ? " on " + device->Info().adapter + ", " + device->RuntimeDescription() : ": " + error));
    if (!device) {
        return 1;
    }
    const char* gpu_upload = std::getenv("PT_D3D12_GPU_UPLOAD");
    Check(MappedInputRun(*device, gpu_upload && gpu_upload[0] == '0'),
          "mapped read-only inputs preserve partial updates and recorded upload snapshots");
    const std::string shader_dir = pt::ResourceDir("shaders", PT_SHADER_DIR).string();
    std::string report;
    const bool covered = device->CheckRootSignature(shader_dir, report);
    std::printf("%s", report.c_str());
    pt::LogInfo("root signature check:\n{}", report);
    Check(covered, "root signature covers every resource of every .dxil.json");

    int compute = 0;
    int compute_ok = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(shader_dir)) {
        const std::string name = e.path().filename().string();
        if (!name.ends_with(".comp.dxil")) {
            continue;
        }
        ++compute;
        gpu::ComputePipelineDesc cd;
        const std::string shader = name.substr(0, name.size() - 5);
        cd.shader = shader.c_str();
        const gpu::PipelineHandle p = device->CreateComputePipeline(cd);
        compute_ok += p ? 1 : 0;
        device->DestroyPipeline(p);
    }
    Check(compute > 0 && compute_ok == compute, std::format("{} of {} compute shaders made D3D12 pipelines", compute_ok, compute));

    VulkanRunner vulkan;
    const bool have_vulkan = vulkan.Init();
    Check(have_vulkan, "Vulkan context" + (have_vulkan ? " on " + vulkan.Adapter() : std::string()));
    GpuRunner d3d12(*device);
    std::unique_ptr<gpu::vk::Device> vk_device = have_vulkan ? std::make_unique<gpu::vk::Device>(vulkan.Context()) : nullptr;
    std::unique_ptr<GpuRunner> rhi_vulkan = vk_device ? std::make_unique<GpuRunner>(*vk_device) : nullptr;
    Check(HdrComposeRun(*device), "D3D12 production compose/bloom/LUT/DOF/motion blur preserve HDR radiance, linear filtering and SDR bounds");
    Check(vk_device && HdrComposeRun(*vk_device), "Vulkan production compose/bloom/LUT/DOF/motion blur preserve HDR radiance, linear filtering and SDR bounds");
    std::vector<Case> cases = {CompositeCase(true), CompositeCase(false), XrCopyCase(), UiCase(), CullCase(gpu::CullMode::Back),
                               CullCase(gpu::CullMode::Front)};
    Case rcas = CompositeCase(false);
    rcas.name = "composite_rcas";
    rcas.draws = {{0, 3, Floats({1,1,0,0, 0,0,0,0, 0,0,0,1, 0,0,1,0, 200,1000,1,0})}};
    cases.push_back(rcas);
    Case improved = CompositeCase(true);
    improved.name = "composite_perceptual_grain_ui";
    improved.draws = {{0, 3, Floats({1,1,1,0, 0,0,0,0, 1,2,.7f,0, .137f,.291f,1.2f,0, 200,1000,0,1})}};
    cases.push_back(improved);
    double cull_coverage[2][2] = {};
    for (const Case& c : cases) {
        Picture raw_image;
        Picture vk_image;
        Picture dx_image;
        const bool raw_ok = have_vulkan && vulkan.Run(c, raw_image);
        const bool vk_ok = rhi_vulkan && rhi_vulkan->Run(c, vk_image);
        const bool dx_ok = d3d12.Run(c, dx_image);
        if (!raw_ok || !vk_ok || !dx_ok) {
            Check(false, std::format("{}: rendered (raw Vulkan {}, pt::gpu on Vulkan {}, D3D12 {})", c.name, raw_ok ? "ok" : "failed",
                                     vk_ok ? "ok" : "failed", dx_ok ? "ok" : "failed"));
            continue;
        }
        std::vector<uint8_t> heat;
        const Diff same = Compare(raw_image, vk_image, heat);
        Check(same.psnr >= 999.0, std::format("{}: pt::gpu on Vulkan equals the raw Vulkan reference (max {} {} {} {})", c.name, same.max[0],
                                              same.max[1], same.max[2], same.max[3]));
        const Diff d = Compare(vk_image, dx_image, heat);
        WritePng(out_dir / (c.name + "_vulkan.png"), c.width, c.height, vk_image.rgba);
        WritePng(out_dir / (c.name + "_d3d12.png"), c.width, c.height, dx_image.rgba);
        WritePng(out_dir / (c.name + "_diff_x16.png"), c.width, c.height, heat);
        const bool mean_ok = d.mean[0] <= 0.5 && d.mean[1] <= 0.5 && d.mean[2] <= 0.5 && d.mean[3] <= 0.5;
        Check(d.psnr >= 45.0 || mean_ok,
              std::format("{}: PSNR {:.2f} dB, mean abs diff R {:.4f} G {:.4f} B {:.4f} A {:.4f}, max {} {} {} {}", c.name, d.psnr, d.mean[0],
                          d.mean[1], d.mean[2], d.mean[3], d.max[0], d.max[1], d.max[2], d.max[3]));
        if (c.name.starts_with("cull_")) {
            const int i = c.cull == gpu::CullMode::Back ? 0 : 1;
            cull_coverage[i][0] = Coverage(vk_image, c.clear);
            cull_coverage[i][1] = Coverage(dx_image, c.clear);
        }
    }
    auto renderer_case = [&](const std::string& name, const std::function<bool(gpu::Device&, Picture&)>& run, const Picture* input) {
        Picture vk_image;
        Picture dx_image;
        const bool vk_ok = vk_device && run(*vk_device, vk_image);
        const bool dx_ok = run(*device, dx_image);
        if (!vk_ok || !dx_ok) {
            Check(false, std::format("{}: rendered (Vulkan {}, D3D12 {})", name, vk_ok ? "ok" : "failed", dx_ok ? "ok" : "failed"));
            return;
        }
        std::vector<uint8_t> heat;
        if (input) {
            const Diff changed = Compare(*input, vk_image, heat);
            size_t moved = 0;
            for (size_t i = 0; i < heat.size(); i += 4) {
                moved += heat[i] != 0 ? 1 : 0;
            }
            Check(moved > 0, std::format("{}: the pass changed {:.1f}% of its target (PSNR {:.2f} dB against its input)", name,
                                         100.0 * moved / (heat.size() / 4), changed.psnr));
            WritePng(out_dir / (name + "_input.png"), input->width, input->height, input->rgba);
        }
        const Diff d = Compare(vk_image, dx_image, heat);
        WritePng(out_dir / (name + "_vulkan.png"), vk_image.width, vk_image.height, vk_image.rgba);
        WritePng(out_dir / (name + "_d3d12.png"), dx_image.width, dx_image.height, dx_image.rgba);
        WritePng(out_dir / (name + "_diff_x16.png"), vk_image.width, vk_image.height, heat);
        const bool mean_ok = d.mean[0] <= 0.5 && d.mean[1] <= 0.5 && d.mean[2] <= 0.5 && d.mean[3] <= 0.5;
        Check(d.psnr >= 45.0 || mean_ok, std::format("{}: PSNR {:.2f} dB, mean abs diff R {:.4f} G {:.4f} B {:.4f} A {:.4f}, max {} {} {} {}", name,
                                                     d.psnr, d.mean[0], d.mean[1], d.mean[2], d.mean[3], d.max[0], d.max[1], d.max[2], d.max[3]));
    };
    const Picture sss_input = SssInput();
    renderer_case("subsurface_pass", SubsurfaceRun, &sss_input);
    renderer_case("texture_manager", TextureManagerRun, nullptr);
    {
        DepthPicture vk_depth;
        DepthPicture dx_depth;
        const bool vk_ok = vk_device && DepthRun(*vk_device, vk_depth);
        const bool dx_ok = DepthRun(*device, dx_depth);
        if (!vk_ok || !dx_ok) {
            Check(false, std::format("depth_pass: rendered (Vulkan {}, D3D12 {})", vk_ok ? "ok" : "failed", dx_ok ? "ok" : "failed"));
        } else {
            size_t covered[2] = {};
            size_t differ = 0;
            size_t mask_differ = 0;
            double max_diff = 0.0;
            for (size_t i = 0; i < vk_depth.depth.size(); ++i) {
                const bool a = vk_depth.depth[i] != 0.0f;
                const bool b = dx_depth.depth[i] != 0.0f;
                covered[0] += a ? 1 : 0;
                covered[1] += b ? 1 : 0;
                mask_differ += a != b ? 1 : 0;
                const double d = std::abs(static_cast<double>(vk_depth.depth[i]) - dx_depth.depth[i]);
                differ += d > 0.0 ? 1 : 0;
                max_diff = std::max(max_diff, d);
            }
            WritePng(out_dir / "depth_pass_vulkan.png", vk_depth.width, vk_depth.height, DepthPng(vk_depth));
            WritePng(out_dir / "depth_pass_d3d12.png", dx_depth.width, dx_depth.height, DepthPng(dx_depth));
            const size_t n = vk_depth.depth.size();
            Check(covered[0] > n / 4 && mask_differ == 0 && max_diff <= 1.0e-5,
                  std::format("depth_pass: drawn {:.1f}% / {:.1f}% (Vulkan / D3D12), {} texels differ in coverage, {} in value, largest difference {:.3g}",
                              100.0 * covered[0] / n, 100.0 * covered[1] / n, mask_differ, differ, max_diff));
        }
    }
    {
        ComputeResult vk_result;
        ComputeResult dx_result;
        const bool vk_ok = vk_device && ComputeRun(*vk_device, vk_result);
        const bool dx_ok = ComputeRun(*device, dx_result);
        const bool same = vk_ok && dx_ok && std::memcmp(vk_result.values, dx_result.values, sizeof(vk_result.values)) == 0;
        Check(same && vk_result.values[3] == 1.0f,
              std::format("reflect_colour.comp: Vulkan ({:.4f} {:.4f} {:.4f} {:.1f}), D3D12 ({:.4f} {:.4f} {:.4f} {:.1f}){}", vk_result.values[0],
                          vk_result.values[1], vk_result.values[2], vk_result.values[3], dx_result.values[0], dx_result.values[1], dx_result.values[2],
                          dx_result.values[3], same ? ", bit-identical" : ", differ"));
        Check(vk_result.timestamps && dx_result.timestamps,
              std::format("timestamps around the dispatch: Vulkan {} ticks ({:.1f} us), D3D12 {} ticks ({:.1f} us)", vk_result.ticks,
                          vk_device ? vk_result.ticks * vk_device->Info().timestamp_period_ns / 1000.0 : 0.0, dx_result.ticks,
                          dx_result.ticks * device->Info().timestamp_period_ns / 1000.0));
    }
    for (float level : {0.0f, 0.001f, 0.18f, 4.0f, 100000.0f}) {
        ComputeResult vk_meter, dx_meter;
        bool ok = vk_device && ComputeRun(*vk_device,vk_meter,true,level) && ComputeRun(*device,dx_meter,true,level);
        Check(ok && std::abs(vk_meter.metered-level)<std::max(1e-6f,level*1e-5f) &&
              std::abs(dx_meter.metered-level)<std::max(1e-6f,level*1e-5f),
              std::format("luminance.comp covered/uncovered HDR level {}: Vulkan {}, D3D12 {}",level,vk_meter.metered,dx_meter.metered));
    }
    const bool vk_one = (cull_coverage[0][0] > 0.99) != (cull_coverage[1][0] > 0.99);
    const bool same = (cull_coverage[0][0] > 0.99) == (cull_coverage[0][1] > 0.99) && (cull_coverage[1][0] > 0.99) == (cull_coverage[1][1] > 0.99);
    Check(vk_one && same, std::format("winding after the Y flip: cull back draws {:.0f}% / {:.0f}%, cull front {:.0f}% / {:.0f}% (Vulkan / D3D12)",
                                      cull_coverage[0][0] * 100, cull_coverage[0][1] * 100, cull_coverage[1][0] * 100, cull_coverage[1][1] * 100));

    if (window) {
        WindowTest(*device, out_dir, frames);
    }
    const uint32_t debug_errors = device->DebugErrors();
    Check(debug_errors == 0, std::format("D3D12 debug layer errors: {}{}", debug_errors, options.debug ? "" : " (debug layer off)"));
    device.reset();
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
