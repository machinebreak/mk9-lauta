// mk9-lauta: the window before the game while something it needs downloads (mk9_fetch_window.h).

#include "mk9_fetch_window.h"

#if defined(_WIN32) || (defined(__linux__) && !defined(__SWITCH__))

#include <algorithm>
#include <cstdlib>

#include <fmt/format.h>
#include <rex/logging.h>

#include "online/i18n.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>

#include <thread>
#else
#include "imgui.h"
#include "mk9_small_window.h"
#endif

namespace mk9::fetch_window {
namespace {

// "<n> de <m> MB" (millions of bytes, as the texts say the size)
std::string Amount(float progress, uint64_t bytes) {
  return fmt::format(fmt::runtime(i18n::T("{} de {} MB")),
                     int(std::clamp(progress, 0.0f, 1.0f) * float(bytes) / 1000000.0f), int((bytes + 500000) / 1000000));
}

#if defined(_WIN32)
constexpr UINT_PTR kTimer = 1;

struct Window {
  const Words* words = nullptr;
  const std::function<bool(float&)>* downloading = nullptr;
  HWND hwnd = nullptr, bar = nullptr, status = nullptr;
  HFONT font = nullptr, title_font = nullptr;
  UINT dpi = 96;
  bool closed = false;  // its X: the game closes
};
Window* g_window = nullptr;  // one at a time

// A UTF-8 text (the window's, translated) as Windows' wide one.
std::wstring WideText(const std::string& utf8) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring wide(size_t(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), n);
  return wide;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Window& w = *g_window;
  switch (msg) {
    case WM_TIMER: {
      float progress = 0;
      if (!(*w.downloading)(progress)) {
        DestroyWindow(hwnd);
        return 0;
      }
      SendMessageW(w.bar, PBM_SETPOS, WPARAM(std::clamp(progress, 0.0f, 1.0f) * 1000.0f), 0);
      SetWindowTextW(w.status, WideText(Amount(progress, w.words->bytes)).c_str());
      return 0;
    }
    case WM_CLOSE:
      w.closed = true;
      DestroyWindow(hwnd);
      return 0;
    case WM_CTLCOLORSTATIC:
      SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_DESTROY:
      KillTimer(hwnd, kTimer);
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void RunWindow(Window& w) {
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.lpszClassName = L"mk9LautaFetch";
  RegisterClassExW(&wc);  // the second window's call fails: the class is there
  w.dpi = GetDpiForSystem();
  auto scale = [&](int v) { return MulDiv(v, int(w.dpi), 96); };
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT area{0, 0, scale(560), scale(230)};
  AdjustWindowRectExForDpi(&area, style, FALSE, 0, w.dpi);
  const int ww = area.right - area.left, wh = area.bottom - area.top;
  w.hwnd = CreateWindowExW(0, wc.lpszClassName, WideText(w.words->title).c_str(), style,
                           (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh,
                           nullptr, nullptr, instance, nullptr);
  if (!w.hwnd) return;  // no window: the caller waits for the download without one
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, w.dpi);
  metrics.lfMessageFont.lfHeight = -MulDiv(10, int(w.dpi), 72);
  w.font = CreateFontIndirectW(&metrics.lfMessageFont);
  LOGFONTW title = metrics.lfMessageFont;
  title.lfHeight = -MulDiv(15, int(w.dpi), 72);
  title.lfWeight = FW_BOLD;
  w.title_font = CreateFontIndirectW(&title);
  auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int cw, int ch) {
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, scale(x), scale(y), scale(cw), scale(ch),
                                   w.hwnd, nullptr, instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(w.font), TRUE);
    return control;
  };
  HWND heading = make(L"STATIC", WideText(w.words->heading).c_str(), 0, 24, 16, 512, 30);
  SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(w.title_font), TRUE);
  make(L"STATIC", WideText(w.words->text).c_str(), 0, 24, 52, 512, 56);
  w.bar = make(PROGRESS_CLASSW, L"", 0, 24, 118, 512, 20);
  SendMessageW(w.bar, PBM_SETRANGE32, 0, 1000);
  w.status = make(L"STATIC", L"", SS_LEFT, 24, 146, 512, 22);
  make(L"STATIC", WideText(w.words->closing).c_str(), 0, 24, 180, 512, 40);
  ShowWindow(w.hwnd, SW_SHOW);
  SetForegroundWindow(w.hwnd);
  SetTimer(w.hwnd, kTimer, 150, nullptr);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(w.hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  DeleteObject(w.font);
  DeleteObject(w.title_font);
}
#endif

}  // namespace

bool Run(const Words& words, const std::function<bool(float& progress)>& downloading) {
#if defined(_WIN32)
  Window w;
  w.words = &words;
  w.downloading = &downloading;
  g_window = &w;
  std::thread ui([&w] { RunWindow(w); });  // a message loop of its own
  ui.join();
  g_window = nullptr;
  return !w.closed;
#else
  bool closed = false;
  small_window::Run(
      words.title.c_str(), 560, 230,
      [&](const small_window::Look& look) {
        float progress = 0;
        if (!downloading(progress)) return false;
        const float s = look.scale;
        ImGui::PushFont(look.title, 28.0f * s);
        ImGui::TextUnformatted(words.heading.c_str());
        ImGui::PopFont();
        ImGui::TextWrapped("%s", words.text.c_str());
        ImGui::Spacing();
        ImGui::ProgressBar(std::clamp(progress, 0.0f, 1.0f), ImVec2(-1.0f, 20.0f * s), "");
        ImGui::TextUnformatted(Amount(progress, words.bytes).c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.7f, 1.0f));
        ImGui::TextWrapped("%s", words.closing.c_str());
        ImGui::PopStyleColor();
        return true;
      },
      [&closed] {
        closed = true;
        return true;
      });
  return !closed;
#endif
}

void CloseGame() {
  rex::FlushLogging();
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), 0);
#endif
  std::_Exit(0);
}

}  // namespace mk9::fetch_window

#endif
