#include <CoreServices/CoreServices.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <unordered_map>
#include "engine.hh"
#include <set>

namespace lumine {
class MacOS : public Platform {
  struct Subscription {
    MacOS* owner;
    Source source;
    fs::path canonical;
    std::unordered_map<std::string, struct stat> seen;
    FSEventStreamRef stream = nullptr;
  };
  CFRunLoopRef loop;
  CFRunLoopSourceRef wakeSource;
  std::map<Id, std::unique_ptr<Subscription>> sources;
  std::set<Id> pendingClose;
public:
  explicit MacOS(Engine& engine) : Platform(engine), loop(CFRunLoopGetCurrent()) {
    CFRetain(loop);
    CFRunLoopSourceContext context{};
    context.perform = [](void*) {};
    wakeSource = CFRunLoopSourceCreate(nullptr, 0, &context);
    CFRunLoopAddSource(loop, wakeSource, kCFRunLoopDefaultMode);
  }
  ~MacOS() override { CFRunLoopSourceInvalidate(wakeSource); CFRelease(wakeSource); CFRelease(loop); }
  void add(const Source& source) override {
    auto canonical = fs::canonical(fs::u8path(source.path));
    if (!fs::is_directory(canonical)) throw fs::filesystem_error("Expected a directory", canonical, std::make_error_code(std::errc::not_a_directory));
    auto sub = std::make_unique<Subscription>();
    sub->owner = this; sub->source = source; sub->canonical = canonical;
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
    engine.ready(source.id);
  }
  void remove(Id id) override {
    auto found = sources.find(id);
    if (found != sources.end()) {
      // Source storage outlives its stream. Stop, unschedule and invalidate on
      // the runloop owner before releasing the raw callback context.
      auto stream = found->second->stream;
      FSEventStreamStop(stream);
      FSEventStreamUnscheduleFromRunLoop(stream, loop, kCFRunLoopDefaultMode);
      FSEventStreamInvalidate(stream); FSEventStreamRelease(stream);
      sources.erase(found);
    }
    pendingClose.erase(id);
    engine.closed(id);
  }
  void removeAll() override { while (!sources.empty()) remove(sources.begin()->first); }
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
      bool contentChanged = flags[i] & kFSEventStreamEventFlagItemModified;
      auto previous = sub.seen.find(lexical);
      if (contentChanged && inspected && previous != sub.seen.end()) {
        const auto& old = previous->second;
        bool sameContentStamp = current.st_dev == old.st_dev && current.st_ino == old.st_ino && current.st_size == old.st_size && current.st_mtimespec.tv_sec == old.st_mtimespec.tv_sec && current.st_mtimespec.tv_nsec == old.st_mtimespec.tv_nsec;
        bool permissionsChanged = current.st_mode != old.st_mode || current.st_uid != old.st_uid || current.st_gid != old.st_gid;
        // ItemModified, like ItemCreated, may remain set on a later chmod.
        // A metadata-only transition does not force a content reread. Writes
        // restoring size/mtime still force rereads when permissions are stable.
        if (sameContentStamp && permissionsChanged) contentChanged = false;
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
