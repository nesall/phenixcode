#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <mutex>
#include <cstdint>
#include <cstdio> // For popen/pclose alternative on Unix if needed, though waitpid is used

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
struct AutoHandle {
  HANDLE h = NULL;
  void reset(HANDLE new_h = NULL) {
    if (h != NULL && h != INVALID_HANDLE_VALUE) {
      CloseHandle(h);
    }
    h = new_h;
  }
  ~AutoHandle() { reset(); }
  operator HANDLE() const { return h; }
  AutoHandle(HANDLE handle_in = NULL) : h(handle_in) {}
  AutoHandle(const AutoHandle &) = delete;
  AutoHandle &operator=(const AutoHandle &) = delete;
  AutoHandle(AutoHandle &&other) noexcept : h(other.h) { other.h = NULL; }
  AutoHandle &operator=(AutoHandle &&other) noexcept {
    if (this != &other) {
      reset();
      h = other.h;
      other.h = NULL;
    }
    return *this;
  }
};

struct MutableProcessInfo {
  AutoHandle hProcess;
  AutoHandle hThread;
  DWORD dwProcessId = 0;
};
#else // Unix/Linux
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <cstring> // For strerror
#include <stdexcept>
#endif

namespace ProcessUtils {

#ifdef _WIN32
  inline std::string quoteArg(const std::string &arg) {
    if (arg.find(' ') != std::string::npos || arg.find('"') != std::string::npos || arg.empty()) {
      return "\"" + arg + "\"";
    }
    return arg;
  }
#endif // _WIN32

} // namespace ProcessUtils

class ProcessManager {
public:
  ProcessManager() :
    running_(false),
    exitCode_(-1)
  {
#ifdef _WIN32
    ZeroMemory(&processInfo_, sizeof(processInfo_));

    //jobObject_ = AutoHandle{ CreateJobObject(NULL, NULL) };
    //if (jobObject_) {
    //  JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };
    //  jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    //  if (!SetInformationJobObject(jobObject_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
    //    // Log error but proceed, job object might still be usable
    //  }
    //}
#endif
  }

  ProcessManager(const ProcessManager &) = delete;
  ProcessManager &operator=(const ProcessManager &) = delete;
  ProcessManager(ProcessManager &&) = delete;
  ProcessManager &operator=(ProcessManager &&) = delete;

  ~ProcessManager() {
    stopProcess(true);
  }

  bool startProcess(const std::string &command, const std::vector<std::string> &args = {}) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (running_) {
      std::cerr << "Process is already running_" << std::endl;
      return false;
    }
    running_ = false;
    exitCode_ = -1;
#ifdef _WIN32
    std::string cmdLine = ProcessUtils::quoteArg(command);
    for (const auto &arg : args) {
      cmdLine += " " + ProcessUtils::quoteArg(arg);
    }
    // CreateProcessA modifies the command line buffer, so create a mutable copy
    std::vector<char> cmdLineBuffer(cmdLine.begin(), cmdLine.end());
    cmdLineBuffer.push_back('\0');
    STARTUPINFOA startupInfo;
    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION tempProcessInfo; // Use temporary struct for CreateProcessA
    ZeroMemory(&tempProcessInfo, sizeof(tempProcessInfo));
    DWORD creationFlags = CREATE_NO_WINDOW;
    BOOL success = CreateProcessA(
      NULL,                   // No module name (use command line)
      cmdLineBuffer.data(),    // Command line (mutable copy needed)
      NULL,                   // Process handle not inheritable
      NULL,                   // Thread handle not inheritable
      FALSE,                  // Set handle inheritance to FALSE
      creationFlags,          // No console window for the child process on Windows
      NULL,                   // Use parent's environment block
      NULL,                   // Use parent's starting directory
      &startupInfo,           // Pointer to STARTUPINFO structure
      &tempProcessInfo        // Pointer to PROCESS_INFORMATION structure
    );
    if (!success) {
      std::cerr << "Failed to create process. Error: " << GetLastError() << std::endl;
      return false;
    }
    processInfo_.hProcess = AutoHandle{ tempProcessInfo.hProcess };
    processInfo_.hThread = AutoHandle{ tempProcessInfo.hThread };
    processInfo_.dwProcessId = tempProcessInfo.dwProcessId;
    //if (jobObject_) {
    //  AssignProcessToJobObject(jobObject_, processInfo_.hProcess);
    //}
    running_ = true;
    return true;

#else
    // Unix/Linux implementation
    pid = fork();

    if (pid == -1) {
      std::cerr << "Failed to fork process: " << strerror(errno) << std::endl;
      return false;
    }

    if (pid == 0) {
      // Child process
      std::vector<char *> argv;
      argv.push_back(const_cast<char *>(command.c_str()));

      for (const auto &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
      }
      argv.push_back(nullptr); // NULL terminated

      // Suppress output in the child process unless user redirected it
      // A more complete solution would redirect child's stdout/stderr, 
      // but for now, we only print the execvp error.

      execvp(command.c_str(), argv.data());

      // If execvp returns, there was an error
      std::cerr << "Failed to execute command '" << command << "'. Error: " << strerror(errno) << std::endl;
      _exit(EXIT_FAILURE); // Use _exit to avoid flushing C++ streams
    } else {
      // Parent process
      running_ = true;
      return true;
    }
#endif
  }

  bool stopProcess(bool force = false) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!running_) {
      return true;
    }

#ifdef _WIN32
    if (!processInfo_.hProcess) {
      running_ = false;
      return true; // Nothing to stop, but state says running_. Fixed by RAII.
    }

    // --- GRACEFUL TERMINATION (Ctrl+C equivalent) ---
    DWORD pid = processInfo_.dwProcessId;

    // 1. Attach the process to our console (temporarily)
    if (AttachConsole(pid)) {

      // 2. Send the Ctrl+C event
      SetConsoleCtrlHandler(NULL, TRUE); // Temporarily disable our own Ctrl+C handler
      GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0); // Send to all processes attached to this console
      SetConsoleCtrlHandler(NULL, FALSE); // Restore our own handler

      // 3. Wait for the process to self-terminate (e.g., 500ms)
      DWORD waitResult = WaitForSingleObject(processInfo_.hProcess, 500);

      // Detach from the console
      FreeConsole();

      if (waitResult == WAIT_OBJECT_0) {
        // Success: Process terminated gracefully
        // Final handle cleanup should be done here or in isRunning/waitForCompletion
        // For now, rely on force=true or standard cleanup paths.
        return true;
      }
    } else {
      // If AttachConsole fails, the process might already be dead or it's a GUI app.
      // Proceed to forceful termination.
    }
    // --- END GRACEFUL TERMINATION ---

    if (!force) {
      std::cerr << "Warning: Graceful shutdown failed. Forcing termination." << std::endl;
    }

    if (TerminateProcess(processInfo_.hProcess, 1)) {
      WaitForSingleObject(processInfo_.hProcess, INFINITE);
      processInfo_.hProcess = NULL;
      processInfo_.hThread = NULL;
      processInfo_.dwProcessId = 0;
      running_ = false;
      exitCode_ = 1;
      return true;
    }

#else
    if (kill(pid, SIGTERM) == 0) {
      // Wait for process to terminate to prevent zombie
      int status;
      // Use waitpid with WNOHANG for non-blocking check, then block if needed
      if (waitpid(pid, &status, 0) == pid) {
        running_ = false;

        if (WIFEXITED(status)) {
          exitCode_ = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
          exitCode_ = 128 + WTERMSIG(status); // Common signal exit code convention
        } else {
          exitCode_ = -1;
        }
        return true;
      } else {
        // Process might have already been waited on, or waitpid failed
        if (!force) {
          std::cerr << "Warning: stopProcess could not wait for PID " << pid << std::endl;
        }
      }
    } else {
      if (errno != ESRCH && !force) { // ESRCH means process doesn't exist (already dead)
        std::cerr << "Failed to send SIGTERM to PID " << pid << ": " << strerror(errno) << std::endl;
      }
    }
#endif

    return false;
  }

  bool isRunning() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return running_;
  }

  bool testUpdatedRunningStatus() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!running_) {
      return false;
    }

#ifdef _WIN32
    DWORD exitCodeRaw;
    if (processInfo_.hProcess && GetExitCodeProcess(processInfo_.hProcess, &exitCodeRaw)) {
      if (exitCodeRaw == STILL_ACTIVE) {
        return true;
      } else {
        running_ = false;
        exitCode_ = exitCodeRaw;
        processInfo_.hProcess = NULL;
        processInfo_.hThread = NULL;
        processInfo_.dwProcessId = 0;
        return false;
      }
    }
    running_ = false; // Assume process is not running_ if GetExitCodeProcess fails
    return false;

#else
    int status;
    // WNOHANG: return immediately if no child has exited
    pid_t result = waitpid(pid, &status, WNOHANG);

    if (result == 0) {
      // Process is still running_
      return true;
    } else if (result == pid) {
      // Process has exited
      running_ = false;
      if (WIFEXITED(status)) {
        exitCode_ = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        exitCode_ = 128 + WTERMSIG(status);
      } else {
        exitCode_ = -1;
      }
      return false;
    } else { // result == -1 (Error, likely ESRCH - no such process)
      running_ = false;
      return false;
    }
#endif
  }

  bool waitForCompletion(int timeoutMs = -1) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!running_) {
      return true;
    }
#ifdef _WIN32
    DWORD waitTimeout = (timeoutMs == -1) ? INFINITE : static_cast<DWORD>(timeoutMs);
    DWORD result = WaitForSingleObject(processInfo_.hProcess, waitTimeout);
    if (result == WAIT_OBJECT_0) {
      DWORD exitCodeRaw;
      if (GetExitCodeProcess(processInfo_.hProcess, &exitCodeRaw)) {
        exitCode_ = exitCodeRaw;
      }
      running_ = false;
      processInfo_.hProcess = NULL;
      processInfo_.hThread = NULL;
      processInfo_.dwProcessId = 0;
      return true;
    }
    return false; // Timeout or failure
#else
    if (timeoutMs == -1) {
      // Blocking wait
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      int status;
      if (waitpid(pid, &status, 0) == pid) {
        running_ = false;
        if (WIFEXITED(status)) {
          exitCode_ = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
          exitCode_ = 128 + WTERMSIG(status);
        }
        return true;
      }
      return false;
    } else {
      // Timeout handling with exponential backoff
      auto start = std::chrono::steady_clock::now();
      int sleepMs = 1;
      const int maxSleepMs = 100;

      while (true) {
        if (!isRunning()) {
          return true;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start
        ).count();

        if (elapsed >= timeoutMs) {
          return false;
        }

        int remainingMs = timeoutMs - elapsed;
        int actualSleep = std::min({ sleepMs, maxSleepMs, remainingMs });

        usleep(actualSleep * 1000);
        sleepMs = std::min(sleepMs * 2, maxSleepMs); // Exponential backoff
      }
    }
#endif
    return running_;
  }

  int getExitCode() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return exitCode_;
  }

  uint64_t getProcessId() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
#ifdef _WIN32
    return static_cast<uint64_t>(processInfo_.dwProcessId);
#else
    return static_cast<uint64_t>(pid);
#endif
  }

  uint64_t detach() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    running_ = false;
#ifdef _WIN32
    uint64_t detachedPid = static_cast<uint64_t>(processInfo_.dwProcessId);
    processInfo_.hProcess = NULL;
    processInfo_.hThread = NULL;
    processInfo_.dwProcessId = 0;
    return detachedPid;
#else
    uint64_t detachedPid = static_cast<uint64_t>(pid);
    pid = -1;
    return detachedPid;
#endif
  }

private:
#ifdef _WIN32
  mutable MutableProcessInfo processInfo_;
  //mutable AutoHandle jobObject_;
#else
  mutable pid_t pid = -1;
#endif

  bool running_ = false;
  int exitCode_ = 0;
  mutable std::recursive_mutex mutex_;
};

#endif // PROCESS_MANAGER_H