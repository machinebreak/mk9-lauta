// mk9-lauta (2026-11-11): the release's logo and social networks (mk9_brand.h).

#include "mk9_brand.h"

#include "imgui_internal.h"  // ImGui::RegisterUserTexture

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "first_run_ui.h"
#include "mk9_overlay_input.h"
#include "mk9_updater.h"
#include "online/fight_state.h"
#include "online/i18n.h"
#include "online/netplay.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../sdk/thirdparty/stb/stb_image.h"

REXCVAR_DEFINE_BOOL(mk9_brand_menu, true, "MK9",
                    "The game's main menu shows MK9 Online by Lauta and Lauta's social networks at the bottom right "
                    "(mk9_brand.h)");

namespace mk9::brand {
namespace {

using i18n::T;

const unsigned char kLogo1024[] = {
#embed "brand/logo_1024.png"
};
const unsigned char kLogo640[] = {
#embed "brand/logo_640.png"
};
const unsigned char kLogo400[] = {
#embed "brand/logo_400.png"
};
const unsigned char kLogo240[] = {
#embed "brand/logo_240.png"
};
const unsigned char kIcons[] = {
#embed "brand/social_icons.png"
};

constexpr float kLogoAspect = 388.0f / 1024.0f;  // height / width (logo_1024.png)
constexpr int kIconRows[] = {128, 64, 32};       // social_icons.png: a row of six cells of each size, in this order

const Link kLinks[kSocialCount] = {
    {Social::kDiscord, "Discord", "Unirme al Discord", "https://discord.gg/7hbqkDmKHA", IM_COL32(88, 101, 242, 255)},
    {Social::kWhatsApp, "WhatsApp", "Canal de WhatsApp", "https://whatsapp.com/channel/0029VbDpnc1545uwf4F6ju0Z",
     IM_COL32(37, 211, 102, 255)},
    {Social::kInstagram, "Instagram", "Seguirme en Instagram", "https://www.instagram.com/lautatwt/",
     IM_COL32(225, 48, 108, 255)},
    {Social::kKick, "Kick", "Seguirme en Kick", "https://kick.com/lautatech", IM_COL32(83, 252, 24, 255)},
    {Social::kYouTube, "YouTube", "Seguirme en YouTube", "https://www.youtube.com/@LautaTWT?sub_confirmation=1",
     IM_COL32(255, 0, 51, 255)},
    {Social::kX, "X", "Seguirme en Twitter", "https://x.com/LautaTWT", IM_COL32(231, 231, 234, 255)},
};

// An embedded picture as a texture of the current ImGui context (the overlay's), decoded at its first use there.
struct Picture {
  const unsigned char* png;
  size_t size;
  int width = 0, height = 0;
  ImTextureData* texture = nullptr;
  bool failed = false;
};

Picture g_logos[] = {{kLogo240, sizeof(kLogo240)},
                     {kLogo400, sizeof(kLogo400)},
                     {kLogo640, sizeof(kLogo640)},
                     {kLogo1024, sizeof(kLogo1024)}};
constexpr int kLogoWidths[] = {240, 400, 640, 1024};
Picture g_icons{kIcons, sizeof(kIcons)};

ImTextureData* Texture(Picture& p) {
  if (p.texture || p.failed) return p.texture;
  int w = 0, h = 0, channels = 0;
  unsigned char* rgba = stbi_load_from_memory(p.png, int(p.size), &w, &h, &channels, 4);
  if (!rgba) {
    p.failed = true;
    REXLOG_WARN("[brand] a picture did not decode ({} bytes)", p.size);
    return nullptr;
  }
  auto* texture = IM_NEW(ImTextureData)();
  texture->Create(ImTextureFormat_RGBA32, w, h);
  std::memcpy(texture->GetPixels(), rgba, size_t(w) * size_t(h) * 4);
  stbi_image_free(rgba);
  texture->UseColors = true;
  texture->SetStatus(ImTextureStatus_WantCreate);
  ImGui::RegisterUserTexture(texture);
  p.width = w;
  p.height = h;
  p.texture = texture;
  return texture;
}

// The logo picture for a width: the narrowest one at least that wide (else the widest).
Picture& LogoFor(float width) {
  for (int i = 0; i < 4; ++i) {
    if (float(kLogoWidths[i]) >= width) return g_logos[i];
  }
  return g_logos[3];
}

// A color with the style's alpha (a fading window) on top of its own.
ImU32 Faded(ImU32 color) {
  const float alpha = ImGui::GetStyle().Alpha;
  if (alpha >= 1.0f) return color;
  const ImU32 a = ImU32(float((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
  return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  auto channel = [&](int shift) {
    const float x = float((a >> shift) & 0xFF), y = float((b >> shift) & 0xFF);
    return ImU32(x + (y - x) * t + 0.5f) << shift;
  };
  return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) | channel(IM_COL32_A_SHIFT);
}

// A light color behind (Kick's green, X's white): the icon and the words in black on it.
bool Light(ImU32 color) {
  const float r = float((color >> IM_COL32_R_SHIFT) & 0xFF), g = float((color >> IM_COL32_G_SHIFT) & 0xFF),
              b = float((color >> IM_COL32_B_SHIFT) & 0xFF);
  return 0.299f * r + 0.587f * g + 0.114f * b > 160.0f;
}

ImFont* Strong() {
  ImFont* font = renut::gpu::overlay::StrongFont();
  return font ? font : ImGui::GetFont();
}

ImFont* Title() {
  ImFont* font = renut::gpu::overlay::TitleFont();
  return font ? font : ImGui::GetFont();
}

}  // namespace

const Link& LinkOf(Social social) { return kLinks[int(social)]; }

float LogoHeight(float width) { return width * kLogoAspect; }

void LogoAt(ImDrawList* list, const ImVec2& pos, float width, ImU32 tint) {
  Picture& picture = LogoFor(width);
  ImTextureData* texture = Texture(picture);
  if (!texture || !list) return;
  list->AddImage(texture->GetTexRef(), pos, ImVec2(pos.x + width, pos.y + LogoHeight(width)), ImVec2(0, 0),
                 ImVec2(1, 1), Faded(tint));
}

void Logo(float width, bool center) {
  if (center) {
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - width) * 0.5f);
  }
  const ImVec2 at = ImGui::GetCursorScreenPos();
  LogoAt(ImGui::GetWindowDrawList(), at, width);
  ImGui::Dummy(ImVec2(width, LogoHeight(width)));
}

void IconAt(ImDrawList* list, Social social, const ImVec2& pos, float size, ImU32 color) {
  ImTextureData* texture = Texture(g_icons);
  if (!texture || !list) return;
  int row = 0, y = 0;
  for (int i = 2; i >= 0; --i) {  // the smallest row at least `size` px (else the biggest)
    if (float(kIconRows[i]) >= size) {
      row = i;
      break;
    }
  }
  for (int i = 0; i < row; ++i) y += kIconRows[i];
  const int cell = kIconRows[row];
  const float w = float(texture->Width), h = float(texture->Height);
  const ImVec2 uv0(float(int(social) * cell) / w, float(y) / h);
  const ImVec2 uv1(float((int(social) + 1) * cell) / w, float(y + cell) / h);
  list->AddImage(texture->GetTexRef(), pos, ImVec2(pos.x + size, pos.y + size), uv0, uv1, Faded(color));
}

bool SocialButton(Social social, const ImVec2& size, bool solid, const char* label) {
  const Link& link = LinkOf(social);
  const char* text = label ? label : T(link.label);
  ImGui::PushID(int(social));
  ImGui::PushID(text);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##social", size);
  const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
  const bool held = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
  if (ImGui::IsItemHovered()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetTooltip("%s", link.url);
  }
  ImDrawList* list = ImGui::GetWindowDrawList();
  const ImVec2 end(at.x + size.x, at.y + size.y);
  const float rounding = size.y * 0.24f;
  const ImU32 text_color = IM_COL32(236, 236, 240, 255);
  ImU32 fill, border, icon, words;
  if (solid) {
    fill = held ? Mix(link.color, IM_COL32(0, 0, 0, 255), 0.30f)
         : hovered ? Mix(link.color, IM_COL32(0, 0, 0, 255), 0.18f)
                   : link.color;
    border = fill;
    icon = words = Light(link.color) ? IM_COL32(10, 10, 12, 255) : IM_COL32_WHITE;
  } else {
    const ImU32 base = IM_COL32(16, 13, 14, 236);
    fill = held ? Mix(base, link.color, 0.22f) : hovered ? Mix(base, link.color, 0.11f) : base;
    border = hovered ? link.color : IM_COL32(58, 44, 46, 255);
    icon = link.color;
    words = text_color;
  }
  list->AddRectFilled(at, end, Faded(fill), rounding);
  list->AddRect(at, end, Faded(border), rounding, 0, std::max(1.0f, size.y / 40.0f));
  const float icon_size = std::round(size.y * 0.46f);
  const float pad = size.y * 0.34f;
  const ImVec2 icon_at(std::round(at.x + pad), std::round(at.y + (size.y - icon_size) * 0.5f));
  IconAt(list, social, icon_at, icon_size, icon);
  // the words, then a chevron at the right end while there is room for it
  ImFont* font = Strong();
  const float font_size = std::round(size.y * 0.40f);
  const ImVec2 extent = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text);
  const float text_x = icon_at.x + icon_size + size.y * 0.26f;
  const float chevron = size.y * 0.13f;
  const bool room = text_x + extent.x + pad + chevron * 2.0f < end.x;
  list->PushClipRect(at, ImVec2(end.x - pad * 0.5f, end.y), true);
  list->AddText(font, font_size, ImVec2(std::round(text_x), std::round(at.y + (size.y - extent.y) * 0.5f)),
                Faded(words), text);
  list->PopClipRect();
  if (room) {
    const float cx = end.x - pad - (hovered ? 0.0f : chevron * 0.5f), cy = at.y + size.y * 0.5f;
    const ImU32 c = solid ? Faded(Mix(words, fill, 0.30f)) : Faded(hovered ? text_color : IM_COL32(108, 104, 112, 255));
    const float t = std::max(1.2f, size.y / 26.0f);
    list->AddLine(ImVec2(cx - chevron, cy - chevron), ImVec2(cx, cy), c, t);
    list->AddLine(ImVec2(cx, cy), ImVec2(cx - chevron, cy + chevron), c, t);
  }
  if (pressed) {
    REXLOG_INFO("[brand] {} opened", link.name);
    updater::Open(link.url);
  }
  ImGui::PopID();
  ImGui::PopID();
  return pressed;
}

void SocialButtons(float width, int columns, float height) {
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float left = ImGui::GetCursorPosX();
  SocialButton(Social::kDiscord, ImVec2(width, height), true, T("Unirme a la comunidad de Discord"));
  const Social others[] = {Social::kWhatsApp, Social::kInstagram, Social::kKick, Social::kYouTube, Social::kX};
  const int count = int(std::size(others));
  columns = std::max(1, columns);
  const float w = std::floor((width - gap * float(columns - 1)) / float(columns));
  for (int i = 0; i < count; ++i) {
    const int column = i % columns;
    if (column == 0) {
      // the last row, if shorter, centered
      const int in_row = std::min(columns, count - i);
      ImGui::SetCursorPosX(left + (width - (float(in_row) * w + gap * float(in_row - 1))) * 0.5f);
    } else {
      ImGui::SameLine(0, gap);
    }
    SocialButton(others[i], ImVec2(w, height));
  }
}

namespace {

// The game's main menu: "MK9 Online by Lauta" over the networks' buttons at the bottom right of the frame (at 1080
// lines: the menu's prompts are at the bottom left, its item's description just above this corner's top).
class MenuCorner : public renut::gpu::overlay::Dialog {
 public:
  void Draw() override {
    const bool wanted = REXCVAR_GET(mk9_brand_menu) && !netplay::SessionGame() &&
                        netplay::GameState() == fight_state::kStateMainMenu && !overlay_input::AnyOpen() &&
                        !first_run_ui::Active();
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    alpha_ = wanted ? std::min(1.0f, alpha_ + dt / 0.35f) : std::max(0.0f, alpha_ - dt / 0.2f);
    if (alpha_ <= 0.0f) return;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!renut::gpu::overlay::FramePointToOutput(0, 0, &x0, &y0) ||
        !renut::gpu::overlay::FramePointToOutput(1, 1, &x1, &y1)) {
      const ImGuiIO& io = ImGui::GetIO();
      x0 = y0 = 0;
      x1 = io.DisplaySize.x;
      y1 = io.DisplaySize.y;
    }
    const float frame_h = y1 - y0;
    if (frame_h < 300.0f) return;
    const float s = frame_h / 1080.0f;
    // at 1080 lines: six buttons of 168 x 48, 8 apart; the caption above them; the logo at their left
    const ImVec2 button(std::round(168.0f * s), std::round(48.0f * s));
    const float gap = std::round(8.0f * s);
    const float row_w = button.x * 6 + gap * 5;
    const float right = x1 - std::round(28.0f * s), bottom = y1 - std::round(26.0f * s);
    const float row_x = right - row_w, row_y = bottom - button.y;
    const float caption = std::round(34.0f * s);
    const float caption_y = row_y - std::round(10.0f * s) - caption;
    const float logo_w = std::round(182.0f * s), logo_h = LogoHeight(logo_w);
    const float logo_x = row_x - std::round(18.0f * s) - logo_w;
    const float logo_y = std::round((caption_y + bottom) * 0.5f - logo_h * 0.5f);
    const ImVec2 corner(logo_x - std::round(70.0f * s), caption_y - std::round(26.0f * s));

    ImGui::GetStyle().FontScaleMain = 1.0f;  // the sizes here are in the frame's pixels
    ImGui::SetNextWindowPos(corner, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(x1 - corner.x, y1 - corner.y), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("##brand_corner", nullptr, flags)) {
      ImDrawList* list = ImGui::GetWindowDrawList();
      // a shade from nothing at the top and the left to dark at the bottom right, under the words and the buttons
      const ImU32 none = Faded(IM_COL32(0, 0, 0, 0));
      list->AddRectFilledMultiColor(corner, ImVec2(x1, y1), none, none, Faded(IM_COL32(0, 0, 0, 200)),
                                    Faded(IM_COL32(0, 0, 0, 70)));
      LogoAt(list, ImVec2(logo_x, logo_y), logo_w);
      // "MK9 ONLINE" in the logo's gold, "by Lauta" lighter
      ImFont* title = Title();
      const char* first = "MK9 ONLINE";
      const ImVec2 first_size = title->CalcTextSizeA(caption, FLT_MAX, 0.0f, first);
      const float d = std::max(1.0f, std::round(2.0f * s));
      list->AddText(title, caption, ImVec2(row_x + d, caption_y + d), Faded(IM_COL32(0, 0, 0, 200)), first);
      list->AddText(title, caption, ImVec2(row_x, caption_y), Faded(IM_COL32(255, 196, 64, 255)), first);
      const char* second = " by Lauta";
      ImFont* strong = Strong();
      const float second_size = std::round(caption * 0.78f);
      const float second_y = caption_y + (first_size.y - strong->CalcTextSizeA(second_size, FLT_MAX, 0.0f, second).y);
      list->AddText(strong, second_size, ImVec2(row_x + first_size.x + d, second_y + d), Faded(IM_COL32(0, 0, 0, 200)),
                    second);
      list->AddText(strong, second_size, ImVec2(row_x + first_size.x, second_y), Faded(IM_COL32(236, 232, 226, 255)),
                    second);
      // the buttons: the networks' names alone
      const Social order[] = {Social::kDiscord, Social::kWhatsApp, Social::kInstagram,
                              Social::kKick,    Social::kYouTube,  Social::kX};
      for (int i = 0; i < kSocialCount; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(row_x + float(i) * (button.x + gap), row_y));
        const Link& link = LinkOf(order[i]);
        SocialButton(order[i], button, order[i] == Social::kDiscord, order[i] == Social::kX ? "Twitter" : link.name);
      }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
  }

 private:
  float alpha_ = 0.0f;
};

}  // namespace

void Install() {
  static std::once_flag once;
  std::call_once(once, [] {
    static MenuCorner corner;
    renut::gpu::overlay::AddDialog(&corner);
  });
}

}  // namespace mk9::brand
