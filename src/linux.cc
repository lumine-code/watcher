#include "engine.hh"
#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <set>
#include <unordered_map>

namespace lumine {
constexpr uint32_t MASK = IN_ATTRIB | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO | IN_ONLYDIR | IN_EXCL_UNLINK;
class Linux : public Platform {
  struct Subscription { Source source; std::map<int, std::string> paths; };
  int descriptor = -1, pipe[2]{-1, -1};
  std::map<Id, Subscription> sources;
  std::map<int, std::set<Id>> owners;
  std::set<int> retired;
public:
  explicit Linux(Engine& engine) : Platform(engine) {
    if (pipe2(pipe, O_CLOEXEC | O_NONBLOCK) < 0) throw std::system_error(errno, std::generic_category(), "Cannot open wake pipe");
    descriptor = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (descriptor < 0) { close(pipe[0]); close(pipe[1]); throw std::system_error(errno, std::generic_category(), "Cannot open inotify"); }
  }
  ~Linux() override { close(descriptor); close(pipe[0]); close(pipe[1]); }
  void add(const Source& source) override {
    sources.emplace(source.id, Subscription{source, {}});
    attach(sources.at(source.id), fs::u8path(source.path), false);
    engine.ready(source.id);
  }
  void remove(Id id) override {
    auto found = sources.find(id);
    if (found != sources.end()) {
      auto paths = found->second.paths;
      for (const auto& item : paths) detach(id, item.first);
      sources.erase(id);
    }
    engine.closed(id);
  }
  void removeAll() override { while (!sources.empty()) remove(sources.begin()->first); }
  bool empty() const override { return sources.empty(); }
  void wake() override { char byte = 0; auto ignored = write(pipe[1], &byte, 1); (void)ignored; }
  void pump() override {
    pollfd fds[2]{{pipe[0], POLLIN, 0}, {descriptor, POLLIN, 0}};
    int result;
    do { result = poll(fds, 2, -1); } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::system_error(errno, std::generic_category(), "Cannot poll inotify");
    if (fds[0].revents) { char bytes[256]; while (read(pipe[0], bytes, sizeof(bytes)) > 0) {} }
    if (fds[1].revents) drain();
  }
private:
  void attach(Subscription& sub, const fs::path& root, bool notify) {
    // Arm each directory before enumerating it. Moved-in populated trees use
    // exactly the same traversal, closing the old descendant coverage hole.
    std::vector<fs::path> pending{root};
    std::vector<Event> discovered;
    while (!pending.empty()) {
      auto path = pending.back(); pending.pop_back();
      int wd = inotify_add_watch(descriptor, path.c_str(), MASK);
      if (wd < 0) {
        if (path != root && (errno == ENOENT || errno == ENOTDIR)) continue;
        throw fs::filesystem_error("Cannot watch directory", path, std::error_code(errno, std::generic_category()));
      }
      sub.paths[wd] = utf8(path); owners[wd].insert(sub.source.id);
      if (!sub.source.recursive) continue;
      std::error_code error;
      fs::directory_iterator iterator(path, error);
      if (error) {
        if (error == std::errc::no_such_file_or_directory) continue;
        throw fs::filesystem_error("Cannot enumerate directory", path, error);
      }
      for (const auto& entry : iterator) {
        auto status = entry.symlink_status(error);
        if (error) { if (error == std::errc::no_such_file_or_directory) continue; throw fs::filesystem_error("Cannot inspect directory entry", entry.path(), error); }
        if (notify) discovered.push_back({"created", utf8(entry.path())});
        if (fs::is_directory(status)) pending.push_back(entry.path());
        if (discovered.size() >= 256) { engine.changes(sub.source.id, std::move(discovered)); discovered.clear(); }
      }
    }
    engine.changes(sub.source.id, std::move(discovered));
  }
  void detach(Id id, int wd) {
    auto found = owners.find(wd);
    if (found == owners.end()) return;
    found->second.erase(id);
    auto source = sources.find(id);
    if (source != sources.end()) source->second.paths.erase(wd);
    if (found->second.empty()) {
      if (inotify_rm_watch(descriptor, wd) == 0) retired.insert(wd);
      owners.erase(found);
    }
  }
  void drain() {
    alignas(inotify_event) char buffer[64 * 1024];
    while (true) {
      ssize_t size = read(descriptor, buffer, sizeof(buffer));
      if (size < 0) { if (errno == EAGAIN) return; if (errno == EINTR) continue; throw std::system_error(errno, std::generic_category(), "Cannot read inotify"); }
      if (!size) return;
      for (ssize_t offset = 0; offset < size;) {
        auto event = reinterpret_cast<inotify_event*>(buffer + offset);
        if (size - offset < static_cast<ssize_t>(sizeof(inotify_event)) || event->len > size - offset - sizeof(inotify_event)) throw std::runtime_error("Invalid inotify event length");
        offset += sizeof(inotify_event) + event->len;
        if (event->mask & IN_Q_OVERFLOW) {
          // The fd's queue is shared: every source must be rebuilt by its owner.
          std::vector<Id> ids;
          for (const auto& pair : sources) ids.push_back(pair.first);
          for (auto id : ids) { engine.invalidate(id, "native-overflow"); remove(id); }
          continue;
        }
        if ((event->mask & IN_IGNORED) && retired.erase(event->wd)) continue;
        auto found = owners.find(event->wd);
        if (found == owners.end()) continue;
        auto ids = found->second;
        for (auto id : ids) process(id, *event);
      }
    }
  }
  void process(Id id, const inotify_event& event) {
    auto found = sources.find(id);
    if (found == sources.end()) return;
    auto& sub = found->second;
    auto directory = sub.paths.find(event.wd);
    if (directory == sub.paths.end()) return;
    std::string path = directory->second;
    if (event.len) path = utf8(fs::u8path(path) / event.name);
    if (!event.len && (event.mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED | IN_UNMOUNT))) {
      if (path == sub.source.path) { engine.invalidate(id, "root-changed"); remove(id); }
      else if (event.mask & (IN_IGNORED | IN_UNMOUNT)) { engine.invalidate(id, "directory-watch-lost"); remove(id); }
      return;
    }
    std::string action;
    if (event.mask & (IN_CREATE | IN_MOVED_TO)) action = "created";
    else if (event.mask & (IN_DELETE | IN_MOVED_FROM)) action = "deleted";
    else if (event.mask & (IN_MODIFY | IN_ATTRIB)) action = "updated";
    if (!action.empty()) engine.changes(id, {{action, path, (event.mask & IN_MODIFY) != 0}});
    if ((event.mask & IN_ISDIR) && sub.source.recursive) {
      if (action == "deleted") {
        std::vector<int> descriptors;
        for (const auto& item : sub.paths) if (within(item.second, path)) descriptors.push_back(item.first);
        for (auto wd : descriptors) detach(id, wd);
      } else if (action == "created") {
        try { attach(sub, fs::u8path(path), true); }
        catch (const fs::filesystem_error& error) {
          if (error.code() == std::errc::no_such_file_or_directory || error.code() == std::errc::not_a_directory) return;
          engine.error(sub.source, error.what(), "ERR_WATCH_COVERAGE"); remove(id);
        }
      }
    }
  }
};
std::unique_ptr<Platform> createPlatform(Engine& engine) { return std::make_unique<Linux>(engine); }
const char* backendName() { return "inotify"; }
}
