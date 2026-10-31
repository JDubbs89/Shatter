# 04: Make the application build and run on multiple operating systems

**What to build:** Shatter's process lifecycle, control transport, and build can support another operating system without changing mixer behavior.

**Blocked by:** 02: Put PipeWire behind an audio backend seam.

**Status:** ready-for-human

- [x] Platform-specific process and control-transport operations are isolated behind platform-specific implementations.
- [x] The build can select supported audio and platform implementations without requiring PipeWire on every platform.
- [x] The existing Linux and PipeWire build and control workflow continues to work.
- [x] Platform selection and unsupported combinations produce clear build or startup errors.

Windows uses the native process API and a per-user named pipe, and defaults to the explicit no-audio backend until a Windows audio backend is implemented. A native Windows toolchain was not available in the implementation environment, so the Windows build and CLI lifecycle test remain to be run on Windows.
