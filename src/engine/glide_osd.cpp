#include "repiu/engine/glide_osd.h"

#include "repiu/engine/glide_post_process.h"
#include "repiu/engine/imgui_ui_scale.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>

#include <cfloat>
#include <string>

namespace repiu::engine
{
GlideOsd::GlideOsd() = default;

GlideOsd::~GlideOsd()
{
    Shutdown();
}

bool GlideOsd::Initialize(void* sdl_window, void* gl_context,
                          const char* glsl_version, std::string* message)
{
    if (initialized_)
    {
        return true;
    }
    if (sdl_window == nullptr || gl_context == nullptr)
    {
        if (message != nullptr)
        {
            *message = "OSD needs a window and a current GL context";
        }
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    // Issue #55: a pad drives the overlay while it is open (ImGui's SDL3
    // backend reads the gamepad itself each frame). Keyboard navigation stays
    // off: keys keep reaching the game while the overlay is open.
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    AddImGuiUiFont();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL3_InitForOpenGL(
            static_cast<SDL_Window*>(sdl_window),
            static_cast<SDL_GLContext>(gl_context)))
    {
        ImGui::DestroyContext();
        if (message != nullptr)
        {
            *message = "ImGui SDL3 backend initialization failed";
        }
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(glsl_version))
    {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        if (message != nullptr)
        {
            *message = "ImGui OpenGL3 backend initialization failed";
        }
        return false;
    }
    base_style_ = std::make_unique<ImGuiStyle>(ImGui::GetStyle());
    applied_scale_ = 0.0F;
    initialized_ = true;
    return true;
}

void GlideOsd::Shutdown()
{
    if (!initialized_)
    {
        return;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
    visible_ = false;
}

bool GlideOsd::WantsMouse() const
{
    return initialized_ && visible_ && ImGui::GetIO().WantCaptureMouse;
}

void GlideOsd::ProcessEvent(const void* sdl_event)
{
    if (!initialized_ || sdl_event == nullptr)
    {
        return;
    }
    ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(sdl_event));
}

void GlideOsd::SetRendererIdentity(const GlRendererIdentity& identity)
{
    renderer_identity_ = identity;
    has_renderer_identity_ = true;
}

void GlideOsd::SetInfoLines(const std::vector<std::string>& lines)
{
    info_lines_ = lines;
}

namespace
{

// #5: what draws the picture. A software rasterizer is the one case worth
// shouting about: the game runs, only slowly, and nothing else says why.
void DrawRendererSection(const GlRendererIdentity& identity)
{
    ImGui::SeparatorText("Renderer");
    if (identity.software)
    {
        const ImVec4 warning(1.0F, 0.45F, 0.35F, 1.0F);
        ImGui::TextColored(warning, "%s", identity.renderer.c_str());
        ImGui::TextColored(warning, "Software rendering: no 3D acceleration");
    }
    else
    {
        ImGui::TextUnformatted(identity.renderer.c_str());
    }
    ImGui::TextDisabled("Vendor: %s", identity.vendor.c_str());
    ImGui::TextDisabled("OpenGL: %s", identity.version.c_str());
    ImGui::TextDisabled("Video driver: %s%s", identity.video_driver.c_str(),
                        identity.wsl_d3d12 ? " (Mesa D3D12 for WSL)" : "");
}

// Task 768: the shader list, Reload, and the active shader's parameters.
void DrawPostProcessMenu(GlidePostProcess* post_process)
{
    ImGui::SeparatorText("Screen shader");
    const std::string current = post_process->active_id();
    std::string chosen;
    if (ImGui::BeginCombo("Shader", current.c_str()))
    {
        if (ImGui::Selectable(kPostShaderNoneId, current == kPostShaderNoneId))
        {
            chosen = kPostShaderNoneId;
        }
        for (const PostShaderEntry& entry : post_process->catalog())
        {
            const std::string label =
                entry.builtin ? entry.id + "  (built-in)" : entry.id;
            if (ImGui::Selectable(label.c_str(), current == entry.id))
            {
                chosen = entry.id;
            }
        }
        ImGui::EndCombo();
    }
    // Compiled after the combo closes so the menu never draws half a switch.
    if (!chosen.empty() && chosen != current)
    {
        post_process->Select(chosen);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
    {
        post_process->Reload();
    }
    if (!post_process->last_error().empty())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0F);
        ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s",
                           post_process->last_error().c_str());
        ImGui::PopTextWrapPos();
    }
    for (PostShaderParameter& parameter : post_process->parameters())
    {
        const std::string& label = parameter.description.empty()
            ? parameter.name
            : parameter.description;
        ImGui::PushID(parameter.name.c_str());
        ImGui::SliderFloat(label.c_str(), &parameter.value, parameter.minimum,
                           parameter.maximum, "%.2f");
        ImGui::PopID();
    }
    ImGui::TextDisabled("Files: %s/*.glsl. Changes last for this run;",
                        post_process->shader_directory().c_str());
    ImGui::TextDisabled("the launcher stores the default.");
}

}  // namespace

void GlideOsd::Render(std::atomic<bool>* lfb_high_precision,
                      std::atomic<bool>* texture_full_precision,
                      GlidePostProcess* post_process,
                      GlideOsdDisplayOptions* display)
{
    if (!initialized_ || !visible_)
    {
        return;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    // #15: the SDL backend has just set the display size, so the scale for
    // this frame is known before ImGui lays anything out.
    ImGuiIO& io = ImGui::GetIO();
    const float scale = UiScaleForHeight(io.DisplaySize.y, kOsdUiScale);
    if (scale != applied_scale_ && base_style_ != nullptr)
    {
        ApplyImGuiUiScale(*base_style_, scale);
        applied_scale_ = scale;
    }
    ImGui::NewFrame();
    // Issue #55: B closes the overlay, unless a menu was open, which B closes
    // instead (ImGui's own cancel).
    const bool close_requested = !popup_open_last_frame_ &&
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    if (focus_pending_)
    {
        ImGui::SetNextWindowFocus();
        focus_pending_ = false;
    }
    // re2DJ's layout: pinned across the full width at the top, the height
    // following the content. The width is held by a constraint because
    // auto-resize would otherwise shrink it to the content as well.
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(io.DisplaySize.x, 0.0F),
                                        ImVec2(io.DisplaySize.x, FLT_MAX));
    if (ImGui::Begin("rePIU OSD", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings))
    {
        for (const std::string& line : info_lines_)
        {
            ImGui::TextUnformatted(line.c_str());
        }
        if (has_renderer_identity_)
        {
            DrawRendererSection(renderer_identity_);
            ImGui::Separator();
        }
        // Issue #45: unlike the items below, these two are stored in
        // cfg/repiu.ini, so the next run starts the same way.
        if (display != nullptr)
        {
            if (ImGui::Checkbox("Fullscreen", &display->fullscreen))
            {
                display->fullscreen_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::BeginItemTooltip())
            {
                ImGui::TextUnformatted(
                    "Borderless at the desktop resolution. Alt+Enter or a "
                    "double click toggles it too. Saved for the next run.");
                ImGui::EndTooltip();
            }
            if (ImGui::Checkbox("Keep aspect ratio", &display->keep_aspect))
            {
                display->keep_aspect_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::BeginItemTooltip())
            {
                ImGui::TextUnformatted(
                    "On keeps the game's 4:3 picture with black bars. Off "
                    "stretches it to fill the window or screen. Saved for "
                    "the next run.");
                ImGui::EndTooltip();
            }
            ImGui::Separator();
        }
        if (lfb_high_precision != nullptr)
        {
            bool enabled =
                lfb_high_precision->load(std::memory_order_relaxed);
            if (ImGui::Checkbox("LFB high precision (32-bit)", &enabled))
            {
                lfb_high_precision->store(enabled,
                                          std::memory_order_relaxed);
            }
        }
        // Issue #37: the game's own driver cuts textures to 4444/565; this
        // swaps in the 8-bit originals it still holds.
        if (texture_full_precision != nullptr)
        {
            bool enabled =
                texture_full_precision->load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Full-precision textures (8-bit)", &enabled))
            {
                texture_full_precision->store(enabled,
                                              std::memory_order_relaxed);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::BeginItemTooltip())
            {
                ImGui::TextUnformatted(
                    "Uses the game's original 8-bit textures instead of the "
                    "4444/565 copies its graphics driver makes. Off matches "
                    "the arcade hardware.");
                ImGui::EndTooltip();
            }
        }
        if (post_process != nullptr)
        {
            DrawPostProcessMenu(post_process);
        }
        ImGui::Separator();
        ImGui::TextDisabled(
            "Tab or LT+RT+Y closes this overlay. Pad: D-pad moves, A selects, "
            "B closes; the game gets no pad input while it is open.");
    }
    ImGui::End();
    popup_open_last_frame_ =
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (close_requested)
    {
        visible_ = false;
        popup_open_last_frame_ = false;
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace repiu::engine
