'use strict';
const path = require('node:path');

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => {
    resolve = yes;
    reject = no;
  });
  // Cancellation is allowed before callers have observed ready. Keep the
  // original promise rejecting without a process-level unhandled rejection.
  promise.catch(() => {});
  return {promise, resolve, reject};
}

function failure(message, code) {
  return Object.assign(new Error(message), {code});
}

exports.createWrapper = (NativeEngine) => ({
  createEngine() {
    const handles = new Map();
    const closed = deferred();
    let closing = false,
      nextId = 0;
    const native = new NativeEngine((message) => {
      if (message.type === 'engineClosed') {
        for (const handle of handles.values()) {
          handle.disposed = true;
          handle.ready.reject(
            failure(
              'The filesystem engine closed before readiness',
              'ERR_ENGINE_CLOSED',
            ),
          );
          handle.closed.resolve();
        }
        handles.clear();
        native.release();
        closed.resolve();
        return;
      }
      if (message.type === 'engineError') {
        for (const handle of handles.values()) {
          const error = Object.assign(
            new Error(message.error.message),
            message.error,
          );
          handle.ready.reject(error);
          if (!handle.disposed)
            handle.callback({type: 'error', error: message.error});
        }
        closing = true;
        return;
      }
      const handle = handles.get(message.id);
      if (!handle) return;
      if (message.type === 'closed') {
        handle.disposed = true;
        handle.ready.reject(
          failure(
            'The directory watch closed before readiness',
            'ERR_WATCH_CLOSED',
          ),
        );
        handle.closed.resolve();
        handles.delete(message.id);
      } else if (message.type === 'ready') {
        if (!handle.disposed) handle.ready.resolve();
      } else if (!handle.disposed) {
        if (message.type === 'error')
          handle.ready.reject(
            Object.assign(new Error(message.error.message), message.error),
          );
        const {id: _id, ...event} = message;
        handle.callback(
          handle.guard && event.type === 'changes' ? {type: 'guard'} : event,
        );
      }
    });
    return {
      watchDirectory(directory, options, callback) {
        if (closing)
          throw failure('The filesystem engine is closed', 'ERR_ENGINE_CLOSED');
        if (
          typeof directory !== 'string' ||
          !directory ||
          directory.includes('\0')
        )
          throw new TypeError(
            'Expected a nonempty directory path without null bytes',
          );
        if (!options || typeof options !== 'object' || Array.isArray(options))
          throw new TypeError('Expected directory watch options');
        if (
          options.recursive !== undefined &&
          typeof options.recursive !== 'boolean'
        )
          throw new TypeError('Expected recursive to be a boolean');
        if (options.guard !== undefined && typeof options.guard !== 'boolean')
          throw new TypeError('Expected guard to be a boolean');
        if (options.guard && options.recursive)
          throw new TypeError('A directory guard cannot be recursive');
        if (typeof callback !== 'function')
          throw new TypeError('Expected a directory watch callback');
        const id = ++nextId;
        const handle = {
          ready: deferred(),
          closed: deferred(),
          callback,
          guard: options.guard ?? false,
          disposed: false,
        };
        handles.set(id, handle);
        native.watch(
          id,
          path.resolve(directory),
          options.recursive ?? false,
          options.guard ?? false,
        );
        return {
          ready: handle.ready.promise,
          closed: handle.closed.promise,
          dispose() {
            if (handle.disposed) return;
            handle.disposed = true;
            handle.ready.reject(
              Object.assign(
                failure('The directory watch was cancelled', 'ABORT_ERR'),
                {name: 'AbortError'},
              ),
            );
            native.unwatch(id);
          },
        };
      },
      close() {
        if (!closing) {
          closing = true;
          for (const handle of handles.values()) {
            handle.disposed = true;
            handle.ready.reject(
              Object.assign(
                failure('The directory watch was cancelled', 'ABORT_ERR'),
                {name: 'AbortError'},
              ),
            );
          }
          native.close();
        }
        return closed.promise;
      },
    };
  },
});
