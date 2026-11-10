#ifndef __ANDROID__

#include "scene_panel.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace vkr {
namespace {

void post(std::vector<ControlCommand>& out, ControlCommand::Type type, int slot = -1, int i = 0,
          double d = 0.0, const char* str = nullptr) {
    ControlCommand c{type};
    c.slot = slot;
    c.i = i;
    c.d = d;
    if (str) c.str = str;
    out.push_back(std::move(c));
}

} // namespace

bool ScenePanel::init(VkContext& vk) {
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 8;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &size;
    if (vkCreateDescriptorPool(vk.device, &dpci, nullptr, &pool_) != VK_SUCCESS) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // an appliance should not litter its working directory
    ImGui::StyleColorsDark();
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(vk.window, &xscale, &yscale);
    const float scale = std::max(1.0f, xscale);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().WindowRounding = 6.0f * scale;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 0.85f;
    io.FontGlobalScale = scale;

    // true: chains the key callback SceneHost installed, so scene hotkeys keep
    // working on this window. Requires that callback to be set first.
    if (!ImGui_ImplGlfw_InitForVulkan(vk.window, true)) return false;

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = vk.instance;
    info.PhysicalDevice = vk.phys;
    info.Device = vk.device;
    info.QueueFamily = vk.queueFamily;
    info.Queue = vk.queue;
    info.DescriptorPool = pool_;
    info.MinImageCount = 2;
    info.ImageCount = std::max<uint32_t>(2, (uint32_t)vk.swapImages.size());
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &vk.swapFormat;
    if (!ImGui_ImplVulkan_Init(&info)) {
        ImGui_ImplGlfw_Shutdown();
        return false;
    }
    ready_ = true;
    return true;
}

void ScenePanel::destroy(VkContext& vk) {
    if (ready_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        ready_ = false;
    }
    if (pool_) vkDestroyDescriptorPool(vk.device, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
}

bool ScenePanel::drawable() const {
    if (!ready_ || !visible_) return false;
    const ImDrawData* d = ImGui::GetDrawData();
    return d && d->CmdListsCount > 0;
}

bool ScenePanel::wantsKeyboard() const { return ready_ && ImGui::GetIO().WantCaptureKeyboard; }

void ScenePanel::build(const Bindings& b) {
    if (!ready_) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    if (visible_) drawWindow(b);
    ImGui::Render();
}

void ScenePanel::drawWindow(const Bindings& b) {
    using T = ControlCommand::Type;
    const ControlSnapshot& s = *b.snapshot;
    std::vector<ControlCommand>& out = *b.commands;

    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (!ImGui::Begin("vkrender controls", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f fps   canvas %.0fx%.0f   %zu window(s)", ImGui::GetIO().Framerate,
                b.canvasWidth, b.canvasHeight, b.windows);

    ImGui::SeparatorText("Scene");
    static const char* const kLayouts[] = {"presentation", "two-cubes", "single-cube", "wall"};
    int current = -1;
    for (int i = 0; i < IM_ARRAYSIZE(kLayouts); ++i)
        if (s.layout == kLayouts[i]) current = i;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::Combo("Layout", &current, kLayouts, IM_ARRAYSIZE(kLayouts)))
        post(out, T::Layout, -1, 0, 0.0, kLayouts[current]);
    bool paused = s.paused;
    if (ImGui::Checkbox("Paused", &paused)) post(out, T::Pause, -1, paused ? 1 : 0);
    ImGui::SameLine();
    ImGui::Checkbox("Backdrop", b.backdrop);
    if (s.cameraAvailable) {
        bool camera = s.cameraOn;
        ImGui::SameLine();
        if (ImGui::Checkbox("Camera feed", &camera)) post(out, T::Camera, -1, camera ? 1 : 0);
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::SliderFloat("Speed", b.speed, 0.1f, 8.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);

    ImGui::SeparatorText("Streams");
    int maxVisible = (int)s.maxVisible;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::SliderInt("Max visible", &maxVisible, 1, std::max<int>(1, (int)s.maxSlots)))
        post(out, T::Config, -1, maxVisible, -1.0);
    float rotate = (float)s.rotateSeconds;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::SliderFloat("Rotate (s)", &rotate, 1.0f, 60.0f, "%.0f"))
        post(out, T::Config, -1, -1, (double)rotate);
    if (s.streams.empty()) ImGui::TextDisabled("No streams connected");
    for (const ControlStreamInfo& st : s.streams) {
        const int slot = (int)st.slot;
        ImGui::PushID(slot);
        ImGui::Text("%2d  %ux%u  %.0f fps%s", slot, st.width, st.height, st.fps,
                    st.connected ? (st.visible ? "" : "  (off scene)") : "  (gone)");
        ImGui::SameLine();
        if (ImGui::SmallButton(st.hidden ? "Show" : "Hide"))
            post(out, st.hidden ? T::Show : T::Hide, slot);
        ImGui::SameLine();
        if (ImGui::SmallButton(st.pinned ? "Unpin" : "Pin"))
            post(out, st.pinned ? T::Unpin : T::Pin, slot);
        ImGui::SameLine();
        if (ImGui::SmallButton(st.spotlit ? "Unsolo" : "Solo"))
            post(out, st.spotlit ? T::Unspotlight : T::Spotlight, slot);
        ImGui::SameLine();
        if (ImGui::SmallButton("Kick")) post(out, T::Kick, slot);
        ImGui::PopID();
    }

    ImGui::SeparatorText("Displays");
    for (const ControlDisplayInfo& d : s.displays) {
        ImGui::PushID(d.id);
        bool active = d.active;
        const std::string label = d.name + (d.primary ? " (primary)" : "");
        ImGui::BeginDisabled(!s.displaysAvailable || d.primary);
        if (ImGui::Checkbox(label.c_str(), &active)) post(out, T::Display, d.id, active ? 1 : 0);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%dx%d at %d,%d", d.width, d.height, d.x, d.y);
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::Button("Stop renderer")) *b.quit = true;
    ImGui::SameLine();
    ImGui::TextDisabled("ESC stops   TAB hides this panel");
    ImGui::End();
}

void ScenePanel::record(VkCommandBuffer cmd, VkImage image, VkImageView view, VkExtent2D extent) {
    // The scene's rendering pass has ended; loading its pixels is a read.
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask =
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

} // namespace vkr

#endif // !__ANDROID__
