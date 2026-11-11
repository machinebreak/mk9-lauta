// mk9-lauta (2026-11-11, PLAN-0.4 E1/E2): the renderer's start, watched. A graphics API (Direct3D 12 or Vulkan) that
// twice in a row did not get an image on screen (a driver that crashes or hangs making the device, its first
// pipelines, its swap chain) or lost its device is swapped for the other one at the next start, saved in mk9.toml, and
// the player is told so on screen; a lost device says so in a message instead of a frozen image. The outcome of each
// start is in logs/renderer_start.txt ("<api> <failed starts in a row> <pending|ok|lost>").

#pragma once

namespace mk9::renderer_start {

// Before the render device is made (video.cpp CreateHostDevice): the last start's outcome read; the second failure
// in a row with this API: the other one from now on (renut_gpu_backend).
void BeforeDevice();
// The device was made, with this API ("d3d12" or "vulkan").
void DeviceReady(const char* api);
// The GPU finished 120 frames, shown or not (the render thread; a game started minimized presents nothing): this
// start is good.
void FramesDone();
// The device was lost (backend.cpp ReportDeviceLost, the first time).
void DeviceLost();
// Once the overlay runs (the pad read's hook): the notice of an API swapped at this start, for a few seconds.
void Install();

}  // namespace mk9::renderer_start
