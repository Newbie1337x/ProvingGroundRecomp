// Native folder picker for the launcher: the system dialog on Windows, zenity or kdialog
// elsewhere. Without one the launcher's path field still works.
#include "pg_ui.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

namespace pg {

#if REX_PLATFORM_WIN32

namespace {

std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(size_t(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
  return out;
}

}  // namespace

std::optional<std::filesystem::path> PickFolder(const std::string& title) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::optional<std::filesystem::path> result;
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(Widen(title).c_str());
    if (SUCCEEDED(dialog->Show(GetActiveWindow()))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          result = std::filesystem::path(path);
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return result;
}

#else

namespace {

std::optional<std::string> RunPicker(const std::string& command) {
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) return std::nullopt;
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
  const int status = pclose(pipe);
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
  if (status != 0 || output.empty()) return std::nullopt;
  return output;
}

bool HasCommand(const char* name) {
  return std::system((std::string("command -v ") + name + " >/dev/null 2>&1").c_str()) == 0;
}

std::string Quote(const std::string& text) {
  std::string out = "'";
  for (char c : text) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  return out + "'";
}

}  // namespace

std::optional<std::filesystem::path> PickFolder(const std::string& title) {
  std::optional<std::string> picked;
  if (HasCommand("zenity")) {
    picked = RunPicker("zenity --file-selection --directory --title=" + Quote(title) +
                       " 2>/dev/null");
  } else if (HasCommand("kdialog")) {
    picked = RunPicker("kdialog --getexistingdirectory ~ --title " + Quote(title) +
                       " 2>/dev/null");
  }
  if (!picked) return std::nullopt;
  return std::filesystem::path(*picked);
}

#endif

}  // namespace pg
