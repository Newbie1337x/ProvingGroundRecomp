#pragma once

// Controller access to the options menu, for handhelds and couch play without a keyboard:
// View (Back) + Menu (Start) pressed together opens or closes it, Start alone closes it. While
// it is open the game sees untouched controllers and the menu reads them instead.
#include <cstdint>
#include <functional>

struct ImGuiIO;

namespace rex::input {
class InputSystem;
}

namespace pg {

// Installs the controller filter on the game's input system. request_menu(open) is called on
// the game's input polling threads; it should move to the UI thread to open or close the menu.
void InstallMenuInput(rex::input::InputSystem* input, std::function<void(bool open)> request_menu);

// Stops calling request_menu (at shutdown, before the app goes away).
void StopMenuRequests();

// Tells the filter whether the menu is open (UI thread, when it opens or closes).
void SetMenuInputOpen(bool open);

// Hands the controllers' state to Dear ImGui's gamepad navigation; call while the menu draws.
void FeedMenuGamepad(ImGuiIO& io);

// Releases the gamepad keys in Dear ImGui (when the menu closes), so none stays held.
void ReleaseMenuGamepad(ImGuiIO& io);

}  // namespace pg
