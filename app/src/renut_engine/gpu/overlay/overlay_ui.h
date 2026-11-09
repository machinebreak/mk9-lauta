/**
 * @file    gpu/overlay/overlay_ui.h
 * @brief   reNut's own overlay: an ImGui context of its own, fed from the game
 *          window's messages, built on the window's thread and drawn by the
 *          renderer into each presented frame.
 *
 * The overlay has a Dear ImGui of its own, compiled into the executable
 * (gpu.cmake): the SDK's copy in rexruntime has one current context, which
 * the SDK switches to its own from any thread. The frame is built on the
 * SDK's UI thread (a call the overlay's pump queues there 60 times a second);
 * the window's input is queued as it arrives and applied by the next build.
 * What a build produces is copied out, and Present draws the latest copy, so
 * neither thread waits for the other.
 */
#pragma once

#include <cstdint>
#include <functional>

#include <plume_render_interface.h>

namespace renut::gpu::overlay {

// An overlay window. Draw builds its widgets, on the UI thread with the
// overlay's context current, once per overlay frame.
class Dialog {
public:
  virtual ~Dialog() = default;
  virtual void Draw() = 0;
};

// Any thread.
void AddDialog(Dialog *dialog);
void RemoveDialog(Dialog *dialog);

// Runs a function on the SDK's UI thread, later (WindowedAppContext::
// CallInUIThreadDeferred).
using UIThreadRunner = std::function<void(std::function<void()>)>;

// Once the window exists: takes the window's input and starts building frames
// through 'run_on_ui_thread'. Without a window it does nothing and returns false.
bool Start(UIThreadRunner run_on_ui_thread);
void Stop();

// Present, State().mutex held, the back buffer's framebuffer set: draws the
// latest overlay frame over it.
void DrawLocked(plume::RenderCommandList *cmd, uint32_t width, uint32_t height);

// mk9-lauta: a controller for the overlay's ImGui (gamepad navigation). The
// reader runs on the UI thread at each build; false: no controller.
struct Pad {
  uint16_t buttons = 0;  // XINPUT_GAMEPAD's
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0;
};
using PadReader = bool (*)(Pad *out);
void SetPadReader(PadReader reader);

// mk9-lauta: the window's close (its X, Alt+F4) goes through 'filter' (UI
// thread): true lets it close; false keeps the window (the caller asks the
// player first, and CloseNow closes it past the filter).
using CloseFilter = bool (*)();
void SetCloseFilter(CloseFilter filter);
void CloseNow();

} // namespace renut::gpu::overlay
