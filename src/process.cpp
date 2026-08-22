#include "process.h"

bool startProcessDetached(const std::wstring& exe,
                          const std::wstring& args,
                          const std::wstring& workDir,
                          ProcInfo& out,
                          bool hidden) {
    if (!fileExists(exe)) return false;
    std::wstring cmdline = L"\"" + exe + L"\"";
    if (!args.empty()) cmdline += L" " + args;

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (hidden) si.dwFlags |= STARTF_USESHOWWINDOW, si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back(0);

    std::wstring wd = workDir;
    if (wd.empty()) wd = exeDir();

    if (!CreateProcessW(exe.c_str(), &cmdBuf[0], nullptr, nullptr, FALSE,
                        hidden ? CREATE_NO_WINDOW : 0, nullptr,
                        wd.empty() ? nullptr : wd.c_str(), &si, &pi)) {
        return false;
    }
    out.pid = pi.dwProcessId;
    out.hProcess = pi.hProcess;
    CloseHandle(pi.hThread);
    return true;
}

bool isPidAlive(DWORD pid) {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD exitCode = 0;
    bool alive = GetExitCodeProcess(h, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

bool killProcessByPid(DWORD pid) {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return false;
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok != FALSE;
}

// Encode an env map into a block; include current process env for unspecified vars.
// Returns allocated buffer (caller must free with freeEnvBlock).
wchar_t* buildEnvBlock(const std::map<std::wstring, std::wstring>& overrides) {
    std::wstring block;
    wchar_t* cur = GetEnvironmentStringsW();
    if (cur) {
        wchar_t* p = cur;
        while (*p) {
            std::wstring line(p);
            // environment entries that start with '=' (like =C:=...) are special
            // drive-current-dir entries; skip the leading '=' handling by checking
            // first '=' strictly after position 0 for real name=value pairs.
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos) {
                std::wstring key = line.substr(0, eq);
                if (overrides.find(key) == overrides.end()) {
                    block += line;
                    block += L'\0';
                }
            }
            p += line.size() + 1;
        }
        FreeEnvironmentStringsW(cur);
    }
    for (auto& kv : overrides) {
        block += kv.first + L"=" + kv.second;
        block += L'\0';
    }
    block += L'\0';
    // env block contains embedded nulls; must copy with memcpy (wcscpy would stop at first null)
    wchar_t* buf = new wchar_t[block.size() + 1];
    memcpy(buf, block.c_str(), (block.size() + 1) * sizeof(wchar_t));
    return buf;
}

RunResult runProcessCapture(const std::wstring& exe,
                            const std::wstring& args,
                            const std::wstring& workDir,
                            int timeoutMs,
                            const std::map<std::wstring, std::wstring>& env) {
    RunResult result;
    if (!fileExists(exe)) {
        result.output = L"命令不存在: " + exe;
        return result;
    }
    std::wstring cmdline = L"\"" + exe + L"\"";
    if (!args.empty()) cmdline += L" " + args;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return result;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back(0);

    // Temporarily set env vars in current process, spawn, then restore.
    // (avoids building a custom env block, which is fragile with inherited pipes)
    std::vector<std::wstring> savedKeys;
    std::vector<std::wstring> savedVals;
    std::vector<bool> existed;
    if (!env.empty()) {
        for (auto& kv : env) {
            wchar_t buf[32768];
            DWORD n = GetEnvironmentVariableW(kv.first.c_str(), buf, 32768);
            savedKeys.push_back(kv.first);
            if (n == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                existed.push_back(false);
                savedVals.push_back(L"");
            } else {
                existed.push_back(true);
                savedVals.push_back(std::wstring(buf, n));
            }
            SetEnvironmentVariableW(kv.first.c_str(), kv.second.c_str());
        }
    }

    std::wstring wd = workDir.empty() ? exeDir() : workDir;

    BOOL created = CreateProcessW(exe.c_str(), &cmdBuf[0], nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, wd.c_str(), &si, &pi);

    if (!env.empty()) {
        for (size_t i = 0; i < savedKeys.size(); ++i) {
            if (existed[i])
                SetEnvironmentVariableW(savedKeys[i].c_str(), savedVals[i].c_str());
            else
                SetEnvironmentVariableW(savedKeys[i].c_str(), nullptr);
        }
    }
    CloseHandle(hWritePipe); // child holds its own copy

    if (!created) {
        CloseHandle(hReadPipe);
        result.output = L"创建进程失败 (错误 " + std::to_wstring(GetLastError()) + L")";
        return result;
    }

    CloseHandle(pi.hThread);

    // Read output asynchronously
    std::string rawOutput;
    std::thread reader([&]() {
        char buf[8192];
        DWORD n = 0;
        while (ReadFile(hReadPipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
            rawOutput.append(buf, n);
        }
    });

    DWORD waitResult = WaitForSingleObject(pi.hProcess, timeoutMs > 0 ? timeoutMs : INFINITE);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        result.output = L"命令超时被终止";
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exitCode = code;
        result.ok = (code == 0);
    }

    reader.join();
    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);

    // Decode output (try UTF-8, fall back to ACP)
    if (!rawOutput.empty()) {
        int len = MultiByteToWideChar(CP_UTF8, 0, rawOutput.c_str(), (int)rawOutput.size(), nullptr, 0);
        if (len <= 0) {
            len = MultiByteToWideChar(CP_ACP, 0, rawOutput.c_str(), (int)rawOutput.size(), nullptr, 0);
        }
        std::wstring w(len, L'\0');
        if (len > 0) {
            if (MultiByteToWideChar(CP_UTF8, 0, rawOutput.c_str(), (int)rawOutput.size(), &w[0], len) == 0)
                MultiByteToWideChar(CP_ACP, 0, rawOutput.c_str(), (int)rawOutput.size(), &w[0], len);
            result.output = w;
        }
    }
    return result;
}

int runWait(const std::wstring& exe, const std::wstring& args, const std::wstring& workDir, int timeoutMs) {
    ProcInfo pi;
    if (!startProcessDetached(exe, args, workDir, pi, true)) return -1;
    DWORD r = WaitForSingleObject(pi.hProcess, timeoutMs > 0 ? timeoutMs : INFINITE);
    if (r == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        return -2;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}