// ============================================================================
//  lock_frame module: 010_platform.inc
//
//  Log/exit plumbing and game-window lookup.
//
//  Included by lock_frame/lock_frame.cpp as part of the single-translation-unit build.
//  Modules are in dependency order; a module may use only symbols declared above it.
// ============================================================================


// Exit now, without teardown.
//
// `return 0` from main runs the static destructors of the D3D12/COM/XeFG objects,
// and that teardown can block for seconds (measured: the launcher had to wait 1.2 s
// for WM_CLOSE, another 0.8 s for WM_QUIT and then TerminateProcess the process).
// An overlay that is being torn down has nothing worth waiting for, and the log is
// already flushed by logf_, so killing ourselves is both correct and instant.
static void hard_exit(const char* why);
// Every exit path goes through hard_exit(), so the z-order repair lives there too.
static void restore_game_zorder(const char* why);

// Line output. Every logf_ used to fflush(stdout), which is one write syscall per line and
// shows up in CPU time as soon as a diagnostic mode prints per frame (--dump, --fg-probe,
// --mv-shadow). A log line is worth a few hundred milliseconds at most, so the flush is
// paced instead: flush at most every 200 ms, and always for a line that looks like bad
// news or for anything that is about to exit. `--log-flush` restores per-line flushing
// for debugging, where the last few lines before a hard_exit are the interesting ones.
static int    g_log_flush_each = 0;      // --log-flush: fflush every line
static DWORD  g_log_last_flush = 0;

static bool log_needs_flush(const char* fmt)
{
    if (g_log_flush_each) { return true; }
    // Durability where it matters: a warning is the line someone is looking for, and it
    // is never in a hot loop.
    static const char* const kAlways[] = {
        "WARNING", "FAILED", "FAIL", "error", "ERROR", "note:", "note(", "exiting", "MISMATCH",
    };
    for (size_t i = 0; i < sizeof(kAlways) / sizeof(kAlways[0]); ++i) {
        if (strstr(fmt, kAlways[i])) { return true; }
    }
    const DWORD now = GetTickCount();
    if (now - g_log_last_flush >= 200) { g_log_last_flush = now; return true; }
    return false;
}

static void logf_(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    if (log_needs_flush(fmt)) { fflush(stdout); }
}

static void hard_exit(const char* why)
{
    logf_("[lock] %s - exiting now\n", why);
    restore_game_zorder(why);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);          // not reached
}

// printf's %ls stops at the first character the C locale cannot represent, which
// truncated a log line (and swallowed its newline) when the DLL sat under a path
// with Chinese characters. Convert explicitly instead.
static std::string narrow_utf8(const std::wstring& w)
{
    if (w.empty()) { return std::string(); }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) { return std::string(); }
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], n, nullptr, nullptr);
    return s;
}

// The library reports why it refused to interpolate; without this the only
// symptom is frameGenResult = ERR_INVALID_ARGUMENT.
static void fg_log_cb(const char* message, xefg_swapchain_logging_level_t level, void*)
{
    static const char* names[] = { "debug", "info", "warn", "error" };
    logf_("[xefg %s] %s\n", names[(level <= 3) ? level : 3], message ? message : "");
}

static const char* xefg_str(xefg_swapchain_result_t r)
{
    switch (r) {
    case XEFG_SWAPCHAIN_RESULT_SUCCESS:                  return "SUCCESS";
    case XEFG_SWAPCHAIN_RESULT_WARNING_OLD_DRIVER:       return "WARN_OLD_DRIVER";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE: return "ERR_UNSUPPORTED_DEVICE";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER: return "ERR_UNSUPPORTED_DRIVER";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED:      return "ERR_UNINITIALIZED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT:   return "ERR_INVALID_ARGUMENT";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE:             return "ERR_DEVICE";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED:        return "ERR_UNSUPPORTED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED:
        return "ERR_LATENCY_REDUCTION_UNSUPPORTED";
    case XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER:   return "ERR_WRONG_CALL_ORDER";
    default:                                            return "ERR_OTHER";
    }
}

// ------------------------------------------------------------ window lookup

// A game can run as more than one process with the same name (launcher + shipping exe,
// crash handler, ...), so collect every PID that matches and pick the largest window of
// any of them. Picking the first match used to grab the launcher, which owns no window -
// that is the "game window not found" case on UE5 titles.
struct FindWindowCtx
{
    DWORD pid = 0;          // current PID being enumerated
    HWND  best = nullptr;
    LONG  best_area = 0;
    DWORD best_pid = 0;
    int   windows = 0;      // visible windows seen for the current PID
};

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindWindowCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid || !IsWindowVisible(hwnd)) { return TRUE; }
    RECT r{};
    if (!GetClientRect(hwnd, &r)) { return TRUE; }
    const LONG area = (r.right - r.left) * (r.bottom - r.top);
    ++ctx->windows;
    if (area > ctx->best_area) {
        ctx->best_area = area;
        ctx->best = hwnd;
        ctx->best_pid = pid;
    }
    return TRUE;
}

// --game <exe>: which process owns the window we capture. Defaults to Cyberpunk, but
// any game with a ReShade add-on + DLSS works - e.g. `--game PBZ.exe`.
static std::wstring g_game_exe;               // --game <exe> (empty = use the foreground window)
static bool         g_game_exe_set = false;   // --game was given explicitly
// --pid N / --hwnd H: target a process or an exact window instead of a name. The GUI
// front-end (src/mgpu_ui) uses these so "any window" works without knowing the exe.
static DWORD  g_game_pid = 0;
static HWND   g_game_hwnd = nullptr;

// The window a GUI picker would have chosen: the largest visible, unowned, non-tool
// top-level window of that process.
static BOOL CALLBACK pick_by_pid_proc(HWND h, LPARAM lp)
{
    struct Pick { DWORD pid; HWND best; LONG area; };
    Pick* p = reinterpret_cast<Pick*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != p->pid) { return TRUE; }
    if (!IsWindowVisible(h)) { return TRUE; }
    if (GetWindow(h, GW_OWNER) != nullptr) { return TRUE; }
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) { return TRUE; }
    RECT r{};
    if (!GetClientRect(h, &r)) { return TRUE; }
    const LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > p->area) { p->area = area; p->best = h; }
    return TRUE;
}

static HWND find_window_of_pid(DWORD pid)
{
    struct Pick { DWORD pid; HWND best; LONG area; } p{ pid, nullptr, 0 };
    EnumWindows(pick_by_pid_proc, reinterpret_cast<LPARAM>(&p));
    return p.best;
}

static HWND find_game_window(const wchar_t* process_name)
{
    DWORD pids[8]{};
    int   pid_count = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return nullptr; }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, process_name) == 0) {
                if (pid_count < 8) { pids[pid_count++] = pe.th32ProcessID; }
            }
        } while (Process32NextW(snap, &pe));
    }
    if (pid_count == 0 && process_name[0]) {
        // Fall back to a substring match: UE5 titles often ship as
        // "Something-Win64-Shipping.exe" while the launcher is the short name.
        std::wstring stem(process_name);
        const size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) { stem.erase(dot); }
        if (Process32FirstW(snap, &pe)) {
            do {
                if (stem.size() >= 2 && _wcsnicmp(pe.szExeFile, stem.c_str(), stem.size()) == 0) {
                    if (pid_count < 8) { pids[pid_count++] = pe.th32ProcessID; }
                }
            } while (Process32NextW(snap, &pe));
        }
    }
    CloseHandle(snap);

    if (pid_count == 0) {
        logf_("[lock] no process named %ls is running\n", process_name);
        return nullptr;
    }

    FindWindowCtx ctx{};
    for (int i = 0; i < pid_count; ++i) {
        ctx.pid = pids[i];
        ctx.windows = 0;
        EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
        logf_("[lock] pid %u: %d visible window(s)\n", pids[i], ctx.windows);
    }
    if (ctx.best) {
        RECT r{};
        GetClientRect(ctx.best, &r);
        logf_("[lock] using window %p of pid %u (client %ldx%ld)\n",
              reinterpret_cast<void*>(ctx.best), ctx.best_pid,
              static_cast<long>(r.right - r.left), static_cast<long>(r.bottom - r.top));
    } else {
        logf_("[lock] %d matching process(es) but none has a visible window\n", pid_count);
    }
    return ctx.best;
}

