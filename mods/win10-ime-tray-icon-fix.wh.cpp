// ==WindhawkMod==
// @id              win10-ime-tray-icon-fix
// @name            Windows 10 IME Tray Icon Fix
// @description     Stops the notification area from reflowing when the input language changes.
// @version         1.0
// @author          yumeyao
// @github          https://github.com/yumeyao
// @include         explorer.exe
// @compilerOptions -lcomctl32 -luser32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Windows 10 IME Tray Icon Fix

## Default Stock Layout And The Bug

```text
  Screen Left  <-----------------------------------------------------  Screen Right Edge
  [Chevron ^]  |  [SysPager (Tray Icons)]  |  [IME Indicator]  |  [Clock]  |  [Show Desktop]
```

The notification area (`TrayNotifyWnd`) is **anchored to the right edge** of the screen, while child windows are laid out left-to-right. Notice that the **IME Indicator sits right in the middle**, between the third-party tray icons (`SysPager`) and the clock.

When **"Let me set a different input method for each app window"** is enabled in a multilingual setup (e.g., English, Chinese, Japanese):
1. While typing in an app, the indicator often displays **2 glyphs** (e.g., `CH` + `中` or `JP` + `あ`), occupying a wider slot.
2. When you click a tray icon (such as Steam or WiFi), `WM_LBUTTONDOWN` immediately shifts focus to `explorer.exe`.
3. The shell defaults to English, shrinking the indicator down to **1 glyph** (`ENG`).
4. Because the clock on the right is pinned to the right edge, the shrunk width forces **everything to the left of the indicator (`SysPager` and `Chevron ^`) to slide rightward**.
5. When `WM_LBUTTONUP` fires, the icon you aimed at has moved, and your click registers on whichever neighboring icon slid under your cursor.

## Fix Modes

#### Mode 1: Move to the Left of the Chevron (Recommended)
```text
[IME Indicator]  |  [Chevron ^]  |  [SysPager (Tray Icons)]  |  [Clock]  |  [Show Desktop]
```
Relocates the IME indicator to index 0 (the far left of the entire tray), mimicking the classic Windows 7 docked Language Bar. Even if the indicator expands or contracts, it only extends into empty taskbar space to its left. All tray icons and the clock remain completely static relative to the right edge, permanently eliminating misclicks.

#### Mode 2: Lock Width to 2 Glyphs (Fixed-Width Placeholder)
Keeps the original layout order unchanged, but intercepts width reporting (private message `0x464`). It forces the indicator to always report and occupy a fixed 2-glyph width, leaving padding when only 1 glyph (`ENG`) is active. Because the container width never changes, the layout never reflows.

#### Mode 3: Remove Secondary Mode Glyphs (Single-Glyph Mode)
Keeps the original layout order, but strips secondary mode indicators (`中`, `英`, `あ`, `A`), forcing the indicator to only show the current IME (`ENG`, `拼`, `Ⓙ`). The indicator remains permanently locked at a single-glyph width, preventing any width fluctuation.

## Notes and known limitations

- Source code and reverse engineering credit goes to AI (hy4 @ workbuddy).
- After enabling this mod, the notification area may appear blank initially. This is simply because a redraw is not immediately triggered. Switching the IME via your hotkey or triggering any tray update will instantly force a redraw.
- Tested on Windows 10 21H2. Windows 10 20H2 and above (including LTSC 2021, Builds 19042–19045) share the exact same binary codebase, so it should work flawlessly. Earlier builds (1903/1909 and prior) should theoretically work as well, but remain untested.

*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- moveToLeftOfChevron: true
  $name: Move the input indicator to the left of the chevron
  $description: Puts the indicator at chain position 1, so a width change cannot move any other tray window. This is the recommended fix and works on its own.
- fixedWidth: false
  $name: Reserve a fixed width
  $description: Keeps a constant slot so the notification area never changes width. Leaves a small blank area in the one-glyph state. Ignored when "Hide the supplementary icon" is on.
- hideSupplementaryIcons: false
  $name: Hide the supplementary IME mode icon
  $description: Hides the IME mode button (中 / 英 / あ) so only the main language glyph is drawn and the width is naturally constant. Takes priority over "Reserve a fixed width".
- extraPadding: 0
  $name: Extra padding (pixels)
  $description: Extra pixels added to the constant width. Increase this only if the two-glyph state is clipped on your system.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <commctrl.h>

#include <windhawk_api.h>
#include <windhawk_utils.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cwchar>

namespace {

// Private message used by CTrayNotify to ask a notification-area component
// for its minimum size. The reply packs a SIZE as MAKELONG(cx, cy).
constexpr UINT kTrayGetMinSizeMsg = 0x0464;  // WM_USER + 0x64

constexpr wchar_t kShellTrayClass[] = L"Shell_TrayWnd";
constexpr wchar_t kSecondaryShellTrayClass[] = L"Shell_SecondaryTrayWnd";
constexpr wchar_t kTrayNotifyClass[] = L"TrayNotifyWnd";
constexpr wchar_t kInputIndicatorClass[] = L"TrayInputIndicatorWClass";
constexpr wchar_t kMainButtonClass[] = L"InputIndicatorButton";
constexpr wchar_t kImeModeButtonClass[] = L"IMEModeButton";

// CTrayChevron::RuntimeClassInitialize creates the chevron with (HMENU)0x5DE.
constexpr int kChevronCtrlId = 0x05DE;

std::atomic<bool> g_moveToLeft{true};
std::atomic<bool> g_fixedWidth{false};
std::atomic<bool> g_hideSupplementary{false};
std::atomic<int> g_extraPadding{0};

bool IsShellTrayClass(const wchar_t* className) {
    return wcscmp(className, kShellTrayClass) == 0 ||
           wcscmp(className, kSecondaryShellTrayClass) == 0;
}

void LoadSettings() {
    g_moveToLeft.store(Wh_GetIntSetting(L"moveToLeftOfChevron") != 0,
                       std::memory_order_relaxed);
    g_fixedWidth.store(Wh_GetIntSetting(L"fixedWidth") != 0,
                       std::memory_order_relaxed);
    g_hideSupplementary.store(
        Wh_GetIntSetting(L"hideSupplementaryIcons") != 0,
        std::memory_order_relaxed);
    int padding = Wh_GetIntSetting(L"extraPadding");
    g_extraPadding.store(std::max(0, std::min(padding, 64)),
                         std::memory_order_relaxed);
}

// Returns a width derived only from cy, which does not depend on the glyph
// count. singleGlyph uses the one-glyph ratio, otherwise the two-glyph ratio.
// SM_CYBORDER is 1 at 96 DPI and 2 on some scaled systems; take the larger
// of both ratios so the result is never too narrow.
int ConstantWidth(int cy, bool singleGlyph) {
    int byDpi1;
    int byDpi2;
    if (singleGlyph) {
        byDpi1 = MulDiv(36, cy, 34);  // (2*1 + 34) * cy / (2*1 + 32)
        byDpi2 = MulDiv(38, cy, 36);  // (2*2 + 34) * cy / (2*2 + 32)
    } else {
        byDpi1 = MulDiv(58, cy, 34);  // (4*1 + 54) * cy / (2*1 + 32)
        byDpi2 = MulDiv(62, cy, 36);  // (4*2 + 54) * cy / (2*2 + 32)
    }

    // +1 absorbs the double rounding of MulDiv(value, dpi, 96).
    int width = std::max(byDpi1, byDpi2) + 1 +
                g_extraPadding.load(std::memory_order_relaxed);
    return std::min(width, 0xFFFF);
}

// ---------------------------------------------------------------------------
// 0x464 subclass: constant-width strategies
// ---------------------------------------------------------------------------

LRESULT CALLBACK InputIndicatorSubclassProc(HWND hWnd,
                                            UINT uMsg,
                                            WPARAM wParam,
                                            LPARAM lParam,
                                            DWORD_PTR /*dwRefData*/) {
    LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

    // A zero reply means the indicator is hidden. Keep it that way.
    if (uMsg != kTrayGetMinSizeMsg || result == 0) {
        return result;
    }

    WORD cy = HIWORD(result);
    if (cy == 0) {
        return result;
    }

    // Hiding the supplementary icon takes priority over reserving width.
    bool hide = g_hideSupplementary.load(std::memory_order_relaxed);
    bool fixed = g_fixedWidth.load(std::memory_order_relaxed);
    if (!hide && !fixed) {
        return result;
    }

    return MAKELONG(ConstantWidth(cy, hide), cy);
}

void AttachSubclass(HWND hWnd) {
    if (!WindhawkUtils::SetWindowSubclassFromAnyThread(
            hWnd, InputIndicatorSubclassProc, 0)) {
        Wh_Log(L"Failed to subclass input indicator %p", hWnd);
    }
}

void DetachSubclass(HWND hWnd) {
    WindhawkUtils::RemoveWindowSubclassFromAnyThread(
        hWnd, InputIndicatorSubclassProc);
}

// ---------------------------------------------------------------------------
// Window enumeration
// ---------------------------------------------------------------------------

struct EnumContext {
    void (*callback)(HWND);
};

BOOL CALLBACK EnumDescendants(HWND hWnd, LPARAM lParam) {
    wchar_t className[64];
    if (GetClassNameW(hWnd, className, ARRAYSIZE(className)) != 0 &&
        wcscmp(className, kInputIndicatorClass) == 0) {
        auto* context = reinterpret_cast<EnumContext*>(lParam);
        context->callback(hWnd);
    }
    return TRUE;
}

BOOL CALLBACK EnumTopLevel(HWND hWnd, LPARAM lParam) {
    wchar_t className[64];
    if (GetClassNameW(hWnd, className, ARRAYSIZE(className)) != 0 &&
        IsShellTrayClass(className)) {
        // EnumChildWindows walks all descendants, so TrayNotifyWnd and the
        // secondary taskbars on multi-monitor setups are covered too.
        EnumChildWindows(hWnd, EnumDescendants, lParam);
    }
    return TRUE;
}

void ForEachInputIndicator(void (*callback)(HWND)) {
    EnumContext context{callback};
    EnumWindows(EnumTopLevel, reinterpret_cast<LPARAM>(&context));
}

// ---------------------------------------------------------------------------
// DeferWindowPos batch tracking for the move-to-left transform
// ---------------------------------------------------------------------------

using DeferWindowPos_t = HDWP(WINAPI*)(HDWP, HWND, HWND, int, int, int, int,
                                       UINT);
using EndDeferWindowPos_t = BOOL(WINAPI*)(HDWP);
using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                        int, int, int, HWND, HMENU, HINSTANCE,
                                        LPVOID);

DeferWindowPos_t g_originalDeferWindowPos = nullptr;
EndDeferWindowPos_t g_originalEndDeferWindowPos = nullptr;
CreateWindowExW_t g_originalCreateWindowExW = nullptr;

constexpr int kMaxEntries = 16;
constexpr int kMaxBatches = 4;
constexpr int kMaxParents = 8;

struct Entry {
    HWND hwnd;
    int x;
    int y;
    int cx;
    int cy;
};

struct Batch {
    bool active = false;
    HDWP hdwp = nullptr;
    DWORD threadId = 0;
    HWND parent = nullptr;
    DWORD startTick = 0;
    Entry entries[kMaxEntries]{};
    int count = 0;
};

SRWLOCK g_lock = SRWLOCK_INIT;
Batch g_batches[kMaxBatches];
HWND g_trayNotifyParents[kMaxParents]{};
int g_trayNotifyParentCount = 0;
HWND g_otherParents[kMaxParents]{};
int g_otherParentNext = 0;

// Must be called with g_lock held exclusively.
bool IsTrayNotifyParentLocked(HWND parent) {
    if (!parent) {
        return false;
    }
    for (int i = 0; i < g_trayNotifyParentCount; ++i) {
        if (g_trayNotifyParents[i] == parent) {
            return true;
        }
    }
    // Negative cache: most DeferWindowPos calls in explorer.exe belong to
    // other parts of the taskbar. Avoid a GetClassNameW call for those.
    for (HWND other : g_otherParents) {
        if (other == parent) {
            return false;
        }
    }

    wchar_t className[32];
    if (GetClassNameW(parent, className, ARRAYSIZE(className)) == 0 ||
        wcscmp(className, kTrayNotifyClass) != 0) {
        g_otherParents[g_otherParentNext] = parent;
        g_otherParentNext = (g_otherParentNext + 1) % kMaxParents;
        return false;
    }

    if (g_trayNotifyParentCount <
        static_cast<int>(ARRAYSIZE(g_trayNotifyParents))) {
        g_trayNotifyParents[g_trayNotifyParentCount++] = parent;
    }
    return true;
}

bool IsInputIndicator(HWND hwnd) {
    wchar_t className[64];
    return GetClassNameW(hwnd, className, ARRAYSIZE(className)) != 0 &&
           wcscmp(className, kInputIndicatorClass) == 0;
}

void ApplyReorder(const Batch& batch) {
    if (batch.count < 3) {
        return;
    }

    int inputIndex = -1;
    int chevronIndex = -1;
    int leftmostIndex = -1;
    int leftmostX = INT_MAX;
    for (int i = 0; i < batch.count; ++i) {
        const Entry& e = batch.entries[i];
        if (IsInputIndicator(e.hwnd)) {
            inputIndex = i;
        }
        if (GetDlgCtrlID(e.hwnd) == kChevronCtrlId) {
            chevronIndex = i;
        }
        if (e.x < leftmostX) {
            leftmostX = e.x;
            leftmostIndex = i;
        }
    }
    if (inputIndex < 0) {
        return;
    }
    if (chevronIndex < 0) {
        chevronIndex = leftmostIndex;
    }
    if (chevronIndex < 0 || chevronIndex == inputIndex) {
        return;
    }

    const Entry& input = batch.entries[inputIndex];
    const Entry& chevron = batch.entries[chevronIndex];
    int inputX = input.x;
    int inputWidth = input.cx;
    int chevronRight = chevron.x + chevron.cx;

    // explorer normally passes the real cx. If a layout pass positions the
    // indicator with SWP_NOSIZE, the recorded cx can be zero, so fall back
    // to the live window width.
    if (inputWidth <= 0) {
        RECT rc{};
        if (GetWindowRect(input.hwnd, &rc)) {
            inputWidth = rc.right - rc.left;
        }
    }

    // Guard against a hidden indicator, an unexpected order, and the
    // vertical / tablet-mode layout where children stack along y.
    if (inputWidth <= 0 || inputX <= chevron.x ||
        chevronRight > inputX || input.y != chevron.y) {
        return;
    }

    int newX[kMaxEntries];
    for (int i = 0; i < batch.count; ++i) {
        newX[i] = batch.entries[i].x;
    }

    // The indicator becomes chain position 1. Everything that used to be to
    // its left shifts right by its width; everything to its right is left
    // alone. Total width T is unchanged.
    newX[inputIndex] = chevron.x;
    for (int i = 0; i < batch.count; ++i) {
        if (i != inputIndex && batch.entries[i].x < inputX) {
            newX[i] = batch.entries[i].x + inputWidth;
        }
    }

    HDWP hdwp = BeginDeferWindowPos(batch.count);
    if (!hdwp) {
        return;
    }
    for (int i = 0; i < batch.count; ++i) {
        const Entry& e = batch.entries[i];
        // Use the trampolines so this second batch does not re-enter our
        // hooks. SWP_NOSIZE keeps the original width and height.
        hdwp = g_originalDeferWindowPos(
            hdwp, e.hwnd, nullptr, newX[i], e.y, 0, 0,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
        if (!hdwp) {
            return;
        }
    }
    g_originalEndDeferWindowPos(hdwp);
}

// ---------------------------------------------------------------------------
// Supplementary-icon hiding
// ---------------------------------------------------------------------------

// Applies the child-window override immediately, without waiting for the
// next layout pass. Uses SetWindowPos so it does not re-enter our hooks.
// When the option is off, nothing is done here: explorer's next layout pass
// (or the next input language switch) restores both children through the
// unmodified DeferWindowPos path.
void ApplyChildOverride(HWND host) {
    if (!g_hideSupplementary.load(std::memory_order_relaxed)) {
        return;
    }

    HWND imeMode =
        FindWindowExW(host, nullptr, kImeModeButtonClass, nullptr);
    HWND main = FindWindowExW(host, nullptr, kMainButtonClass, nullptr);
    if (!imeMode && !main) {
        return;
    }

    RECT hostRect{};
    if (!GetClientRect(host, &hostRect)) {
        return;
    }
    int hostWidth = hostRect.right - hostRect.left;

    if (imeMode) {
        SetWindowPos(imeMode, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (main) {
        RECT mainRect{};
        if (GetWindowRect(main, &mainRect)) {
            int mainWidth = mainRect.right - mainRect.left;
            int x = (hostWidth - mainWidth) / 2;
            SetWindowPos(main, nullptr, x, 0, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

HDWP WINAPI DeferWindowPos_Hook(HDWP hWinPosInfo,
                                HWND hWnd,
                                HWND hWndInsertAfter,
                                int x,
                                int y,
                                int cx,
                                int cy,
                                UINT uFlags) {
    bool reorder = g_moveToLeft.load(std::memory_order_relaxed);
    bool hide = g_hideSupplementary.load(std::memory_order_relaxed);

    if (hWinPosInfo && hWnd && reorder) {
        HWND parent = GetParent(hWnd);
        DWORD threadId = GetCurrentThreadId();
        DWORD now = GetTickCount();
        AcquireSRWLockExclusive(&g_lock);
        if (IsTrayNotifyParentLocked(parent)) {
            Batch* batch = nullptr;
            for (Batch& candidate : g_batches) {
                if (candidate.active && candidate.hdwp == hWinPosInfo &&
                    candidate.threadId == threadId) {
                    batch = &candidate;
                    break;
                }
            }
            if (!batch) {
                // DeferWindowPos may return a different HDWP when it has to
                // grow the array. Continue the same batch if the thread and
                // the parent match, otherwise start a fresh one.
                for (Batch& candidate : g_batches) {
                    if (candidate.active && candidate.threadId == threadId &&
                        candidate.parent == parent &&
                        candidate.count < kMaxEntries) {
                        candidate.hdwp = hWinPosInfo;
                        batch = &candidate;
                        break;
                    }
                }
            }
            if (!batch) {
                for (Batch& candidate : g_batches) {
                    if (!candidate.active) {
                        candidate = Batch{};
                        candidate.active = true;
                        candidate.hdwp = hWinPosInfo;
                        candidate.threadId = threadId;
                        candidate.parent = parent;
                        candidate.startTick = now;
                        batch = &candidate;
                        break;
                    }
                }
                // Recover a slot if an earlier batch never reached
                // EndDeferWindowPos (for example after a failed layout).
                if (!batch) {
                    for (Batch& candidate : g_batches) {
                        if (now - candidate.startTick > 5000) {
                            candidate = Batch{};
                            candidate.active = true;
                            candidate.hdwp = hWinPosInfo;
                            candidate.threadId = threadId;
                            candidate.parent = parent;
                            candidate.startTick = now;
                            batch = &candidate;
                            break;
                        }
                    }
                }
            }
            if (batch && batch->count < kMaxEntries) {
                Entry& entry = batch->entries[batch->count++];
                entry.hwnd = hWnd;
                entry.x = x;
                entry.y = y;
                entry.cx = cx;
                entry.cy = cy;
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }

    int outX = x;
    int outCx = cx;
    int outCy = cy;
    if (hide && hWinPosInfo && hWnd) {
        wchar_t className[64];
        if (GetClassNameW(hWnd, className, ARRAYSIZE(className)) != 0) {
            if (wcscmp(className, kImeModeButtonClass) == 0) {
                // A zero-size child is not painted. explorer passes 0x114
                // (no SWP_SHOWWINDOW / SWP_HIDEWINDOW), so no visibility
                // state needs to be tracked and unloading restores it.
                outCx = 0;
                outCy = 0;
            } else if (wcscmp(className, kMainButtonClass) == 0) {
                HWND parent = GetParent(hWnd);
                RECT rc{};
                if (parent && GetClientRect(parent, &rc)) {
                    outX = ((rc.right - rc.left) - cx) / 2;
                }
            }
        }
    }

    return g_originalDeferWindowPos(hWinPosInfo, hWnd, hWndInsertAfter, outX,
                                    y, outCx, outCy, uFlags);
}

BOOL WINAPI EndDeferWindowPos_Hook(HDWP hWinPosInfo) {
    BOOL result = g_originalEndDeferWindowPos(hWinPosInfo);

    if (g_moveToLeft.load(std::memory_order_relaxed)) {
        Batch snapshot{};
        bool found = false;
        DWORD threadId = GetCurrentThreadId();
        AcquireSRWLockExclusive(&g_lock);
        for (Batch& batch : g_batches) {
            if (batch.active && batch.hdwp == hWinPosInfo &&
                batch.threadId == threadId) {
                snapshot = batch;
                batch.active = false;
                batch.count = 0;
                found = true;
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (found) {
            ApplyReorder(snapshot);
        }
    }

    return result;
}

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle,
                                 LPCWSTR lpClassName,
                                 LPCWSTR lpWindowName,
                                 DWORD dwStyle,
                                 int x,
                                 int y,
                                 int nWidth,
                                 int nHeight,
                                 HWND hWndParent,
                                 HMENU hMenu,
                                 HINSTANCE hInstance,
                                 LPVOID lpParam) {
    HWND hWnd = g_originalCreateWindowExW(
        dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, nWidth, nHeight,
        hWndParent, hMenu, hInstance, lpParam);

    // The class name can also be an integer atom. IS_INTRESOURCE guards that.
    if (hWnd && lpClassName && !IS_INTRESOURCE(lpClassName)) {
        if (wcscmp(lpClassName, kInputIndicatorClass) == 0) {
            // This hook runs on the thread that owns the new window, so the
            // single-threaded subclass path is used here.
            AttachSubclass(hWnd);
            ApplyChildOverride(hWnd);
        } else if (wcscmp(lpClassName, kImeModeButtonClass) == 0 ||
                   wcscmp(lpClassName, kMainButtonClass) == 0) {
            HWND parent = GetParent(hWnd);
            if (parent) {
                ApplyChildOverride(parent);
            }
        }
    }

    return hWnd;
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

// CTrayNotify::v_WndProc answers 0x464 with _CalcMinSize, which is exactly
// the value the taskbar uses to size TrayNotifyWnd. Applying that size with
// the right edge fixed produces a real WM_SIZE; CTrayNotify::v_WndProc then
// runs _SizeWindows(apply=true), which rebuilds the 11-window batch that our
// hooks transform.
BOOL CALLBACK RefreshOneTaskbar(HWND shellTray, LPARAM lParam) {
    wchar_t className[64];
    if (GetClassNameW(shellTray, className, ARRAYSIZE(className)) == 0 ||
        !IsShellTrayClass(className)) {
        return TRUE;
    }

    HWND trayNotify =
        FindWindowExW(shellTray, nullptr, kTrayNotifyClass, nullptr);
    if (!trayNotify) {
        return TRUE;
    }

    RECT rc{};
    if (!GetWindowRect(trayNotify, &rc) || IsRectEmpty(&rc)) {
        return TRUE;
    }
    int currentCx = rc.right - rc.left;
    int currentCy = rc.bottom - rc.top;

    DWORD_PTR minSize = 0;
    if (!SendMessageTimeoutW(trayNotify, kTrayGetMinSizeMsg,
                             static_cast<WPARAM>(currentCx),
                             static_cast<LPARAM>(currentCy),
                             SMTO_ABORTIFHUNG, 1000, &minSize) ||
        minSize == 0) {
        return TRUE;
    }

    int requiredCx = LOWORD(minSize);
    if (requiredCx <= 0) {
        requiredCx = currentCx;
    }
    if (requiredCx != currentCx) {
        SetWindowPos(trayNotify, nullptr, rc.right - requiredCx, rc.top,
                     requiredCx, currentCy, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Always force one apply=true layout pass, even when the size did not
    // change, so the move-to-left transform takes effect immediately.
    LPARAM size = MAKELPARAM(requiredCx, currentCy);
    if (lParam) {
        DWORD_PTR unused = 0;
        SendMessageTimeoutW(trayNotify, WM_SIZE, SIZE_RESTORED, size,
                            SMTO_ABORTIFHUNG, 1000, &unused);
    } else {
        PostMessageW(trayNotify, WM_SIZE, SIZE_RESTORED, size);
    }
    return TRUE;
}

// Nudges CTrayInputIndicator into running _ResizeChildWindows, which is what
// restores the two child buttons after the hide option is turned off.
void NudgeIndicator(HWND host) {
    RECT rc{};
    if (!GetClientRect(host, &rc)) {
        return;
    }
    LPARAM size = MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top);
    DWORD_PTR unused = 0;
    SendMessageTimeoutW(host, WM_SIZE, SIZE_RESTORED, size,
                        SMTO_ABORTIFHUNG, 1000, &unused);
}

// Synchronous mode is used from settings change and uninit, where the hooks
// are already installed. Wh_ModInit posts instead, because Windhawk applies
// hooks only after Wh_ModInit returns.
void RefreshAll(bool synchronous) {
    EnumWindows(RefreshOneTaskbar, synchronous ? 1 : 0);
    if (synchronous) {
        ForEachInputIndicator(NudgeIndicator);
    }
    ForEachInputIndicator(ApplyChildOverride);
}

}  // namespace

BOOL Wh_ModInit() {
    Wh_Log(L"Init");
    LoadSettings();

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(&DeferWindowPos),
            reinterpret_cast<void*>(&DeferWindowPos_Hook),
            reinterpret_cast<void**>(&g_originalDeferWindowPos)) ||
        !Wh_SetFunctionHook(
            reinterpret_cast<void*>(&EndDeferWindowPos),
            reinterpret_cast<void*>(&EndDeferWindowPos_Hook),
            reinterpret_cast<void**>(&g_originalEndDeferWindowPos)) ||
        !Wh_SetFunctionHook(
            reinterpret_cast<void*>(&CreateWindowExW),
            reinterpret_cast<void*>(&CreateWindowExW_Hook),
            reinterpret_cast<void**>(&g_originalCreateWindowExW))) {
        Wh_Log(L"Failed to install hooks");
        return FALSE;
    }

    // Cover taskbars that already exist when the mod is enabled at runtime.
    ForEachInputIndicator(AttachSubclass);
    RefreshAll(false);

    return TRUE;
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"SettingsChanged");
    LoadSettings();
    RefreshAll(true);
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");

    // Stop transforming first, then force one more layout pass so the
    // original left-to-right order and both child buttons are restored
    // before the DLL is unloaded.
    g_moveToLeft.store(false, std::memory_order_relaxed);
    g_fixedWidth.store(false, std::memory_order_relaxed);
    g_hideSupplementary.store(false, std::memory_order_relaxed);
    ForEachInputIndicator(DetachSubclass);
    RefreshAll(true);
}
