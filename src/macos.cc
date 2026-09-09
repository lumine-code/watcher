#include <CoreServices/CoreServices.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/event.h>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include "engine.hh"
#include <set>

namespace lumine {
class MacOS : public Platform {
  struct GuardNode {
    std::string path;
    int fd = -1;
    uintptr_t token = 0;
    uint32_t mask = 0;
    dev_t device = 0;
    ino_t inode = 0;
    std::map<Id, bool> owners;
    ~GuardNode() { if (fd >= 0) close(fd); }
  };
  struct Subscription {
    MacOS* owner;
    Source source;
    fs::path canonical;
    std::unordered_map<std::string, struct stat> seen;
    FSEventStreamRef stream = nullptr;
    std::vector<std::shared_ptr<GuardNode>> guardNodes;
  };
  CFRunLoopRef loop;
  CFRunLoopSourceRef wakeSource;
  int guardQueue = -1;
  CFFileDescriptorRef guardDescriptor = nullptr;
  CFRunLoopSourceRef guardRunSource = nullptr;
  uintptr_t nextGuardToken = 0;
  std::map<std::string, std::shared_ptr<GuardNode>> guardPaths;
  std::map<int, std::shared_ptr<GuardNode>> guardFds;
  std::map<Id, std::unique_ptr<Subscription>> sources;
  std::set<Id> pendingClose;
  const bool guardDiagnostics = std::getenv("LUMINE_GUARD_DIAGNOSTICS") != nullptr;
  size_t streamAdds = 0, guardAdds = 0, guardCallbacks = 0, guardEvents = 0;
  size_t lastGuardCallbackStream = 0;
public:
  explicit MacOS(Engine& engine) : Platform(engine), loop(CFRunLoopGetCurrent()) {
    CFRetain(loop);
    CFRunLoopSourceContext context{};
    context.perform = [](void*) {};
    wakeSource = CFRunLoopSourceCreate(nullptr, 0, &context);
    CFRunLoopAddSource(loop, wakeSource, kCFRunLoopDefaultMode);
  }
  ~MacOS() override {
    clearGuardQueue();
    CFRunLoopSourceInvalidate(wakeSource); CFRelease(wakeSource); CFRelease(loop);
  }
  void add(const Source& source) override {
    auto canonical = fs::canonical(fs::u8path(source.path));
    if (!fs::is_directory(canonical)) throw fs::filesystem_error("Expected a directory", canonical, std::make_error_code(std::errc::not_a_directory));
    auto sub = std::make_unique<Subscription>();
    sub->owner = this; sub->source = source; sub->canonical = canonical;
    if (source.guard) {
      ++guardAdds;
      sources.emplace(source.id, std::move(sub));
      addGuard(*sources.at(source.id));
      engine.ready(source.id);
      return;
    }
    ++streamAdds;
    auto root = CFStringCreateWithCString(nullptr, utf8(canonical).c_str(), kCFStringEncodingUTF8);
    const void* values[]{root};
    auto paths = CFArrayCreate(nullptr, values, 1, &kCFTypeArrayCallBacks);
    FSEventStreamContext context{0, sub.get(), nullptr, nullptr, nullptr};
    sub->stream = FSEventStreamCreate(nullptr, [](ConstFSEventStreamRef, void* context, size_t count, void* paths, const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
      auto sub = static_cast<Subscription*>(context);
      try { sub->owner->process(*sub, count, static_cast<char**>(paths), flags); }
      catch (const std::exception& error) {
        sub->owner->engine.error(sub->source, error.what(), "ERR_WATCH_READ");
        sub->owner->pendingClose.insert(sub->source.id);
      }
    }, &context, paths, kFSEventStreamEventIdSinceNow, 0.02, kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths); CFRelease(root);
    if (!sub->stream) throw std::runtime_error("Cannot create FSEvents stream");
    FSEventStreamScheduleWithRunLoop(sub->stream, loop, kCFRunLoopDefaultMode);
    if (!FSEventStreamStart(sub->stream)) {
      FSEventStreamInvalidate(sub->stream); FSEventStreamRelease(sub->stream);
      throw std::runtime_error("Cannot start FSEvents stream");
    }
    sources.emplace(source.id, std::move(sub));
    if (!source.recursive) {
      auto& state = *sources.at(source.id);
      // Shallow sources back fixed files. Seed immediate metadata while the
      // stream is armed so delayed pre-subscription ItemModified flags can be
      // distinguished from a later write restoring the same size and mtime.
      // Recursive project streams never crawl their tree for this metadata.
      for (const auto& entry : fs::directory_iterator(canonical)) {
        struct stat status{};
        if (lstat(entry.path().c_str(), &status) == 0) {
          state.seen.emplace(utf8(fs::u8path(source.path) / entry.path().filename()), status);
        } else if (errno != ENOENT) throw fs::filesystem_error("Cannot inspect initial directory entry", entry.path(), std::error_code(errno, std::generic_category()));
      }
    }
    engine.ready(source.id);
  }
  void remove(Id id) override {
    auto found = sources.find(id);
    if (found != sources.end()) {
      // Source storage outlives its stream. Stop, unschedule and invalidate on
      // the runloop owner before releasing the raw callback context.
      auto stream = found->second->stream;
      if (stream) {
        FSEventStreamStop(stream);
        FSEventStreamUnscheduleFromRunLoop(stream, loop, kCFRunLoopDefaultMode);
        FSEventStreamInvalidate(stream); FSEventStreamRelease(stream);
      }
      for (const auto& node : found->second->guardNodes) releaseGuardNode(node, id);
      sources.erase(found);
    }
    pendingClose.erase(id);
    engine.closed(id);
  }
  void removeAll() override {
    if (guardDiagnostics && guardQueue >= 0) {
      std::fprintf(stderr, "GUARD_DIAGNOSTICS queue=%d valid=%d streams=%zu guards=%zu callbacks=%zu events=%zu lastCallbackStream=%zu liveFds=%zu liveSources=%zu\n", guardQueue, guardDescriptor ? CFFileDescriptorIsValid(guardDescriptor) : 0, streamAdds, guardAdds, guardCallbacks, guardEvents, lastGuardCallbackStream, guardFds.size(), sources.size());
      struct kevent events[64];
      const struct timespec immediate{};
      int count = kevent(guardQueue, nullptr, 0, events, 64, &immediate);
      std::fprintf(stderr, "GUARD_DIAGNOSTICS pending=%d errno=%d\n", count, errno);
      for (int i = 0; i < count; ++i) {
        auto found = guardFds.find(static_cast<int>(events[i].ident));
        bool live = found != guardFds.end() && reinterpret_cast<uintptr_t>(events[i].udata) == found->second->token;
        std::fprintf(stderr, "GUARD_DIAGNOSTICS fd=%zu filter=%d flags=%u fflags=%u token=%zu live=%d\n", events[i].ident, events[i].filter, events[i].flags, events[i].fflags, reinterpret_cast<uintptr_t>(events[i].udata), live);
      }
    }
    while (!sources.empty()) remove(sources.begin()->first);
  }
  bool empty() const override { return sources.empty(); }
  void pump() override {
    // The wake source keeps an otherwise empty runloop asleep until a command
    // wakes it; there is no polling interval or permanent idle CPU wakeup.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 3600, true);
    auto closing = pendingClose;
    for (auto id : closing) remove(id);
  }
  void wake() override {
    CFRunLoopSourceSignal(wakeSource);
    CFRunLoopWakeUp(loop);
  }
private:
  void clearGuardQueue() {
    if (guardRunSource) {
      CFRunLoopRemoveSource(loop, guardRunSource, kCFRunLoopDefaultMode);
      CFRelease(guardRunSource);
      guardRunSource = nullptr;
    }
    if (guardDescriptor) {
      CFFileDescriptorInvalidate(guardDescriptor);
      CFRelease(guardDescriptor);
      guardDescriptor = nullptr;
    }
    if (guardQueue >= 0) { close(guardQueue); guardQueue = -1; }
  }

  void ensureGuardQueue() {
    if (guardQueue >= 0) return;
    guardQueue = kqueue();
    if (guardQueue < 0) throw std::system_error(errno, std::generic_category(), "Cannot open vnode guard queue");
    if (fcntl(guardQueue, F_SETFD, FD_CLOEXEC) < 0) {
      int error = errno;
      clearGuardQueue();
      throw std::system_error(error, std::generic_category(), "Cannot configure vnode guard queue");
    }
    CFFileDescriptorContext context{0, this, nullptr, nullptr, nullptr};
    guardDescriptor = CFFileDescriptorCreate(nullptr, guardQueue, false, [](CFFileDescriptorRef descriptor, CFOptionFlags, void* context) {
      auto self = static_cast<MacOS*>(context);
      ++self->guardCallbacks;
      self->lastGuardCallbackStream = self->streamAdds;
      try { self->drainGuards(); }
      catch (const std::exception& error) {
        for (const auto& pair : self->sources) {
          if (!pair.second->source.guard) continue;
          self->engine.error(pair.second->source, error.what(), "ERR_WATCH_GUARD");
          self->pendingClose.insert(pair.first);
        }
      }
      // CFFileDescriptor disables callbacks when firing; re-enable after the
      // bounded nonblocking kevent drain, always on the engine's owner thread.
      CFFileDescriptorEnableCallBacks(descriptor, kCFFileDescriptorReadCallBack);
    }, &context);
    if (!guardDescriptor) { clearGuardQueue(); throw std::bad_alloc(); }
    guardRunSource = CFFileDescriptorCreateRunLoopSource(nullptr, guardDescriptor, 0);
    if (!guardRunSource) { clearGuardQueue(); throw std::bad_alloc(); }
    CFRunLoopAddSource(loop, guardRunSource, kCFRunLoopDefaultMode);
    CFFileDescriptorEnableCallBacks(guardDescriptor, kCFFileDescriptorReadCallBack);
  }

  void configureGuardNode(const std::shared_ptr<GuardNode>& node) {
    uint32_t mask = NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE;
    for (const auto& owner : node->owners) if (owner.second) { mask |= NOTE_WRITE | NOTE_LINK; break; }
    if ((node->mask & mask) == mask) return;
    mask |= node->mask;
    struct kevent change;
    EV_SET(&change, node->fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, mask, 0, reinterpret_cast<void*>(node->token));
    if (kevent(guardQueue, &change, 1, nullptr, 0, nullptr) < 0) throw fs::filesystem_error("Cannot arm directory guard", fs::u8path(node->path), std::error_code(errno, std::generic_category()));
    node->mask = mask;
  }

  void addGuard(Subscription& sub) {
    ensureGuardQueue();
    // NOTE_WRITE on a directory is immediate membership activity, not writes
    // to descendant contents. Ancestors only need relocation/unmount signals;
    // shared vnode descriptors keep a guard for '/' cheap and non-recursive.
    std::vector<fs::path> paths;
    auto current = sub.canonical;
    while (true) {
      paths.push_back(current);
      auto parent = current.parent_path();
      if (parent == current) break;
      current = parent;
    }
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
      auto key = utf8(*it);
      struct stat status{};
      if (stat(it->c_str(), &status) < 0) throw fs::filesystem_error("Cannot inspect guard directory", *it, std::error_code(errno, std::generic_category()));
      std::shared_ptr<GuardNode> node;
      auto found = guardPaths.find(key);
      if (found != guardPaths.end() && found->second->device == status.st_dev && found->second->inode == status.st_ino) node = found->second;
      if (!node) {
        node = std::make_shared<GuardNode>();
        node->path = key;
        node->fd = open(it->c_str(), O_EVTONLY | O_DIRECTORY | O_CLOEXEC);
        if (node->fd < 0) throw fs::filesystem_error("Cannot open guard directory", *it, std::error_code(errno, std::generic_category()));
        if (fstat(node->fd, &status) < 0) throw fs::filesystem_error("Cannot inspect guard descriptor", *it, std::error_code(errno, std::generic_category()));
        node->device = status.st_dev; node->inode = status.st_ino;
        node->token = ++nextGuardToken;
        guardPaths[key] = node;
        guardFds[node->fd] = node;
      }
      sub.guardNodes.push_back(node);
      node->owners[sub.source.id] = *it == sub.canonical;
      configureGuardNode(node);
    }
  }

  void releaseGuardNode(const std::shared_ptr<GuardNode>& node, Id id) {
    node->owners.erase(id);
    if (!node->owners.empty()) {
      // Keep existing registration intact for other owners. Any membership
      // hints left in its mask are ignored when no remaining owner needs them.
      return;
    }
    struct kevent change;
    EV_SET(&change, node->fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
    kevent(guardQueue, &change, 1, nullptr, 0, nullptr);
    guardFds.erase(node->fd);
    auto found = guardPaths.find(node->path);
    if (found != guardPaths.end() && found->second == node) guardPaths.erase(found);
    // Closing the last fd removes any queued vnode event. Event tokens also
    // reject stale deliveries if the OS later reuses the descriptor number.
    close(node->fd);
    node->fd = -1;
  }

  void drainGuards() {
    struct kevent events[64];
    const struct timespec immediate{};
    for (unsigned batch = 0; batch < 16; ++batch) {
      int count = kevent(guardQueue, nullptr, 0, events, 64, &immediate);
      if (count < 0) { if (errno == EINTR) continue; throw std::system_error(errno, std::generic_category(), "Cannot read vnode guard queue"); }
      if (!count) return;
      guardEvents += count;
      for (int i = 0; i < count; ++i) {
        auto found = guardFds.find(static_cast<int>(events[i].ident));
        if (found == guardFds.end() || reinterpret_cast<uintptr_t>(events[i].udata) != found->second->token) continue;
        auto node = found->second;
        for (const auto& owner : node->owners) {
          if (events[i].flags & EV_ERROR) {
            engine.error(sources.at(owner.first)->source, std::system_category().message(static_cast<int>(events[i].data)), "ERR_WATCH_GUARD");
            pendingClose.insert(owner.first);
          } else if (events[i].fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) {
            engine.invalidate(owner.first, "root-changed");
            pendingClose.insert(owner.first);
          } else if (owner.second && (events[i].fflags & (NOTE_WRITE | NOTE_LINK))) engine.emit({"guard", owner.first});
        }
      }
    }
  }

  void process(Subscription& sub, size_t count, char** paths, const FSEventStreamEventFlags flags[]) {
    std::vector<Event> events;
    for (size_t i = 0; i < count; ++i) {
      // Loss/root flags precede shallow filtering: dropped events may name '/'.
      if (flags[i] & (kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagUnmount)) {
        engine.invalidate(sub.source.id, "root-changed"); pendingClose.insert(sub.source.id); return;
      }
      if (flags[i] & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagEventIdsWrapped)) {
        engine.invalidate(sub.source.id, "native-overflow"); continue;
      }
      auto path = fs::u8path(paths[i]);
      auto relative = path.lexically_relative(sub.canonical);
      if (relative.empty() || *relative.begin() == "..") continue;
      if (!sub.source.recursive && path != sub.canonical && path.parent_path() != sub.canonical) continue;
      auto lexical = utf8((fs::u8path(sub.source.path) / relative).lexically_normal());
      bool created = flags[i] & kFSEventStreamEventFlagItemCreated;
      bool removed = flags[i] & kFSEventStreamEventFlagItemRemoved;
      bool renamed = flags[i] & kFSEventStreamEventFlagItemRenamed;
      std::string action = "updated";
      if ((created && removed) || renamed) {
        std::error_code error;
        auto status = fs::symlink_status(path, error);
        bool exists = fs::exists(status);
        // On case-insensitive volumes the old spelling still resolves after a
        // case-only rename. F_GETPATH returns the actual spelling of the entry.
        if (exists && renamed) {
          int fd = open(path.c_str(), O_EVTONLY | O_SYMLINK);
          if (fd >= 0) {
            char actual[PATH_MAX];
            if (fcntl(fd, F_GETPATH, actual) == 0) exists = path == fs::path(actual);
            close(fd);
          }
        }
        action = !exists ? "deleted" : sub.seen.count(lexical) ? "updated" : "created";
      } else if (created) action = sub.seen.count(lexical) ? "updated" : "created";
      else if (removed) action = "deleted";
      struct stat current{};
      bool inspected = lstat(path.c_str(), &current) == 0;
      bool contentChanged = action == "updated" && (flags[i] & kFSEventStreamEventFlagItemModified);
      auto previous = sub.seen.find(lexical);
      if (contentChanged && inspected && previous != sub.seen.end()) {
        const auto& old = previous->second;
        bool sameContentStamp = current.st_dev == old.st_dev && current.st_ino == old.st_ino && current.st_size == old.st_size && current.st_mtimespec.tv_sec == old.st_mtimespec.tv_sec && current.st_mtimespec.tv_nsec == old.st_mtimespec.tv_nsec;
        bool permissionsChanged = current.st_mode != old.st_mode || current.st_uid != old.st_uid || current.st_gid != old.st_gid;
        bool accessTimeChanged = current.st_atimespec.tv_sec != old.st_atimespec.tv_sec || current.st_atimespec.tv_nsec != old.st_atimespec.tv_nsec;
        bool statusTimeChanged = current.st_ctimespec.tv_sec != old.st_ctimespec.tv_sec || current.st_ctimespec.tv_nsec != old.st_ctimespec.tv_nsec;
        // ItemModified, like ItemCreated, may remain set on a later chmod.
        // A metadata-only transition does not force a content reread. Writes
        // restoring size/mtime still force rereads when permissions are stable.
        if (sameContentStamp && (!statusTimeChanged || permissionsChanged || accessTimeChanged)) contentChanged = false;
      }
      // FSEvents can retain ItemCreated on a later write notification. Keep
      // only live entry metadata needed to disambiguate subsequent activity.
      if (action == "deleted") {
        for (auto it = sub.seen.begin(); it != sub.seen.end();) {
          if (within(it->first, lexical)) it = sub.seen.erase(it); else ++it;
        }
      } else if (inspected) sub.seen.insert_or_assign(lexical, current);
      events.push_back({action, lexical, contentChanged});
    }
    engine.changes(sub.source.id, std::move(events));
  }
};
std::unique_ptr<Platform> createPlatform(Engine& engine) { return std::make_unique<MacOS>(engine); }
const char* backendName() { return "fs-events"; }
}
