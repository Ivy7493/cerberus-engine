#include "eng/core/CrashLog.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace crashlog {

namespace {

// Duplicates every character written to it into both the original
// console-backed streambuf and the log file, so console output and the
// on-disk transcript always stay identical without changing any existing
// std::cout/std::cerr call site.
class TeeBuf : public std::streambuf {
public:
    TeeBuf(std::streambuf* console, std::ofstream& file) : m_console(console), m_file(file) {}

protected:
    int overflow(int c) override {
        if (c != EOF) {
            if (m_console) m_console->sputc(static_cast<char>(c));
            m_file.put(static_cast<char>(c));
        }
        return c;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (m_console) m_console->sputn(s, n);
        m_file.write(s, n);
        // Flushed on every write rather than left to the usual buffering —
        // a crash can strike between any two writes, and a log that's
        // missing its last few lines is exactly the part most likely to
        // explain why the process died.
        m_file.flush();
        return n;
    }

private:
    std::streambuf* m_console;
    std::ofstream&  m_file;
};

std::ofstream g_logFile;
TeeBuf* g_outTee = nullptr;
TeeBuf* g_errTee = nullptr;

const char* signalName(int sig) {
    switch (sig) {
        case SIGABRT: return "SIGABRT";
        case SIGSEGV: return "SIGSEGV (access violation)";
        case SIGFPE:  return "SIGFPE (arithmetic error)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        default:      return "unknown signal";
    }
}

void logCrashLine(const std::string& message) {
    std::cerr << message << std::endl;
    g_logFile.flush();
}

// Signal handlers below call iostream/ofstream code that isn't strictly
// async-signal-safe — the correct approach for a hardened crash reporter
// would write through raw write()/WriteFile() instead. For a best-effort
// "what just happened" transcript on a game client, the practical risk of
// re-entering a stream's lock mid-write is far smaller than the value of
// reusing the exact same logging path as everything else in the file, so
// the tradeoff is made deliberately here.
[[noreturn]] void onSignal(int sig) {
    logCrashLine(std::string("\n[crash] terminated by ") + signalName(sig));
    std::signal(sig, SIG_DFL);
    std::raise(sig);  // re-raise with the default handler restored, so the
                       // OS still produces its normal crash behavior
                       // (core dump / WER report) after this log line.
    std::abort();      // unreachable in practice; satisfies [[noreturn]]
}

[[noreturn]] void onTerminate() {
    if (std::exception_ptr eptr = std::current_exception()) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            logCrashLine(std::string("\n[crash] unhandled exception: ") + e.what());
        } catch (...) {
            logCrashLine("\n[crash] unhandled exception of unknown type");
        }
    } else {
        logCrashLine("\n[crash] std::terminate called with no active exception");
    }
    std::abort();
}

#ifdef _WIN32
// Catches the Windows-specific crashes a signal handler won't reliably see
// under MinGW — access violations, stack overflows, etc.
LONG WINAPI onUnhandledSeh(EXCEPTION_POINTERS* info) {
    char msg[160];
    std::snprintf(msg, sizeof(msg), "\n[crash] unhandled SEH exception 0x%08lX at address %p",
                  info->ExceptionRecord->ExceptionCode,
                  static_cast<void*>(info->ExceptionRecord->ExceptionAddress));
    logCrashLine(msg);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

}  // namespace

void init(const char* appName) {
    // Append, not truncate — a crash from the *previous* run is exactly the
    // evidence a "why did it crash" investigation needs, and a server that
    // gets restarted between sessions (or auto-restarted after a crash)
    // would otherwise silently destroy it before anyone gets to read it.
    std::string path = std::string(appName) + "_log.txt";
    g_logFile.open(path, std::ios::out | std::ios::app);
    if (!g_logFile.is_open()) {
        std::cerr << "[crashlog] failed to open " << path << " for writing\n";
        return;
    }

    std::time_t now = std::time(nullptr);
    char timeStr[64];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    g_logFile << "=== " << appName << " log started " << timeStr << " ===\n";
    g_logFile.flush();

    // Tee instead of plain redirect — keeps the console window's live
    // output for anyone running from a terminal, while also persisting it.
    g_outTee = new TeeBuf(std::cout.rdbuf(), g_logFile);
    g_errTee = new TeeBuf(std::cerr.rdbuf(), g_logFile);
    std::cout.rdbuf(g_outTee);
    std::cerr.rdbuf(g_errTee);

    std::signal(SIGABRT, onSignal);
    std::signal(SIGSEGV, onSignal);
    std::signal(SIGFPE,  onSignal);
    std::signal(SIGILL,  onSignal);
    std::set_terminate(onTerminate);

#ifdef _WIN32
    SetUnhandledExceptionFilter(onUnhandledSeh);
#endif
}

}  // namespace crashlog
