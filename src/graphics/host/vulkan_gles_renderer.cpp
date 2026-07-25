#include "vulkan_gles_renderer.hpp"

#include <shaderc/shaderc.hpp>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ilegacysim/display.hpp"
#include "ilegacysim/gles_abi.hpp"
#include "ilegacysim/gles_resources.hpp"

namespace ilegacysim {
namespace {

constexpr VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
constexpr std::uint64_t fence_timeout_nanoseconds = 5'000'000'000ULL;

std::uint64_t pixel_hash(std::span<const std::uint32_t> pixels,
                         std::uint32_t width, std::uint32_t height) {
    auto hash = 1469598103934665603ULL;
    for (const auto pixel : pixels) {
        hash ^= pixel;
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<std::uint64_t>(width) << 32U | height;
    return hash;
}

constexpr std::string_view vertex_shader_source = R"(
#version 450
layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_texture0;
layout(location = 3) in vec2 in_texture1;
layout(location = 0) out vec4 primary_color;
layout(location = 1) out vec2 texture0;
layout(location = 2) out vec2 texture1;

void main() {
    gl_Position = vec4(
        in_position.x, -in_position.y, in_position.z, in_position.w);
    primary_color = in_color;
    texture0 = in_texture0;
    texture1 = in_texture1;
}
)";

constexpr std::string_view fragment_shader_source = R"(
#version 450

const int GL_ADD = 0x0104;
const int GL_BLEND = 0x0be2;
const int GL_SRC_COLOR = 0x0300;
const int GL_ONE_MINUS_SRC_COLOR = 0x0301;
const int GL_SRC_ALPHA = 0x0302;
const int GL_ONE_MINUS_SRC_ALPHA = 0x0303;
const int GL_REPLACE = 0x1e01;
const int GL_MODULATE = 0x2100;
const int GL_DECAL = 0x2101;
const int GL_TEXTURE = 0x1702;
const int GL_COMBINE = 0x8570;
const int GL_COMBINE_RGB = 0x8571;
const int GL_COMBINE_ALPHA = 0x8572;
const int GL_ADD_SIGNED = 0x8574;
const int GL_INTERPOLATE = 0x8575;
const int GL_CONSTANT = 0x8576;
const int GL_PRIMARY_COLOR = 0x8577;
const int GL_PREVIOUS = 0x8578;
const int GL_SUBTRACT = 0x84e7;
const int GL_DOT3_RGB = 0x86ae;
const int GL_DOT3_RGBA = 0x86af;

struct TextureEnvironment {
    ivec4 mode_combine_enabled;
    vec4 color;
    ivec4 rgb_sources;
    ivec4 alpha_sources;
    ivec4 rgb_operands;
    ivec4 alpha_operands;
    vec4 scales_rectangle;
};

layout(std140, binding = 0) uniform FixedFunctionState {
    TextureEnvironment units[2];
} fixed_state;
layout(binding = 1) uniform sampler2D image0;
layout(binding = 2) uniform sampler2D image1;

layout(location = 0) in vec4 primary_color;
layout(location = 1) in vec2 texture0;
layout(location = 2) in vec2 texture1;
layout(location = 0) out vec4 output_color;

vec4 select_source(
    int source, vec4 texture_color, vec4 constant_color,
    vec4 primary, vec4 previous) {
    if (source == GL_TEXTURE) return texture_color;
    if (source == GL_CONSTANT) return constant_color;
    if (source == GL_PRIMARY_COLOR) return primary;
    if (source == GL_PREVIOUS) return previous;
    return vec4(0.0);
}

vec4 apply_rgb_operand(vec4 source, int operand) {
    if (operand == GL_SRC_ALPHA) return vec4(source.a);
    if (operand == GL_ONE_MINUS_SRC_ALPHA) return vec4(1.0 - source.a);
    if (operand == GL_ONE_MINUS_SRC_COLOR) return vec4(1.0) - source;
    return source;
}

float apply_alpha_operand(vec4 source, int operand) {
    return operand == GL_ONE_MINUS_SRC_ALPHA ? 1.0 - source.a : source.a;
}

float combine_component(int mode, float a, float b, float c) {
    if (mode == GL_REPLACE) return a;
    if (mode == GL_MODULATE) return a * b;
    if (mode == GL_ADD) return a + b;
    if (mode == GL_ADD_SIGNED) return a + b - 0.5;
    if (mode == GL_INTERPOLATE) return a * c + b * (1.0 - c);
    if (mode == GL_SUBTRACT) return a - b;
    return 0.0;
}

vec4 sample_image(
    sampler2D image, vec2 coordinate, bool rectangle_coordinates) {
    if (!rectangle_coordinates) return texture(image, coordinate);
    vec2 size = vec2(textureSize(image, 0));
    vec2 texel = (floor(coordinate) + vec2(0.5)) / size;
    return texture(image, texel);
}

vec4 apply_environment(
    TextureEnvironment environment, vec4 sampled, vec4 primary,
    vec4 previous) {
    int mode = environment.mode_combine_enabled.x;
    if (mode == GL_REPLACE) return sampled;
    if (mode == GL_MODULATE) return previous * sampled;
    if (mode == GL_DECAL) {
        return vec4(
            mix(previous.rgb, sampled.rgb, sampled.a), previous.a);
    }
    if (mode == GL_BLEND) {
        return vec4(
            mix(previous.rgb, environment.color.rgb, sampled.rgb),
            previous.a * sampled.a);
    }
    if (mode == GL_ADD) {
        return vec4(previous.rgb + sampled.rgb, previous.a * sampled.a);
    }
    if (mode != GL_COMBINE) return previous;

    vec4 rgb_arguments[3];
    float alpha_arguments[3];
    for (int argument = 0; argument < 3; ++argument) {
        vec4 rgb_source = select_source(
            environment.rgb_sources[argument], sampled,
            environment.color, primary, previous);
        rgb_arguments[argument] = apply_rgb_operand(
            rgb_source, environment.rgb_operands[argument]);
        vec4 alpha_source = select_source(
            environment.alpha_sources[argument], sampled,
            environment.color, primary, previous);
        alpha_arguments[argument] = apply_alpha_operand(
            alpha_source, environment.alpha_operands[argument]);
    }

    int rgb_mode = environment.mode_combine_enabled.y;
    int alpha_mode = environment.mode_combine_enabled.z;
    vec4 result = previous;
    if (rgb_mode == GL_DOT3_RGB || rgb_mode == GL_DOT3_RGBA) {
        float value = 4.0 * dot(
            rgb_arguments[0].rgb - vec3(0.5),
            rgb_arguments[1].rgb - vec3(0.5));
        result.rgb = vec3(value);
        if (rgb_mode == GL_DOT3_RGBA) result.a = value;
    } else {
        for (int component = 0; component < 3; ++component) {
            result[component] = combine_component(
                rgb_mode, rgb_arguments[0][component],
                rgb_arguments[1][component],
                rgb_arguments[2][component]);
        }
    }
    if (rgb_mode != GL_DOT3_RGBA) {
        result.a = combine_component(
            alpha_mode, alpha_arguments[0], alpha_arguments[1],
            alpha_arguments[2]);
    }
    result.rgb *= environment.scales_rectangle.x;
    result.a *= environment.scales_rectangle.y;
    return result;
}

void main() {
    vec4 result = primary_color;
    if (fixed_state.units[0].mode_combine_enabled.w != 0) {
        vec4 sampled = sample_image(
            image0, texture0,
            fixed_state.units[0].scales_rectangle.z != 0.0);
        result = apply_environment(
            fixed_state.units[0], sampled, primary_color, result);
    }
    if (fixed_state.units[1].mode_combine_enabled.w != 0) {
        vec4 sampled = sample_image(
            image1, texture1,
            fixed_state.units[1].scales_rectangle.z != 0.0);
        result = apply_environment(
            fixed_state.units[1], sampled, primary_color, result);
    }
    output_color = clamp(result, 0.0, 1.0);
}
)";

void require_success(VkResult result, std::string_view operation) {
    if (result == VK_SUCCESS)
        return;
    throw std::runtime_error{std::string{operation} + " failed with VkResult " +
                             std::to_string(static_cast<std::int32_t>(result))};
}

template <typename Structure>
Structure make_vulkan_structure(VkStructureType type) {
    Structure value{};
    value.sType = type;
    return value;
}

std::vector<std::uint32_t> compile_shader(std::string_view source,
                                          shaderc_shader_kind kind,
                                          std::string_view name) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan,
                                 shaderc_env_version_vulkan_1_0);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    const auto result = compiler.CompileGlslToSpv(
        source.data(), source.size(), kind, std::string{name}.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        throw std::runtime_error{std::string{name} + ": " +
                                 result.GetErrorMessage()};
    }
    return {result.cbegin(), result.cend()};
}

struct GpuVertex {
    std::array<float, 4> position{};
    std::array<float, 4> color{};
    std::array<float, 2> texture0{};
    std::array<float, 2> texture1{};
};

struct alignas(16) GpuTextureEnvironment {
    std::array<std::int32_t, 4> mode_combine_enabled{};
    std::array<float, 4> color{};
    std::array<std::int32_t, 4> rgb_sources{};
    std::array<std::int32_t, 4> alpha_sources{};
    std::array<std::int32_t, 4> rgb_operands{};
    std::array<std::int32_t, 4> alpha_operands{};
    std::array<float, 4> scales_rectangle{};
};

struct alignas(16) GpuFixedFunctionState {
    std::array<GpuTextureEnvironment, gles_abi::texture_unit_count> units{};
};

static_assert(sizeof(GpuVertex) == 48);
static_assert(sizeof(GpuTextureEnvironment) == 112);

struct PipelineKey {
    bool blend_enabled{};
    std::uint32_t blend_source{};
    std::uint32_t blend_destination{};
    std::uint8_t color_mask{};

    auto operator<=>(const PipelineKey&) const = default;
};

class VulkanGlesRenderer final : public GlesRenderer {
  public:
    VulkanGlesRenderer();
    ~VulkanGlesRenderer() override;

    VulkanGlesRenderer(const VulkanGlesRenderer&) = delete;
    VulkanGlesRenderer& operator=(const VulkanGlesRenderer&) = delete;

    bool draw(DisplayFrame& frame, GlesRenderTargetKey target,
              std::span<const GlesRasterVertex> vertices, std::uint32_t mode,
              const GlesRasterState& state) override;
    bool synchronize(DisplayFrame& frame, GlesRenderTargetKey target) override;
    void invalidate(GlesRenderTargetKey target) override;
    void release(GlesRenderTargetKey target) override;
    [[nodiscard]] std::string_view name() const override {
        return renderer_name_;
    }
    [[nodiscard]] bool accelerated() const override { return true; }

  private:
    struct Buffer {
        VkDevice device{};
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        VkDeviceSize size{};
        bool coherent{};

        Buffer() = default;
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;
        ~Buffer();
    };

    struct Image {
        VkDevice device{};
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkSampler sampler{};
        std::uint32_t width{};
        std::uint32_t height{};
        bool rectangle{};

        Image() = default;
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        Image(Image&& other) noexcept;
        Image& operator=(Image&& other) noexcept;
        ~Image();
    };

    [[nodiscard]] std::uint32_t
    find_memory_type(std::uint32_t candidates, VkMemoryPropertyFlags required,
                     VkMemoryPropertyFlags preferred = 0) const;
    [[nodiscard]] Buffer
    create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags required,
                  VkMemoryPropertyFlags preferred = 0) const;
    [[nodiscard]] Image create_image(std::uint32_t width, std::uint32_t height,
                                     VkImageUsageFlags usage, bool sampled,
                                     bool rectangle) const;
    void ensure_buffer(Buffer& buffer, VkDeviceSize size,
                       VkBufferUsageFlags usage) const;
    void ensure_target(std::uint32_t width, std::uint32_t height);
    void ensure_texture(std::size_t index, std::uint32_t width,
                        std::uint32_t height, bool rectangle);
    void upload(Buffer& buffer, const void* data, std::size_t size) const;
    void download(Buffer& buffer, void* destination, std::size_t size) const;
    [[nodiscard]] VkShaderModule
    create_shader_module(std::span<const std::uint32_t> code) const;
    [[nodiscard]] VkPipeline pipeline(const PipelineKey& key);
    [[nodiscard]] std::optional<VkBlendFactor>
    blend_factor(std::uint32_t factor) const;
    [[nodiscard]] std::vector<GpuVertex>
    expand_vertices(std::span<const GlesRasterVertex> vertices,
                    std::uint32_t mode, const GlesRasterState& state) const;
    [[nodiscard]] GpuFixedFunctionState
    fixed_function_state(const GlesRasterState& state) const;
    void transition_image(VkCommandBuffer command, VkImage image,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkPipelineStageFlags source_stage,
                          VkPipelineStageFlags destination_stage,
                          VkAccessFlags source_access,
                          VkAccessFlags destination_access) const;
    void destroy() noexcept;

    VkInstance instance_{};
    VkPhysicalDevice physical_device_{};
    VkDevice device_{};
    VkQueue queue_{};
    std::uint32_t queue_family_{};
    VkPhysicalDeviceMemoryProperties memory_properties_{};
    VkCommandPool command_pool_{};
    VkCommandBuffer command_buffer_{};
    VkFence fence_{};
    VkRenderPass render_pass_{};
    VkDescriptorSetLayout descriptor_layout_{};
    VkPipelineLayout pipeline_layout_{};
    VkDescriptorPool descriptor_pool_{};
    VkDescriptorSet descriptor_{};
    VkShaderModule vertex_shader_{};
    VkShaderModule fragment_shader_{};
    std::map<PipelineKey, VkPipeline> pipelines_;
    Buffer target_upload_;
    Buffer target_download_;
    Buffer vertex_buffer_;
    Buffer uniform_buffer_;
    std::array<Buffer, gles_abi::texture_unit_count> texture_uploads_;
    Image target_image_;
    std::array<Image, gles_abi::texture_unit_count> texture_images_;
    VkFramebuffer framebuffer_{};
    bool target_valid_{};
    std::uint64_t target_cpu_hash_{};
    VkImageLayout target_layout_{VK_IMAGE_LAYOUT_UNDEFINED};
    std::optional<GlesRenderTargetKey> active_target_;
    std::array<bool, gles_abi::texture_unit_count> texture_initialized_{};
    std::array<std::uint64_t, gles_abi::texture_unit_count> texture_hashes_{};
    std::string renderer_name_;
    std::mutex mutex_;
};

VulkanGlesRenderer::Buffer::Buffer(Buffer&& other) noexcept
    : device{std::exchange(other.device, VK_NULL_HANDLE)},
      buffer{std::exchange(other.buffer, VK_NULL_HANDLE)},
      memory{std::exchange(other.memory, VK_NULL_HANDLE)},
      size{std::exchange(other.size, 0)},
      coherent{std::exchange(other.coherent, false)} {}

VulkanGlesRenderer::Buffer&
VulkanGlesRenderer::Buffer::operator=(Buffer&& other) noexcept {
    if (this == &other)
        return *this;
    if (buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE)
        vkFreeMemory(device, memory, nullptr);
    device = std::exchange(other.device, VK_NULL_HANDLE);
    buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
    memory = std::exchange(other.memory, VK_NULL_HANDLE);
    size = std::exchange(other.size, 0);
    coherent = std::exchange(other.coherent, false);
    return *this;
}

VulkanGlesRenderer::Buffer::~Buffer() {
    if (buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE)
        vkFreeMemory(device, memory, nullptr);
}

VulkanGlesRenderer::Image::Image(Image&& other) noexcept
    : device{std::exchange(other.device, VK_NULL_HANDLE)},
      image{std::exchange(other.image, VK_NULL_HANDLE)},
      memory{std::exchange(other.memory, VK_NULL_HANDLE)},
      view{std::exchange(other.view, VK_NULL_HANDLE)},
      sampler{std::exchange(other.sampler, VK_NULL_HANDLE)},
      width{std::exchange(other.width, 0)},
      height{std::exchange(other.height, 0)},
      rectangle{std::exchange(other.rectangle, false)} {}

VulkanGlesRenderer::Image&
VulkanGlesRenderer::Image::operator=(Image&& other) noexcept {
    if (this == &other)
        return *this;
    if (sampler != VK_NULL_HANDLE)
        vkDestroySampler(device, sampler, nullptr);
    if (view != VK_NULL_HANDLE)
        vkDestroyImageView(device, view, nullptr);
    if (image != VK_NULL_HANDLE)
        vkDestroyImage(device, image, nullptr);
    if (memory != VK_NULL_HANDLE)
        vkFreeMemory(device, memory, nullptr);
    device = std::exchange(other.device, VK_NULL_HANDLE);
    image = std::exchange(other.image, VK_NULL_HANDLE);
    memory = std::exchange(other.memory, VK_NULL_HANDLE);
    view = std::exchange(other.view, VK_NULL_HANDLE);
    sampler = std::exchange(other.sampler, VK_NULL_HANDLE);
    width = std::exchange(other.width, 0);
    height = std::exchange(other.height, 0);
    rectangle = std::exchange(other.rectangle, false);
    return *this;
}

VulkanGlesRenderer::Image::~Image() {
    if (sampler != VK_NULL_HANDLE)
        vkDestroySampler(device, sampler, nullptr);
    if (view != VK_NULL_HANDLE)
        vkDestroyImageView(device, view, nullptr);
    if (image != VK_NULL_HANDLE)
        vkDestroyImage(device, image, nullptr);
    if (memory != VK_NULL_HANDLE)
        vkFreeMemory(device, memory, nullptr);
}

VulkanGlesRenderer::VulkanGlesRenderer() {
    try {
        const auto vertex_code =
            compile_shader(vertex_shader_source, shaderc_glsl_vertex_shader,
                           "iLegacySim GLES vertex shader");
        const auto fragment_code =
            compile_shader(fragment_shader_source, shaderc_glsl_fragment_shader,
                           "iLegacySim GLES fragment shader");

        auto application = make_vulkan_structure<VkApplicationInfo>(
            VK_STRUCTURE_TYPE_APPLICATION_INFO);
        application.pApplicationName = "iLegacySim";
        application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        application.pEngineName = "iLegacySim GLES HLE";
        application.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        application.apiVersion = VK_API_VERSION_1_0;

        auto instance_info = make_vulkan_structure<VkInstanceCreateInfo>(
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        instance_info.pApplicationInfo = &application;
        require_success(vkCreateInstance(&instance_info, nullptr, &instance_),
                        "vkCreateInstance");

        std::uint32_t device_count{};
        require_success(
            vkEnumeratePhysicalDevices(instance_, &device_count, nullptr),
            "vkEnumeratePhysicalDevices");
        if (device_count == 0) {
            throw std::runtime_error{"Vulkan exposes no physical devices"};
        }
        std::vector<VkPhysicalDevice> devices(device_count);
        require_success(vkEnumeratePhysicalDevices(instance_, &device_count,
                                                   devices.data()),
                        "vkEnumeratePhysicalDevices");

        int best_score = std::numeric_limits<int>::min();
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
                continue;
            }
            std::uint32_t family_count{};
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count,
                                                     nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count,
                                                     families.data());
            for (std::uint32_t family = 0; family < family_count; ++family) {
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) ==
                    0) {
                    continue;
                }
                int score{};
                if (properties.deviceType ==
                    VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                    score = 300;
                } else if (properties.deviceType ==
                           VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                    score = 200;
                } else if (properties.deviceType ==
                           VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) {
                    score = 100;
                }
                if (score <= best_score)
                    continue;
                best_score = score;
                physical_device_ = candidate;
                queue_family_ = family;
                renderer_name_ = "iLegacySim GLES 1.1 Vulkan (" +
                                 std::string{properties.deviceName} + ")";
            }
        }
        if (physical_device_ == VK_NULL_HANDLE) {
            throw std::runtime_error{
                "Vulkan exposes no hardware graphics queue"};
        }

        vkGetPhysicalDeviceMemoryProperties(physical_device_,
                                            &memory_properties_);
        constexpr float queue_priority = 1.0F;
        auto queue_info = make_vulkan_structure<VkDeviceQueueCreateInfo>(
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;
        auto device_info = make_vulkan_structure<VkDeviceCreateInfo>(
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        require_success(
            vkCreateDevice(physical_device_, &device_info, nullptr, &device_),
            "vkCreateDevice");
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

        auto command_pool_info = make_vulkan_structure<VkCommandPoolCreateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        command_pool_info.flags =
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_pool_info.queueFamilyIndex = queue_family_;
        require_success(vkCreateCommandPool(device_, &command_pool_info,
                                            nullptr, &command_pool_),
                        "vkCreateCommandPool");
        auto command_info = make_vulkan_structure<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        command_info.commandPool = command_pool_;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        require_success(
            vkAllocateCommandBuffers(device_, &command_info, &command_buffer_),
            "vkAllocateCommandBuffers");
        auto fence_info = make_vulkan_structure<VkFenceCreateInfo>(
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        require_success(vkCreateFence(device_, &fence_info, nullptr, &fence_),
                        "vkCreateFence");

        VkAttachmentDescription attachment{};
        attachment.format = color_format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color_reference{
            0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_reference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        auto render_pass_info = make_vulkan_structure<VkRenderPassCreateInfo>(
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments = &attachment;
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;
        render_pass_info.dependencyCount = 1;
        render_pass_info.pDependencies = &dependency;
        require_success(vkCreateRenderPass(device_, &render_pass_info, nullptr,
                                           &render_pass_),
                        "vkCreateRenderPass");

        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        for (std::uint32_t binding = 1; binding < bindings.size(); ++binding) {
            bindings[binding].binding = binding;
            bindings[binding].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[binding].descriptorCount = 1;
            bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        auto descriptor_layout_info =
            make_vulkan_structure<VkDescriptorSetLayoutCreateInfo>(
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        descriptor_layout_info.bindingCount =
            static_cast<std::uint32_t>(bindings.size());
        descriptor_layout_info.pBindings = bindings.data();
        require_success(
            vkCreateDescriptorSetLayout(device_, &descriptor_layout_info,
                                        nullptr, &descriptor_layout_),
            "vkCreateDescriptorSetLayout");
        auto pipeline_layout_info =
            make_vulkan_structure<VkPipelineLayoutCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &descriptor_layout_;
        require_success(vkCreatePipelineLayout(device_, &pipeline_layout_info,
                                               nullptr, &pipeline_layout_),
                        "vkCreatePipelineLayout");

        std::array<VkDescriptorPoolSize, 2> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
        pool_sizes[1] = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            static_cast<std::uint32_t>(gles_abi::texture_unit_count)};
        auto descriptor_pool_info =
            make_vulkan_structure<VkDescriptorPoolCreateInfo>(
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        descriptor_pool_info.maxSets = 1;
        descriptor_pool_info.poolSizeCount =
            static_cast<std::uint32_t>(pool_sizes.size());
        descriptor_pool_info.pPoolSizes = pool_sizes.data();
        require_success(vkCreateDescriptorPool(device_, &descriptor_pool_info,
                                               nullptr, &descriptor_pool_),
                        "vkCreateDescriptorPool");
        auto descriptor_info =
            make_vulkan_structure<VkDescriptorSetAllocateInfo>(
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        descriptor_info.descriptorPool = descriptor_pool_;
        descriptor_info.descriptorSetCount = 1;
        descriptor_info.pSetLayouts = &descriptor_layout_;
        require_success(
            vkAllocateDescriptorSets(device_, &descriptor_info, &descriptor_),
            "vkAllocateDescriptorSets");

        vertex_shader_ = create_shader_module(vertex_code);
        fragment_shader_ = create_shader_module(fragment_code);
    } catch (...) {
        destroy();
        throw;
    }
}

VulkanGlesRenderer::~VulkanGlesRenderer() {
    destroy();
}

void VulkanGlesRenderer::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (framebuffer_ != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_, framebuffer_, nullptr);
            framebuffer_ = VK_NULL_HANDLE;
        }
        target_image_ = {};
        for (auto& image : texture_images_) {
            image = {};
        }
        target_upload_ = {};
        target_download_ = {};
        vertex_buffer_ = {};
        uniform_buffer_ = {};
        for (auto& buffer : texture_uploads_) {
            buffer = {};
        }
        active_target_.reset();
        for (const auto& [key, value] : pipelines_) {
            static_cast<void>(key);
            vkDestroyPipeline(device_, value, nullptr);
        }
        pipelines_.clear();
        if (fragment_shader_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, fragment_shader_, nullptr);
        }
        if (vertex_shader_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, vertex_shader_, nullptr);
        }
        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        }
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        }
        if (descriptor_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
        }
        if (render_pass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, render_pass_, nullptr);
        }
        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, fence_, nullptr);
        }
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
        }
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

std::uint32_t
VulkanGlesRenderer::find_memory_type(std::uint32_t candidates,
                                     VkMemoryPropertyFlags required,
                                     VkMemoryPropertyFlags preferred) const {
    std::optional<std::uint32_t> fallback;
    for (std::uint32_t index = 0; index < memory_properties_.memoryTypeCount;
         ++index) {
        if ((candidates & (1U << index)) == 0)
            continue;
        const auto flags = memory_properties_.memoryTypes[index].propertyFlags;
        if ((flags & required) != required)
            continue;
        if ((flags & preferred) == preferred)
            return index;
        if (!fallback)
            fallback = index;
    }
    if (fallback)
        return *fallback;
    throw std::runtime_error{"Vulkan has no compatible memory type"};
}

VulkanGlesRenderer::Buffer
VulkanGlesRenderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags required,
                                  VkMemoryPropertyFlags preferred) const {
    Buffer result;
    result.device = device_;
    result.size = size;
    auto buffer_info = make_vulkan_structure<VkBufferCreateInfo>(
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    require_success(
        vkCreateBuffer(device_, &buffer_info, nullptr, &result.buffer),
        "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, result.buffer, &requirements);
    const auto memory_type =
        find_memory_type(requirements.memoryTypeBits, required, preferred);
    result.coherent =
        (memory_properties_.memoryTypes[memory_type].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    auto allocation = make_vulkan_structure<VkMemoryAllocateInfo>(
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    require_success(
        vkAllocateMemory(device_, &allocation, nullptr, &result.memory),
        "vkAllocateMemory(buffer)");
    require_success(
        vkBindBufferMemory(device_, result.buffer, result.memory, 0),
        "vkBindBufferMemory");
    return result;
}

VulkanGlesRenderer::Image
VulkanGlesRenderer::create_image(std::uint32_t width, std::uint32_t height,
                                 VkImageUsageFlags usage, bool sampled,
                                 bool rectangle) const {
    Image result;
    result.device = device_;
    result.width = width;
    result.height = height;
    result.rectangle = rectangle;
    auto image_info = make_vulkan_structure<VkImageCreateInfo>(
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = color_format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    require_success(vkCreateImage(device_, &image_info, nullptr, &result.image),
                    "vkCreateImage");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, result.image, &requirements);
    auto allocation = make_vulkan_structure<VkMemoryAllocateInfo>(
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = find_memory_type(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    require_success(
        vkAllocateMemory(device_, &allocation, nullptr, &result.memory),
        "vkAllocateMemory(image)");
    require_success(vkBindImageMemory(device_, result.image, result.memory, 0),
                    "vkBindImageMemory");

    auto view_info = make_vulkan_structure<VkImageViewCreateInfo>(
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view_info.image = result.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = color_format;
    view_info.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    require_success(
        vkCreateImageView(device_, &view_info, nullptr, &result.view),
        "vkCreateImageView");

    if (sampled) {
        auto sampler_info = make_vulkan_structure<VkSamplerCreateInfo>(
            VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = rectangle
                                        ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                        : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeV = sampler_info.addressModeU;
        sampler_info.addressModeW = sampler_info.addressModeU;
        sampler_info.maxAnisotropy = 1.0F;
        sampler_info.minLod = 0.0F;
        sampler_info.maxLod = 0.0F;
        require_success(
            vkCreateSampler(device_, &sampler_info, nullptr, &result.sampler),
            "vkCreateSampler");
    }
    return result;
}

void VulkanGlesRenderer::ensure_buffer(Buffer& buffer, VkDeviceSize size,
                                       VkBufferUsageFlags usage) const {
    if (buffer.buffer != VK_NULL_HANDLE && buffer.size >= size)
        return;
    const auto capacity = std::bit_ceil(std::max<VkDeviceSize>(size, 4096));
    buffer = create_buffer(capacity, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

void VulkanGlesRenderer::ensure_target(std::uint32_t width,
                                       std::uint32_t height) {
    if (target_image_.image != VK_NULL_HANDLE && target_image_.width == width &&
        target_image_.height == height) {
        return;
    }
    if (framebuffer_ != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
    }
    target_image_ = create_image(width, height,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 false, false);
    auto framebuffer_info = make_vulkan_structure<VkFramebufferCreateInfo>(
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    framebuffer_info.renderPass = render_pass_;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &target_image_.view;
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = 1;
    require_success(
        vkCreateFramebuffer(device_, &framebuffer_info, nullptr, &framebuffer_),
        "vkCreateFramebuffer");
    target_valid_ = false;
    target_cpu_hash_ = 0;
    target_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanGlesRenderer::ensure_texture(std::size_t index, std::uint32_t width,
                                        std::uint32_t height, bool rectangle) {
    auto& image = texture_images_.at(index);
    if (image.image != VK_NULL_HANDLE && image.width == width &&
        image.height == height && image.rectangle == rectangle) {
        return;
    }
    image = create_image(width, height,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT,
                         true, rectangle);
    texture_initialized_.at(index) = false;
    texture_hashes_.at(index) = 0;
}

void VulkanGlesRenderer::upload(Buffer& buffer, const void* data,
                                std::size_t size) const {
    if (size > buffer.size) {
        throw std::runtime_error{"Vulkan buffer upload exceeds allocation"};
    }
    void* mapped{};
    require_success(vkMapMemory(device_, buffer.memory, 0,
                                static_cast<VkDeviceSize>(size), 0, &mapped),
                    "vkMapMemory(upload)");
    std::memcpy(mapped, data, size);
    if (!buffer.coherent) {
        auto range = make_vulkan_structure<VkMappedMemoryRange>(
            VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        require_success(vkFlushMappedMemoryRanges(device_, 1, &range),
                        "vkFlushMappedMemoryRanges");
    }
    vkUnmapMemory(device_, buffer.memory);
}

void VulkanGlesRenderer::download(Buffer& buffer, void* destination,
                                  std::size_t size) const {
    if (size > buffer.size) {
        throw std::runtime_error{"Vulkan buffer download exceeds allocation"};
    }
    void* mapped{};
    require_success(vkMapMemory(device_, buffer.memory, 0,
                                static_cast<VkDeviceSize>(size), 0, &mapped),
                    "vkMapMemory(download)");
    if (!buffer.coherent) {
        auto range = make_vulkan_structure<VkMappedMemoryRange>(
            VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        require_success(vkInvalidateMappedMemoryRanges(device_, 1, &range),
                        "vkInvalidateMappedMemoryRanges");
    }
    std::memcpy(destination, mapped, size);
    vkUnmapMemory(device_, buffer.memory);
}

VkShaderModule VulkanGlesRenderer::create_shader_module(
    std::span<const std::uint32_t> code) const {
    auto info = make_vulkan_structure<VkShaderModuleCreateInfo>(
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    info.codeSize = code.size_bytes();
    info.pCode = code.data();
    VkShaderModule module{};
    require_success(vkCreateShaderModule(device_, &info, nullptr, &module),
                    "vkCreateShaderModule");
    return module;
}

std::optional<VkBlendFactor>
VulkanGlesRenderer::blend_factor(std::uint32_t factor) const {
    switch (factor) {
    case gles_abi::zero: return VK_BLEND_FACTOR_ZERO;
    case gles_abi::one: return VK_BLEND_FACTOR_ONE;
    case gles_abi::source_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case gles_abi::one_minus_source_alpha:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    default: return std::nullopt;
    }
}

VkPipeline VulkanGlesRenderer::pipeline(const PipelineKey& key) {
    if (const auto found = pipelines_.find(key); found != pipelines_.end()) {
        return found->second;
    }
    const auto source = blend_factor(key.blend_source);
    const auto destination = blend_factor(key.blend_destination);
    if (key.blend_enabled && (!source || !destination)) {
        return VK_NULL_HANDLE;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(GpuVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 4> attributes{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(GpuVertex, position))};
    attributes[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(GpuVertex, color))};
    attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(GpuVertex, texture0))};
    attributes[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(GpuVertex, texture1))};
    auto vertex_input =
        make_vulkan_structure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount =
        static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();
    auto assembly =
        make_vulkan_structure<VkPipelineInputAssemblyStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    auto viewport = make_vulkan_structure<VkPipelineViewportStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    auto rasterization =
        make_vulkan_structure<VkPipelineRasterizationStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;
    auto multisample =
        make_vulkan_structure<VkPipelineMultisampleStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = key.blend_enabled ? VK_TRUE : VK_FALSE;
    blend.srcColorBlendFactor =
        key.blend_enabled ? *source : VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor =
        key.blend_enabled ? *destination : VK_BLEND_FACTOR_ZERO;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    // The reference renderer preserves Porter-Duff alpha for the two
    // LayerKit blend modes instead of multiplying source alpha twice.
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = key.blend_enabled
                                    ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                    : VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = 0;
    if ((key.color_mask & 0x01U) != 0) {
        blend.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
    }
    if ((key.color_mask & 0x02U) != 0) {
        blend.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
    }
    if ((key.color_mask & 0x04U) != 0) {
        blend.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    }
    if ((key.color_mask & 0x08U) != 0) {
        blend.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    }
    auto blend_state =
        make_vulkan_structure<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend;
    constexpr std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR};
    auto dynamic = make_vulkan_structure<VkPipelineDynamicStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    dynamic.dynamicStateCount =
        static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    auto info = make_vulkan_structure<VkGraphicsPipelineCreateInfo>(
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend_state;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    info.renderPass = render_pass_;
    info.subpass = 0;
    VkPipeline created{};
    require_success(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info,
                                              nullptr, &created),
                    "vkCreateGraphicsPipelines");
    pipelines_.emplace(key, created);
    return created;
}

std::vector<GpuVertex>
VulkanGlesRenderer::expand_vertices(std::span<const GlesRasterVertex> vertices,
                                    std::uint32_t mode,
                                    const GlesRasterState& state) const {
    std::vector<GpuVertex> result;
    result.reserve(vertices.size() * 3U);
    const auto emit = [&](std::size_t a, std::size_t b, std::size_t c) {
        const std::array indices{a, b, c};
        std::array<std::array<float, 2>, 3> normalized{};
        for (std::size_t index = 0; index < indices.size(); ++index) {
            const auto& position = vertices[indices[index]].position;
            if (position[3] == 0.0F)
                return;
            normalized[index] = {position[0] / position[3],
                                 position[1] / position[3]};
        }
        const auto area = (normalized[1][0] - normalized[0][0]) *
                              (normalized[2][1] - normalized[0][1]) -
                          (normalized[1][1] - normalized[0][1]) *
                              (normalized[2][0] - normalized[0][0]);
        if (std::abs(area) < 1.0e-6F)
            return;
        const auto front_facing =
            state.front_face == gles_abi::counter_clockwise ? area > 0.0F
                                                            : area < 0.0F;
        if (state.cull_enabled &&
            (state.cull_mode == gles_abi::front_and_back ||
             (state.cull_mode == gles_abi::front && front_facing) ||
             (state.cull_mode == gles_abi::back && !front_facing))) {
            return;
        }
        for (const auto index : indices) {
            const auto& vertex = vertices[index];
            result.push_back(GpuVertex{vertex.position, vertex.color,
                                       vertex.texture[0], vertex.texture[1]});
        }
    };
    if (mode == gles_abi::triangles) {
        for (std::size_t index = 0; index + 2U < vertices.size(); index += 3U) {
            emit(index, index + 1U, index + 2U);
        }
    } else if (mode == gles_abi::triangle_strip) {
        for (std::size_t index = 0; index + 2U < vertices.size(); ++index) {
            if ((index & 1U) == 0) {
                emit(index, index + 1U, index + 2U);
            } else {
                emit(index + 1U, index, index + 2U);
            }
        }
    } else if (mode == gles_abi::triangle_fan) {
        for (std::size_t index = 1; index + 1U < vertices.size(); ++index) {
            emit(0, index, index + 1U);
        }
    }
    return result;
}

GpuFixedFunctionState
VulkanGlesRenderer::fixed_function_state(const GlesRasterState& state) const {
    GpuFixedFunctionState result;
    for (std::size_t index = 0; index < state.texture_units.size(); ++index) {
        const auto& source = state.texture_units[index];
        auto& destination = result.units[index];
        const auto& environment = source.environment;
        destination.mode_combine_enabled = {
            static_cast<std::int32_t>(environment.mode),
            static_cast<std::int32_t>(environment.combine_rgb),
            static_cast<std::int32_t>(environment.combine_alpha),
            source.enabled ? 1 : 0};
        destination.color = environment.color;
        for (std::size_t argument = 0; argument < 3; ++argument) {
            destination.rgb_sources[argument] =
                static_cast<std::int32_t>(environment.rgb_sources[argument]);
            destination.alpha_sources[argument] =
                static_cast<std::int32_t>(environment.alpha_sources[argument]);
            destination.rgb_operands[argument] =
                static_cast<std::int32_t>(environment.rgb_operands[argument]);
            destination.alpha_operands[argument] =
                static_cast<std::int32_t>(environment.alpha_operands[argument]);
        }
        destination.scales_rectangle = {environment.rgb_scale,
                                        environment.alpha_scale,
                                        source.rectangle ? 1.0F : 0.0F, 0.0F};
    }
    return result;
}

void VulkanGlesRenderer::transition_image(
    VkCommandBuffer command, VkImage image, VkImageLayout old_layout,
    VkImageLayout new_layout, VkPipelineStageFlags source_stage,
    VkPipelineStageFlags destination_stage, VkAccessFlags source_access,
    VkAccessFlags destination_access) const {
    auto barrier = make_vulkan_structure<VkImageMemoryBarrier>(
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, source_stage, destination_stage, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
}

bool VulkanGlesRenderer::synchronize(DisplayFrame& frame,
                                     GlesRenderTargetKey target) {
    static_cast<void>(frame);
    static_cast<void>(target);
    return true;
}

void VulkanGlesRenderer::invalidate(GlesRenderTargetKey target) {
    std::lock_guard lock{mutex_};
    if (active_target_ && *active_target_ == target)
        target_valid_ = false;
}

void VulkanGlesRenderer::release(GlesRenderTargetKey target) {
    std::lock_guard lock{mutex_};
    if (active_target_ && *active_target_ == target) {
        active_target_.reset();
        target_valid_ = false;
    }
}

bool VulkanGlesRenderer::draw(DisplayFrame& frame, GlesRenderTargetKey target,
                              std::span<const GlesRasterVertex> vertices,
                              std::uint32_t mode,
                              const GlesRasterState& state) {
    std::lock_guard lock{mutex_};
    if (frame.width == 0 || frame.height == 0 ||
        frame.pixels.size() !=
            static_cast<std::size_t>(frame.width) * frame.height ||
        state.viewport_width == 0 || state.viewport_height == 0 ||
        vertices.size() < 3 ||
        (mode != gles_abi::triangles && mode != gles_abi::triangle_strip &&
         mode != gles_abi::triangle_fan)) {
        return false;
    }
    if (std::any_of(vertices.begin(), vertices.end(),
                    [](const GlesRasterVertex& vertex) {
                        return vertex.position[3] == 0.0F;
                    })) {
        return false;
    }
    if (state.blend_enabled &&
        !((state.blend_source == gles_abi::source_alpha ||
           state.blend_source == gles_abi::one) &&
          state.blend_destination == gles_abi::one_minus_source_alpha)) {
        return false;
    }

    std::int64_t scissor_left = 0;
    std::int64_t scissor_bottom = 0;
    std::int64_t scissor_right = frame.width;
    std::int64_t scissor_top = frame.height;
    if (state.scissor_enabled) {
        const auto requested_left =
            static_cast<std::int64_t>(state.scissor_box[0]);
        const auto requested_bottom =
            static_cast<std::int64_t>(state.scissor_box[1]);
        const auto requested_right =
            requested_left + std::max<std::int64_t>(0, state.scissor_box[2]);
        const auto requested_top =
            requested_bottom + std::max<std::int64_t>(0, state.scissor_box[3]);
        scissor_left = std::clamp<std::int64_t>(requested_left, 0, frame.width);
        scissor_bottom =
            std::clamp<std::int64_t>(requested_bottom, 0, frame.height);
        scissor_right =
            std::clamp<std::int64_t>(requested_right, 0, frame.width);
        scissor_top = std::clamp<std::int64_t>(requested_top, 0, frame.height);
        if (scissor_right <= scissor_left || scissor_top <= scissor_bottom) {
            return true;
        }
    }

    auto expanded = expand_vertices(vertices, mode, state);
    if (expanded.empty())
        return true;
    PipelineKey pipeline_key;
    pipeline_key.blend_enabled = state.blend_enabled;
    pipeline_key.blend_source = state.blend_source;
    pipeline_key.blend_destination = state.blend_destination;
    for (std::size_t component = 0; component < state.color_mask.size();
         ++component) {
        if (state.color_mask[component]) {
            pipeline_key.color_mask |=
                static_cast<std::uint8_t>(1U << component);
        }
    }
    bool submitted = false;
    try {
        const auto selected_pipeline = pipeline(pipeline_key);
        if (selected_pipeline == VK_NULL_HANDLE)
            return false;

        if (!active_target_ || *active_target_ != target) {
            active_target_ = target;
            target_valid_ = false;
        }

        const auto frame_bytes =
            static_cast<VkDeviceSize>(frame.pixels.size()) *
            sizeof(std::uint32_t);
        const auto vertex_bytes =
            static_cast<VkDeviceSize>(expanded.size()) * sizeof(GpuVertex);
        ensure_target(frame.width, frame.height);
        const auto frame_hash =
            pixel_hash(frame.pixels, frame.width, frame.height);
        if (target_valid_ && frame_hash != target_cpu_hash_)
            target_valid_ = false;
        if (!target_valid_) {
            ensure_buffer(target_upload_, frame_bytes,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            upload(target_upload_, frame.pixels.data(),
                   static_cast<std::size_t>(frame_bytes));
        }
        ensure_buffer(target_download_, frame_bytes,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        ensure_buffer(vertex_buffer_, vertex_bytes,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        ensure_buffer(uniform_buffer_, sizeof(GpuFixedFunctionState),
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        upload(vertex_buffer_, expanded.data(),
               static_cast<std::size_t>(vertex_bytes));
        const auto uniforms = fixed_function_state(state);
        upload(uniform_buffer_, &uniforms, sizeof(uniforms));

        constexpr std::array<std::uint32_t, 1> white_pixel{0xffffffffU};
        std::array<bool, gles_abi::texture_unit_count> texture_changed{};
        std::array<std::uint64_t, gles_abi::texture_unit_count>
            texture_hashes{};
        for (std::size_t index = 0; index < texture_images_.size(); ++index) {
            const auto& unit = state.texture_units[index];
            const GlesResourceStore::TextureLevel* level{};
            if (unit.enabled && unit.texture != 0 &&
                state.resources != nullptr) {
                if (const auto* texture =
                        state.resources->texture(unit.texture)) {
                    const auto found = texture->levels.find(0);
                    if (found != texture->levels.end() &&
                        found->second.width != 0 && found->second.height != 0 &&
                        found->second.argb.size() ==
                            static_cast<std::size_t>(found->second.width) *
                                found->second.height) {
                        level = &found->second;
                    }
                }
            }
            const auto width = level ? level->width : 1U;
            const auto height = level ? level->height : 1U;
            const auto* pixels =
                level ? level->argb.data() : white_pixel.data();
            const auto byte_count = static_cast<VkDeviceSize>(width) * height *
                                    sizeof(std::uint32_t);
            ensure_texture(index, width, height, unit.rectangle);
            auto hash = pixel_hash(
                std::span<const std::uint32_t>{
                    pixels, static_cast<std::size_t>(width) * height},
                width, height);
            hash ^= unit.rectangle ? 0x9e3779b97f4a7c15ULL : 0;
            texture_hashes[index] = hash;
            texture_changed[index] =
                !texture_initialized_[index] || texture_hashes_[index] != hash;
            if (texture_changed[index]) {
                ensure_buffer(texture_uploads_[index], byte_count,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
                upload(texture_uploads_[index], pixels,
                       static_cast<std::size_t>(byte_count));
            }
        }

        require_success(vkResetFences(device_, 1, &fence_), "vkResetFences");
        require_success(vkResetCommandBuffer(command_buffer_, 0),
                        "vkResetCommandBuffer");
        VkDescriptorBufferInfo uniform_info{uniform_buffer_.buffer, 0,
                                            sizeof(GpuFixedFunctionState)};
        std::array<VkDescriptorImageInfo, gles_abi::texture_unit_count>
            image_infos{};
        for (std::size_t index = 0; index < image_infos.size(); ++index) {
            image_infos[index] = {texture_images_[index].sampler,
                                  texture_images_[index].view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptor_;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &uniform_info;
        for (std::uint32_t index = 0; index < image_infos.size(); ++index) {
            auto& write = writes[index + 1U];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptor_;
            write.dstBinding = index + 1U;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image_infos[index];
        }
        vkUpdateDescriptorSets(device_,
                               static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);

        auto begin = make_vulkan_structure<VkCommandBufferBeginInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require_success(vkBeginCommandBuffer(command_buffer_, &begin),
                        "vkBeginCommandBuffer");
        if (!target_valid_) {
            auto source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            VkAccessFlags source_access{};
            if (target_layout_ == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
                source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                source_access = VK_ACCESS_TRANSFER_READ_BIT;
            } else if (target_layout_ ==
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                source_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                source_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            }
            transition_image(command_buffer_, target_image_.image,
                             target_layout_,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, source_stage,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, source_access,
                             VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy target_copy{};
            target_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            target_copy.imageSubresource.layerCount = 1;
            target_copy.imageExtent = {frame.width, frame.height, 1};
            vkCmdCopyBufferToImage(
                command_buffer_, target_upload_.buffer, target_image_.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &target_copy);
            transition_image(command_buffer_, target_image_.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        } else if (target_layout_ != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            transition_image(command_buffer_, target_image_.image,
                             target_layout_,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        }
        for (std::size_t index = 0; index < texture_images_.size(); ++index) {
            if (!texture_changed[index])
                continue;
            auto& image = texture_images_[index];
            transition_image(
                command_buffer_, image.image,
                texture_initialized_[index]
                    ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                texture_initialized_[index]
                    ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                    : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                texture_initialized_[index] ? VK_ACCESS_SHADER_READ_BIT : 0,
                VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {image.width, image.height, 1};
            vkCmdCopyBufferToImage(
                command_buffer_, texture_uploads_[index].buffer, image.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            transition_image(command_buffer_, image.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_SHADER_READ_BIT);
        }

        auto render_begin = make_vulkan_structure<VkRenderPassBeginInfo>(
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        render_begin.renderPass = render_pass_;
        render_begin.framebuffer = framebuffer_;
        render_begin.renderArea.extent = {frame.width, frame.height};
        vkCmdBeginRenderPass(command_buffer_, &render_begin,
                             VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          selected_pipeline);
        constexpr VkDeviceSize vertex_offset = 0;
        vkCmdBindVertexBuffers(command_buffer_, 0, 1, &vertex_buffer_.buffer,
                               &vertex_offset);
        vkCmdBindDescriptorSets(
            command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
            0, 1, &descriptor_, 0, nullptr);
        VkViewport viewport{};
        viewport.x = static_cast<float>(state.viewport_x);
        viewport.y = static_cast<float>(frame.height) -
                     static_cast<float>(state.viewport_y) -
                     static_cast<float>(state.viewport_height);
        viewport.width = static_cast<float>(state.viewport_width);
        viewport.height = static_cast<float>(state.viewport_height);
        viewport.minDepth = 0.0F;
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(command_buffer_, 0, 1, &viewport);
        VkRect2D scissor{};
        scissor.offset.x = static_cast<std::int32_t>(scissor_left);
        scissor.offset.y = static_cast<std::int32_t>(
            static_cast<std::int64_t>(frame.height) - scissor_top);
        scissor.extent.width =
            static_cast<std::uint32_t>(scissor_right - scissor_left);
        scissor.extent.height =
            static_cast<std::uint32_t>(scissor_top - scissor_bottom);
        vkCmdSetScissor(command_buffer_, 0, 1, &scissor);
        vkCmdDraw(command_buffer_, static_cast<std::uint32_t>(expanded.size()),
                  1, 0, 0);
        vkCmdEndRenderPass(command_buffer_);

        transition_image(command_buffer_, target_image_.image,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy readback_copy{};
        readback_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        readback_copy.imageSubresource.layerCount = 1;
        readback_copy.imageExtent = {frame.width, frame.height, 1};
        vkCmdCopyImageToBuffer(command_buffer_, target_image_.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               target_download_.buffer, 1, &readback_copy);
        auto host_barrier = make_vulkan_structure<VkBufferMemoryBarrier>(
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
        host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.buffer = target_download_.buffer;
        host_barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &host_barrier, 0, nullptr);
        require_success(vkEndCommandBuffer(command_buffer_),
                        "vkEndCommandBuffer");
        auto submit =
            make_vulkan_structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer_;
        require_success(vkQueueSubmit(queue_, 1, &submit, fence_),
                        "vkQueueSubmit");
        submitted = true;
        require_success(vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                                        fence_timeout_nanoseconds),
                        "vkWaitForFences");
        submitted = false;
        download(target_download_, frame.pixels.data(),
                 static_cast<std::size_t>(frame_bytes));
        target_valid_ = true;
        target_cpu_hash_ = pixel_hash(frame.pixels, frame.width, frame.height);
        target_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        for (std::size_t index = 0; index < texture_images_.size(); ++index) {
            if (texture_changed[index]) {
                texture_initialized_[index] = true;
                texture_hashes_[index] = texture_hashes[index];
            }
        }
        return true;
    } catch (...) {
        if (submitted) {
            vkDeviceWaitIdle(device_);
        }
        texture_initialized_.fill(false);
        return false;
    }
}

} // namespace

std::unique_ptr<GlesRenderer> create_vulkan_gles_renderer() noexcept {
    try {
        auto renderer = std::make_unique<VulkanGlesRenderer>();
        std::clog << "[gles-renderer] selected " << renderer->name() << "\n";
        return renderer;
    } catch (const std::exception& error) {
        std::clog << "[gles-renderer] Vulkan unavailable: " << error.what()
                  << "; using software\n";
        return {};
    } catch (...) {
        std::clog << "[gles-renderer] Vulkan unavailable: unknown error; "
                     "using software\n";
        return {};
    }
}

} // namespace ilegacysim
