#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <rex/ui/imgui_dialog.h>

struct ImFontAtlas;
struct ImGuiStyle;

namespace pg {

// Settings shown in the launcher and the in-game menu, saved next to the executable.
void SaveSettings(const std::filesystem::path& config_path);

// Applies the settings that the GPU reads when the runtime starts (internal resolution).
void ApplyStartupSettings();

// What the launcher found in a candidate game folder.
struct GameFolderCheck {
  enum class Status { kEmpty, kMissing, kUnknownVersion, kOk };
  Status status = Status::kEmpty;
  // The folder that actually holds default.xex (it may be a subfolder of the chosen one).
  std::filesystem::path folder;
};
GameFolderCheck CheckGameFolder(const std::filesystem::path& chosen);

// Native folder picker; nullopt if cancelled or unavailable on this system.
std::optional<std::filesystem::path> PickFolder(const std::string& title);

namespace ui {

// Sets the UI scale from the window height in logical pixels; call before the fonts are built.
void SetScale(float window_height);
void ConfigureFonts(ImFontAtlas* atlas);
void ConfigureStyle(ImGuiStyle& style);

}  // namespace ui

// Start screen: game folder and the main options, then Play.
class Launcher final : public rex::ui::ImGuiDialog {
 public:
  using PlayCallback = std::function<void(const std::filesystem::path& game_folder)>;
  Launcher(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
           std::filesystem::path initial_folder, PlayCallback on_play, std::function<void()> on_quit);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void SetFolder(const std::filesystem::path& folder);

  std::filesystem::path config_path_;
  PlayCallback on_play_;
  std::function<void()> on_quit_;
  char path_buffer_[1024] = {};
  GameFolderCheck check_;
  bool started_ = false;
};

// In-game options, toggled with Insert.
class GameMenu final : public rex::ui::ImGuiDialog {
 public:
  GameMenu(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
           std::function<void()> on_close);
  ~GameMenu() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::filesystem::path config_path_;
  std::function<void()> on_close_;
  bool dirty_ = false;
};

// Small frame rate readout in the corner, shown with the show_fps setting.
class FpsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace pg
