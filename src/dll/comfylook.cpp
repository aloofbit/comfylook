// comfylook.dll: mouselook from raw mouse input.
//
// The 1.12 client turns the camera by the distance the hidden cursor moves. Windows applies its
// pointer speed and "Enhance pointer precision" to that distance, so a fast hand move turns the camera
// much farther than a slow one over the same distance on the desk. This DLL reads the mouse with raw
// input instead and gives the client those counts, scaled by the client's own mouseSpeed setting.
//
// 1. It subclasses the game window and registers it for raw mouse input.
// 2. While mouselook is on, each WM_INPUT gives a turn event to the client's input queue. When the last
//    event in the queue is a turn the client has not read yet, the delta is added to it, so a 1000 Hz
//    mouse cannot fill the 16-slot queue.
// 3. While mouselook is on, WM_MOUSEMOVE does not reach the client, so the cursor gives no turn of its
//    own. The DLL puts the hidden cursor back at the client's centre instead.
// 4. Synergy, a remote desktop and a touch screen send absolute positions. The turn for those is the
//    distance between two positions from the sender, not the distance from the centre.
//
// Nothing in WoW.exe is patched. The DLL checks the client's bytes first and does nothing on a client it
// does not know. src/README.md has each address and how it was checked.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    wchar_t g_logPath[MAX_PATH] = {};   // Logs\comfylook.log, in the client folder

    void Log(const char* fmt, ...)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_logPath, L"a") != 0 || !f)
            return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }

    intptr_t Slide()
    {
        static const intptr_t slide = reinterpret_cast<intptr_t>(GetModuleHandleW(nullptr)) - 0x00400000;
        return slide;
    }

    template <typename T>
    T& At(uintptr_t addr)
    {
        return *reinterpret_cast<T*>(addr + Slide());
    }

    bool SafeCopy(uintptr_t src, void* dst, size_t n)
    {
        __try
        {
            memcpy(dst, reinterpret_cast<const void*>(src), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The bytes at a client address, -1 for a byte that may be anything.
    bool HeadMatches(uintptr_t addr, const int* head, size_t n)
    {
        unsigned char b[32] = {};
        if (n > sizeof(b) || !SafeCopy(addr + Slide(), b, n))
            return false;
        for (size_t i = 0; i < n; ++i)
            if (head[i] >= 0 && b[i] != head[i])
                return false;
        return true;
    }

#define HEAD(addr, h) HeadMatches(addr, h, sizeof(h) / sizeof(h[0]))

    // ---------------------------------------------------------------------------------------------
    // The client's mouse state

    const uintptr_t kMouseMode   = 0x884E5C;   // 1 while mouselook is on, else 0. Set by 0x42CC90
    const uintptr_t kSpeed       = 0x884CB4;   // mouseSpeed x 10, as a Windows pointer speed 1..20
    const uintptr_t kQueue       = 0x884CC0;   // 16 input events of 5 ints: type, 0, a, b, time
    const uintptr_t kQueueRead   = 0x884E34;   // index of the next event the client reads
    const uintptr_t kQueueWrite  = 0x884E50;   // index of the next free slot
    const uintptr_t kPostEvent   = 0x42D780;   // __fastcall(type, 0, a, b, 0) adds an event
    const uintptr_t kRecenter    = 0x42CD20;   // puts the cursor back at the stored centre
    const uintptr_t kCentre      = 0x884E44;   // the centre, x then y, in client coordinates
    const int       kTurnEvent   = 0xC;        // a = dx, b = dy, in pixels

    using PostEventFn = void(__fastcall*)(int type, int zero, int a, int b, int c);
    using RecenterFn = void(__cdecl*)();

    // The client code the addresses above come from. Each is the start of the function, or the
    // instructions that read or write the address. A client with other bytes here is not the one this
    // DLL was made for.
    const int kSetModeHead[] = {   // 0x42CC90: SetMouseMode(ecx). cmp ecx,2 / jl / ... / mov eax,[0x884E5C]
        0x83, 0xF9, 0x02, 0x7C, 0x08, 0x6A, 0x57, 0xE8, -1, -1, -1, -1, 0xC3,
        0xA1, 0x5C, 0x4E, 0x88, 0x00 };
    const int kPostHead[] = {      // 0x42D780: push ebp / mov ebp,esp / mov eax,[0x884E50] / cmp eax,0xF
        0x55, 0x8B, 0xEC, 0xA1, 0x50, 0x4E, 0x88, 0x00, 0x83, 0xF8, 0x0F };
    const int kRecenterHead[] = {  // 0x42CD20: ... mov eax,[0x884E44] / mov ecx,[0x884E48]
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0xA1, 0x44, 0x4E, 0x88, 0x00, 0x8B, 0x0D, 0x48, 0x4E, 0x88, 0x00 };
    const int kTurnHead[] = {      // 0x42D340: the cursor turn. push 0 / sub ecx,esi / push ecx / sub eax,edx
        0x6A, 0x00, 0x2B, 0xCE, 0x51, 0x2B, 0xC2, 0x50, 0x33, 0xD2, 0xB9, 0x0C, 0x00, 0x00, 0x00 };
    const int kSpeedHead[] = {     // 0x42CDA6: fmul [10.0] / call ftol / ... / mov [0x884CB4],eax
        0xD8, 0x0D, 0x24, 0x16, 0x80, 0x00, 0xE8, -1, -1, -1, -1, 0x8B, 0x0D, 0xB0, 0x33, 0x83, 0x00,
        0x85, 0xC9, 0xA3, 0xB4, 0x4C, 0x88, 0x00 };

    bool ClientMatches()
    {
        struct Check { uintptr_t addr; const int* head; size_t n; const char* what; };
        const Check checks[] = {
            { 0x42CC90, kSetModeHead,  sizeof(kSetModeHead) / sizeof(int),  "SetMouseMode" },
            { 0x42D780, kPostHead,     sizeof(kPostHead) / sizeof(int),     "the event post" },
            { 0x42CD20, kRecenterHead, sizeof(kRecenterHead) / sizeof(int), "the recentre" },
            { 0x42D340, kTurnHead,     sizeof(kTurnHead) / sizeof(int),     "the cursor turn" },
            { 0x42CDA6, kSpeedHead,    sizeof(kSpeedHead) / sizeof(int),    "the mouseSpeed store" },
        };
        bool ok = true;
        for (const Check& c : checks)
        {
            if (!HeadMatches(c.addr, c.head, c.n))
            {
                Log("client check: %s at 0x%06X has other bytes", c.what, static_cast<unsigned>(c.addr));
                ok = false;
            }
        }
        return ok;
    }

    bool MouseLookOn()
    {
        return At<int>(kMouseMode) == 1;
    }

    // mouseSpeed, the in-game "Mouse Sensitivity". 1.0 gives one pixel of turn for each mouse count.
    float Scale()
    {
        int speed = At<int>(kSpeed);
        if (speed < 1 || speed > 20)
            speed = 10;
        return speed / 10.0f;
    }

    // Gives a turn to the client: a distance in mouse counts or pixels, which are the same at Windows
    // pointer speed 10. The fraction below one pixel is kept for the next turn.
    void Turn(float x, float y)
    {
        static float restX = 0, restY = 0;
        const float s = Scale();
        const float fx = x * s + restX;
        const float fy = y * s + restY;
        const int dx = static_cast<int>(std::trunc(fx));
        const int dy = static_cast<int>(std::trunc(fy));
        restX = fx - dx;
        restY = fy - dy;
        if (dx == 0 && dy == 0)
            return;

        int* queue = &At<int>(kQueue);
        const int read = At<int>(kQueueRead);
        const int write = At<int>(kQueueWrite);
        if (read != write && read >= 0 && read < 16 && write >= 0 && write < 16)
        {
            int* last = queue + ((write + 15) & 15) * 5;
            if (last[0] == kTurnEvent)
            {
                last[2] += dx;
                last[3] += dy;
                return;
            }
        }
        reinterpret_cast<PostEventFn>(kPostEvent + Slide())(kTurnEvent, 0, dx, dy, 0);
    }

    // ---------------------------------------------------------------------------------------------
    // The game window

    HWND g_window = nullptr;          // the window that has our procedure
    WNDPROC g_next = nullptr;         // its procedure before ours
    bool g_registered = false;        // raw input is registered for g_window

    // One mouselook drag, for the log. A drag where the cursor went much farther than the raw counts
    // shows the Windows acceleration that comfylook takes out.
    struct Drag
    {
        bool on = false;
        DWORD start = 0;
        long events = 0, absolute = 0;   // raw events: relative, absolute
        long long rawX = 0, rawY = 0;    // sum of |raw counts|
        long moves = 0;                  // WM_MOUSEMOVE taken from the client
        long long curX = 0, curY = 0;    // sum of |cursor - centre| in those moves
    };
    Drag g_drag;

    // The first raw event of each device, for the log: its name, and whether it is relative.
    void NoteDevice(HANDLE device, USHORT flags)
    {
        static HANDLE seen[16] = {};
        static int count = 0;
        for (int i = 0; i < count; ++i)
            if (seen[i] == device)
                return;
        if (count < 16)
            seen[count++] = device;
        char name[256] = "(injected input, no device)";
        if (device)
        {
            UINT n = sizeof(name);
            if (GetRawInputDeviceInfoA(device, RIDI_DEVICENAME, name, &n) == static_cast<UINT>(-1))
                strcpy_s(name, "(name unknown)");
        }
        Log("device %p: %s, %s", device, flags & MOUSE_MOVE_ABSOLUTE ? "absolute" : "relative", name);
    }

    // ---------------------------------------------------------------------------------------------
    // The cursor after a drag with Synergy
    //
    // When mouselook ends, the client puts the cursor back where the drag started. Synergy moved its own
    // cursor during the drag and does not see that, so its next move puts the cursor where its own cursor
    // is. A low-level mouse hook maps each later Synergy move to a cursor position, so the cursor stays
    // where the drag started.
    //
    // The map has two straight parts on each axis. Synergy's position at the end of the drag gives the
    // drag start, and each screen edge gives the same edge. So Synergy reaches its edge when the cursor
    // reaches the screen edge: each part of the screen stays in reach, and Synergy changes screen at the
    // visible edge. When Synergy reaches an edge, the two positions are the same and the map on that axis
    // ends. A fixed offset in place of the map made part of the screen unreachable, and each drag added
    // to the offset.
    //
    // The hook starts only after the first absolute raw event, so a normal mouse never has it.

    HMODULE g_module = nullptr;
    std::atomic<long> g_senderX{ 0 }, g_senderY{ 0 }; // the newest position from the sender
    std::atomic<bool> g_haveSender{ false };
    std::atomic<bool> g_absFresh{ false };            // the hook dropped moves: no distance to the next one

    // A part of the map moves the cursor at most this many pixels for each Synergy pixel, and at least
    // the inverse. A larger change in speed is easy to feel. Past that, the cursor goes to Synergy's
    // position. A short drag moves Synergy a short way, so the cursor stays at the drag start.
    const double kMaxSlope = 1.25;

    struct Axis
    {
        std::atomic<bool> on{ false };
        std::atomic<long> sender{ 0 };   // Synergy's position at the end of the drag
        std::atomic<long> cursor{ 0 };   // the drag start
    };
    Axis g_mapX, g_mapY;

    void ScreenBounds(long& l, long& t, long& r, long& b)
    {
        l = GetSystemMetrics(SM_XVIRTUALSCREEN);
        t = GetSystemMetrics(SM_YVIRTUALSCREEN);
        r = l + GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1;
        b = t + GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1;
    }

    // False if a part is too steep or too flat. Synergy against an edge gives a part of no width.
    bool AxisFits(long sender, long cursor, long lo, long hi)
    {
        if (sender == cursor)
            return true;
        if (sender <= lo || sender >= hi || cursor <= lo || cursor >= hi)
            return false;
        const double low = static_cast<double>(cursor - lo) / (sender - lo);
        const double high = static_cast<double>(hi - cursor) / (hi - sender);
        return low <= kMaxSlope && low >= 1 / kMaxSlope && high <= kMaxSlope && high >= 1 / kMaxSlope;
    }

    void SetAxis(Axis& axis, long sender, long cursor)
    {
        axis.on = false;
        axis.sender = sender;
        axis.cursor = cursor;
        axis.on = sender != cursor;
    }

    long MapAxis(Axis& axis, long p, long lo, long hi)
    {
        if (!axis.on)
            return p;
        if (p <= lo || p >= hi)
        {
            axis.on = false;
            return p;
        }
        const long s = axis.sender, c = axis.cursor;
        if (p <= s)
            return lo + std::lround(static_cast<double>(p - lo) * (c - lo) / (s - lo));
        return c + std::lround(static_cast<double>(p - s) * (hi - c) / (hi - s));
    }

    LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp)
    {
        if (code == HC_ACTION && wp == WM_MOUSEMOVE)
        {
            const MSLLHOOKSTRUCT* h = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            if (h->flags & LLMHF_INJECTED)
            {
                g_senderX = h->pt.x;
                g_senderY = h->pt.y;
                g_haveSender = true;
                if (g_mapX.on || g_mapY.on)
                {
                    if (!g_window || GetForegroundWindow() != g_window)
                    {
                        // Out of the game, the cursor goes where Synergy puts it.
                        g_mapX.on = false;
                        g_mapY.on = false;
                    }
                    else if (!MouseLookOn())
                    {
                        long l, t, r, b;
                        ScreenBounds(l, t, r, b);
                        const long x = MapAxis(g_mapX, h->pt.x, l, r);
                        const long y = MapAxis(g_mapY, h->pt.y, t, b);
                        g_absFresh = true;
                        SetCursorPos(x, y);
                        return 1;
                    }
                }
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    DWORD WINAPI HookThread(LPVOID)
    {
        HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, g_module, 0);
        if (!hook)
        {
            Log("mouse hook: SetWindowsHookEx failed, error %lu", GetLastError());
            return 0;
        }
        Log("mouse hook: in place, for the cursor after a drag with absolute input");
        // A low-level hook runs on the thread that set it, from that thread's message loop.
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
        }
        return 0;
    }

    void StartHook()
    {
        static bool started = false;
        if (started)
            return;
        started = true;
        if (HANDLE t = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr))
            CloseHandle(t);
    }

    void DragStart()
    {
        g_drag = Drag();
        g_drag.on = true;
        g_drag.start = GetTickCount();
    }

    void DragEnd(HWND hwnd)
    {
        const Drag& d = g_drag;
        g_drag.on = false;

        // The client has put the cursor back where the drag started. Keep it there.
        char held[160] = "";
        POINT centre = { At<LONG>(kCentre), At<LONG>(kCentre + 4) };
        if (d.absolute > 0 && g_haveSender && ClientToScreen(hwnd, &centre))
        {
            const long sx = g_senderX, sy = g_senderY;
            long l, t, r, b;
            ScreenBounds(l, t, r, b);
            // Both axes or neither: a cursor that jumps on one axis is not at the drag start either.
            const bool hold = AxisFits(sx, centre.x, l, r) && AxisFits(sy, centre.y, t, b);
            SetAxis(g_mapX, sx, hold ? centre.x : sx);
            SetAxis(g_mapY, sy, hold ? centre.y : sy);
            sprintf_s(held, "; Synergy at %ld,%ld, drag start %ld,%ld, offset %ld,%ld, cursor %s",
                sx, sy, centre.x, centre.y, centre.x - sx, centre.y - sy,
                hold ? "held" : "follows Synergy");
        }
        Log("drag %lu ms: %ld raw events (%ld absolute), raw %lld x %lld; %ld cursor moves, cursor %lld x %lld; "
            "mouseSpeed scale %.1f%s",
            GetTickCount() - d.start, d.events, d.absolute, d.rawX, d.rawY, d.moves, d.curX, d.curY, Scale(), held);
    }

    void Register(HWND hwnd)
    {
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x01;   // generic desktop
        rid.usUsage = 0x02;       // mouse
        rid.dwFlags = 0;          // legacy messages stay on: the menus and the UI use them
        rid.hwndTarget = hwnd;
        if (RegisterRawInputDevices(&rid, 1, sizeof(rid)))
            Log("raw input: registered for window %p", hwnd);
        else
            Log("raw input: RegisterRawInputDevices failed, error %lu", GetLastError());
    }

    void OnInput(HRAWINPUT handle)
    {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData(handle, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
            return;
        if (ri.header.dwType != RIM_TYPEMOUSE)
            return;
        const RAWMOUSE& m = ri.data.mouse;
        NoteDevice(ri.header.hDevice, m.usFlags);
        if (m.usFlags & MOUSE_MOVE_ABSOLUTE)
        {
            // Synergy, a remote desktop or a touch screen sends positions, not distances. The client
            // turns by the distance from the centre to that position, but the sender does not see the
            // client put the cursor back at the centre, so each turn is the full distance again. The
            // distance between two positions from the sender is the hand movement.
            static float lastX = 0, lastY = 0;
            static DWORD lastTime = 0;
            static bool have = false;
            StartHook();
            if (g_absFresh.exchange(false))
                have = false;
            const bool desktop = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
            const int w = GetSystemMetrics(desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
            const int h = GetSystemMetrics(desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
            const float x = m.lLastX * w / 65536.0f;
            const float y = m.lLastY * h / 65536.0f;
            const DWORD now = GetTickCount();
            // After a pause, a touch screen starts where the finger lands. That is no movement.
            const bool moved = have && now - lastTime < 250;
            const float dx = x - lastX, dy = y - lastY;
            lastX = x;
            lastY = y;
            lastTime = now;
            have = true;
            if (!MouseLookOn())
                return;
            ++g_drag.absolute;
            if (!moved)
                return;
            g_drag.rawX += static_cast<long long>(std::fabs(dx));
            g_drag.rawY += static_cast<long long>(std::fabs(dy));
            Turn(dx, dy);
            return;
        }
        if (!MouseLookOn())
            return;
        ++g_drag.events;
        g_drag.rawX += labs(m.lLastX);
        g_drag.rawY += labs(m.lLastY);
        Turn(static_cast<float>(m.lLastX), static_cast<float>(m.lLastY));
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        // Raw input goes to the thread that registers it, so register here and not from the watcher.
        if (!g_registered && hwnd == g_window)
        {
            g_registered = true;
            Register(hwnd);
        }

        const bool look = MouseLookOn();
        if (look && !g_drag.on)
            DragStart();
        else if (!look && g_drag.on)
            DragEnd(hwnd);

        if (msg == WM_INPUT)
            OnInput(reinterpret_cast<HRAWINPUT>(lp));
        else if (msg == WM_MOUSEMOVE && look)
        {
            // The client would turn by the cursor distance. Put the cursor back at the centre and drop the
            // message. The client does the same move, so the cursor stays where the client expects it.
            POINT now, centre = { At<LONG>(kCentre), At<LONG>(kCentre + 4) };
            if (GetCursorPos(&now) && ClientToScreen(hwnd, &centre) && (now.x != centre.x || now.y != centre.y))
            {
                ++g_drag.moves;
                g_drag.curX += labs(now.x - centre.x);
                g_drag.curY += labs(now.y - centre.y);
                reinterpret_cast<RecenterFn>(kRecenter + Slide())();
            }
            return 0;
        }
        const LRESULT result = CallWindowProcA(g_next, hwnd, msg, wp, lp);
        // The button release that ends mouselook is handled in the call above. End the drag now, before
        // the hook sees the next Synergy move.
        if (g_drag.on && !MouseLookOn())
            DragEnd(hwnd);
        return result;
    }

    // The client's window: a top-level window of this process with the class GxWindowClassD3d or
    // GxWindowClassOpenGl.
    BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId())
            return TRUE;
        char cls[64] = {};
        GetClassNameA(hwnd, cls, sizeof(cls));
        if (strncmp(cls, "GxWindowClass", 13) != 0)
            return TRUE;
        *reinterpret_cast<HWND*>(out) = hwnd;
        return FALSE;
    }

    // The client makes its window after the DLL loads, and makes a new one if the old one closes. Look
    // for it twice a second and put our procedure on each new one.
    DWORD WINAPI Watch(LPVOID)
    {
        for (;;)
        {
            HWND hwnd = nullptr;
            EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&hwnd));
            if (hwnd && hwnd != g_window)
            {
                g_registered = false;
                g_window = hwnd;
                g_next = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
                if (g_next)
                    Log("window %p: procedure in place", hwnd);
                else
                {
                    Log("window %p: SetWindowLongPtr failed, error %lu", hwnd, GetLastError());
                    g_window = nullptr;
                }
            }
            // Another mod can put its procedure on top of ours. That is fine while it calls ours. If
            // mouselook goes back to the old behaviour, this line in the log tells why.
            static bool told = false;
            if (g_window && !told
                && GetWindowLongPtrA(g_window, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&WindowProc))
            {
                Log("window %p: another procedure is now on top of ours", g_window);
                told = true;
            }
            Sleep(500);
        }
    }

    void Attach()
    {
        wchar_t dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        if (wchar_t* slash = wcsrchr(dir, L'\\'))
            slash[1] = 0;
        // The client's own logs are in Logs. The client makes the folder, but later than this.
        wchar_t logs[MAX_PATH] = {};
        swprintf_s(logs, L"%lsLogs", dir);
        CreateDirectoryW(logs, nullptr);
        swprintf_s(g_logPath, L"%lsLogs\\comfylook.log", dir);

        FILE* f = nullptr;
        if (_wfopen_s(&f, g_logPath, L"w") == 0 && f)
            fclose(f);
        Log("comfylook.dll 0.1.1-alpha loaded");

        if (!ClientMatches())
        {
            Log("unknown client: mouselook left as it is");
            return;
        }
        if (HANDLE t = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr))
            CloseHandle(t);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        g_module = module;
        Attach();
    }
    return TRUE;
}
