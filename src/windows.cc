#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "engine.hh"
#include <cstring>
#include <unordered_map>

namespace lumine {
class Windows : public Platform {
  struct Subscription {
    Windows* owner;
    Source source;
    HANDLE directory = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    std::vector<DWORD> read = std::vector<DWORD>(16384), write = std::vector<DWORD>(16384);
    bool pending = false, closing = false;
    ~Subscription() { if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory); }
  };
  std::unordered_map<Id, std::shared_ptr<Subscription>> sources;
  HANDLE ownerThread;
public:
  explicit Windows(Engine& engine) : Platform(engine) {
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &ownerThread, 0, FALSE, DUPLICATE_SAME_ACCESS)) throw std::runtime_error("Cannot open filesystem engine thread");
  }
  ~Windows() override { CloseHandle(ownerThread); }
  void add(const Source& source) override {
    auto status = fs::status(fs::u8path(source.path));
    if (!fs::exists(status)) throw fs::filesystem_error("Directory does not exist", fs::u8path(source.path), std::make_error_code(std::errc::no_such_file_or_directory));
    if (!fs::is_directory(status)) throw fs::filesystem_error("Expected a directory", fs::u8path(source.path), std::make_error_code(std::errc::not_a_directory));
    auto sub = std::make_shared<Subscription>();
    sub->source = source;
    sub->owner = this;
    sub->overlapped.hEvent = sub.get();
    sub->directory = CreateFileW(fs::u8path(source.path).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (sub->directory == INVALID_HANDLE_VALUE) throw fs::filesystem_error("Cannot open directory", fs::u8path(source.path), std::error_code(GetLastError(), std::system_category()));
    sources.emplace(source.id, sub);
    if (arm(sub)) engine.ready(source.id);
  }
  void remove(Id id) override {
    auto found = sources.find(id);
    if (found == sources.end()) { engine.closed(id); return; }
    auto sub = found->second;
    if (sub->closing) return;
    sub->closing = true;
    // CancelIo is only a request. The source, buffers and OVERLAPPED remain
    // owned by sources until the completion routine acknowledges cancellation.
    if (sub->pending) CancelIo(sub->directory);
    else finish(sub);
  }
  void removeAll() override {
    std::vector<Id> ids;
    for (const auto& pair : sources) ids.push_back(pair.first);
    for (auto id : ids) remove(id);
  }
  bool empty() const override { return sources.empty(); }
  void pump() override { SleepEx(INFINITE, TRUE); }
  void wake() override { QueueUserAPC([](ULONG_PTR) {}, ownerThread, 0); }
private:
  void finish(const std::shared_ptr<Subscription>& sub) {
    sources.erase(sub->source.id);
    CloseHandle(sub->directory);
    sub->directory = INVALID_HANDLE_VALUE;
    engine.closed(sub->source.id);
  }
  bool arm(const std::shared_ptr<Subscription>& sub) {
    BOOL result = ReadDirectoryChangesW(sub->directory, sub->write.data(), static_cast<DWORD>(sub->write.size() * sizeof(DWORD)), sub->source.recursive,
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
      nullptr, &sub->overlapped, [](DWORD error, DWORD bytes, OVERLAPPED* overlapped) {
        auto raw = static_cast<Subscription*>(overlapped->hEvent);
        // The map owns every pending source. Keep a local reference while
        // complete() may remove it; all operations run on the owner thread.
        auto self = raw->owner->sources.at(raw->source.id);
        self->pending = false;
        self->owner->complete(self, error, bytes);
      });
    if (!result) {
      auto error = GetLastError();
      engine.error(sub->source, std::system_category().message(error), error == ERROR_ACCESS_DENIED ? "EACCES" : "ERR_WATCH_READ");
      finish(sub);
      return false;
    }
    sub->pending = true;
    return true;
  }
  void complete(const std::shared_ptr<Subscription>& sub, DWORD error, DWORD bytes) {
    if (sub->closing || error == ERROR_OPERATION_ABORTED) { finish(sub); return; }
    if (error == ERROR_ACCESS_DENIED || error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND) {
      engine.invalidate(sub->source.id, "root-changed"); finish(sub); return;
    }
    if (error != ERROR_SUCCESS && error != ERROR_NOTIFY_ENUM_DIR) {
      engine.error(sub->source, std::system_category().message(error), "ERR_WATCH_READ"); finish(sub); return;
    }
    std::swap(sub->read, sub->write);
    if (!arm(sub)) return;
    if (bytes == 0 || error == ERROR_NOTIFY_ENUM_DIR) { engine.invalidate(sub->source.id, "native-overflow"); return; }
    std::vector<Event> events;
    const auto* buffer = reinterpret_cast<const unsigned char*>(sub->read.data());
    size_t offset = 0;
    const size_t header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    while (offset < bytes) {
      if (bytes - offset < header) { engine.invalidate(sub->source.id, "invalid-native-event"); return; }
      auto info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
      if (info->FileNameLength % sizeof(WCHAR) || info->FileNameLength > bytes - offset - header) { engine.invalidate(sub->source.id, "invalid-native-event"); return; }
      auto name = fs::path(std::wstring(info->FileName, info->FileNameLength / sizeof(WCHAR)));
      auto path = utf8(fs::u8path(sub->source.path) / name);
      std::string action;
      switch (info->Action) {
        case FILE_ACTION_ADDED: case FILE_ACTION_RENAMED_NEW_NAME: action = "created"; break;
        case FILE_ACTION_REMOVED: case FILE_ACTION_RENAMED_OLD_NAME: action = "deleted"; break;
        case FILE_ACTION_MODIFIED: action = "updated"; break;
        default: engine.invalidate(sub->source.id, "unknown-native-event"); break;
      }
      if (!action.empty()) events.push_back({action, path, action == "updated"});
      if (!info->NextEntryOffset) break;
      if (info->NextEntryOffset < header + info->FileNameLength || info->NextEntryOffset % alignof(DWORD) || info->NextEntryOffset >= bytes - offset) { engine.invalidate(sub->source.id, "invalid-native-event"); return; }
      offset += info->NextEntryOffset;
    }
    engine.changes(sub->source.id, std::move(events));
  }
};
std::unique_ptr<Platform> createPlatform(Engine& engine) { return std::make_unique<Windows>(engine); }
const char* backendName() { return "windows"; }
}
