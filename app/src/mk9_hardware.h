// mk9-lauta (2026-11-09): what this PC is, for its defaults. On the first start (mk9.toml without mk9_preset_detected)
// the quality preset is chosen by the graphics (and the memory) and written to mk9.toml, as the options window (F2)
// does; and a PC with 6 GB of RAM or less saves memory (the renderer's memory policy: idle textures released, a smaller
// stream cache). The player's own choices in mk9.toml are never overwritten.

#pragma once

#include <cstdint>
#include <string>

namespace mk9::hardware {

struct Info {
  std::string gpu;          // the adapter Direct3D 12 takes (the first hardware one)
  uint32_t vendor = 0;      // PCI vendor: 0x8086 Intel, 0x1002 AMD, 0x10DE NVIDIA
  uint64_t vram = 0;        // its own memory (an integrated GPU: what the firmware sets aside, often little)
  uint64_t ram = 0;         // the PC's memory
  uint32_t threads = 0;     // logical processors
  // mk9-lauta (2026-11-11): what Direct3D 12 offers on that adapter (Detect(true) only): its resource binding and heap
  // tiers (1-3), the highest shader model (0x60 = 6.0), unified memory (an integrated GPU), the driver's version
  // (four 16-bit parts). A player's log says it: what a report of one GPU needs.
  bool d3d12 = false;
  int binding_tier = 0, heap_tier = 0, shader_model = 0;
  bool uma = false;
  uint64_t driver = 0;
};
Info Detect(bool d3d12 = false);

// The preset for this PC: "Muy baja", "Baja", "Consola" or "Alta" (options_ui.h).
std::string ChoosePreset(const Info& info);

// Once at start, before the renderer (Mk9App::OnPostInitLogging).
void ApplyDefaults();

}  // namespace mk9::hardware
