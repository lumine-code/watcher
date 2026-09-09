'use strict';
if (!['win32', 'darwin', 'linux'].includes(process.platform)) {
  throw new Error(`@lumine-code/watcher does not support ${process.platform}`);
}
const {createWrapper} = require('./wrapper');
const {NativeEngine} = require('./build/Release/watcher.node');
module.exports = createWrapper(NativeEngine);
