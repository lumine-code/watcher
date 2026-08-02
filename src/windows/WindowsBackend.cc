#include <string>
#include <stack>
#include "../DirTree.hh"
#include "../shared/BruteForceBackend.hh"
#include "./WindowsBackend.hh"
#include "./win_utils.hh"

#define DEFAULT_BUF_SIZE 1024 * 1024
#define NETWORK_BUF_SIZE 64 * 1024
#define CONVERT_TIME(ft) ULARGE_INTEGER{ft.dwLowDateTime, ft.dwHighDateTime}.QuadPart

void BruteForceBackend::readTree(WatcherRef watcher, std::shared_ptr<DirTree> tree) {
  std::stack<std::string> directories;

  directories.push(watcher->mDir);

  while (!directories.empty()) {
    HANDLE hFind = INVALID_HANDLE_VALUE;

    std::string path = directories.top();
    std::string spec = path + "\\*";
    directories.pop();

    WIN32_FIND_DATA ffd;
    hFind = FindFirstFile(spec.c_str(), &ffd);

    if (hFind == INVALID_HANDLE_VALUE)  {
      if (path == watcher->mDir) {
        FindClose(hFind);
        throw WatcherError("Error opening directory", watcher);
      }

      tree->remove(path);
      continue;
    }

    do {
      if (strcmp(ffd.cFileName, ".") != 0 && strcmp(ffd.cFileName, "..") != 0) {
        std::string fullPath = path + "\\" + ffd.cFileName;
        if (watcher->isIgnored(fullPath)) {
          continue;
        }

        tree->add(fullPath, CONVERT_TIME(ffd.ftLastWriteTime), ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          directories.push(fullPath);
        }
      }
    } while (FindNextFile(hFind, &ffd) != 0);

    FindClose(hFind);
  }
}

WindowsBackend::WindowsBackend()
  : mRunFlag(std::make_shared<std::atomic<bool>>(true)),
    mArmedCount(std::make_shared<std::atomic<int>>(0)) {}

void WindowsBackend::start() {
  // Local copies: when the last watcher errors out, this backend can be
  // destroyed from this very thread (Backend::handleWatcherError →
  // removeShared), so the loop must not read members after that.
  auto runFlag = mRunFlag;
  auto armedCount = mArmedCount;

  notifyStarted();

  while (runFlag->load()) {
    SleepEx(INFINITE, true);
  }

  // Drain cancelled I/O completions so every armed Subscription is released
  // before the destructor joins this thread. Bounded: a completion that never
  // arrives only leaks that subscription — its buffers stay valid for the
  // kernel, which is safe; freeing them early is what is not.
  int spins = 0;
  while (armedCount->load() > 0 && spins++ < 400) {
    SleepEx(5, true);
  }
}

WindowsBackend::~WindowsBackend() {
  // Mark as stopped, and queue a noop function in the thread to break the loop
  mRunFlag->store(false);
  QueueUserAPC([](__in ULONG_PTR) {}, mThread.native_handle(), (ULONG_PTR)this);
}

// Lifecycle contract: everything below (run/stop/poll/processEvents and the
// I/O completion routine) executes on the backend thread only — it is reached
// exclusively through APCs queued to it. The kernel holds raw pointers into
// this object (mOverlapped, mWriteBuffer) while a ReadDirectoryChangesW is
// pending, and the completion routine receives a raw `this` via
// `overlapped->hEvent`, so the object must stay alive from arm to completion.
// `mSelfRef` holds it across exactly that window; only the completion routine
// releases it. Destruction therefore always happens on the backend thread
// with no I/O in flight. Freeing on the unsubscribing thread instead (the old
// behavior) let a pending completion run over freed memory — and, because
// CancelIo is asynchronous, let the kernel write notify data into the freed
// buffer.
class Subscription: public WatcherState {
public:
  Subscription(WindowsBackend *backend, WatcherRef watcher, std::shared_ptr<DirTree> tree) {
    mRunning = true;
    mIoPending = false;
    mBackend = backend;
    mWatcher = watcher;
    mTree = tree;
    mArmedCount = backend->armedCount();
    ZeroMemory(&mOverlapped, sizeof(OVERLAPPED));
    mOverlapped.hEvent = this;
    mReadBuffer.resize(DEFAULT_BUF_SIZE);
    mWriteBuffer.resize(DEFAULT_BUF_SIZE);

    mDirectoryHandle = CreateFileW(
      utf8ToUtf16(watcher->mDir).data(),
      FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
      NULL
    );

    if (mDirectoryHandle == INVALID_HANDLE_VALUE) {
      throw WatcherError("Invalid handle", mWatcher);
    }

    // Ensure that the path is a directory
    BY_HANDLE_FILE_INFORMATION info;
    bool success = GetFileInformationByHandle(
      mDirectoryHandle,
      &info
    );

    if (!success) {
      throw WatcherError("Could not get file information", mWatcher);
    }

    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      throw WatcherError("Not a directory", mWatcher);
    }
  }

  virtual ~Subscription() {
    if (mDirectoryHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(mDirectoryHandle);
    }
  }

  void run(std::shared_ptr<Subscription>& self) {
    try {
      poll(self);
    } catch (WatcherError &err) {
      mBackend->handleWatcherError(err);
    }
  }

  void stop() {
    if (!mRunning) {
      return;
    }
    mRunning = false;
    if (mIoPending) {
      // The ERROR_OPERATION_ABORTED completion releases mSelfRef.
      CancelIo(mDirectoryHandle);
    } else {
      mSelfRef.reset();
    }
  }

  void poll(std::shared_ptr<Subscription>& self) {
    if (!mRunning) {
      return;
    }

    // Asynchronously wait for changes.
    int success = ReadDirectoryChangesW(
      mDirectoryHandle,
      mWriteBuffer.data(),
      static_cast<DWORD>(mWriteBuffer.size()),
      TRUE, // recursive
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES
        | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
      NULL,
      &mOverlapped,
      [](DWORD errorCode, DWORD numBytes, LPOVERLAPPED overlapped) {
        auto sub = reinterpret_cast<Subscription *>(overlapped->hEvent);
        // Take over the arm-window reference. If the subscription does not
        // re-arm below, this scope's copy is the last one and the object is
        // destroyed here, on the backend thread, with no I/O pending.
        std::shared_ptr<Subscription> self = std::move(sub->mSelfRef);
        sub->mIoPending = false;
        sub->mArmedCount->fetch_sub(1);
        if (errorCode == ERROR_OPERATION_ABORTED || !sub->mRunning) {
          return;
        }
        try {
          sub->processEvents(errorCode, self);
        } catch (WatcherError &err) {
          sub->mBackend->handleWatcherError(err);
        }
      }
    );

    if (!success) {
      throw WatcherError("Failed to read changes", mWatcher);
    }

    mIoPending = true;
    mArmedCount->fetch_add(1);
    mSelfRef = self;
  }

  void processEvents(DWORD errorCode, std::shared_ptr<Subscription>& self) {
    switch (errorCode) {
      case ERROR_OPERATION_ABORTED:
        return;
      case ERROR_INVALID_PARAMETER:
        // resize buffers to network size (64kb), and try again
        mReadBuffer.resize(NETWORK_BUF_SIZE);
        mWriteBuffer.resize(NETWORK_BUF_SIZE);
        poll(self);
        return;
      case ERROR_NOTIFY_ENUM_DIR:
        throw WatcherError("Buffer overflow. Some events may have been lost.", mWatcher);
      case ERROR_ACCESS_DENIED: {
        // This can happen if the watched directory is deleted. Check if that is the case,
        // and if so emit a delete event. Otherwise, fall through to default error case.
        DWORD attrs = GetFileAttributesW(utf8ToUtf16(mWatcher->mDir).data());
        bool isDir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
        if (!isDir) {
          mWatcher->mEvents.remove(mWatcher->mDir);
          mTree->remove(mWatcher->mDir);
          mWatcher->notify();
          stop();
          return;
        }
      }
      default:
        if (errorCode != ERROR_SUCCESS) {
          throw WatcherError("Unknown error", mWatcher);
        }
    }

    // Swap read and write buffers, and poll again
    std::swap(mWriteBuffer, mReadBuffer);
    poll(self);

    // Read change events
    BYTE *base = mReadBuffer.data();
    while (true) {
      PFILE_NOTIFY_INFORMATION info = (PFILE_NOTIFY_INFORMATION)base;
      processEvent(info);

      if (info->NextEntryOffset == 0) {
        break;
      }

      base += info->NextEntryOffset;
    }

    mWatcher->notify();
  }

  void processEvent(PFILE_NOTIFY_INFORMATION info) {
    std::string path = mWatcher->mDir + "\\" + utf16ToUtf8(info->FileName, info->FileNameLength / sizeof(WCHAR));
    if (mWatcher->isIgnored(path)) {
      return;
    }

    switch (info->Action) {
      case FILE_ACTION_ADDED:
      case FILE_ACTION_RENAMED_NEW_NAME: {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(utf8ToUtf16(path).data(), GetFileExInfoStandard, &data)) {
          mWatcher->mEvents.create(path);
          mTree->add(path, CONVERT_TIME(data.ftLastWriteTime), data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        }
        break;
      }
      case FILE_ACTION_MODIFIED: {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(utf8ToUtf16(path).data(), GetFileExInfoStandard, &data)) {
          mTree->update(path, CONVERT_TIME(data.ftLastWriteTime));
          if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            mWatcher->mEvents.update(path);
          }
        }
        break;
      }
      case FILE_ACTION_REMOVED:
      case FILE_ACTION_RENAMED_OLD_NAME:
        mWatcher->mEvents.remove(path);
        mTree->remove(path);
        break;
    }
  }

private:
  WindowsBackend *mBackend;
  std::shared_ptr<Watcher> mWatcher;
  std::shared_ptr<DirTree> mTree;
  bool mRunning;
  bool mIoPending;
  std::shared_ptr<std::atomic<int>> mArmedCount;
  std::shared_ptr<Subscription> mSelfRef;
  HANDLE mDirectoryHandle;
  std::vector<BYTE> mReadBuffer;
  std::vector<BYTE> mWriteBuffer;
  OVERLAPPED mOverlapped;
};

// This function is called by Backend::watch which takes a lock on mMutex
void WindowsBackend::subscribe(WatcherRef watcher) {
  // Create a subscription for this watcher
  auto sub = std::make_shared<Subscription>(this, watcher, getTree(watcher, false));
  watcher->state = sub;

  // Hand the backend thread its own reference. The subscription can be
  // unsubscribed (dropping the registry reference above) before the APC runs,
  // so the APC owns a boxed reference rather than a raw pointer.
  auto boxed = new std::shared_ptr<Subscription>(sub);
  bool success = QueueUserAPC([](__in ULONG_PTR ptr) {
    auto box = reinterpret_cast<std::shared_ptr<Subscription> *>(ptr);
    std::shared_ptr<Subscription> self = std::move(*box);
    delete box;
    self->run(self);
  }, mThread.native_handle(), (ULONG_PTR)boxed);

  if (!success) {
    delete boxed;
    throw std::runtime_error("Unable to queue APC");
  }
}

// This function is called by Backend::unwatch which takes a lock on mMutex
void WindowsBackend::unsubscribe(WatcherRef watcher) {
  auto sub = std::static_pointer_cast<Subscription>(watcher->state);
  watcher->state = nullptr;
  if (!sub) {
    return;
  }

  // Teardown must happen on the backend thread — see the lifecycle contract
  // above the Subscription class.
  auto boxed = new std::shared_ptr<Subscription>(std::move(sub));
  bool success = QueueUserAPC([](__in ULONG_PTR ptr) {
    auto box = reinterpret_cast<std::shared_ptr<Subscription> *>(ptr);
    std::shared_ptr<Subscription> self = std::move(*box);
    delete box;
    self->stop();
  }, mThread.native_handle(), (ULONG_PTR)boxed);

  if (!success) {
    // The thread is gone (backend teardown). If the subscription still has
    // I/O in flight, its self-reference keeps the kernel-visible memory
    // alive; dropping our reference here at worst leaks it, never frees it
    // early.
    delete boxed;
  }
}
