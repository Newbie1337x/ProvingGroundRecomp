#include "pg_input_script.h"

#include <array>
#include <cstring>
#include <iterator>

#include <rex/chrono/clock.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/ui/virtual_key.h>

// The SDK's X_ERROR_* / X_STATUS_* macros expand to unqualified type names.
using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace {

using rex::input::DeviceId;
using rex::ui::VirtualKey;

constexpr DeviceId kScriptDevice = static_cast<DeviceId>(0x50475200);  // "PGR"

// Same thresholds and repeat timing as the SDK's SDL driver, so replayed
// keystrokes look like the ones the recording session produced.
constexpr int16_t kThumbThreshold = 0x4E00;
constexpr uint8_t kTriggerThreshold = 0x1F;
constexpr uint32_t kRepeatDelayMs = 400;
constexpr uint32_t kRepeatRateMs = 100;

// Bit order matches SDLInputDriver::GetDeviceKeystroke: 16 XInput buttons, then
// analog inputs turned into virtual buttons.
constexpr std::array<VirtualKey, 34> kVkLookup = {
    VirtualKey::kXInputPadDpadUp,       VirtualKey::kXInputPadDpadDown,
    VirtualKey::kXInputPadDpadLeft,     VirtualKey::kXInputPadDpadRight,
    VirtualKey::kXInputPadStart,        VirtualKey::kXInputPadBack,
    VirtualKey::kXInputPadLThumbPress,  VirtualKey::kXInputPadRThumbPress,
    VirtualKey::kXInputPadLShoulder,    VirtualKey::kXInputPadRShoulder,
    VirtualKey::kNone,                  VirtualKey::kNone,
    VirtualKey::kXInputPadA,            VirtualKey::kXInputPadB,
    VirtualKey::kXInputPadX,            VirtualKey::kXInputPadY,
    VirtualKey::kXInputPadLTrigger,     VirtualKey::kXInputPadRTrigger,
    VirtualKey::kXInputPadLThumbUp,     VirtualKey::kXInputPadLThumbDown,
    VirtualKey::kXInputPadLThumbRight,  VirtualKey::kXInputPadLThumbLeft,
    VirtualKey::kXInputPadLThumbUpLeft, VirtualKey::kXInputPadLThumbUpRight,
    VirtualKey::kXInputPadLThumbDownRight, VirtualKey::kXInputPadLThumbDownLeft,
    VirtualKey::kXInputPadRThumbUp,     VirtualKey::kXInputPadRThumbDown,
    VirtualKey::kXInputPadRThumbRight,  VirtualKey::kXInputPadRThumbLeft,
    VirtualKey::kXInputPadRThumbUpLeft, VirtualKey::kXInputPadRThumbUpRight,
    VirtualKey::kXInputPadRThumbDownRight, VirtualKey::kXInputPadRThumbDownLeft,
};

// Guards the nested InputSystem::GetState call made while recording.
thread_local bool t_sampling = false;

enum : int64_t { kMarkNone = 0, kMarkMeasureStart = 1, kMarkMeasureStop = 2, kMarkOther = 3 };

void SetFrameMarker(int64_t code, const std::string& text) {
#ifdef REXGLUE_PERF_HAS_MARKER
  // One value per frame: never let a plain mark hide a measurement boundary.
  if (code == kMarkMeasureStart || code == kMarkMeasureStop ||
      (code == kMarkOther && !rex::perf::GetCounter(rex::perf::CounterId::kMarker))) {
    rex::perf::SetCounter(rex::perf::CounterId::kMarker, code);
  }
#else
  (void)code;
#endif
#ifdef REXGLUE_ENABLE_PROFILING
  if (TracyIsStarted) TracyMessage(text.data(), text.size());
#endif
  REXLOG_INFO("Input script: {}", text);
}

}  // namespace

std::unique_ptr<PgInputScript> PgInputScript::Create(rex::input::InputSystem* system, Mode mode,
                                                     const std::string& path,
                                                     std::function<void()> request_quit,
                                                     bool quit_on_measure_stop) {
  std::unique_ptr<PgInputScript> script(
      new PgInputScript(system, mode, std::move(request_quit), quit_on_measure_stop));
  // Not yet shared with any polling thread, so no lock is needed here.
  if (mode == Mode::kRecord) {
    script->file_ = rex::filesystem::OpenFile(rex::to_path(path), "w");
    if (!script->file_) {
      REXLOG_ERROR("Input script: cannot create {}", path);
      return nullptr;
    }
    std::fputs("# pg-input v1: guest ms since first poll; pad buttons(hex) lt rt lx ly rx ry\n",
               script->file_);
    REXLOG_INFO("Input script: recording to {} (F9 toggles the measurement window)", path);
  } else {
    if (!script->LoadLocked(path)) return nullptr;
    REXLOG_INFO("Input script: replaying {} ({} events)", path, script->events_.size());
  }
  return script;
}

PgInputScript::PgInputScript(rex::input::InputSystem* system, Mode mode,
                             std::function<void()> request_quit, bool quit_on_measure_stop)
    : InputDriver(nullptr, 0),
      system_(system),
      mode_(mode),
      request_quit_(std::move(request_quit)),
      quit_on_measure_stop_(quit_on_measure_stop) {}

X_STATUS PgInputScript::Setup() {
  return X_STATUS_SUCCESS;
}

PgInputScript::~PgInputScript() {
  std::lock_guard lock(mutex_);
  if (file_) {
    if (started_) WriteLocked(NowLocked(), "end");
    std::fclose(file_);
  }
}

bool PgInputScript::LoadLocked(const std::string& path) {
  std::FILE* file = rex::filesystem::OpenFile(rex::to_path(path), "r");
  if (!file) {
    REXLOG_ERROR("Input script: cannot open {}", path);
    return false;
  }
  char line[512];
  int line_number = 0;
  uint32_t last_ms = 0;
  bool ok = true;
  while (std::fgets(line, sizeof(line), file)) {
    ++line_number;
    char* text = line + std::strspn(line, " \t");
    text[std::strcspn(text, "\r\n")] = '\0';
    if (!*text || *text == '#') continue;
    Event event;
    char kind[32] = {};
    int consumed = 0;
    if (std::sscanf(text, "%u %31s %n", &event.ms, kind, &consumed) < 2) {
      REXLOG_ERROR("Input script: {}:{}: expected '<ms> <command>'", path, line_number);
      ok = false;
      break;
    }
    const char* rest = text + consumed;
    if (!std::strcmp(kind, "pad")) {
      unsigned buttons, lt, rt;
      int lx, ly, rx, ry;
      if (std::sscanf(rest, "%x %u %u %d %d %d %d", &buttons, &lt, &rt, &lx, &ly, &rx, &ry) != 7 ||
          buttons > 0xFFFF || lt > 255 || rt > 255) {
        REXLOG_ERROR("Input script: {}:{}: bad pad values", path, line_number);
        ok = false;
        break;
      }
      event.pad = {uint16_t(buttons), uint8_t(lt),  uint8_t(rt), int16_t(lx),
                   int16_t(ly),       int16_t(rx), int16_t(ry)};
    } else if (!std::strcmp(kind, "mark")) {
      event.kind = Kind::kMark;
      event.text = rest;
    } else if (!std::strcmp(kind, "measure_start")) {
      event.kind = Kind::kMeasureStart;
    } else if (!std::strcmp(kind, "measure_stop")) {
      event.kind = Kind::kMeasureStop;
    } else if (!std::strcmp(kind, "quit")) {
      event.kind = Kind::kQuit;
    } else if (!std::strcmp(kind, "end")) {
      continue;  // written by the recorder; replay ends after the last event anyway
    } else {
      REXLOG_ERROR("Input script: {}:{}: unknown command '{}'", path, line_number, kind);
      ok = false;
      break;
    }
    if (event.ms < last_ms) {
      REXLOG_ERROR("Input script: {}:{}: times must not decrease", path, line_number);
      ok = false;
      break;
    }
    last_ms = event.ms;
    events_.push_back(std::move(event));
  }
  std::fclose(file);
  return ok;
}

uint32_t PgInputScript::NowLocked() {
  uint32_t uptime = rex::chrono::Clock::QueryGuestUptimeMillis();
  if (!started_) {
    started_ = true;
    start_ms_ = uptime;
  }
  return uptime - start_ms_;
}

void PgInputScript::WriteLocked(uint32_t ms, const char* line) {
  std::fprintf(file_, "%u %s\n", ms, line);
}

void PgInputScript::RecordSample() {
  // The nested GetState polls every device on user 0 except this one, so the
  // sample is exactly the merged state the guest would see.
  X_INPUT_STATE state = {};
  t_sampling = true;
  X_RESULT result = system_->GetState(0, &state);
  t_sampling = false;
  Pad pad;
  if (result == X_ERROR_SUCCESS) {
    pad = {uint16_t(state.gamepad.buttons), state.gamepad.left_trigger,
           state.gamepad.right_trigger,    int16_t(state.gamepad.thumb_lx),
           int16_t(state.gamepad.thumb_ly), int16_t(state.gamepad.thumb_rx),
           int16_t(state.gamepad.thumb_ry)};
  }

  std::lock_guard lock(mutex_);
  uint32_t now = NowLocked();
  if (!recorded_any_ || pad != last_recorded_) {
    char line[96];
    std::snprintf(line, sizeof(line), "pad %04x %u %u %d %d %d %d", pad.buttons,
                  pad.left_trigger, pad.right_trigger, pad.thumb_lx, pad.thumb_ly, pad.thumb_rx,
                  pad.thumb_ry);
    WriteLocked(now, line);
    last_recorded_ = pad;
    recorded_any_ = true;
  }
  if (now - last_flush_ms_ >= 1000) {
    std::fflush(file_);
    last_flush_ms_ = now;
  }
}

void PgInputScript::ToggleMeasure() {
  std::lock_guard lock(mutex_);
  if (mode_ != Mode::kRecord || !file_) return;
  measuring_ = !measuring_;
  WriteLocked(NowLocked(), measuring_ ? "measure_start" : "measure_stop");
  std::fflush(file_);
  SetFrameMarker(measuring_ ? kMarkMeasureStart : kMarkMeasureStop,
                 measuring_ ? "measure_start (recorded)" : "measure_stop (recorded)");
  if (on_measure_) on_measure_(measuring_);
}

void PgInputScript::AdvanceLocked(uint32_t now) {
  while (next_event_ < events_.size() && events_[next_event_].ms <= now) {
    FireLocked(events_[next_event_++]);
  }
  if (!finished_ && next_event_ == events_.size()) {
    finished_ = true;
    // Release everything so the physical controller takes over.
    current_ = {};
    SetFrameMarker(kMarkNone, "replay finished; physical controller has control");
  }
}

void PgInputScript::FireLocked(const Event& event) {
  switch (event.kind) {
    case Kind::kPad:
      current_ = event.pad;
      break;
    case Kind::kMark:
      SetFrameMarker(kMarkOther, "mark " + event.text);
      break;
    case Kind::kMeasureStart:
      SetFrameMarker(kMarkMeasureStart, "measure_start");
      if (on_measure_) on_measure_(true);
      break;
    case Kind::kMeasureStop:
      SetFrameMarker(kMarkMeasureStop, "measure_stop");
      if (on_measure_) on_measure_(false);
      if (quit_on_measure_stop_ && request_quit_) request_quit_();
      break;
    case Kind::kQuit:
      SetFrameMarker(kMarkOther, "quit");
      if (request_quit_) request_quit_();
      break;
  }
}

void PgInputScript::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  rex::input::DeviceInfo info;
  info.id = kScriptDevice;
  info.name = mode_ == Mode::kRecord ? "Input recorder" : "Input replay";
  info.synthetic = true;
  out.push_back(info);
}

X_RESULT PgInputScript::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (id != kScriptDevice || t_sampling) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (mode_ == Mode::kRecord) {
    RecordSample();
    // Contribute nothing; the guest gets the physical devices unchanged.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(mutex_);
  AdvanceLocked(NowLocked());
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    // A changing packet number tells the guest the state may have changed.
    out_state->packet_number = uint32_t(next_event_);
    out_state->gamepad.buttons = current_.buttons;
    out_state->gamepad.left_trigger = current_.left_trigger;
    out_state->gamepad.right_trigger = current_.right_trigger;
    out_state->gamepad.thumb_lx = current_.thumb_lx;
    out_state->gamepad.thumb_ly = current_.thumb_ly;
    out_state->gamepad.thumb_rx = current_.thumb_rx;
    out_state->gamepad.thumb_ry = current_.thumb_ry;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT PgInputScript::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                              X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (id != kScriptDevice || mode_ == Mode::kRecord) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT PgInputScript::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  (void)vibration;
  if (id != kScriptDevice || mode_ == Mode::kRecord) return X_ERROR_DEVICE_NOT_CONNECTED;
  return X_ERROR_SUCCESS;
}

X_RESULT PgInputScript::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                           X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  if (id != kScriptDevice || t_sampling) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (mode_ == Mode::kRecord) {
    // Menus may read only keystrokes, so sample here too.
    RecordSample();
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!out_keystroke) return X_ERROR_BAD_ARGUMENTS;

  std::lock_guard lock(mutex_);
  uint32_t now = NowLocked();
  AdvanceLocked(now);

  uint64_t buttons = current_.buttons;
  buttons |= uint64_t(current_.left_trigger > kTriggerThreshold) << 16;
  buttons |= uint64_t(current_.right_trigger > kTriggerThreshold) << 17;
  int16_t thumb_x = current_.thumb_lx, thumb_y = current_.thumb_ly;
  for (unsigned i = 0; i <= 8; i += 8) {
    uint64_t u = thumb_y > kThumbThreshold, d = thumb_y < ~kThumbThreshold;
    uint64_t r = thumb_x > kThumbThreshold, l = thumb_x < ~kThumbThreshold;
    if (u && l) { u = l = 0; buttons |= uint64_t(1) << (22 + i); }
    if (u && r) { u = r = 0; buttons |= uint64_t(1) << (23 + i); }
    if (d && r) { d = r = 0; buttons |= uint64_t(1) << (24 + i); }
    if (d && l) { d = l = 0; buttons |= uint64_t(1) << (25 + i); }
    buttons |= u << (18 + i) | d << (19 + i) | r << (20 + i) | l << (21 + i);
    thumb_x = current_.thumb_rx;
    thumb_y = current_.thumb_ry;
  }

  auto emit = [&](uint8_t index, uint16_t key_flags) {
    out_keystroke->virtual_key = uint16_t(kVkLookup[index]);
    out_keystroke->unicode = 0;
    out_keystroke->flags = key_flags;
    out_keystroke->user_index = 0;  // InputSystem stamps the real user
    out_keystroke->hid_code = 0;
    return X_ERROR_SUCCESS;
  };

  if (repeat_ == Repeat::kWaiting && repeat_time_ + kRepeatDelayMs < now) {
    repeat_ = Repeat::kRepeating;
  }
  if (repeat_ == Repeat::kRepeating && repeat_time_ + kRepeatRateMs < now) {
    repeat_time_ = now;
    return emit(repeat_index_, rex::input::X_INPUT_KEYSTROKE_KEYDOWN | rex::input::X_INPUT_KEYSTROKE_REPEAT);
  }

  uint64_t changed = buttons ^ keystroke_buttons_;
  // Releases first, as the SDL driver does, then presses.
  for (int pass = 0; pass < 2 && changed; ++pass) {
    for (uint8_t i = 0; i < uint8_t(kVkLookup.size()); ++i) {
      uint64_t bit = uint64_t(1) << i;
      if (!(changed & bit) || kVkLookup[i] == VirtualKey::kNone) continue;
      bool pressed = buttons & bit;
      if (pass == 0 && !pressed) {
        keystroke_buttons_ &= ~bit;
        repeat_ = Repeat::kIdle;
        return emit(i, rex::input::X_INPUT_KEYSTROKE_KEYUP);
      }
      if (pass == 1 && pressed) {
        keystroke_buttons_ |= bit;
        repeat_ = Repeat::kWaiting;
        repeat_index_ = i;
        repeat_time_ = now;
        return emit(i, rex::input::X_INPUT_KEYSTROKE_KEYDOWN);
      }
    }
  }
  return X_ERROR_EMPTY;
}
