#pragma once
#include <napi.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lumine {
namespace fs = std::filesystem;
using Id = uint64_t;
struct Event { std::string action, path; bool contentChanged = false; };
struct Message {
  std::string type;
  Id id = 0;
  std::vector<Event> events;
  std::string reason, message, code, path;
};
struct Source { Id id; std::string path; bool recursive; bool guard = false; };
struct Command { enum Type { Watch, Unwatch, Close } type; Source source; };
class Engine;
class Platform {
public:
  explicit Platform(Engine& engine) : engine(engine) {}
  virtual ~Platform() = default;
  virtual void add(const Source& source) = 0;
  virtual void remove(Id id) = 0;
  virtual void removeAll() = 0;
  virtual bool empty() const = 0;
  virtual void pump() = 0;
  virtual void wake() = 0;
protected:
  Engine& engine;
};
std::unique_ptr<Platform> createPlatform(Engine& engine);
const char* backendName();
class Engine {
public:
  Engine(Napi::Env env, Napi::Function callback);
  ~Engine();
  void enqueue(Command command);
  void emit(Message message);
  void ready(Id id);
  void closed(Id id);
  void changes(Id id, std::vector<Event> events);
  void invalidate(Id id, std::string reason);
  void error(const Source& source, std::string message, std::string code);
  void shutdown();
private:
  void run();
  void deliver(Napi::Env env, Napi::Function callback);
  Napi::ThreadSafeFunction callback;
  std::thread thread;
  std::unique_ptr<Platform> platform;
  std::mutex commandsMutex;
  std::deque<Command> commands;
  std::mutex startupMutex;
  std::condition_variable startupCondition;
  bool started = false;
  std::string startupError;
  std::mutex messagesMutex;
  std::deque<Message> messages;
  size_t queuedEvents = 0;
  bool scheduled = false;
  std::atomic<bool> stopping{false}, finished{false};
};
inline std::string utf8(const fs::path& path) { return path.u8string(); }
inline bool within(const std::string& path, const std::string& root) {
  if (path == root) return true;
  auto relative = fs::u8path(path).lexically_relative(fs::u8path(root));
  return !relative.empty() && *relative.begin() != "..";
}
}
