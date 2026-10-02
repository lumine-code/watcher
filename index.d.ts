declare namespace Watcher {
  interface Event {
    action: 'created' | 'updated' | 'deleted';
    path: string;
    /** Native content hint; reread even if size and mtime are unchanged. */
    contentChanged?: true;
  }
  interface WatchError {
    message: string;
    code: string;
    path: string;
    backend: 'windows' | 'inotify' | 'fs-events';
  }
  type Message =
    | {type: 'changes'; events: Event[]}
    | {type: 'guard'}
    | {type: 'invalidate'; reason: string}
    | {type: 'error'; error: WatchError};
  interface DirectoryWatch {
    readonly ready: Promise<void>;
    readonly closed: Promise<void>;
    dispose(): void;
  }
  interface Engine {
    watchDirectory(
      path: string,
      options: {recursive?: boolean; guard?: boolean},
      callback: (message: Message) => void,
    ): DirectoryWatch;
    close(): Promise<void>;
  }
  function createEngine(): Engine;
}
export = Watcher;
