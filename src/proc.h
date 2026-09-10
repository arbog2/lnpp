// Process helpers. NOTE: this file used to be named process.h, which shadowed
// the CRT's <process.h> (an /I pointing at src\ then broke <thread> with
// "_beginthreadex: not a member"). Keep the name distinct from any CRT header.
#pragma once
#ifndef LNPP_PROC_H
#define LNPP_PROC_H

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

// True if the calling thread owns the main window (or is a thread that
// pumped the message loop on its behalf). Synchronous component ops like
// pgBackup / compStart can take many seconds and should never run on this
// thread — call them via runAsync / a worker thread.
void registerUiThread(DWORD tid);
bool isUiThread();

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

#endif