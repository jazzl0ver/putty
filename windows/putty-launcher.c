#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <commctrl.h>

#include "putty.h"
#include "storage.h"
#include "filestore.h"

#pragma comment(lib, "comctl32.lib")

/* --- Constants and IDs --- */
#define IDC_SEARCH_EDIT  101
#define IDC_SESSION_LIST 102
#define IDC_LOAD_BUTTON  103
#define IDC_NEW_BUTTON   104
#define IDC_TRAY_EXIT    2003

#define TRAY_UID 1
#define WM_TRAY  (WM_APP + 1)

static HINSTANCE ghInst;
static HWND g_hwndMain = NULL;
static HWND g_hwndSearch = NULL;
static HWND g_hwndList = NULL;
static HWND g_hwndLoad = NULL;
static HWND g_hwndNew = NULL;
static NOTIFYICONDATAW gnid;

static const wchar_t *g_wndclass  = L"PuTTYLauncherWindow";
static const wchar_t *g_mutexname = L"PuTTYLauncherSingleton";

/* --- State --- */
static char g_current_path[MAX_PATH] = ""; 

typedef struct {
    char *full_name;  /* "Folder/Session" */
    char *display;    /* "Session" or "Folder" */
    const wchar_t *tag;
    bool is_dir;
} SessionItem;

typedef struct {
    SessionItem *items;
    int count, capacity;
} SessionList;

/* --- Helpers --- */

static void sl_add(SessionList *sl, const char *full_name, const char *display, const wchar_t *tag, bool is_dir) {
    if (sl->count == sl->capacity) {
        sl->capacity = sl->capacity ? sl->capacity * 2 : 128;
        sl->items = sresize(sl->items, sl->capacity, SessionItem);
    }
    sl->items[sl->count].full_name = full_name ? dupstr(full_name) : NULL;
    sl->items[sl->count].display = dupstr(display);
    sl->items[sl->count].tag = tag;
    sl->items[sl->count].is_dir = is_dir;
    sl->count++;
}

static void sl_free(SessionList *sl) {
    for (int i = 0; i < sl->count; i++) {
        sfree(sl->items[i].full_name);
        sfree(sl->items[i].display);
    }
    sfree(sl->items);
    memset(sl, 0, sizeof(*sl));
}

static void clear_list_data() {
    int cnt = (int)SendMessage(g_hwndList, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < cnt; i++) {
        SessionItem *si = (SessionItem *)SendMessage(g_hwndList, LB_GETITEMDATA, i, 0);
        if (si && si != (void*)LB_ERR) {
            sfree(si->full_name);
            sfree(si->display);
            sfree(si);
        }
    }
    SendMessage(g_hwndList, LB_RESETCONTENT, 0, 0);
}

static int compare_sessions(const void *a, const void *b) {
    const SessionItem *sa = (const SessionItem *)a;
    const SessionItem *sb = (const SessionItem *)b;
    if (sa->is_dir != sb->is_dir) return sb->is_dir - sa->is_dir;
    return _stricmp(sa->display, sb->display);
}

static void update_button_state(void);

static void launch_putty_with_session(const char *session_name, bool edit_mode) {
    wchar_t path[MAX_PATH];
    wchar_t cmd[MAX_PATH * 2];
    STARTUPINFOW si_proc = {sizeof(si_proc)};
    PROCESS_INFORMATION pi = {0};

    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *p = wcsrchr(path, L'\\');
    if (p) *p = 0;
    wcscat(path, L"\\putty.exe");

    if (session_name) {
        wchar_t *ws = dup_mb_to_wc(CP_UTF8, session_name);
        if (edit_mode) {
            swprintf(cmd, lenof(cmd), L"\"%ls\" -load \"%ls\" -edit", path, ws);
        } else {
            swprintf(cmd, lenof(cmd), L"\"%ls\" -load \"%ls\"", path, ws);
        }
        sfree(ws);
    } else {
        swprintf(cmd, lenof(cmd), L"\"%ls\"", path);
    }

    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si_proc, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ShowWindow(g_hwndMain, SW_HIDE);
    }
}

/* --- Core Logic --- */

static void collect_raw_sessions(SessionList *raw) {
    HKEY key = open_regkey_ro(HKEY_CURRENT_USER, PUTTY_REG_POS "\\Sessions");
    if (key) {
        for (int i = 0; ; i++) {
            char *name = enum_regkey(key, i);
            if (!name) break;
            strbuf *sb = strbuf_new();
            unescape_registry_key(name, sb);
            sl_add(raw, sb->s, sb->s, L"Reg", false);
            strbuf_free(sb); sfree(name);
        }
        close_regkey(key);
    }
    settings_e *fe = fs_enum_settings_start();
    if (fe) {
        strbuf *name = strbuf_new();
        while (fs_enum_settings_next(fe, name)) {
            sl_add(raw, name->s, name->s, L"File", false);
            strbuf_clear(name);
        }
        strbuf_free(name); fs_enum_settings_finish(fe);
    }
}

static void update_view() {
    wchar_t filter[MAX_PATH] = {0};
    GetWindowTextW(g_hwndSearch, filter, MAX_PATH);
    bool is_searching = (wcslen(filter) > 0);

    SessionList raw = {0};
    collect_raw_sessions(&raw);

    clear_list_data();
    SessionList view = {0};

    if (is_searching) {
        /* Flat search mode */
        for (int i = 0; i < raw.count; i++) {
            wchar_t *wname = dup_mb_to_wc(CP_UTF8, raw.items[i].full_name);
            if (wcsstr(wname, filter)) {
                const wchar_t *tag = NULL;
                bool is_f = false, is_r = false;
                for (int j = 0; j < raw.count; j++) {
                    if (!strcmp(raw.items[j].full_name, raw.items[i].full_name)) {
                        if (raw.items[j].tag && !wcscmp(raw.items[j].tag, L"File")) is_f = true;
                        if (raw.items[j].tag && !wcscmp(raw.items[j].tag, L"Reg")) is_r = true;
                    }
                }
                if (is_f && is_r) tag = raw.items[i].tag;

                bool exists = false;
                for(int j=0; j<view.count; j++) if(!strcmp(view.items[j].display, raw.items[i].full_name)) { exists=true; break; }
                if(!exists) sl_add(&view, raw.items[i].full_name, raw.items[i].full_name, tag, false);
            }
            sfree(wname);
        }
    } else {
        /* Explorer mode */
        if (g_current_path[0]) sl_add(&view, NULL, ".. [Go Up]", NULL, true);
        size_t cur_len = strlen(g_current_path);

        for (int i = 0; i < raw.count; i++) {
            char *full = raw.items[i].full_name;
            if (cur_len > 0) {
                if (strncmp(full, g_current_path, cur_len) != 0 || full[cur_len] != '/') continue;
                full += cur_len + 1;
            }
            char *slash = strchr(full, '/');
            if (slash) {
                char dir_name[MAX_PATH];
                size_t dlen = slash - full;
                memcpy(dir_name, full, dlen); dir_name[dlen] = '\0';
                bool exists = false;
                for (int j = 0; j < view.count; j++) if (view.items[j].is_dir && !strcmp(view.items[j].display, dir_name)) { exists = true; break; }
                if (!exists) sl_add(&view, NULL, dir_name, NULL, true);
            } else {
                const wchar_t *tag = NULL;
                bool is_f = false, is_r = false;
                for (int j = 0; j < raw.count; j++) {
                    if (!strcmp(raw.items[j].full_name, raw.items[i].full_name)) {
                        if (raw.items[j].tag && !wcscmp(raw.items[j].tag, L"File")) is_f = true;
                        if (raw.items[j].tag && !wcscmp(raw.items[j].tag, L"Reg")) is_r = true;
                    }
                }
                if (is_f && is_r) tag = raw.items[i].tag;
                bool exists = false;
                for(int j=0; j<view.count; j++) if(!view.items[j].is_dir && !strcmp(view.items[j].display, full)) { exists=true; break; }
                if(!exists) sl_add(&view, raw.items[i].full_name, full, tag, false);
            }
        }
    }

    if (view.count > 0) qsort(view.items, view.count, sizeof(SessionItem), compare_sessions);

    for (int i = 0; i < view.count; i++) {
        wchar_t buf[MAX_PATH + 64];
        wchar_t *wd = dup_mb_to_wc(CP_UTF8, view.items[i].display);
        if (view.items[i].is_dir) swprintf(buf, lenof(buf), L"> %ls", wd);
        else if (view.items[i].tag) swprintf(buf, lenof(buf), L"%ls [%ls]", wd, view.items[i].tag);
        else swprintf(buf, lenof(buf), L"%ls", wd);
        
        int idx = (int)SendMessageW(g_hwndList, LB_ADDSTRING, 0, (LPARAM)buf);
        SessionItem *save = snew(SessionItem);
        save->is_dir = view.items[i].is_dir;
        save->full_name = view.items[i].full_name ? dupstr(view.items[i].full_name) : NULL;
        save->display = dupstr(view.items[i].display);
        SendMessage(g_hwndList, LB_SETITEMDATA, idx, (LPARAM)save);
        sfree(wd);
    }
    
    /* Auto-select first item so Enter works immediately */
    if (view.count > 0) {
        SendMessage(g_hwndList, LB_SETCURSEL, 0, 0);
    }

    sl_free(&raw); sl_free(&view);
    update_button_state();
}

static SessionItem *get_selected_item(void) {
    int sel = (int)SendMessage(g_hwndList, LB_GETCURSEL, 0, 0);
    
    /* If nothing is selected, default to the first item in the list */
    if (sel == LB_ERR) {
        if (SendMessage(g_hwndList, LB_GETCOUNT, 0, 0) > 0) {
            sel = 0;
        } else {
            return NULL;
        }
    }

    SessionItem *si = (SessionItem *)SendMessage(g_hwndList, LB_GETITEMDATA, sel, 0);
    if (!si) return NULL;

    return si;
}

static void handle_selection() {
    SessionItem *si = get_selected_item();
    if (!si) return;

    if (si->is_dir) {
        if (!strcmp(si->display, ".. [Go Up]")) {
            char *last = strrchr(g_current_path, '/');
            if (last) *last = '\0'; else g_current_path[0] = '\0';
        } else {
            if (g_current_path[0]) strcat(g_current_path, "/");
            strcat(g_current_path, si->display);
        }
        SetWindowTextW(g_hwndSearch, L"");
        update_view();
    } else {
        launch_putty_with_session(si->full_name, false);
    }
}

static void handle_load_button(void) {
    SessionItem *si = get_selected_item();
    if (!si || si->is_dir)
        return;

    launch_putty_with_session(si->full_name, true);
}

static void handle_new_button(void) {
    launch_putty_with_session(NULL, false);
}

static void update_button_state(void) {
    SessionItem *si = get_selected_item();
    EnableWindow(g_hwndLoad, (si && !si->is_dir) ? TRUE : FALSE);
    EnableWindow(g_hwndNew, TRUE);
}

static void handle_list_selection_change(void) {
    update_button_state();
}

static void reset_and_show_launcher(HWND hwnd) {
    g_current_path[0] = '\0';
    SetWindowTextW(g_hwndSearch, L"");
    update_view();
    update_button_state();

    POINT pt;
    GetCursorPos(&pt);
    SetWindowPos(hwnd, HWND_TOPMOST, pt.x - 175, pt.y - 510, 350, 500, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    SetFocus(g_hwndSearch);
}

static void navigate_up(void) {
    char *last = strrchr(g_current_path, '/');
    if (last) *last = '\0'; else g_current_path[0] = '\0';
    update_view();
    update_button_state();
}

static void handle_search_enter(void) {
    SessionItem *si = get_selected_item();
    if (!si) return;

    handle_selection();
}

/* --- Window Procedure --- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_hwndSearch = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 5, 5, 330, 25, hwnd, (HMENU)IDC_SEARCH_EDIT, ghInst, NULL);
            g_hwndList = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS, 5, 35, 330, 410, hwnd, (HMENU)IDC_SESSION_LIST, ghInst, NULL);
            g_hwndLoad = CreateWindowExW(0, L"BUTTON", L"Load", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 5, 450, 162, 35, hwnd, (HMENU)IDC_LOAD_BUTTON, ghInst, NULL);
            g_hwndNew = CreateWindowExW(0, L"BUTTON", L"New", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 173, 450, 162, 35, hwnd, (HMENU)IDC_NEW_BUTTON, ghInst, NULL);
            SendMessage(g_hwndSearch, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SendMessage(g_hwndList, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SendMessage(g_hwndLoad, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SendMessage(g_hwndNew, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            update_button_state();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_SEARCH_EDIT && HIWORD(wParam) == EN_CHANGE) update_view();
            if (LOWORD(wParam) == IDC_SESSION_LIST && HIWORD(wParam) == LBN_DBLCLK) handle_selection();
            if (LOWORD(wParam) == IDC_SESSION_LIST && HIWORD(wParam) == LBN_SELCHANGE) handle_list_selection_change();
            if (LOWORD(wParam) == IDC_LOAD_BUTTON && HIWORD(wParam) == BN_CLICKED) handle_load_button();
            if (LOWORD(wParam) == IDC_NEW_BUTTON && HIWORD(wParam) == BN_CLICKED) handle_new_button();
            if (LOWORD(wParam) == IDC_TRAY_EXIT) DestroyWindow(hwnd);
            return 0;
        case WM_TRAY:
            if (lParam == WM_RBUTTONUP) {
                HMENU m = CreatePopupMenu(); AppendMenuW(m, MF_STRING, IDC_TRAY_EXIT, L"Exit Launcher");
                POINT pt; GetCursorPos(&pt); SetForegroundWindow(hwnd);
                TrackPopupMenu(m, TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(m);
            } else if (lParam == WM_LBUTTONUP) {
                reset_and_show_launcher(hwnd);
            }
            return 0;
        case WM_DESTROY:
            clear_list_data(); Shell_NotifyIconW(NIM_DELETE, &gnid); PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    ghInst = hInst;
    HANDLE hM = CreateMutexW(NULL, TRUE, g_mutexname);
    if (hM && GetLastError() == ERROR_ALREADY_EXISTS) { if (hM) CloseHandle(hM); return 0; }

    WNDCLASSW wc = {0}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = g_wndclass; RegisterClassW(&wc);

    g_hwndMain = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, g_wndclass, L"PuTTY Launcher", WS_POPUP | WS_BORDER, 0, 0, 350, 500, NULL, NULL, hInst, NULL);
    
    memset(&gnid, 0, sizeof(gnid)); gnid.cbSize = sizeof(gnid); gnid.hWnd = g_hwndMain; gnid.uID = TRAY_UID;
    gnid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; gnid.uCallbackMessage = WM_TRAY;
    gnid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(200)); if (!gnid.hIcon) gnid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy(gnid.szTip, L"PuTTY Launcher"); Shell_NotifyIconW(NIM_ADD, &gnid);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.hwnd == g_hwndSearch) {
                if (msg.wParam == VK_DOWN) { 
                    int count = (int)SendMessage(g_hwndList, LB_GETCOUNT, 0, 0);
                    if (count > 0) {
                        SetFocus(g_hwndList);
                        if (count > 1) {
                            SendMessage(g_hwndList, LB_SETCURSEL, 1, 0);
                        } else {
                            SendMessage(g_hwndList, LB_SETCURSEL, 0, 0);
                        }
                    }
                    continue; 
                }
                if (msg.wParam == VK_UP) {
                    int count = (int)SendMessage(g_hwndList, LB_GETCOUNT, 0, 0);
                    if (count > 0) {
                        SetFocus(g_hwndList);
                        SendMessage(g_hwndList, LB_SETCURSEL, count - 1, 0);
                    }
                    continue;
                }
                if (msg.wParam == VK_RETURN) { handle_search_enter(); continue; }
            } else if (msg.hwnd == g_hwndList) {
                if (msg.wParam == VK_RETURN) { handle_selection(); continue; }
                if (msg.wParam == VK_UP && SendMessage(g_hwndList, LB_GETCURSEL, 0, 0) == 0) { SetFocus(g_hwndSearch); continue; }
                if (msg.wParam == VK_BACK) {
                    navigate_up();
                    continue;
                }
            }
            if (msg.wParam == VK_ESCAPE) { ShowWindow(g_hwndMain, SW_HIDE); continue; }
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (hM) { ReleaseMutex(hM); CloseHandle(hM); }
    return 0;
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s) { return wWinMain(h, p, GetCommandLineW(), s); }
