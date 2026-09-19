#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>

struct OsdActivityConfig;

enum class TransientActivityKind : std::uint8_t {
  VolumeOutput,
  VolumeInput,
  Brightness,
  Notification,
};

enum class TransientActivityPresentation : std::uint8_t { Standalone, Section, Attached };
enum class TransientActivityPlacement : std::uint8_t { Auto, Before, After, Below, Above, Left, Right };
enum class TransientActivityMotion : std::uint8_t { Inherit, Off };
enum class TransientActivityMaterial : std::uint8_t { Inherit, Surface, Transparent };

inline constexpr int kTransientActivityPriorityNotificationLow = 30;
inline constexpr int kTransientActivityPriorityNotificationNormal = 40;
inline constexpr int kTransientActivityPriorityDevice = 60;
inline constexpr int kTransientActivityPriorityNotificationCritical = 100;

struct TransientActivityRoute {
  TransientActivityPresentation presentation = TransientActivityPresentation::Standalone;
  std::string bar;
  std::string section;
  std::string output = "focused";
  TransientActivityPlacement placement = TransientActivityPlacement::Auto;
  TransientActivityMotion motion = TransientActivityMotion::Inherit;
  TransientActivityMaterial material = TransientActivityMaterial::Inherit;

  bool operator==(const TransientActivityRoute&) const = default;
};

struct TransientActivityViewModel {
  std::uint64_t serial = 0;
  std::string sourceKey;
  TransientActivityKind kind = TransientActivityKind::VolumeOutput;
  int priority = 0;
  std::string icon;
  std::string title;
  std::string body;
  std::string value;
  float progress = 0.0F;
  bool showProgress = true;
  bool overLimit = false;
  bool inactive = false;
  std::optional<std::uint32_t> notificationId;
  std::string openContext;
  std::function<void(float)> setProgress;
  std::function<void()> activate;
  std::function<void()> dismiss;
  std::function<void(bool)> hoverChanged;
  std::chrono::milliseconds timeout{1800};
  std::chrono::steady_clock::time_point expiresAt{};
};

[[nodiscard]] TransientActivityRoute
resolveTransientActivityRoute(const OsdActivityConfig& config, TransientActivityKind kind);
[[nodiscard]] std::optional<std::string> validateTransientActivityConfig(const OsdActivityConfig& config);

// Owns ordering and lifetime only. Device state and notification history remain in their existing services.
class TransientActivityService {
public:
  using Clock = std::chrono::steady_clock;
  using CanPresent = std::function<bool(const TransientActivityRoute&)>;
  using Present = std::function<bool(const TransientActivityViewModel&, const TransientActivityRoute&)>;
  using Withdraw = std::function<void(std::uint64_t)>;
  using Fallback = std::function<void(const TransientActivityViewModel&)>;

  struct Presenter {
    CanPresent canPresent;
    Present present;
    Withdraw withdraw;
    Withdraw withdrawNow;
  };

  void setPresenter(Presenter presenter);
  void setFallback(Fallback fallback);

  // True means an embedded/attached presenter accepted ownership. False tells the producer to use
  // its existing standalone path. A queued item is accepted only after canPresent acknowledges its route.
  [[nodiscard]] bool publish(
      TransientActivityViewModel model, const TransientActivityRoute& route,
      Clock::time_point now = Clock::now()
  );
  void closeNotification(std::uint32_t notificationId, Clock::time_point now = Clock::now());
  void expire(Clock::time_point now = Clock::now());
  void reloadConfig(const OsdActivityConfig& config, Clock::time_point now = Clock::now());
  void refreshPresentation(Clock::time_point now = Clock::now());
  void setHovered(std::uint64_t serial, bool hovered, Clock::time_point now = Clock::now());
  void clear();

  [[nodiscard]] const TransientActivityViewModel* active() const noexcept;
  [[nodiscard]] std::size_t pendingCount() const noexcept { return m_pending.size(); }

private:
  struct Entry {
    TransientActivityViewModel model;
    TransientActivityRoute route;
    std::optional<std::chrono::milliseconds> pausedRemaining;
  };

  [[nodiscard]] bool routeAvailable(const TransientActivityRoute& route) const;
  [[nodiscard]] bool present(const Entry& entry);
  void releaseHover(Entry& entry, Clock::time_point now);
  void rejectPresentation(Entry previous, Clock::time_point now, bool fallbackPrevious);
  void armTimer(Clock::time_point now);
  void fallbackOnce(Entry entry);
  void promote(Clock::time_point now);
  void pruneExpired(Clock::time_point now);

  Presenter m_presenter;
  Fallback m_fallback;
  std::optional<Entry> m_active;
  std::deque<Entry> m_pending;
  std::uint64_t m_nextSerial = 1;
  std::uint64_t m_generation = 0;
  Timer m_timer;
};
