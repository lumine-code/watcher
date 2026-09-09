#include <CoreServices/CoreServices.h>
#include "engine.hh"
#include <set>

namespace lumine {
class MacOS : public Platform {
  struct Subscription {
    MacOS* owner;
    Source source;
    fs::path canonical;
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
        action = !fs::exists(status) ? "deleted" : created || renamed ? "created" : "updated";
      } else if (created) action = "created";
      else if (removed) action = "deleted";
      events.push_back({action, lexical, (flags[i] & kFSEventStreamEventFlagItemModified) != 0});
    }
    engine.changes(sub.source.id, std::move(events));
  }
};
std::unique_ptr<Platform> createPlatform(Engine& engine) { return std::make_unique<MacOS>(engine); }
const char* backendName() { return "fs-events"; }
}
