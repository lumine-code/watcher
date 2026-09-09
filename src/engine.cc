#include "engine.hh"
#include <algorithm>
#include <stdexcept>

namespace lumine {
constexpr size_t MAX_QUEUED_EVENTS = 8192;
Engine::Engine(Napi::Env env, Napi::Function fn) {
  callback = Napi::ThreadSafeFunction::New(env, fn, "Lumine filesystem engine", 1, 1);
  try { thread = std::thread([this] { run(); }); }
  catch (...) { callback.Release(); throw; }
  std::unique_lock<std::mutex> lock(startupMutex);
  startupCondition.wait(lock, [this] { return started; });
  if (!startupError.empty()) { thread.join(); throw std::runtime_error(startupError); }
}
Engine::~Engine() { shutdown(); }
void Engine::enqueue(Command command) {
  if (finished.load()) return;
  { std::lock_guard<std::mutex> lock(commandsMutex); commands.push_back(std::move(command)); }
  platform->wake();
}
void Engine::shutdown() {
  if (thread.joinable()) { enqueue({Command::Close, {0, "", false}}); thread.join(); }
}
void Engine::run() {
  try { platform = createPlatform(*this); }
  catch (const std::exception& error) { startupError = error.what(); }
  { std::lock_guard<std::mutex> lock(startupMutex); started = true; }
  startupCondition.notify_one();
  if (!startupError.empty()) { finished = true; callback.Release(); return; }
  try {
    while (true) {
      std::deque<Command> batch;
      { std::lock_guard<std::mutex> lock(commandsMutex); batch.swap(commands); }
      for (const auto& command : batch) {
        if (command.type == Command::Close) { stopping = true; platform->removeAll(); }
        else if (command.type == Command::Unwatch) platform->remove(command.source.id);
        else if (stopping) {
          error(command.source, "The filesystem engine is closed", "ERR_ENGINE_CLOSED");
          closed(command.source.id);
        } else {
          try { platform->add(command.source); }
          catch (const fs::filesystem_error& failure) {
            auto code = failure.code();
            error(command.source, failure.what(), code == std::errc::no_such_file_or_directory ? "ENOENT" : code == std::errc::not_a_directory ? "ENOTDIR" : code == std::errc::permission_denied ? "EACCES" : "ERR_WATCH_START");
            platform->remove(command.source.id);
          } catch (const std::exception& failure) {
            error(command.source, failure.what(), "ERR_WATCH_START");
            platform->remove(command.source.id);
          }
        }
      }
      if (stopping && platform->empty()) break;
      platform->pump();
    }
  } catch (const std::exception& failure) {
    emit({"engineError", 0, {}, "", failure.what(), "ERR_WATCH_ENGINE", ""});
    platform->removeAll();
    while (!platform->empty()) platform->pump();
  }
  finished = true;
  emit({"engineClosed"});
  callback.Release();
}
void Engine::emit(Message message) {
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lock(messagesMutex);
    // Once a source is invalid, more activity adds no information until JS has
    // consumed the invalidation and started reconciliation. This also bounds a
    // sustained stream of kernel-overflow notifications while JS is stalled.
    if (message.type == "changes" || message.type == "invalidate") {
      if (std::any_of(messages.begin(), messages.end(), [&](const Message& pending) {
        return pending.id == message.id && pending.type == "invalidate";
      })) return;
    }
    if (message.type == "changes" && queuedEvents + message.events.size() > MAX_QUEUED_EVENTS) {
      // Lifecycle messages survive overflow; every lost batch invalidates its source.
      std::map<Id, bool> lost;
      lost[message.id] = true;
      for (auto it = messages.begin(); it != messages.end();) {
        if (it->type == "changes") { lost[it->id] = true; it = messages.erase(it); }
        else ++it;
      }
      queuedEvents = 0;
      for (const auto& item : lost) {
        bool present = std::any_of(messages.begin(), messages.end(), [&](const Message& pending) { return pending.id == item.first && pending.type == "invalidate"; });
        if (!present) messages.push_back({"invalidate", item.first, {}, "queue-overflow"});
      }
    } else { queuedEvents += message.events.size(); messages.push_back(std::move(message)); }
    if (!scheduled) { scheduled = true; schedule = true; }
  }
  if (schedule) callback.NonBlockingCall([this](Napi::Env env, Napi::Function fn) { if (env) deliver(env, fn); });
}
void Engine::deliver(Napi::Env env, Napi::Function fn) {
  std::deque<Message> batch;
  { std::lock_guard<std::mutex> lock(messagesMutex); batch.swap(messages); queuedEvents = 0; scheduled = false; }
  for (const auto& message : batch) {
    auto object = Napi::Object::New(env);
    object.Set("type", message.type);
    object.Set("id", Napi::Number::New(env, static_cast<double>(message.id)));
    if (message.type == "changes") {
      auto events = Napi::Array::New(env, message.events.size());
      for (size_t i = 0; i < message.events.size(); ++i) {
        auto event = Napi::Object::New(env);
        event.Set("action", message.events[i].action); event.Set("path", message.events[i].path);
        if (message.events[i].contentChanged) event.Set("contentChanged", true);
        events.Set(static_cast<uint32_t>(i), event);
      }
      object.Set("events", events);
    } else if (message.type == "invalidate") object.Set("reason", message.reason);
    else if (message.type == "error" || message.type == "engineError") {
      auto error = Napi::Object::New(env);
      error.Set("message", message.message); error.Set("code", message.code);
      error.Set("path", message.path); error.Set("backend", backendName()); object.Set("error", error);
    }
    try { fn.Call({object}); }
    catch (const Napi::Error& error) { napi_fatal_exception(env, error.Value()); }
    if (env.IsExceptionPending()) { auto error = env.GetAndClearPendingException(); napi_fatal_exception(env, error.Value()); }
  }
}
void Engine::ready(Id id) { emit({"ready", id}); }
void Engine::closed(Id id) { emit({"closed", id}); }
void Engine::changes(Id id, std::vector<Event> events) { if (!events.empty()) emit({"changes", id, std::move(events)}); }
void Engine::invalidate(Id id, std::string reason) { emit({"invalidate", id, {}, std::move(reason)}); }
void Engine::error(const Source& source, std::string message, std::string code) { emit({"error", source.id, {}, "", std::move(message), std::move(code), source.path}); }

class NativeEngine : public Napi::ObjectWrap<NativeEngine> {
public:
  static Napi::Function define(Napi::Env env) {
    return DefineClass(env, "NativeEngine", {InstanceMethod("watch", &NativeEngine::watch), InstanceMethod("unwatch", &NativeEngine::unwatch), InstanceMethod("close", &NativeEngine::close), InstanceMethod("release", &NativeEngine::release)});
  }
  explicit NativeEngine(const Napi::CallbackInfo& info) : Napi::ObjectWrap<NativeEngine>(info) {
    if (info.Length() != 1 || !info[0].IsFunction()) throw Napi::TypeError::New(info.Env(), "Expected a callback");
    try { engine = std::make_unique<Engine>(info.Env(), info[0].As<Napi::Function>()); }
    catch (const std::exception& failure) {
      auto error = Napi::Error::New(info.Env(), failure.what());
      error.Value().Set("code", "ERR_WATCH_ENGINE");
      throw error;
    }
    napi_add_env_cleanup_hook(info.Env(), cleanup, this);
    Ref();
  }
  ~NativeEngine() override { if (!cleaned) napi_remove_env_cleanup_hook(Env(), cleanup, this); }
private:
  std::unique_ptr<Engine> engine;
  bool cleaned = false;
  bool released = false;
  static void cleanup(void* data) { auto self = static_cast<NativeEngine*>(data); self->cleaned = true; self->engine->shutdown(); }
  void watch(const Napi::CallbackInfo& info) {
    if (info.Length() != 3 || !info[0].IsNumber() || !info[1].IsString() || !info[2].IsBoolean()) throw Napi::TypeError::New(info.Env(), "Expected an id, path and recursive flag");
    engine->enqueue({Command::Watch, {static_cast<Id>(info[0].As<Napi::Number>().Int64Value()), info[1].As<Napi::String>().Utf8Value(), info[2].As<Napi::Boolean>().Value()}});
  }
  void unwatch(const Napi::CallbackInfo& info) {
    if (info.Length() != 1 || !info[0].IsNumber()) throw Napi::TypeError::New(info.Env(), "Expected an id");
    engine->enqueue({Command::Unwatch, {static_cast<Id>(info[0].As<Napi::Number>().Int64Value()), "", false}});
  }
  void close(const Napi::CallbackInfo&) { engine->enqueue({Command::Close, {0, "", false}}); }
  void release(const Napi::CallbackInfo&) {
    if (released) return;
    engine->shutdown();
    released = true;
    Unref();
  }
};
Napi::Object Init(Napi::Env env, Napi::Object exports) { exports.Set("NativeEngine", NativeEngine::define(env)); return exports; }
NODE_API_MODULE(watcher, Init)
}
