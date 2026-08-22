#pragma once
#ifndef LNPP_PROCESS_H
#define LNPP_PROCESS_H

#include "common.h"

struct ProcInfo {
    DWORD pid = 0;
    HANDLE hProcess = nullptr;
};

struct RunResult {
    bool ok = false;
    DWORD exitCode = 0;
    std::wstring output;
};

// Start a process detached (server-style). Returns false on spawn failure.
bool startProcessDetached(const std::wstring& exe,
                          const std::wstring& args,
                          const std::wstring& workDir,
                          ProcInfo& out,
                          bool hidden = true);

// Run a process to completion, capturing stdout/stderr. timeoutMs <= 0 means wait forever.
RunResult runProcessCapture(const std::wstring& exe,
                            const std::wstring& args,
                            const std::wstring& workDir,
                            int timeoutMs = 60000,
                            const std::map<std::wstring,std::wstring>& env = {});

// Kill a process by PID (TerminateProcess). Returns true if a process was found.
bool killProcessByPid(DWORD pid);

// Check whether a PID is alive.
bool isPidAlive(DWORD pid);

// Run and wait; returns exit code (or -1 on failure).
int runWait(const std::wstring& exe, const std::wstring& args, const std::wstring& workDir, int timeoutMs);

#endif