#ifndef WINDOWS_H
#define WINDOWS_H

#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <memory>
#include "../shared/BruteForceBackend.hh"

class WindowsBackend : public BruteForceBackend {
public:
  WindowsBackend();
  void start() override;
  ~WindowsBackend();
  void subscribe(WatcherRef watcher) override;
  void unsubscribe(WatcherRef watcher) override;
  std::shared_ptr<std::atomic<int>> armedCount() { return mArmedCount; }
private:
  // Heap-shared with the backend thread's loop: when the last watcher errors
  // out, the backend can be destroyed from that very thread
  // (Backend::handleWatcherError → removeShared), and the loop must not read
  // freed members afterwards.
  std::shared_ptr<std::atomic<bool>> mRunFlag;
  // Number of subscriptions with a ReadDirectoryChangesW in flight. Shared
  // with each Subscription so completions can decrement it even if the
  // backend object is already gone.
  std::shared_ptr<std::atomic<int>> mArmedCount;
};

#endif
