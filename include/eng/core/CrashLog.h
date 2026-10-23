#pragma once

// Best-effort crash diagnostics: tees stdout/stderr to a log file written
// next to wherever the process is run from (so the transcript survives even
// if a double-clicked .exe's console window vanishes the instant the
// process dies) and installs handlers for crashes a try/catch around main()
// never sees — SIGSEGV/SIGABRT/SIGFPE/SIGILL, std::terminate (an uncaught
// C++ exception), and on Windows, unhandled SEH exceptions like access
// violations. Call once, as the very first thing in main(), before anything
// else has a chance to crash.
namespace crashlog {

// appName picks the log file's name: "<appName>_log.txt".
void init(const char* appName);

}  // namespace crashlog
