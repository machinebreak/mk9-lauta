// mk9-lauta (2026-11-13): Linux: the small windows before the game's (mk9_small_window.h).

#include "mk9_small_window.h"

#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/logging.h>

#include "imgui.h"
#include "../../sdk/thirdparty/imgui/backends/imgui_impl_sdl3.h"
#include "../../sdk/thirdparty/imgui/backends/imgui_impl_sdlrenderer3.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

namespace mk9::small_window {
namespace {

SDL_Window* g_current = nullptr;

// The online screens' look (renut_engine/gpu/overlay/overlay_ui.cpp): near-black, a dark red for what can be pressed,
// gold for what is chosen and the bars.
void Style(float scale) {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.FrameRounding = 5.0f;
  style.WindowPadding = ImVec2(24.0f, 18.0f);
  style.FramePadding = ImVec2(8.0f, 5.0f);
  style.ItemSpacing = ImVec2(8.0f, 7.0f);
  style.WindowBorderSize = 0.0f;
  ImVec4* c = style.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.05f, 0.05f, 1.0f);
  c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.11f, 0.10f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.55f, 0.08f, 0.07f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.72f, 0.12f, 0.10f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.85f, 0.18f, 0.12f, 1.0f);
  c[ImGuiCol_PlotHistogram] = ImVec4(0.95f, 0.72f, 0.25f, 1.0f);
  c[ImGuiCol_NavCursor] = ImVec4(0.95f, 0.72f, 0.25f, 1.0f);
  style.ScaleAllSizes(scale);
}

}  // namespace

SDL_Window* Current() { return g_current; }

bool Run(const char* title, int width, int height, const std::function<bool(const Look&)>& frame,
         const std::function<bool()>& close, const std::function<void(const std::filesystem::path&)>& dropped) {
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    REXLOG_ERROR("[window] \"{}\" cannot open (SDL: {})", title, SDL_GetError());
    return false;
  }
  // its close must not be the program's: SDL sends a quit when the last window closes (the SDK's loop would take it)
  const char* quit_hint = SDL_GetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE);
  const std::string previous_quit_hint = quit_hint ? quit_hint : "";
  SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
  float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (!(scale >= 1.0f)) scale = 1.0f;
  SDL_Window* window = SDL_CreateWindow(title, int(float(width) * scale), int(float(height) * scale), 0);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  auto restore_hint = [&] {
    if (previous_quit_hint.empty()) {
      SDL_ResetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE);
    } else {
      SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, previous_quit_hint.c_str());
    }
  };
  if (!window || !renderer) {
    REXLOG_ERROR("[window] \"{}\" did not open: {}", title, SDL_GetError());
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    restore_hint();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return false;
  }
  g_current = window;
  const bool vsync = SDL_SetRenderVSync(renderer, 1);
  REXLOG_INFO("[window] \"{}\" open ({}, scale {:.2f})", title, SDL_GetRendererName(renderer), scale);

  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGuiContext* context = ImGui::CreateContext();
  ImGui::SetCurrentContext(context);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  Style(scale);
  Look look;
  look.scale = scale;
  ImFontConfig config;
  config.FontDataOwnedByAtlas = false;  // static: the atlas must not free it
  size_t size = 0;
  if (const unsigned char* data = renut::gpu::overlay::EmbeddedFont(0, &size)) {
    io.FontDefault =
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), int(size), 18.0f * scale, &config);
  }
  if (const unsigned char* data = renut::gpu::overlay::EmbeddedFont(2, &size)) {
    look.title = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), int(size), 34.0f * scale, &config);
  }
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);
  SDL_RaiseWindow(window);

  const SDL_WindowID id = SDL_GetWindowID(window);
  std::vector<SDL_Event> others;  // the SDK's own events (its wakeups): put back for its loop afterwards
  bool open = true;
  while (open) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type >= SDL_EVENT_USER && event.type <= SDL_EVENT_LAST) {
        others.push_back(event);
        continue;
      }
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT ||
          (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == id)) {
        if (!close || close()) open = false;
      } else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data && dropped) {
        dropped(std::filesystem::path(event.drop.data));
      }
    }
    if (!open) break;
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##small_window", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    open = frame(look);
    ImGui::End();
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 15, 13, 13, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
    if (!vsync || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) SDL_Delay(16);
  }

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext(context);
  ImGui::SetCurrentContext(previous);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  g_current = nullptr;
  restore_hint();
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  for (SDL_Event& event : others) SDL_PushEvent(&event);
  return true;
}

}  // namespace mk9::small_window
