#include "proc.h"

static DWORD g_uiThreadId = 0;

void registerUiThread(DWORD tid) { g_uiThreadId = tid; }

bool isUiThread() {
    return g_uiThreadId != 0 && GetCurrentThreadId() == g_uiThreadId;
}

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
// Returns an allocated buffer (caller must free with delete[]).
wchar_t* buildEnvBlock(const std::map<std::wstring, std::wstring>& overrides) {
    // Windows env var names are case-insensitive; skip inherited entries that
    // collide with an override in any case (e.g. existing "Path" vs "PATH").
    auto isOverridden = [&](const std::wstring& k) {
        std::wstring lk = lowerStr(k);
        for (auto& kv : overrides)
            if (lowerStr(kv.first) == lk) return true;
        return false;
    };
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
                if (!isOverridden(key)) {
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
    // runProcessCapture is synchronous and can block for the full timeout
    // (default 60s, some callers pass 120s+ for pg_restore). When called
    // from the UI thread, the main window freezes — no repaints, no
    // WM_QUIT, nothing. Catch this early in debug builds so a future
    // change that bypasses runAsync fails fast instead of in the wild.
#ifdef _DEBUG
    if (isUiThread()) {
        OutputDebugStringW(L"[lnpp] WARNING: runProcessCapture called from UI thread — will block the window.\n");
    }
#endif
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

    // bInheritHandles=TRUE without a handle list hands *every* inheritable
    // handle this process owns to the child. When two runProcessCapture() calls
    // overlap, the child spawned while another call has its pipe write end open
    // (the window between that call's CreatePipe and CreateProcess) gets a copy
    // of that other pipe. The other call's reader thread then never sees EOF
    // until this child exits — e.g. a 3s pm2 jlist poll could hold a 120s
    // pg_dumpall capture open, or vice versa. Restrict inheritance to exactly
    // this call's write handle.
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<BYTE> attrBuf(attrSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs =
        attrSize ? (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data() : nullptr;
    bool inheritList = attrs != nullptr &&
                       InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) != FALSE;
    HANDLE inheritHandles[1] = { hWritePipe };
    if (inheritList)
        inheritList = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                inheritHandles, sizeof(inheritHandles),
                                                nullptr, nullptr) != FALSE;
    if (!inheritList && attrs) {
        DeleteProcThreadAttributeList(attrs);
        attrs = nullptr;   // fall back to plain inheritance rather than failing
    }

    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    // cbSize must be the STARTUPINFOEX size when EXTENDED_STARTUPINFO_PRESENT
    // is used; the plain size otherwise.
    si.StartupInfo.cb = attrs ? sizeof(si) : sizeof(STARTUPINFO);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.hStdOutput = hWritePipe;
    si.StartupInfo.hStdError = hWritePipe;
    // GUI subsystem: this process has no console stdin. Leave it NULL so a
    // child that reads stdin gets immediate EOF instead of an invalid handle.
    si.StartupInfo.hStdInput = nullptr;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.lpAttributeList = attrs;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back(0);

    // Build a dedicated environment block instead of temporarily mutating the
    // current process's environment (SetEnvironmentVariable) and restoring it
    // afterwards. The save/restore dance is not thread-safe: two concurrent
    // runProcessCapture calls (e.g. the pm2 poller setting PATH while a pg
    // backup sets PGPASSWORD) could interleave and leave the wrong values in
    // place — or leak one process's env into another's child.
    wchar_t* envBlock = nullptr;
    if (!env.empty()) {
        envBlock = buildEnvBlock(env);
        if (!envBlock) {
            CloseHandle(hReadPipe);
            result.output = L"环境变量构建失败";
            return result;
        }
    }

    std::wstring wd = workDir.empty() ? exeDir() : workDir;

    DWORD flags = CREATE_NO_WINDOW | (envBlock ? CREATE_UNICODE_ENVIRONMENT : 0);
    if (attrs) flags |= EXTENDED_STARTUPINFO_PRESENT;

    BOOL created = CreateProcessW(exe.c_str(), &cmdBuf[0], nullptr, nullptr, TRUE,
                                  flags, envBlock, wd.c_str(), &si.StartupInfo, &pi);

    if (attrs) DeleteProcThreadAttributeList(attrs);
    delete[] envBlock;
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

    // Decode output (try UTF-8, fall back to ACP). Size the buffer from the
    // code page actually used — the old code allocated from the UTF-8 length
    // but wrote the ACP result into it, which can overrun when the two
    // encodings disagree on character count.
    if (!rawOutput.empty()) {
        UINT cp = CP_UTF8;
        int len = MultiByteToWideChar(cp, 0, rawOutput.c_str(), (int)rawOutput.size(), nullptr, 0);
        if (len <= 0) {
            cp = CP_ACP;
            len = MultiByteToWideChar(cp, 0, rawOutput.c_str(), (int)rawOutput.size(), nullptr, 0);
        }
        if (len > 0) {
            std::wstring w(len, L'\0');
            MultiByteToWideChar(cp, 0, rawOutput.c_str(), (int)rawOutput.size(), &w[0], len);
            result.output = w;
        }
    }
    return result;
}