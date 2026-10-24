// Launcher, in-game menu and FPS readout, and the settings they edit.
#include "pg_ui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/presenter.h>

#if REX_PLATFORM_WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#include "pg_frame_pacing.h"

REXCVAR_DEFINE_STRING(render_resolution, "720p", "Video",
                      "Internal rendering resolution. The game renders at 1280x720 and only "
                      "integer scales are possible: 720p (1x), 1440p (2x), 4k (3x). 1080p "
                      "renders at 1440p and is scaled down to the window. Overridden by "
                      "resolution_scale when that is set.")
    .allowed({"720p", "1080p", "1440p", "4k"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(renderer, "vulkan", "Video",
                      "Renderer: vulkan or d3d12 (Windows builds with Direct3D 12). The upscaler "
                      "is AMD FSR 3.1 on Vulkan and AMD FSR 4 on Direct3D 12.")
    .allowed({"vulkan", "d3d12"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(upscaler, "off", "Video",
                      "How the rendered image is scaled to the screen: off (bilinear) or fsr (AMD "
                      "FSR: 3.1 with the Vulkan renderer, 4 with Direct3D 12)")
    .allowed({"off", "fsr"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(sharpness, 30, "Video", "Upscaler sharpness, 0 to 100").range(0, 100);
REXCVAR_DEFINE_BOOL(show_fps, false, "Video", "Show a frame rate counter in the corner");
REXCVAR_DEFINE_BOOL(show_launcher, true, "General",
                    "Show the launcher at startup; it also appears when the game folder is not "
                    "set or not valid");

namespace pg {

namespace {

// default.xex of the version the code was generated from (size and FNV-1a 64 of the file).
constexpr uintmax_t kExpectedXexSize = 10366976;
constexpr uint64_t kExpectedXexHash = 0xa02344935b47f717ull;

// Settings written to the config file, in this order.
constexpr std::array<const char*, 11> kSavedSettings = {
    "game_data_root", "renderer",         "render_resolution", "upscaler", "sharpness",
    "fullscreen",     "frame_rate_limit", "swap_post_effect",  "button_prompts", "show_fps",
    "show_launcher",
};

std::function<rex::ui::Presenter*()> g_presenter_source;
uint32_t g_window_physical_height = 720;

// Height of the screen the game is shown on.
uint32_t DisplayHeight() {
#if REX_PLATFORM_WIN32
  if (rex::cvar::Query<bool>("fullscreen")) {
    // The desktop mode of the primary monitor, in physical pixels whatever the DPI scale.
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsHeight) {
      return mode.dmPelsHeight;
    }
  }
#endif
  return std::max<uint32_t>(g_window_physical_height, 1);
}

// FSR 3.1 runs on Vulkan with the FidelityFX runtime the presenter is built with.
bool Fsr3Available() {
  const auto* info = rex::cvar::GetFlagInfo("present_effect");
  if (!info) return false;
  const auto& allowed = info->constraints.allowed_values;
  return std::find(allowed.begin(), allowed.end(), "fsr3") != allowed.end();
}

// AMD's FSR SDK 2 runtime for Direct3D 12, which the D3D12 presenter loads; it runs FSR 4 on GPUs
// whose driver supports it and FSR 3.1 on others.
constexpr const char* kFsr4Runtime = "amd_fidelityfx_loader_dx12.dll";

bool Fsr4RuntimePresent() {
#if REX_PLATFORM_WIN32
  std::error_code ec;
  return std::filesystem::exists(rex::filesystem::GetExecutableFolder() / kFsr4Runtime, ec);
#else
  return false;
#endif
}

// The GPU plugin registers the cvars of every renderer compiled into it.
bool D3D12Available() { return rex::cvar::GetFlagInfo("d3d12_bindless") != nullptr; }

bool UsingD3D12() { return REXCVAR_GET(renderer) == "d3d12" && D3D12Available(); }

bool FsrAvailable() { return Fsr3Available() && (!UsingD3D12() || Fsr4RuntimePresent()); }

bool FsrEnabled() { return REXCVAR_GET(upscaler) == "fsr" && FsrAvailable(); }

int32_t RenderResolutionScale() {
  const std::string resolution = REXCVAR_GET(render_resolution);
  return resolution == "4k" ? 3 : resolution == "1440p" || resolution == "1080p" ? 2 : 1;
}

// The FSR preset that upscales this render height to the screen by about the same ratio.
const char* FsrPresetName(uint32_t render_height, uint32_t display_height) {
  const double ratio = double(display_height) / double(render_height);
  return ratio < 0.95   ? "Supersampling"
         : ratio < 1.15 ? "Native AA"
         : ratio < 1.6  ? "Quality"
         : ratio < 1.85 ? "Balanced"
         : ratio < 2.5  ? "Performance"
                        : "Ultra Performance";
}

// FSR's sharpness parameter: stops of reduction, 0 is the sharpest.
double FsrSharpnessReduction() { return 2.0 * (1.0 - REXCVAR_GET(sharpness) / 100.0); }

std::string TomlString(const std::string& value) {
  // Literal strings keep Windows backslashes as they are.
  if (value.find('\'') == std::string::npos && value.find('\n') == std::string::npos) {
    return "'" + value + "'";
  }
  std::string out = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}

uint64_t HashFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  uint64_t hash = 0xcbf29ce484222325ull;
  std::vector<char> buffer(1 << 20);
  while (file) {
    file.read(buffer.data(), std::streamsize(buffer.size()));
    for (std::streamsize i = 0; i < file.gcount(); ++i) {
      hash ^= uint8_t(buffer[size_t(i)]);
      hash *= 0x100000001b3ull;
    }
  }
  return hash;
}

bool HasXex(const std::filesystem::path& folder) {
  std::error_code ec;
  return std::filesystem::is_regular_file(folder / "default.xex", ec);
}

}  // namespace

void SaveSettings(const std::filesystem::path& config_path) {
  // Keep any other lines the user added, replacing only the settings written here.
  std::vector<std::string> kept;
  {
    std::ifstream in(config_path);
    std::string line;
    while (std::getline(in, line)) {
      const auto key_end = line.find_first_of(" =");
      const std::string key = line.substr(0, key_end);
      const bool ours = std::find_if(kSavedSettings.begin(), kSavedSettings.end(),
                                     [&](const char* name) { return key == name; }) !=
                        kSavedSettings.end();
      if (!ours && line.rfind("# Proving Ground settings", 0) != 0) kept.push_back(line);
    }
  }
  std::ofstream out(config_path, std::ios::trunc);
  if (!out) {
    REXLOG_WARN("Could not save settings to {}", config_path.string());
    return;
  }
  out << "# Proving Ground settings (written by the launcher and the Insert menu)\n";
  for (const char* name : kSavedSettings) {
    const auto* info = rex::cvar::GetFlagInfo(name);
    if (!info) continue;
    const std::string value = rex::cvar::GetFlagByName(name);
    out << name << " = "
        << (info->type == rex::cvar::FlagType::String ? TomlString(value) : value) << "\n";
  }
  for (const auto& line : kept) out << line << "\n";
}

void ApplyStartupSettings() {
  ApplyFrameRateLimit();
  ApplyUpscaler();
  if (rex::cvar::HasNonDefaultValue("resolution_scale")) return;
  const int32_t scale = RenderResolutionScale();
  rex::cvar::SetFlagByName("resolution_scale", std::to_string(scale));
  REXLOG_INFO("Rendering at {}p (resolution_scale {}), upscaler {}, screen height {}", 720 * scale,
              scale, REXCVAR_GET(upscaler), DisplayHeight());
}

void SetPresenterSource(std::function<rex::ui::Presenter*()> source) {
  g_presenter_source = std::move(source);
}

void SetWindowPhysicalHeight(uint32_t height) {
  if (height) g_window_physical_height = height;
}

void ApplyUpscaler() {
  // The game renders at the mode's resolution (resolution_scale) and FSR takes that image to the
  // screen; "nativeaa" makes the presenter run FSR also at 1:1 and never shrink its input first.
  // The presenter's "fsr3" effect is FidelityFX's upscaler, which is FSR 4 where the D3D12
  // runtime offers it. The cvars cover a presenter created later; the paint config changes the
  // running one.
  const bool fsr3 = FsrEnabled();
  rex::cvar::SetFlagByName("present_effect", fsr3 ? "fsr3" : "bilinear");
  if (fsr3) rex::cvar::SetFlagByName("present_fsr_quality_mode", "nativeaa");
  rex::cvar::SetFlagByName("present_fsr_sharpness_reduction",
                           std::to_string(FsrSharpnessReduction()));
  rex::ui::Presenter* presenter = g_presenter_source ? g_presenter_source() : nullptr;
  if (!presenter) return;
  using Config = rex::ui::Presenter::GuestOutputPaintConfig;
  Config config = presenter->GetGuestOutputPaintConfigFromUIThread();
  config.SetEffect(fsr3 ? Config::Effect::kFsr3 : Config::Effect::kBilinear);
  if (fsr3) config.SetFsrQualityMode(Config::FsrQualityMode::kNativeAa);
  config.SetFsrSharpnessReduction(float(FsrSharpnessReduction()));
  presenter->SetGuestOutputPaintConfigFromUIThread(config);
}

const char* RequiredRenderer() { return REXCVAR_GET(renderer) == "d3d12" ? "d3d12" : "vulkan"; }

bool Relaunch(const std::filesystem::path& game_folder) {
#if REX_PLATFORM_WIN32
  // The same command line, with the game folder so the new process skips the launcher.
  const std::filesystem::path exe = rex::filesystem::GetExecutablePath();
  std::wstring command = L"\"" + exe.wstring() + L"\"";
  int argc = 0;
  if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg.starts_with(L"--launcher") || arg.starts_with(L"--game_data_root")) continue;
      command += L" \"" + arg + L"\"";
    }
    LocalFree(argv);
  }
  command += L" \"--game_data_root=" + game_folder.wstring() + L"\"";
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      exe.parent_path().c_str(), &startup, &process)) {
    REXLOG_ERROR("Could not restart the game for the {} renderer", RequiredRenderer());
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
#else
  (void)game_folder;
  return false;
#endif
}

GameFolderCheck CheckGameFolder(const std::filesystem::path& chosen) {
  GameFolderCheck check;
  if (chosen.empty()) return check;
  check.status = GameFolderCheck::Status::kMissing;
  std::error_code ec;
  if (!std::filesystem::is_directory(chosen, ec)) return check;
  std::filesystem::path folder;
  if (HasXex(chosen)) {
    folder = chosen;
  } else {
    // Accept the folder that contains the extracted game, one level up.
    for (const auto& entry : std::filesystem::directory_iterator(chosen, ec)) {
      if (entry.is_directory(ec) && HasXex(entry.path())) {
        folder = entry.path();
        break;
      }
    }
  }
  if (folder.empty()) return check;
  check.folder = folder;
  const auto xex = folder / "default.xex";
  const bool expected = std::filesystem::file_size(xex, ec) == kExpectedXexSize &&
                        HashFile(xex) == kExpectedXexHash;
  check.status = expected ? GameFolderCheck::Status::kOk : GameFolderCheck::Status::kUnknownVersion;
  return check;
}

namespace ui {

namespace {

float g_scale = 1.0f;
ImFont* g_body = nullptr;
ImFont* g_small = nullptr;
ImFont* g_strong = nullptr;
ImFont* g_title = nullptr;

constexpr ImU32 kAccent = IM_COL32(230, 57, 70, 255);
constexpr ImVec4 kAccentV = ImVec4(0.902f, 0.224f, 0.275f, 1.0f);
constexpr ImVec4 kOkColor = ImVec4(0.42f, 0.85f, 0.52f, 1.0f);
constexpr ImVec4 kWarnColor = ImVec4(0.98f, 0.76f, 0.32f, 1.0f);
constexpr ImVec4 kErrorColor = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
constexpr ImVec4 kMutedColor = ImVec4(0.62f, 0.64f, 0.70f, 1.0f);

ImFont* AddFirstFont(ImFontAtlas* atlas, std::initializer_list<const char*> paths, float size) {
  for (const char* path : paths) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) continue;
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    if (ImFont* font = atlas->AddFontFromFileTTF(path, size * g_scale, &config)) return font;
  }
  return nullptr;
}

// Pushes one of the fonts above at the size it was loaded with (the default font if missing).
void PushFont(ImFont* font) {
  if (font) {
    ImGui::PushFont(font, font->LegacySize);
  } else {
    ImGui::PushFont(nullptr, 0.0f);
  }
}

void SectionHeader(const char* text) {
  ImGui::Dummy(ImVec2(0, 6 * g_scale));
  PushFont(g_small);
  ImGui::PushStyleColor(ImGuiCol_Text, kAccentV);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(
      p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), IM_COL32(255, 255, 255, 28),
      1.0f * g_scale);
  ImGui::Dummy(ImVec2(0, 4 * g_scale));
}

// Label on the left, control on the right. Returns true when the value changed.
template <typename Fn>
bool Row(const char* label, const char* help, Fn&& control) {
  const float label_width = 190 * g_scale;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
  ImGui::SameLine(label_width);
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::PushID(label);
  const bool changed = control();
  ImGui::PopID();
  if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
  return changed;
}

struct Choice {
  const char* value;
  const char* label;
};

bool ComboSetting(const char* label, const char* cvar, const std::vector<Choice>& choices,
                  const char* help) {
  if (!rex::cvar::GetFlagInfo(cvar)) return false;
  const std::string current = rex::cvar::GetFlagByName(cvar);
  const char* preview = current.c_str();
  for (const auto& choice : choices) {
    if (current == choice.value) preview = choice.label;
  }
  return Row(label, help, [&] {
    bool changed = false;
    if (ImGui::BeginCombo("##combo", preview)) {
      for (const auto& choice : choices) {
        const bool selected = current == choice.value;
        if (ImGui::Selectable(choice.label, selected) && !selected) {
          changed = rex::cvar::SetFlagByName(cvar, choice.value);
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    return changed;
  });
}

bool ToggleSetting(const char* label, const char* cvar, const char* help) {
  if (!rex::cvar::GetFlagInfo(cvar)) return false;
  bool value = rex::cvar::Query<bool>(cvar);
  return Row(label, help, [&] {
    if (!ImGui::Checkbox("##toggle", &value)) return false;
    return rex::cvar::SetFlagByName(cvar, value ? "true" : "false");
  });
}

bool DisplayModeSetting() {
  bool fullscreen = rex::cvar::Query<bool>("fullscreen");
  int mode = fullscreen ? 0 : 1;
  return Row("Display mode", "F11 switches it while playing.", [&] {
    bool changed = ImGui::RadioButton("Fullscreen", &mode, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Windowed", &mode, 1);
    if (changed) rex::cvar::SetFlagByName("fullscreen", mode == 0 ? "true" : "false");
    return changed;
  });
}

const std::vector<Choice> kResolutions = {
    {"720p", "1280 x 720 (original)"},
    {"1080p", "1920 x 1080"},
    {"1440p", "2560 x 1440"},
    {"4k", "3840 x 2160 (4K)"},
};
const std::vector<Choice> kFrameRates = {
    {"30", "30 FPS (original)"}, {"60", "60 FPS"},   {"120", "120 FPS"}, {"144", "144 FPS"},
    {"165", "165 FPS"},          {"240", "240 FPS"}, {"0", "Unlimited"},
};
const std::vector<Choice> kAntiAliasing = {
    {"none", "Off"},
    {"fxaa", "FXAA"},
    {"fxaa_extreme", "FXAA (high quality)"},
};

constexpr const char* kResolutionHelp =
    "Internal resolution: what the game renders at, before it is scaled to your screen (by FSR "
    "when Upscaling is on). Higher values are sharper and cost more GPU time. The game can render "
    "at 720p, 1440p or 4K only; 1080p renders at 1440p.";
constexpr const char* kFrameRateHelp =
    "30 FPS keeps the original console pacing with vsync. Higher limits run the game uncapped "
    "up to that rate; cutscenes keep their original speed.";
constexpr const char* kAntiAliasingHelp = "Post-process anti-aliasing applied to each frame.";

const std::vector<Choice> kRenderers = {
    {"vulkan", "Vulkan"},
    {"d3d12", "Direct3D 12"},
};
constexpr const char* kRendererHelp =
    "Vulkan is the main renderer and the fastest. Direct3D 12 is there for AMD FSR 4, which AMD "
    "ships for Direct3D 12 only. Changing it restarts the game when you press Play.";
constexpr const char* kUpscalerHelp =
    "How the rendered image is scaled to your screen. AMD FSR upscales and sharpens it, and "
    "smooths edges at your screen's resolution (Native AA).\n\nVulkan uses FSR 3.1. Direct3D 12 "
    "uses AMD's FSR 4 runtime, which picks the newest FSR your GPU driver supports (FSR 4.1 on "
    "Radeon RX 9000 and 7000, and on RX 6000 with recent drivers) and falls back to FSR 3.1.\n\n"
    "The game's renderer has no motion vectors or camera jitter to give FSR, so it rebuilds "
    "each frame on its own, without the temporal detail it adds in PC games.";
std::vector<Choice> UpscalerChoices() {
  return {{"off", "Off (bilinear)"}, {"fsr", UsingD3D12() ? "AMD FSR 4" : "AMD FSR 3.1"}};
}

const std::vector<Choice> kButtonPrompts = {
    {"auto", "Automatic"},
    {"xbox", "Xbox"},
    {"playstation", "PlayStation"},
};
constexpr const char* kButtonPromptsHelp =
    "Button icons shown in the game. Automatic shows PlayStation icons when a PlayStation "
    "controller is connected as the game starts.";

bool SharpnessSetting() {
  int value = REXCVAR_GET(sharpness);
  return Row("Sharpness", "Sharpening strength of the upscaler. Changes apply immediately.", [&] {
    if (!ImGui::SliderInt("##sharpness", &value, 0, 100, "%d%%")) return false;
    return rex::cvar::SetFlagByName("sharpness", std::to_string(value));
  });
}

void MutedNote(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, kMutedColor);
  PushFont(g_small);
  ImGui::TextWrapped("%s", text);
  ImGui::PopFont();
  ImGui::PopStyleColor();
}

bool GraphicsSettings(bool in_game) {
  bool changed = false;
  changed |= DisplayModeSetting();
  if (ComboSetting("Frame rate", "frame_rate_limit", kFrameRates, kFrameRateHelp)) {
    ApplyFrameRateLimit();
    changed = true;
  }
  if (D3D12Available()) {
    changed |= ComboSetting("Renderer", "renderer", kRenderers, kRendererHelp);
  }
  if (FsrAvailable()) {
    changed |= ComboSetting("Upscaling", "upscaler", UpscalerChoices(), kUpscalerHelp);
  }
  const bool fsr = FsrEnabled();
  if (fsr && SharpnessSetting()) {
    ApplyUpscaler();
    changed = true;
  }
  changed |= ComboSetting("Internal resolution", "render_resolution", kResolutions, kResolutionHelp);
  changed |= ComboSetting("Anti-aliasing", "swap_post_effect", kAntiAliasing, kAntiAliasingHelp);
  changed |= ComboSetting("Button icons", "button_prompts", kButtonPrompts, kButtonPromptsHelp);
  changed |= ToggleSetting("Show FPS counter", "show_fps", nullptr);

  const uint32_t render = 720u * uint32_t(RenderResolutionScale());
  const uint32_t screen = DisplayHeight();
  char summary[160];
  if (fsr) {
    // The FSR preset that has the same ratio, as PC games name them.
    std::snprintf(summary, sizeof(summary), "FSR %s: renders at %up, %s to your %up screen.",
                  FsrPresetName(render, screen), render,
                  render < screen ? "upscaled" : render > screen ? "downscaled" : "anti-aliased",
                  screen);
  } else if (render < screen) {
    std::snprintf(summary, sizeof(summary), "Renders at %up and is scaled up to your %up screen.",
                  render, screen);
  } else {
    std::snprintf(summary, sizeof(summary), "Renders at %up for your %up screen.", render, screen);
  }
  MutedNote(summary);
  if (in_game) {
    MutedNote("Renderer, resolution, upscaling, anti-aliasing and button icons apply the next "
              "time the game starts.");
  }
  return changed;
}

bool AccentButton(const char* label, ImVec2 size, bool enabled = true) {
  ImGui::BeginDisabled(!enabled);
  ImGui::PushStyleColor(ImGuiCol_Button, kAccentV);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.33f, 0.38f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.17f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  PushFont(g_strong);
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopFont();
  ImGui::PopStyleColor(4);
  ImGui::EndDisabled();
  return pressed;
}

void TitleBlock(const char* subtitle) {
  PushFont(g_title);
  ImGui::TextUnformatted("PROVING GROUND");
  ImGui::PopFont();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 56 * g_scale, p.y + 4 * g_scale),
                                            kAccent, 2 * g_scale);
  ImGui::Dummy(ImVec2(0, 8 * g_scale));
  ImGui::PushStyleColor(ImGuiCol_Text, kMutedColor);
  ImGui::TextUnformatted(subtitle);
  ImGui::PopStyleColor();
}

}  // namespace

void ConfigureFonts(ImFontAtlas* atlas) {
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  g_body = AddFirstFont(atlas,
                        {"C:\\Windows\\Fonts\\segoeui.ttf",
                         "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                         "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                         "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
                         "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                         "/usr/share/fonts/TTF/DejaVuSans.ttf",
                         "/system/fonts/Roboto-Regular.ttf"},
                        18.0f);
  g_small = AddFirstFont(atlas,
                         {"C:\\Windows\\Fonts\\seguisb.ttf",
                          "/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
                          "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
                          "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                          "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                          "/system/fonts/Roboto-Medium.ttf"},
                         14.0f);
  g_strong = AddFirstFont(atlas,
                          {"C:\\Windows\\Fonts\\seguisb.ttf",
                           "/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
                           "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                           "/system/fonts/Roboto-Medium.ttf"},
                          19.0f);
  g_title = AddFirstFont(atlas,
                         {"C:\\Windows\\Fonts\\segoeuib.ttf",
                          "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                          "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                          "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                          "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                          "/system/fonts/Roboto-Bold.ttf"},
                         38.0f);
  if (g_body) ImGui::GetIO().FontDefault = g_body;
}

void ConfigureStyle(ImGuiStyle& style) {
  style.WindowRounding = 12.0f;
  style.ChildRounding = 8.0f;
  style.FrameRounding = 6.0f;
  style.PopupRounding = 8.0f;
  style.GrabRounding = 6.0f;
  style.TabRounding = 6.0f;
  style.ScrollbarRounding = 8.0f;
  style.WindowBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.WindowPadding = ImVec2(24, 20);
  style.FramePadding = ImVec2(12, 7);
  style.ItemSpacing = ImVec2(10, 10);
  style.ItemInnerSpacing = ImVec2(8, 6);
  style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
  style.Alpha = 1.0f;
  style.DisabledAlpha = 0.45f;

  ImVec4* c = style.Colors;
  c[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.96f, 1.0f);
  c[ImGuiCol_TextDisabled] = kMutedColor;
  c[ImGuiCol_WindowBg] = ImVec4(0.070f, 0.075f, 0.090f, 0.97f);
  c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.105f, 0.125f, 1.0f);
  c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.095f, 0.115f, 0.98f);
  c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
  c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.145f, 0.17f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.195f, 0.23f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.225f, 0.26f, 1.0f);
  c[ImGuiCol_TitleBg] = c[ImGuiCol_WindowBg];
  c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
  c[ImGuiCol_CheckMark] = kAccentV;
  c[ImGuiCol_SliderGrab] = kAccentV;
  c[ImGuiCol_SliderGrabActive] = kAccentV;
  c[ImGuiCol_Button] = ImVec4(0.17f, 0.175f, 0.205f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.23f, 0.235f, 0.27f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.27f, 0.275f, 0.31f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.90f, 0.22f, 0.27f, 0.30f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.90f, 0.22f, 0.27f, 0.45f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.90f, 0.22f, 0.27f, 0.60f);
  c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
  c[ImGuiCol_Tab] = ImVec4(0.12f, 0.125f, 0.15f, 1.0f);
  c[ImGuiCol_TabHovered] = ImVec4(0.90f, 0.22f, 0.27f, 0.55f);
  c[ImGuiCol_TabSelected] = ImVec4(0.90f, 0.22f, 0.27f, 0.85f);
  c[ImGuiCol_TabSelectedOverline] = kAccentV;
  c[ImGuiCol_NavCursor] = kAccentV;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
  style.ScaleAllSizes(g_scale);
}

// Called before the fonts and style are built, from the window's height in logical pixels.
void SetScale(float window_height) { g_scale = std::clamp(window_height / 720.0f, 1.0f, 3.0f); }

}  // namespace ui

// --- Launcher ---

Launcher::Launcher(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                   std::filesystem::path initial_folder, PlayCallback on_play,
                   std::function<void()> on_quit)
    : ImGuiDialog(drawer),
      config_path_(std::move(config_path)),
      on_play_(std::move(on_play)),
      on_quit_(std::move(on_quit)) {
  SetFolder(initial_folder);
}

void Launcher::SetFolder(const std::filesystem::path& folder) {
  const std::string text = folder.string();
  std::snprintf(path_buffer_, sizeof(path_buffer_), "%s", text.c_str());
  check_ = CheckGameFolder(folder);
  if (!check_.folder.empty() && check_.folder != folder) {
    std::snprintf(path_buffer_, sizeof(path_buffer_), "%s", check_.folder.string().c_str());
  }
}

void Launcher::OnDraw(ImGuiIO& io) {
  using ui::g_scale;
  if (started_) return;
  const float s = g_scale;

  // Backdrop with a soft accent glow.
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilledMultiColor(ImVec2(0, 0), io.DisplaySize, IM_COL32(16, 17, 22, 255),
                              IM_COL32(24, 18, 22, 255), IM_COL32(12, 12, 16, 255),
                              IM_COL32(10, 11, 14, 255));
  bg->AddCircleFilled(ImVec2(io.DisplaySize.x * 0.85f, io.DisplaySize.y * 0.1f),
                      io.DisplaySize.y * 0.45f, IM_COL32(230, 57, 70, 18), 64);

  const float width = std::min(680.0f * s, io.DisplaySize.x - 32.0f * s);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, io.DisplaySize.y - 24 * s));
  ImGui::Begin("##pg_launcher", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings);

  ui::TitleBlock("Tony Hawk's Proving Ground  -  PC recompilation");

  ui::SectionHeader("GAME FILES");
  const float browse_width = 120 * s;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_width -
                          ImGui::GetStyle().ItemSpacing.x);
  if (ImGui::InputTextWithHint("##path", "Folder with your extracted Xbox 360 game",
                               path_buffer_, sizeof(path_buffer_),
                               ImGuiInputTextFlags_EnterReturnsTrue) ||
      ImGui::IsItemDeactivatedAfterEdit()) {
    SetFolder(path_buffer_);
  }
  ImGui::SameLine();
  if (ImGui::Button("Browse...", ImVec2(browse_width, 0))) {
    if (auto picked = PickFolder("Select the Proving Ground game folder")) SetFolder(*picked);
  }
  switch (check_.status) {
    case GameFolderCheck::Status::kEmpty:
      ImGui::PushStyleColor(ImGuiCol_Text, ui::kMutedColor);
      ImGui::TextWrapped(
          "Choose the folder with the extracted files of your own Xbox 360 disc (the one "
          "with default.xex).");
      break;
    case GameFolderCheck::Status::kMissing:
      ImGui::PushStyleColor(ImGuiCol_Text, ui::kErrorColor);
      ImGui::TextWrapped("default.xex was not found in this folder.");
      break;
    case GameFolderCheck::Status::kUnknownVersion:
      ImGui::PushStyleColor(ImGuiCol_Text, ui::kWarnColor);
      ImGui::TextWrapped(
          "Found default.xex, but it is not the version this build was made for. The game may "
          "crash or misbehave.");
      break;
    case GameFolderCheck::Status::kOk:
      ImGui::PushStyleColor(ImGuiCol_Text, ui::kOkColor);
      ImGui::TextWrapped("Game found and verified.");
      break;
  }
  ImGui::PopStyleColor();

  ui::SectionHeader("GRAPHICS");
  ui::GraphicsSettings(false);

  ImGui::Dummy(ImVec2(0, 6 * s));
  bool show = REXCVAR_GET(show_launcher);
  if (ImGui::Checkbox("Show this screen at startup", &show)) {
    rex::cvar::SetFlagByName("show_launcher", show ? "true" : "false");
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Start the game directly next time. Press Insert in game to change "
                      "options, or start with --launcher to see this screen again.");
  }

  ImGui::Dummy(ImVec2(0, 8 * s));
  const bool can_play = check_.status == GameFolderCheck::Status::kOk ||
                        check_.status == GameFolderCheck::Status::kUnknownVersion;
  const float quit_width = 110 * s;
  const float play_width =
      ImGui::GetContentRegionAvail().x - quit_width - ImGui::GetStyle().ItemSpacing.x;
  bool play = ui::AccentButton("PLAY", ImVec2(play_width, 46 * s), can_play);
  if (can_play && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) play = true;
  ImGui::SameLine();
  if (ImGui::Button("Quit", ImVec2(quit_width, 46 * s))) {
    ImGui::End();
    on_quit_();
    return;
  }

  ui::PushFont(ui::g_small);
  ImGui::PushStyleColor(ImGuiCol_Text, ui::kMutedColor);
  ImGui::TextUnformatted("In game: Insert opens the options menu.");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::End();

  if (play) {
    started_ = true;
    rex::cvar::SetFlagByName("game_data_root", check_.folder.string());
    SaveSettings(config_path_);
    on_play_(check_.folder);
  }
}

// --- In-game menu ---

GameMenu::GameMenu(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                   std::function<void()> on_close)
    : ImGuiDialog(drawer), config_path_(std::move(config_path)), on_close_(std::move(on_close)) {}

GameMenu::~GameMenu() {
  if (dirty_) SaveSettings(config_path_);
}

void GameMenu::OnDraw(ImGuiIO& io) {
  using ui::g_scale;
  const float s = g_scale;
  const float width = std::min(560.0f * s, io.DisplaySize.x - 32.0f * s);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
  bool open = true;
  ImGui::Begin("Options##pg_menu", &open,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings);

  const double fps = PresentedFps();
  ui::PushFont(ui::g_strong);
  if (fps > 0) {
    ImGui::Text("%.0f FPS", fps);
  } else {
    ImGui::TextUnformatted("Options");
  }
  ImGui::PopFont();
  if (fps > 0) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kMutedColor);
    ImGui::Text("%.2f ms per frame", 1000.0 / fps);
    ImGui::PopStyleColor();
  }

  if (ImGui::BeginTabBar("##tabs")) {
    if (ImGui::BeginTabItem("Graphics")) {
      dirty_ |= ui::GraphicsSettings(true);
      bool show = REXCVAR_GET(show_launcher);
      if (ui::Row("Launcher at startup", "Show the start screen the next time the game starts.",
                  [&] { return ImGui::Checkbox("##launcher", &show); })) {
        rex::cvar::SetFlagByName("show_launcher", show ? "true" : "false");
        dirty_ = true;
      }
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Advanced")) {
      ImGui::PushStyleColor(ImGuiCol_Text, ui::kMutedColor);
      ImGui::TextWrapped("Renderer options for troubleshooting. The defaults are the fastest.");
      ImGui::PopStyleColor();
      dirty_ |= ui::ToggleSetting("Draw extents on CPU", "execute_unclipped_draw_vs_on_cpu",
                     "Runs the vertex shader on the CPU for draws without a viewport, so render "
                     "target copies only cover what was drawn (about 2.7 ms of GPU time per frame "
                     "at 1440p).");
      dirty_ |= ui::ToggleSetting("Threaded submission", "vulkan_async_submission",
                     "Records and submits Vulkan command buffers on a separate thread.");
      dirty_ |= ui::ToggleSetting("Early vblank after swap", "vblank_after_swap",
                     "Without vsync, delivers the game's vblank as soon as a swap is processed.");
      dirty_ |= ui::ToggleSetting("Real-time cutscene clock", "uncapped_real_time",
                     "Keeps cutscenes at their original speed when the frame rate is above 30. "
                     "Takes effect after a restart.");
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("About")) {
      ImGui::TextWrapped(
          "Tony Hawk's Proving Ground, statically recompiled from the Xbox 360 version with the "
          "ReXGlue SDK. The game files come from your own copy of the game.");
      ImGui::Dummy(ImVec2(0, 4 * s));
      ImGui::PushStyleColor(ImGuiCol_Text, ui::kMutedColor);
      ImGui::TextUnformatted("Insert            Options menu");
      ImGui::TextUnformatted("F11               Fullscreen");
      ImGui::PopStyleColor();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::Dummy(ImVec2(0, 4 * s));
  if (ImGui::Button("Resume", ImVec2(ImGui::GetContentRegionAvail().x, 38 * s))) open = false;
  ImGui::End();
  if (!open) on_close_();
}

// --- FPS readout ---

void FpsOverlay::OnDraw(ImGuiIO& io) {
  if (!REXCVAR_GET(show_fps)) return;
  const double fps = PresentedFps();
  if (fps <= 0) return;
  const float s = ui::g_scale;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12 * s, 12 * s), ImGuiCond_Always,
                          ImVec2(1, 0));
  ImGui::SetNextWindowBgAlpha(0.55f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * s, 5 * s));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * s);
  ImGui::Begin("##pg_fps", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
  ui::PushFont(ui::g_strong);
  ImGui::Text("%.0f FPS", fps);
  ImGui::PopFont();
  ImGui::End();
  ImGui::PopStyleVar(2);
}

}  // namespace pg
