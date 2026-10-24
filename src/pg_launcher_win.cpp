// The start-up launcher's own window: a normal Win32 window drawn with Dear ImGui on
// Direct3D 11. It closes before the game's window opens, so nothing of it is carried into the
// game.
#include "pg_ui.h"

#include <algorithm>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);
#endif

namespace pg {

#if REX_PLATFORM_WIN32
namespace {

struct LauncherWindow {
  HWND hwnd = nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swap_chain = nullptr;
  ID3D11RenderTargetView* target = nullptr;
  UINT resize_width = 0, resize_height = 0;
  // Set when the window moves to a monitor with another scale: the fonts are rebuilt for it.
  bool dpi_changed = false;
  bool closed = false;
};

LauncherWindow* g_window = nullptr;

void CreateTarget(LauncherWindow& w) {
  ID3D11Texture2D* back_buffer = nullptr;
  w.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
  if (back_buffer) {
    w.device->CreateRenderTargetView(back_buffer, nullptr, &w.target);
    back_buffer->Release();
  }
}

void ReleaseTarget(LauncherWindow& w) {
  if (w.target) w.target->Release();
  w.target = nullptr;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
  switch (msg) {
    case WM_SIZE:
      if (g_window && wparam != SIZE_MINIMIZED) {
        g_window->resize_width = LOWORD(lparam);
        g_window->resize_height = HIWORD(lparam);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      // Room for the header bar and a card at the smallest.
      const UINT dpi = GetDpiForWindow(hwnd);
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize.x = MulDiv(520, int(dpi), 96);
      info->ptMinTrackSize.y = MulDiv(420, int(dpi), 96);
      return 0;
    }
    case WM_DPICHANGED: {
      const RECT* rect = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
                   rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
      if (g_window) g_window->dpi_changed = true;
      return 0;
    }
    case WM_CLOSE:
      if (g_window) g_window->closed = true;
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

std::string AdapterName(ID3D11Device* device) {
  std::string name;
  IDXGIDevice* dxgi_device = nullptr;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) return name;
  IDXGIAdapter* adapter = nullptr;
  if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
    DXGI_ADAPTER_DESC desc = {};
    if (SUCCEEDED(adapter->GetDesc(&desc))) {
      char text[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, text, sizeof(text), nullptr, nullptr);
      name = text;
    }
    adapter->Release();
  }
  dxgi_device->Release();
  return name;
}

// The monitors as SDL lists them, which is the order of the monitor setting.
std::vector<std::string> MonitorNames() {
  std::vector<std::string> names;
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return names;
  int count = 0;
  if (SDL_DisplayID* displays = SDL_GetDisplays(&count)) {
    for (int i = 0; i < count; ++i) {
      const char* name = SDL_GetDisplayName(displays[i]);
      std::string label = name && *name ? name : "Monitor";
      if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(displays[i])) {
        label += " (" + std::to_string(mode->w) + " x " + std::to_string(mode->h) + ")";
      }
      names.push_back(label);
    }
    SDL_free(displays);
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  return names;
}

void Destroy(LauncherWindow& w, const wchar_t* class_name, HINSTANCE instance) {
  ReleaseTarget(w);
  if (w.swap_chain) w.swap_chain->Release();
  if (w.context) w.context->Release();
  if (w.device) w.device->Release();
  if (w.hwnd) DestroyWindow(w.hwnd);
  UnregisterClassW(class_name, instance);
  g_window = nullptr;
}

}  // namespace
#endif

std::optional<std::filesystem::path> RunLauncherWindow(const std::filesystem::path& config_path,
                                                       const std::filesystem::path& folder) {
#if REX_PLATFORM_WIN32
  ImGui_ImplWin32_EnableDpiAwareness();
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const wchar_t* class_name = L"ProvingGroundLauncher";
  WNDCLASSEXW window_class = {sizeof(window_class)};
  window_class.style = CS_CLASSDC;
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  window_class.lpszClassName = class_name;
  RegisterClassExW(&window_class);

  // 760 x 820 at 100%, centred on the primary monitor's work area.
  const UINT dpi = GetDpiForSystem();
  RECT area = {};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
  RECT rect = {0, 0, MulDiv(760, int(dpi), 96), MulDiv(820, int(dpi), 96)};
  AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
  const int width = std::min<int>(rect.right - rect.left, area.right - area.left);
  const int height = std::min<int>(rect.bottom - rect.top, area.bottom - area.top);

  LauncherWindow w;
  g_window = &w;
  w.hwnd = CreateWindowExW(0, class_name, L"Tony Hawk's Proving Ground", WS_OVERLAPPEDWINDOW,
                           area.left + (area.right - area.left - width) / 2,
                           area.top + (area.bottom - area.top - height) / 2, width, height,
                           nullptr, nullptr, instance, nullptr);
  DXGI_SWAP_CHAIN_DESC desc = {};
  desc.BufferCount = 2;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.OutputWindow = w.hwnd;
  desc.SampleDesc.Count = 1;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  HRESULT result = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc,
      &w.swap_chain, &w.device, nullptr, &w.context);
  if (result == DXGI_ERROR_UNSUPPORTED) {
    result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &desc, &w.swap_chain, &w.device,
                                           nullptr, &w.context);
  }
  if (!w.hwnd || FAILED(result)) {
    // The game's own launcher screen shows instead.
    REXLOG_ERROR("Could not open the launcher window ({:08X})", uint32_t(result));
    Destroy(w, class_name, instance);
    return folder;
  }
  CreateTarget(w);

  StageSettings(config_path);
  LauncherState state;
  SetLauncherFolder(state, folder);
  state.gpu = AdapterName(w.device);
  state.monitors = MonitorNames();

  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGuiContext* context = ImGui::CreateContext();
  ImGui::SetCurrentContext(context);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  // Fonts and sizes follow the monitor's scale.
  const auto apply_scale = [&] {
    ui::SetScale(720.0f * float(GetDpiForWindow(w.hwnd)) / 96.0f);
    io.Fonts->Clear();
    ui::ConfigureFonts(io.Fonts);
    ImGui::GetStyle() = ImGuiStyle();
    ui::ConfigureLauncherStyle(ImGui::GetStyle());
  };
  apply_scale();
  ImGui_ImplWin32_Init(w.hwnd);
  ImGui_ImplDX11_Init(w.device, w.context);

  ShowWindow(w.hwnd, SW_SHOWDEFAULT);
  UpdateWindow(w.hwnd);

  bool play = false;
  while (!play && !w.closed) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (w.closed) break;
    if (IsIconic(w.hwnd)) {
      Sleep(16);
      continue;
    }
    if (w.resize_width && w.resize_height) {
      ReleaseTarget(w);
      w.swap_chain->ResizeBuffers(0, w.resize_width, w.resize_height, DXGI_FORMAT_UNKNOWN, 0);
      w.resize_width = w.resize_height = 0;
      CreateTarget(w);
    }
    if (w.dpi_changed) {
      w.dpi_changed = false;
      apply_scale();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    play = ui::DrawLauncher(state);
    ImGui::Render();
    const float clear[4] = {0.141f, 0.141f, 0.141f, 1.0f};
    w.context->OMSetRenderTargets(1, &w.target, nullptr);
    w.context->ClearRenderTargetView(w.target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    w.swap_chain->Present(1, 0);
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext(context);
  ImGui::SetCurrentContext(previous);
  Destroy(w, class_name, instance);

  // Saved on Play and on close, as the old launcher did.
  if (!state.check.folder.empty()) {
    rex::cvar::SetFlagByName("game_data_root", state.check.folder.string());
  }
  SaveSettings(config_path);
  if (!play) return std::nullopt;
  return state.check.folder;
#else
  (void)config_path, (void)folder;
  return std::nullopt;
#endif
}

}  // namespace pg
