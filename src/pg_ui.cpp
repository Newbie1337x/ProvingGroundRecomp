// Launcher, in-game menu and FPS readout, and the settings they edit.
#include "pg_ui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
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
REXCVAR_DEFINE_STRING(language, "auto", "General",
                      "Game language: auto (the system's, when the game has it, else English), en, "
                      "fr, de, it or es")
    .allowed({"auto", "en", "fr", "de", "it", "es"})
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
constexpr std::array<const char*, 15> kSavedSettings = {
    "game_data_root",    "renderer",         "render_resolution", "upscaler",
    "d3d12_fsr_version", "sharpness",        "anisotropic_override", "fullscreen",
    "monitor",           "frame_rate_limit", "swap_post_effect",  "button_prompts",
    "language",          "show_fps",         "show_launcher",
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

// The GPU plugin registers the cvars of every renderer compiled into it. Before it is loaded (the
// start-up launcher), the SDK's build says.
bool D3D12Available() {
  if (rex::cvar::GetFlagInfo("d3d12_bindless")) return true;
#if defined(REX_HAS_D3D12) && REX_HAS_D3D12
  // A loaded plugin registers this one too: then it was built without Direct3D 12.
  return !rex::cvar::GetFlagInfo("vulkan_readback_resolve");
#else
  return false;
#endif
}

bool UsingD3D12() { return REXCVAR_GET(renderer) == "d3d12" && D3D12Available(); }

bool FsrAvailable() { return Fsr3Available() && (!UsingD3D12() || Fsr4RuntimePresent()); }

bool FsrEnabled() { return REXCVAR_GET(upscaler) == "fsr" && FsrAvailable(); }

int32_t RenderResolutionScale() {
  const std::string resolution = REXCVAR_GET(render_resolution);
  return resolution == "4k" ? 3 : resolution == "1440p" || resolution == "1080p" ? 2 : 1;
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

// Values of the GPU plugin's settings while it is not loaded yet (the start-up launcher): read
// from the settings file and saved back to it, which the plugin reads when it loads.
std::unordered_map<std::string, std::string> g_staged;

std::string SettingValue(const char* name, const char* fallback) {
  if (rex::cvar::GetFlagInfo(name)) return rex::cvar::GetFlagByName(name);
  const auto it = g_staged.find(name);
  return it != g_staged.end() ? it->second : fallback;
}

void SetSetting(const char* name, const std::string& value) {
  if (rex::cvar::GetFlagInfo(name)) {
    rex::cvar::SetFlagByName(name, value);
  } else {
    g_staged[name] = value;
  }
}

// The languages the game was released in, with their Xbox 360 language IDs.
struct GameLanguage {
  const char* code;
  const char* name;
  uint32_t xbox_id;
};
constexpr GameLanguage kGameLanguages[] = {
    {"en", "English", 1},  {"fr", "Fran\xC3\xA7" "ais", 4}, {"de", "Deutsch", 3},
    {"it", "Italiano", 6}, {"es", "Espa\xC3\xB1ol", 5},
};

const char* LanguageName(std::string_view code) {
  for (const auto& language : kGameLanguages) {
    if (code == language.code) return language.name;
  }
  return "English";
}

// The system's language when the game has it, else English.
std::string SystemLanguage() {
  std::string code;
#if REX_PLATFORM_WIN32
  switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_FRENCH: code = "fr"; break;
    case LANG_GERMAN: code = "de"; break;
    case LANG_ITALIAN: code = "it"; break;
    case LANG_SPANISH: code = "es"; break;
  }
#else
  for (const char* variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* value = std::getenv(variable);
    if (value && *value) {
      code = std::string(value).substr(0, 2);
      break;
    }
  }
#endif
  for (const auto& language : kGameLanguages) {
    if (code == language.code) return code;
  }
  return "en";
}

// Keyboard play (the SDK's keyboard-as-controller and its keys), saved when changed.
bool IsKeyboardSetting(std::string_view name) {
  return name == "mnk_mode" || name.starts_with("keybind_");
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
      // A setting of the GPU plugin before it is loaded (the start-up launcher) keeps its line
      // unless the launcher changed it.
      const bool ours = std::find_if(kSavedSettings.begin(), kSavedSettings.end(),
                                     [&](const char* name) { return key == name; }) !=
                            kSavedSettings.end() &&
                            (rex::cvar::GetFlagInfo(key) || g_staged.contains(key)) ||
                        (IsKeyboardSetting(key) && rex::cvar::GetFlagInfo(key));
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
    if (!info) {
      if (const auto it = g_staged.find(name); it != g_staged.end()) {
        out << name << " = " << TomlString(it->second) << "\n";
      }
      continue;
    }
    const std::string value = rex::cvar::GetFlagByName(name);
    out << name << " = "
        << (info->type == rex::cvar::FlagType::String ? TomlString(value) : value) << "\n";
  }
  for (const auto& entry : rex::cvar::GetRegistry()) {
    if (!IsKeyboardSetting(entry.name)) continue;
    const std::string value = entry.getter();
    if (value == entry.default_value) continue;
    out << entry.name << " = "
        << (entry.type == rex::cvar::FlagType::String ? TomlString(value) : value) << "\n";
  }
  for (const auto& line : kept) out << line << "\n";
}

void ApplyStartupSettings() {
  // The language the console reports to the game, unless given on the command line.
  const auto* user_language = rex::cvar::GetFlagInfo("user_language");
  if (user_language && user_language->source != rex::cvar::Source::kCommandLine) {
    std::string code = REXCVAR_GET(language);
    if (code == "auto") code = SystemLanguage();
    for (const auto& language : kGameLanguages) {
      if (code == language.code) {
        rex::cvar::SetFlagByName("user_language", std::to_string(language.xbox_id));
        REXLOG_INFO("Game language: {}", language.name);
      }
    }
  }
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

// Launcher cards (BeginCard / EndCard): rows are drawn as list rows in a rounded card.
bool g_card = false;
bool g_card_first_row = false;
float g_card_width = 0.0f;

// Launcher colours, after libadwaita's dark style.
constexpr ImVec4 kCardBg = ImVec4(0.216f, 0.216f, 0.216f, 1.0f);
constexpr ImU32 kRowLine = IM_COL32(0, 0, 0, 90);
constexpr ImVec4 kBlue = ImVec4(0.208f, 0.518f, 0.894f, 1.0f);
constexpr ImVec4 kBlueHover = ImVec4(0.235f, 0.565f, 0.945f, 1.0f);
constexpr ImVec4 kBlueActive = ImVec4(0.165f, 0.435f, 0.780f, 1.0f);
constexpr ImVec4 kDimText = ImVec4(1.0f, 1.0f, 1.0f, 0.55f);

// A row of a card: title (and subtitle) on the left, the control on the right, control_width
// wide (0: the usual width). Returns what the control returns.
template <typename Fn>
bool CardRow(const char* title, const char* subtitle, ImVec4 subtitle_color, const char* help,
             float control_width, Fn&& control) {
  const float s = g_scale;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  if (!g_card_first_row) {
    const float left = ImGui::GetWindowPos().x;
    draw->AddLine(ImVec2(left, start.y), ImVec2(left + ImGui::GetWindowWidth(), start.y), kRowLine,
                  1.0f * s);
  }
  g_card_first_row = false;
  const float line = ImGui::GetTextLineHeight();
  const float small_line = g_small ? g_small->LegacySize : line;
  const float height = (subtitle ? 64.0f : 52.0f) * s;
  const float text_height = subtitle ? line + 2 * s + small_line : line;
  if (control_width <= 0) control_width = std::min(260.0f * s, width * 0.5f);

  ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (height - text_height) * 0.5f));
  ImGui::PushTextWrapPos(start.x - ImGui::GetWindowPos().x + width - control_width - 12 * s);
  ImGui::TextUnformatted(title);
  if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
  if (subtitle) {
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (height - text_height) * 0.5f + line + 2 * s));
    PushFont(g_small);
    ImGui::PushStyleColor(ImGuiCol_Text, subtitle_color);
    ImGui::TextUnformatted(subtitle);
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  ImGui::PopTextWrapPos();

  ImGui::SetCursorScreenPos(ImVec2(start.x + width - control_width,
                                   start.y + (height - ImGui::GetFrameHeight()) * 0.5f));
  ImGui::SetNextItemWidth(control_width);
  ImGui::PushID(title);
  const bool result = control();
  ImGui::PopID();
  if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);

  // The next row starts right below; a zero-height item keeps the card's size to the row.
  ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height - ImGui::GetStyle().ItemSpacing.y));
  ImGui::Dummy(ImVec2(width, 0));
  return result;
}

// A switch as in GNOME settings, right-aligned in the item width.
bool Switch(const char* id, bool* value) {
  const float s = g_scale;
  const ImVec2 size(46 * s, 26 * s);
  const ImVec2 cursor = ImGui::GetCursorScreenPos();
  const float frame = ImGui::GetFrameHeight();
  ImGui::SetCursorScreenPos(
      ImVec2(cursor.x + ImGui::CalcItemWidth() - size.x, cursor.y + (frame - size.y) * 0.5f));
  const bool pressed = ImGui::InvisibleButton(id, size);
  if (pressed) *value = !*value;
  const ImVec2 p = ImGui::GetItemRectMin();
  const bool hovered = ImGui::IsItemHovered();
  ImVec4 track = *value ? (hovered ? kBlueHover : kBlue)
                        : ImVec4(0.33f, 0.33f, 0.33f, hovered ? 1.0f : 0.9f);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(track),
                      size.y * 0.5f);
  const float radius = size.y * 0.5f - 3 * s;
  const float knob_x = *value ? p.x + size.x - size.y * 0.5f : p.x + size.y * 0.5f;
  draw->AddCircleFilled(ImVec2(knob_x, p.y + size.y * 0.5f), radius, IM_COL32(255, 255, 255, 255));
  return pressed;
}

void BeginCard(const char* title) {
  const float s = g_scale;
  if (title) {
    PushFont(g_small);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2 * s));
  }
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kCardBg);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * s);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 0));
  ImGui::BeginChild(title ? title : "##card", ImVec2(g_card_width, 0),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  // The card's padding and rounding are taken; drop-downs opened in it keep the usual ones.
  ImGui::PopStyleVar(2);
  g_card = true;
  g_card_first_row = true;
}

void EndCard() {
  g_card = false;
  // The card ends where the last row does (each row leaves the cursor at its bottom edge).
  ImGui::Dummy(ImVec2(0, 0));
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, 14 * g_scale));
}

// Label on the left, control on the right. Returns true when the value changed.
template <typename Fn>
bool Row(const char* label, const char* help, Fn&& control) {
  if (g_card) return CardRow(label, nullptr, kDimText, help, 0.0f, control);
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

// A setting of the GPU plugin shows before the plugin is loaded (the start-up launcher) when it
// has a fallback: its default.
bool ComboSetting(const char* label, const char* cvar, const std::vector<Choice>& choices,
                  const char* help, const char* fallback = nullptr) {
  if (!rex::cvar::GetFlagInfo(cvar) && !fallback) return false;
  const std::string current = SettingValue(cvar, fallback ? fallback : "");
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
          SetSetting(cvar, choice.value);
          changed = true;
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
    if (!(g_card ? Switch("##toggle", &value) : ImGui::Checkbox("##toggle", &value))) return false;
    return rex::cvar::SetFlagByName(cvar, value ? "true" : "false");
  });
}

bool DisplayModeSetting() {
  bool fullscreen = rex::cvar::Query<bool>("fullscreen");
  int mode = fullscreen ? 0 : 1;
  return Row("Display mode", "F11 switches it while playing.", [&] {
    bool changed = false;
    if (g_card) {
      // A list row holds one control: a drop-down.
      const char* names[] = {"Fullscreen", "Windowed"};
      changed = ImGui::Combo("##mode", &mode, names, 2);
    } else {
      changed = ImGui::RadioButton("Fullscreen", &mode, 0);
      ImGui::SameLine();
      changed |= ImGui::RadioButton("Windowed", &mode, 1);
    }
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
constexpr const char* kAntiAliasingHelp =
    "FXAA smooths edges in each frame. AMD FSR AA smooths them and also takes the internal "
    "resolution to your screen: FSR 3.1 with Vulkan, FSR 4 with Direct3D 12 (the newest FSR your "
    "GPU driver offers: FSR 4.1 on Radeon RX 9000 and 7000, and on RX 6000 with recent drivers).\n\n"
    "The game's renderer has no motion vectors or camera jitter to give FSR, so it works on each "
    "frame on its own, without the temporal detail it adds in PC games.";
constexpr const char* kFsrVersionHelp =
    "Which FSR from AMD's runtime to use; Newest is what AMD picks for your GPU.";

const std::vector<Choice> kRenderers = {
    {"vulkan", "Vulkan"},
    {"d3d12", "Direct3D 12"},
};
constexpr const char* kRendererHelp =
    "Vulkan is the main renderer and the fastest. Direct3D 12 is there for AMD FSR 4, which AMD "
    "ships for Direct3D 12 only. Changing it restarts the game when you press Play.";
// Anti-aliasing: the game's FXAA, or AMD FSR, which also takes the internal resolution to the
// screen. Stored as swap_post_effect, and upscaler for FSR.
bool AntiAliasingSetting() {
  std::vector<Choice> choices = kAntiAliasing;
  if (FsrAvailable()) {
    choices.push_back({"fsr", UsingD3D12() ? "AMD FSR 4 AA" : "AMD FSR 3.1 AA"});
  }
  const std::string current = FsrEnabled() ? "fsr" : SettingValue("swap_post_effect", "none");
  const char* preview = current.c_str();
  for (const auto& choice : choices) {
    if (current == choice.value) preview = choice.label;
  }
  return Row("Anti-aliasing", kAntiAliasingHelp, [&] {
    bool changed = false;
    if (ImGui::BeginCombo("##combo", preview)) {
      for (const auto& choice : choices) {
        const bool selected = current == choice.value;
        if (ImGui::Selectable(choice.label, selected) && !selected) {
          const bool fsr = std::string_view(choice.value) == "fsr";
          rex::cvar::SetFlagByName("upscaler", fsr ? "fsr" : "off");
          SetSetting("swap_post_effect", fsr ? "none" : choice.value);
          changed = true;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    return changed;
  });
}

// The FSR versions AMD's D3D12 runtime offers on this GPU, as the renderer listed them.
std::vector<Choice> FsrVersionChoices() {
  // The choices point into these, which stay as they are until the next call.
  static std::vector<std::string> names, values;
  static std::string newest;
  names.clear();
  values.clear();
  std::stringstream list(rex::cvar::GetFlagByName("d3d12_fsr_versions"));
  for (std::string name; std::getline(list, name, ';');) {
    if (name.empty()) continue;
    names.push_back(name);
    // The setting keeps the version number, which the renderer matches against the name.
    values.push_back(name.substr(0, name.find_first_not_of("0123456789.")));
  }
  newest = names.empty() ? "Newest" : "Newest (" + names.front() + ")";
  std::vector<Choice> choices = {{"", newest.c_str()}};
  for (size_t i = 0; i < names.size(); ++i) {
    choices.push_back({values[i].c_str(), names[i].c_str()});
  }
  return choices;
}

const std::vector<Choice> kTextureFiltering = {
    {"0", "Off"}, {"2", "2x"}, {"3", "4x"}, {"4", "8x"}, {"5", "16x"},
};
constexpr const char* kTextureFilteringHelp =
    "Anisotropic filtering: keeps the ground and ramps sharp into the distance. 16x costs very "
    "little on current GPUs.";
constexpr const char* kLanguageHelp =
    "The game's text and voices. Automatic uses your system's language when the game has it, "
    "else English. Applies when the game starts.";

// The language choices, Automatic naming the language it picks.
std::vector<Choice> LanguageChoices() {
  static std::string automatic;
  automatic = std::string("Automatic (") + LanguageName(SystemLanguage()) + ")";
  std::vector<Choice> choices = {{"auto", automatic.c_str()}};
  for (const auto& language : kGameLanguages) choices.push_back({language.code, language.name});
  return choices;
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
  changed |= ComboSetting("Internal resolution", "render_resolution", kResolutions, kResolutionHelp);
  changed |= ComboSetting("Texture filtering", "anisotropic_override", kTextureFiltering,
                          kTextureFilteringHelp);
  changed |= AntiAliasingSetting();
  const bool fsr = FsrEnabled();
  if (fsr && UsingD3D12() && !rex::cvar::GetFlagByName("d3d12_fsr_versions").empty()) {
    changed |= ComboSetting("FSR version", "d3d12_fsr_version", FsrVersionChoices(),
                            kFsrVersionHelp);
  }
  if (fsr && SharpnessSetting()) {
    ApplyUpscaler();
    changed = true;
  }
  changed |= ComboSetting("Button icons", "button_prompts", kButtonPrompts, kButtonPromptsHelp);
  changed |= ToggleSetting("Show FPS counter", "show_fps", nullptr);

  if (in_game) {
    MutedNote("Renderer, resolution, anti-aliasing and button icons apply the next "
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

void ConfigureLauncherStyle(ImGuiStyle& style) {
  ConfigureStyle(style);
  // libadwaita's dark colours, with its blue accent.
  ImVec4* c = style.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.141f, 0.141f, 0.141f, 1.0f);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
  c[ImGuiCol_Border] = ImVec4(0, 0, 0, 0.35f);
  c[ImGuiCol_FrameBg] = ImVec4(0.29f, 0.29f, 0.29f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.33f, 0.33f, 0.33f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.37f, 0.37f, 0.37f, 1.0f);
  c[ImGuiCol_Button] = c[ImGuiCol_FrameBg];
  c[ImGuiCol_ButtonHovered] = c[ImGuiCol_FrameBgHovered];
  c[ImGuiCol_ButtonActive] = c[ImGuiCol_FrameBgActive];
  c[ImGuiCol_Header] = ImVec4(1, 1, 1, 0.08f);
  c[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.12f);
  c[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.16f);
  c[ImGuiCol_CheckMark] = kBlue;
  c[ImGuiCol_SliderGrab] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.9f, 0.9f, 0.9f, 1);
  c[ImGuiCol_NavCursor] = kBlue;
  c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.2f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.3f);
  c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.4f);
  style.WindowRounding = 0.0f;
  style.WindowBorderSize = 0.0f;
  style.ChildBorderSize = 0.0f;
  style.FrameRounding = 8.0f * g_scale;
  style.PopupRounding = 10.0f * g_scale;
}

namespace {

// A header-bar button: flat, a soft fill when hovered or selected.
bool HeaderButton(const char* label, ImVec2 size, bool selected) {
  const bool pressed = ImGui::InvisibleButton(label, size);
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  const float alpha = selected ? 0.10f : ImGui::IsItemHovered() ? 0.06f : 0.0f;
  if (alpha > 0) {
    ImGui::GetWindowDrawList()->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)),
                                              8 * g_scale);
  }
  PushFont(g_small);
  const ImVec2 text = ImGui::CalcTextSize(label);
  ImGui::GetWindowDrawList()->AddText(
      ImVec2(a.x + (size.x - text.x) * 0.5f, a.y + (size.y - text.y) * 0.5f),
      ImGui::GetColorU32(ImVec4(1, 1, 1, selected ? 1.0f : 0.8f)), label);
  ImGui::PopFont();
  return pressed;
}

void LauncherSettingsPage(LauncherState& state) {
  const float s = g_scale;
  BeginCard("Game");
  const char* status = "Choose the folder with the extracted files of your Xbox 360 disc";
  ImVec4 status_color = kDimText;
  switch (state.check.status) {
    case GameFolderCheck::Status::kEmpty:
      break;
    case GameFolderCheck::Status::kMissing:
      status = "default.xex was not found in this folder";
      status_color = kErrorColor;
      break;
    case GameFolderCheck::Status::kUnknownVersion:
      status = "Not the version this build was made for; it may not work";
      status_color = kWarnColor;
      break;
    case GameFolderCheck::Status::kOk:
      status = "Game found and verified";
      status_color = kOkColor;
      break;
  }
  CardRow("Game folder", status, status_color, nullptr, 120 * s, [&] {
    if (ImGui::Button("Browse...", ImVec2(-FLT_MIN, 0))) {
      if (auto picked = PickFolder("Select the Proving Ground game folder")) {
        SetLauncherFolder(state, *picked);
      }
    }
    return false;
  });
  const float path_width = std::min(420.0f * s, ImGui::GetContentRegionAvail().x * 0.72f);
  CardRow("Path", nullptr, kDimText, nullptr, path_width, [&] {
    if (ImGui::InputTextWithHint("##path", "Folder with default.xex", state.path,
                                 sizeof(state.path), ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
      SetLauncherFolder(state, state.path);
    }
    return false;
  });
  ComboSetting("Language", "language", LanguageChoices(), kLanguageHelp);
  ToggleSetting("Show this window at startup", "show_launcher",
                "Off: the game starts directly. Start with --launcher to see this window again.");
  EndCard();

  BeginCard("Display");
  DisplayModeSetting();
  if (state.monitors.size() > 1) {
    // The SDK's monitor setting: 0 for the system's main one, else its number.
    static std::vector<std::string> labels, values;
    labels.clear();
    values.clear();
    for (size_t i = 0; i < state.monitors.size(); ++i) {
      labels.push_back(std::to_string(i + 1) + ": " + state.monitors[i]);
      values.push_back(std::to_string(i + 1));
    }
    std::vector<Choice> choices = {{"0", "Main monitor"}};
    for (size_t i = 0; i < labels.size(); ++i) choices.push_back({values[i].c_str(), labels[i].c_str()});
    ComboSetting("Monitor", "monitor", choices, "The monitor the game opens on.");
  }
  ComboSetting("Frame rate", "frame_rate_limit", kFrameRates, kFrameRateHelp);
  ToggleSetting("Show FPS counter", "show_fps", nullptr);
  EndCard();

  BeginCard("Graphics");
  if (!state.gpu.empty()) {
    CardRow("Graphics card", nullptr, kDimText, nullptr, 0.0f, [&] {
      ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
      ImGui::AlignTextToFramePadding();
      const float width = ImGui::CalcTextSize(state.gpu.c_str()).x;
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::CalcItemWidth() - width));
      ImGui::TextUnformatted(state.gpu.c_str());
      ImGui::PopStyleColor();
      return false;
    });
  }
  if (D3D12Available()) ComboSetting("Renderer", "renderer", kRenderers, kRendererHelp);
  ComboSetting("Internal resolution", "render_resolution", kResolutions, kResolutionHelp);
  ComboSetting("Texture filtering", "anisotropic_override", kTextureFiltering,
               kTextureFilteringHelp, "3");
  AntiAliasingSetting();
  if (FsrEnabled()) SharpnessSetting();
  EndCard();
}

// The SDK's name for a key, as the keyboard settings take it; empty for keys it has no name for.
std::string KeyName(ImGuiKey key) {
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return std::string(1, char('A' + (key - ImGuiKey_A)));
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return std::string(1, char('0' + (key - ImGuiKey_0)));
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
    return "Numpad" + std::string(1, char('0' + (key - ImGuiKey_Keypad0)));
  }
  switch (key) {
    case ImGuiKey_Space: return "Space";
    case ImGuiKey_Enter: return "Return";
    case ImGuiKey_KeypadEnter: return "NumpadEnter";
    case ImGuiKey_Tab: return "Tab";
    case ImGuiKey_Backspace: return "Backspace";
    case ImGuiKey_Delete: return "Delete";
    case ImGuiKey_Insert: return "Insert";
    case ImGuiKey_Home: return "Home";
    case ImGuiKey_End: return "End";
    case ImGuiKey_PageUp: return "PageUp";
    case ImGuiKey_PageDown: return "PageDown";
    case ImGuiKey_LeftArrow: return "Left";
    case ImGuiKey_RightArrow: return "Right";
    case ImGuiKey_UpArrow: return "Up";
    case ImGuiKey_DownArrow: return "Down";
    case ImGuiKey_GraveAccent: return "Backtick";
    case ImGuiKey_Minus: return "Minus";
    case ImGuiKey_Equal: return "Plus";
    case ImGuiKey_Comma: return "Comma";
    case ImGuiKey_Period: return "Period";
    case ImGuiKey_Semicolon: return "Semicolon";
    case ImGuiKey_Slash: return "Slash";
    case ImGuiKey_Backslash: return "Backslash";
    case ImGuiKey_LeftBracket: return "LBracket";
    case ImGuiKey_RightBracket: return "RBracket";
    case ImGuiKey_Apostrophe: return "Quote";
    case ImGuiKey_KeypadAdd: return "NumpadPlus";
    case ImGuiKey_KeypadSubtract: return "NumpadMinus";
    case ImGuiKey_KeypadMultiply: return "NumpadStar";
    case ImGuiKey_KeypadDivide: return "NumpadSlash";
    default: return {};
  }
}

// How a key reads on its keycap.
std::string KeyLabel(std::string_view name) {
  static const std::pair<std::string_view, std::string_view> kLabels[] = {
      {"Semicolon", ";"}, {"Quote", "'"},   {"Comma", ","},      {"Period", "."},
      {"Slash", "/"},     {"Backslash", "\\"}, {"LBracket", "["}, {"RBracket", "]"},
      {"Backtick", "`"},  {"Minus", "-"},   {"Plus", "="},       {"Return", "Enter"},
      {"NumpadEnter", "Num Enter"}, {"Control", "Ctrl"}, {"Backspace", "Backspace"}};
  for (const auto& [key, label] : kLabels) {
    if (name == key) return std::string(label);
  }
  if (name.starts_with("Numpad")) return "Num " + std::string(name.substr(6));
  return std::string(name);
}

// A keyboard setting's keys as keycaps, right-aligned in the item width ("Shift+Up" is two
// keys, alternatives are separated by a slash). Clicking it returns true.
bool KeyCaps(const char* id, const std::string& binds, bool capturing) {
  const float s = g_scale;
  const ImVec2 size(ImGui::CalcItemWidth(), ImGui::GetFrameHeight());
  const bool pressed = ImGui::InvisibleButton(id, size);
  const ImVec2 a = ImGui::GetItemRectMin();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  if (capturing) {
    draw->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), ImGui::GetColorU32(kBlue), 8 * s);
    const char* text = "Press a key (Esc cancels)";
    const ImVec2 t = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(a.x + (size.x - t.x) * 0.5f, a.y + (size.y - t.y) * 0.5f),
                  IM_COL32(255, 255, 255, 255), text);
    return pressed;
  }
  if (ImGui::IsItemHovered()) {
    draw->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y),
                        ImGui::GetColorU32(ImVec4(1, 1, 1, 0.06f)), 8 * s);
  }
  // Pieces from left to right: keycaps and the "+" and "/" between them.
  struct Piece {
    std::string text;
    bool cap;
  };
  std::vector<Piece> pieces;
  std::string_view rest = binds;
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    std::string_view alternative = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    if (!pieces.empty()) pieces.push_back({"/", false});
    while (!alternative.empty()) {
      const size_t plus = alternative.find('+');
      pieces.push_back({KeyLabel(alternative.substr(0, plus)), true});
      alternative =
          plus == std::string_view::npos ? std::string_view() : alternative.substr(plus + 1);
      if (!alternative.empty()) pieces.push_back({"+", false});
    }
  }
  if (pieces.empty()) pieces.push_back({"None", false});
  const float pad = 8 * s, gap = 6 * s;
  float total = -gap;
  for (const auto& piece : pieces) {
    total += ImGui::CalcTextSize(piece.text.c_str()).x + (piece.cap ? pad * 2 : 0) + gap;
  }
  float x = a.x + size.x - total - pad * 0.5f;
  const float cap_h = size.y - 6 * s;
  for (const auto& piece : pieces) {
    const ImVec2 t = ImGui::CalcTextSize(piece.text.c_str());
    if (piece.cap) {
      const ImVec2 top(x, a.y + (size.y - cap_h) * 0.5f);
      const ImVec2 bottom(x + t.x + pad * 2, top.y + cap_h);
      draw->AddRectFilled(top, bottom, IM_COL32(74, 74, 74, 255), 6 * s);
      draw->AddLine(ImVec2(top.x + 4 * s, bottom.y), ImVec2(bottom.x - 4 * s, bottom.y),
                    IM_COL32(0, 0, 0, 110), 2 * s);
      draw->AddText(ImVec2(x + pad, a.y + (size.y - t.y) * 0.5f), IM_COL32(255, 255, 255, 255),
                    piece.text.c_str());
      x = bottom.x + gap;
    } else {
      draw->AddText(ImVec2(x, a.y + (size.y - t.y) * 0.5f), ImGui::GetColorU32(kDimText),
                    piece.text.c_str());
      x += t.x + gap;
    }
  }
  return pressed;
}

void LauncherControlsPage(LauncherState& state) {
  const float s = g_scale;
  BeginCard("Controller");
  ComboSetting("Button icons", "button_prompts", kButtonPrompts, kButtonPromptsHelp);
  EndCard();

  BeginCard("Keyboard");
  ToggleSetting("Play with the keyboard", "mnk_mode",
                "The keyboard works as an Xbox 360 controller with the keys below. Controllers "
                "keep working.");
  EndCard();
  if (!rex::cvar::GetFlagInfo("mnk_mode") || !rex::cvar::Query<bool>("mnk_mode")) {
    state.capturing.clear();
    return;
  }

  // One row per controller button: click, then press the key for it (Escape cancels).
  BeginCard("Keys");
  for (const auto& entry : rex::cvar::GetRegistry()) {
    if (!entry.name.starts_with("keybind_") || entry.name == "keybind_guide") continue;
    const bool capturing = state.capturing == entry.name;
    CardRow(entry.description.c_str(), nullptr, kDimText, nullptr, 0.0f, [&] {
      if (KeyCaps("##keys", entry.getter(), capturing)) {
        state.capturing = capturing ? "" : entry.name;
      }
      return false;
    });
    if (!capturing) continue;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      state.capturing.clear();
      continue;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
      if (!ImGui::IsKeyPressed(ImGuiKey(k), false)) continue;
      const std::string name = KeyName(ImGuiKey(k));
      if (name.empty()) continue;
      rex::cvar::SetFlagByName(entry.name, name);
      state.capturing.clear();
      break;
    }
  }
  EndCard();
  if (ImGui::Button("Restore default keys", ImVec2(0, 0))) {
    for (const auto& entry : rex::cvar::GetRegistry()) {
      if (entry.name.starts_with("keybind_")) rex::cvar::SetFlagByName(entry.name, entry.default_value);
    }
    state.capturing.clear();
  }
  ImGui::Dummy(ImVec2(0, 8 * s));
}

}  // namespace

bool DrawLauncher(LauncherState& state) {
  const float s = g_scale;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##launcher", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar();

  // Header bar: the pages in the middle, Play on the right.
  const float bar = 52 * s;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(0, 0), ImVec2(io.DisplaySize.x, bar), IM_COL32(48, 48, 48, 255));
  draw->AddLine(ImVec2(0, bar - 1), ImVec2(io.DisplaySize.x, bar - 1), IM_COL32(0, 0, 0, 120));
  const char* pages[] = {"Settings", "Controls"};
  const ImVec2 tab(120 * s, 36 * s);
  const float tabs_x = (io.DisplaySize.x - tab.x * 2 - 4 * s) * 0.5f;
  for (int i = 0; i < 2; ++i) {
    ImGui::SetCursorPos(ImVec2(tabs_x + i * (tab.x + 4 * s), (bar - tab.y) * 0.5f));
    if (HeaderButton(pages[i], tab, state.page == i)) state.page = i;
  }
  const bool can_play = state.check.status == GameFolderCheck::Status::kOk ||
                        state.check.status == GameFolderCheck::Status::kUnknownVersion;
  const ImVec2 play(104 * s, 36 * s);
  ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - play.x - 10 * s, (bar - play.y) * 0.5f));
  ImGui::BeginDisabled(!can_play);
  ImGui::PushStyleColor(ImGuiCol_Button, kBlue);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBlueHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kBlueActive);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  PushFont(g_small);
  bool play_pressed = ImGui::Button("Play", play);
  ImGui::PopFont();
  ImGui::PopStyleColor(4);
  ImGui::EndDisabled();
  if (!can_play && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Choose the game folder first.");
  }
  if (can_play && !io.WantTextInput && state.capturing.empty() &&
      ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
    play_pressed = true;
  }

  // The page, in a centred column that scrolls.
  ImGui::SetCursorPos(ImVec2(0, bar));
  ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None);
  const float column = std::min(600.0f * s, io.DisplaySize.x - 48 * s);
  g_card_width = column;
  ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - column) * 0.5f, 24 * s));
  ImGui::BeginGroup();
  if (state.page == 0) {
    LauncherSettingsPage(state);
  } else {
    LauncherControlsPage(state);
  }
  ImGui::EndGroup();
  ImGui::Dummy(ImVec2(0, 8 * s));
  ImGui::EndChild();
  ImGui::End();
  return play_pressed;
}

}  // namespace ui

void StageSettings(const std::filesystem::path& config_path) {
  // The plugin's settings in the file, before the plugin registers them: name = 'value'.
  std::ifstream in(config_path);
  for (std::string line; std::getline(in, line);) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, line.find_first_of(" ="));
    if (rex::cvar::GetFlagInfo(key) ||
        std::find_if(kSavedSettings.begin(), kSavedSettings.end(),
                     [&](const char* name) { return key == name; }) == kSavedSettings.end()) {
      continue;
    }
    std::string value = line.substr(line.find_first_not_of(' ', eq + 1));
    if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"')) {
      value = value.substr(1, value.size() - 2);
    }
    g_staged[key] = value;
  }
}

void SetLauncherFolder(LauncherState& state, const std::filesystem::path& folder) {
  state.check = CheckGameFolder(folder);
  const std::filesystem::path shown = state.check.folder.empty() ? folder : state.check.folder;
  std::snprintf(state.path, sizeof(state.path), "%s", shown.string().c_str());
}

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
