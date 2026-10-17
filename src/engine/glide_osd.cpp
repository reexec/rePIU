#include "repiu/engine/glide_osd.h"

#include "repiu/engine/glide_post_process.h"
#include "repiu/engine/imgui_ui_scale.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
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
    // Issue #76 (design decision 6): in a browser page's Worker there is no
    // SDL window, only the page's canvas. ImGui's SDL3 backend then runs with
    // none: SDL answers failure for a missing window, the backend's window id
    // is 0 like the events the browser input bridge pushes, and the display
    // size comes from SetDisplaySize. The backend reads the name of SDL's
    // video driver, so SDL's (dummy) video must be up.
    windowless_ = sdl_window == nullptr;
    if (windowless_ && SDL_GetCurrentVideoDriver() == nullptr &&
        !SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        if (message != nullptr)
        {
            *message = std::string("OSD without a window needs SDL video: ") +
                SDL_GetError();
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

// Issue #88: the frame rate section is right-aligned in a column of fixed
// width -- the widest its summary line can be -- so the column does not jump
// as the numbers change.
constexpr const char* kFrameRateWidestSummary =
    "FPS, last 60s: low 000.0  mean 000.0  high 000.0";

float FrameRateGraphWidth()
{
    const float font = ImGui::GetFontSize();
    return ImGui::CalcTextSize("000").x + font * 0.3F + font * 14.0F;
}

float FrameRateSectionWidth()
{
    return std::max(FrameRateGraphWidth(),
                    ImGui::CalcTextSize(kFrameRateWidestSummary).x);
}

// Moves the cursor so an item `width` wide ends at the column's right edge.
void AlignRight(float width)
{
    const float spare = ImGui::GetContentRegionAvail().x - width;
    if (spare > 0.0F)
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + spare);
    }
}

void TextRight(const char* text, bool disabled)
{
    AlignRight(ImGui::CalcTextSize(text).x);
    if (disabled)
    {
        ImGui::TextDisabled("%s", text);
    }
    else
    {
        ImGui::TextUnformatted(text);
    }
}

// Issue #88: the current rate and the last 60 seconds as a graph, with the
// low, mean and high under it. The graph is drawn by hand rather than with
// PlotLines so it carries a 60 fps reference line, grid lines, and their
// values outside it: the rate on the left, the seconds underneath.
void DrawFrameRateSection(const FrameRateHistory& history)
{
    float values[FrameRateHistory::kSlots] = {};
    const std::size_t count = history.CopyOldestFirst(values);
    if (count == 0U)
    {
        TextRight("FPS --", false);
        TextRight("measuring", true);
        return;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "FPS %.1f", history.latest());
    TextRight(line, false);

    constexpr float kReference = 60.0F;
    // The grid step grows with the scale so the values stay legible.
    const float top_value = std::max(75.0F, history.maximum() * 1.1F);
    const float step = top_value > 200.0F ? 60.0F : 30.0F;
    const float font = ImGui::GetFontSize();
    const ImVec2 size(font * 14.0F, font * 4.0F);
    const float label_width = ImGui::CalcTextSize("000").x + font * 0.3F;

    ImDrawList* const draw = ImGui::GetWindowDrawList();
    AlignRight(FrameRateGraphWidth());
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 origin(cursor.x + label_width, cursor.y + font * 0.5F);
    const ImVec2 corner(origin.x + size.x, origin.y + size.y);
    const auto y_of = [&](float value) {
        const float clamped = std::clamp(value, 0.0F, top_value);
        return corner.y - clamped / top_value * size.y;
    };
    const auto x_of = [&](std::size_t slot) {
        // Slot 0 is 60 seconds ago, the last slot now.
        return origin.x + size.x * static_cast<float>(slot) /
            static_cast<float>(FrameRateHistory::kSlots - 1U);
    };
    const ImU32 frame_colour = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 grid_colour = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.35F);
    const ImU32 label_colour = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 reference_colour = IM_COL32(90, 220, 120, 230);
    const ImU32 line_colour = ImGui::GetColorU32(ImGuiCol_PlotLines);

    draw->AddRectFilled(origin, corner, frame_colour);
    // Grid lines and the rate scale on the left.
    for (float value = 0.0F; value <= top_value; value += step)
    {
        const float y = y_of(value);
        if (value != kReference)
        {
            draw->AddLine(ImVec2(origin.x, y), ImVec2(corner.x, y), grid_colour);
        }
        char text[8];
        std::snprintf(text, sizeof(text), "%.0f", value);
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(origin.x - text_size.x - font * 0.3F, y - text_size.y * 0.5F),
                      value == kReference ? reference_colour : label_colour, text);
    }
    // Ten-second ticks along the bottom, and their labels under it.
    for (int seconds = 60; seconds >= 0; seconds -= 10)
    {
        const std::size_t slot = FrameRateHistory::kSlots - 1U -
            std::min<std::size_t>(static_cast<std::size_t>(seconds),
                                  FrameRateHistory::kSlots - 1U);
        const float x = seconds == 60 ? origin.x : x_of(slot);
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, corner.y), grid_colour);
        if (seconds % 20 == 0)
        {
            char text[8];
            if (seconds == 0)
            {
                std::snprintf(text, sizeof(text), "now");
            }
            else
            {
                std::snprintf(text, sizeof(text), "-%ds", seconds);
            }
            const ImVec2 text_size = ImGui::CalcTextSize(text);
            // Kept inside the graph's width so the first clears the rate
            // scale's 0.
            const float left = std::clamp(x - text_size.x * 0.5F, origin.x,
                                          corner.x - text_size.x);
            draw->AddText(ImVec2(left, corner.y + font * 0.25F), label_colour, text);
        }
    }
    // The 60 fps reference line, over the grid.
    const float reference_y = y_of(kReference);
    draw->AddLine(ImVec2(origin.x, reference_y), ImVec2(corner.x, reference_y),
                  reference_colour, 1.5F);
    // The rate, newest at the right edge.
    const std::size_t first_slot = FrameRateHistory::kSlots - count;
    if (count == 1U)
    {
        draw->AddCircleFilled(ImVec2(x_of(first_slot), y_of(values[0])), 2.0F, line_colour);
    }
    for (std::size_t i = 1U; i < count; ++i)
    {
        draw->AddLine(ImVec2(x_of(first_slot + i - 1U), y_of(values[i - 1U])),
                      ImVec2(x_of(first_slot + i), y_of(values[i])), line_colour, 1.5F);
    }
    draw->AddRect(origin, corner, grid_colour);
    // Room for the graph, its scale and the seconds under it.
    ImGui::Dummy(ImVec2(label_width + size.x, size.y + font * 1.8F));
    std::snprintf(line, sizeof(line), "FPS, last %us: low %.1f  mean %.1f  high %.1f",
                  static_cast<unsigned>(count), history.minimum(),
                  history.mean(), history.maximum());
    TextRight(line, true);
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
    if (windowless_)
    {
        // No window to read it from: the canvas's drawable size, unscaled.
        ImGui::GetIO().DisplaySize = ImVec2(display_width_, display_height_);
        ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.0F, 1.0F);
    }
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
        // Issue #88: the frame rate takes the right of the opening rows,
        // the name and the renderer the rest.
        const bool split = ImGui::BeginTable(
            "##top", 2, ImGuiTableFlags_SizingFixedFit);
        if (split)
        {
            ImGui::TableSetupColumn("##info", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##fps", ImGuiTableColumnFlags_WidthFixed,
                                    FrameRateSectionWidth());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
        }
        for (const std::string& line : info_lines_)
        {
            ImGui::TextUnformatted(line.c_str());
        }
        if (has_renderer_identity_)
        {
            DrawRendererSection(renderer_identity_);
        }
        if (split)
        {
            ImGui::TableSetColumnIndex(1);
            DrawFrameRateSection(frame_rates_);
            ImGui::EndTable();
        }
        if (has_renderer_identity_)
        {
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
