#pragma once

#include "app/poll_source.h"
#include "system/desktop_entry.h"

class DesktopEntryPollSource final : public PollSource {
public:
  // Construct the catalog and queue its initial scan. The first snapshot may
  // be empty; completed scans are adopted here without filesystem I/O.
  DesktopEntryPollSource() { desktopEntries(); }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    if (desktopEntryWatchFd() < 0 || (fds[startIdx].revents & POLLIN) != 0) {
      checkDesktopEntryReload();
    }
  }

  [[nodiscard]] int pollTimeoutMs() const override { return desktopEntryWatchFd() < 0 ? 250 : -1; }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    if (desktopEntryWatchFd() >= 0) {
      fds.push_back({.fd = desktopEntryWatchFd(), .events = POLLIN, .revents = 0});
    }
  }
};
