#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>

// Records the controller state the guest sees for user 0, or replays such a
// recording as a synthetic device merged into user 0. Timestamps are guest
// milliseconds since the guest first polled input, so host startup time does
// not shift the script. The text format is documented in docs/DEVELOPMENT.md.
class PgInputScript final : public rex::input::InputDriver {
 public:
  enum class Mode { kRecord, kReplay };

  // Returns nullptr (after logging why) if the file cannot be opened or parsed.
  static std::unique_ptr<PgInputScript> Create(rex::input::InputSystem* system, Mode mode,
                                               const std::string& path,
                                               std::function<void()> request_quit,
                                               bool quit_on_measure_stop);
  ~PgInputScript() override;

  rex::X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id,
                               rex::input::X_INPUT_STATE* out_state) override;
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                      rex::input::X_INPUT_CAPABILITIES* out_caps) override;
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id,
                                   rex::input::X_INPUT_VIBRATION* vibration) override;
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                                   rex::input::X_INPUT_KEYSTROKE* out_keystroke) override;

  // Recording only: alternately writes measure_start and measure_stop.
  void ToggleMeasure();

  // Called with true at measure_start and false at measure_stop.
  void SetMeasureCallback(std::function<void(bool)> callback) {
    std::lock_guard lock(mutex_);
    on_measure_ = std::move(callback);
  }

 private:
  struct Pad {
    uint16_t buttons = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
    int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
    bool operator==(const Pad&) const = default;
  };
  enum class Kind { kPad, kMark, kMeasureStart, kMeasureStop, kQuit };
  struct Event {
    uint32_t ms = 0;
    Kind kind = Kind::kPad;
    Pad pad;
    std::string text;
  };

  PgInputScript(rex::input::InputSystem* system, Mode mode, std::function<void()> request_quit,
                bool quit_on_measure_stop);

  bool LoadLocked(const std::string& path);
  uint32_t NowLocked();
  void RecordSample();
  void AdvanceLocked(uint32_t now);
  void FireLocked(const Event& event);
  void WriteLocked(uint32_t ms, const char* line);

  rex::input::InputSystem* system_;
  Mode mode_;
  std::function<void()> request_quit_;
  bool quit_on_measure_stop_;
  std::function<void(bool)> on_measure_;

  std::mutex mutex_;
  bool started_ = false;
  uint32_t start_ms_ = 0;

  // Recording
  std::FILE* file_ = nullptr;
  Pad last_recorded_;
  bool recorded_any_ = false;
  bool measuring_ = false;
  uint32_t last_flush_ms_ = 0;

  // Replay
  std::vector<Event> events_;
  size_t next_event_ = 0;
  bool finished_ = false;
  Pad current_;
  uint64_t keystroke_buttons_ = 0;
  enum class Repeat { kIdle, kWaiting, kRepeating } repeat_ = Repeat::kIdle;
  uint8_t repeat_index_ = 0;
  uint32_t repeat_time_ = 0;
};
