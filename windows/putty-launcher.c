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
#define IDC_MIN_SEARCH_EDIT  105
#define IDC_MIN_SESSION_LIST 106
#define IDC_MINIMIZE_ALL_TO_TRAY 2001
#define IDC_TRAY_ABOUT   2002
#define IDC_TRAY_EXIT    2003
#define IDC_MIN_RESTORE_ALL           2004
#define IDC_MIN_CLOSE_ALL             2005
#define IDC_MIN_CTX_RESTORE           3001
#define IDC_MIN_CTX_CLOSE             3002
#define IDC_MIN_CTX_COPY_TITLE        3003
#define IDC_MIN_CTX_COPY_SESSION_NAME 3004

#define TRAY_UID 1
#define WM_TRAY  (WM_APP + 1)

#define LAUNCHER_ABOUT_DETAILS \
    L"Unofficial companion tray launcher for PuTTY sessions.\r\n\r\n" \
    L"2026: Created by jazzl0ver (https://github.com/jazzl0ver/putty)"

static HINSTANCE ghInst;
static HWND g_hwndMain = NULL;
static HWND g_hwndSearch = NULL;
static HWND g_hwndList = NULL;
static HWND g_hwndLoad = NULL;
static HWND g_hwndNew = NULL;
static HWND g_hwndMinimized = NULL;
static HWND g_hwndMinSearch = NULL;
static HWND g_hwndMinList = NULL;
static HWND g_hwndMinimizeAll = NULL;
static HWND g_hwndMinRestoreAll = NULL;
static HWND g_hwndMinCloseAll = NULL;
static HWND g_hwndMinAbout = NULL;
static HWND g_hwndMinExit = NULL;
static WNDPROC g_minListProcOrig = NULL;
static NOTIFYICONDATAW gnid;

static const wchar_t *g_wndclass  = PUTTY_LAUNCHER_WNDCLASS;
static const wchar_t *g_min_wndclass = L"PuTTYLauncherMinimizedWindow";
static const wchar_t *g_mutexname = L"PuTTYLauncherSingleton";

/* --- State --- */
static char g_current_path[MAX_PATH] = ""; 

typedef struct {
    char *full_name;  /* "Folder/Session" */
    char *display;    /* "Session" or "Folder" */
    const wchar_t *tag;
    bool is_dir;
    bool is_host;
} SessionItem;

typedef struct {
    SessionItem *items;
    int count, capacity;
} SessionList;

typedef struct {
    HWND hwnd;
    wchar_t *title;
    wchar_t *session_name;
} MinimizedSessionItem;

static MinimizedSessionItem *g_minimized_sessions = NULL;
static int g_minimized_session_count = 0;
static int g_minimized_session_capacity = 0;

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
    sl->items[sl->count].is_host = false;
    sl->count++;
}

static void sl_add_host(SessionList *sl, const char *host) {
    sl_add(sl, host, host, NULL, false);
    sl->items[sl->count - 1].is_host = true;
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
    if (sa->is_host != sb->is_host) return sb->is_host - sa->is_host;
    return _stricmp(sa->display, sb->display);
}

static void update_button_state(void);
static void update_minimized_view(void);
static void reset_and_show_minimized_sessions(HWND hwnd);
static LRESULT CALLBACK MinimizedWndProc(HWND hwnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK MinimizedListProc(HWND hwnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam);

static void show_about_box(HWND hwnd) {
    char *buildinfo_text = buildinfo("\r\n");
    wchar_t *wver = dup_mb_to_wc(CP_UTF8, ver);
    wchar_t *wbuildinfo = dup_mb_to_wc(CP_UTF8, buildinfo_text);
    wchar_t text[4096];

    swprintf(text, lenof(text),
             L"PuTTY Launcher\r\n\r\n%ls\r\n\r\n%ls\r\n\r\n%ls",
             wver, wbuildinfo, LAUNCHER_ABOUT_DETAILS);

    MessageBoxW(hwnd, text, L"About PuTTY Launcher",
                MB_OK | MB_ICONINFORMATION);

    sfree(wbuildinfo);
    sfree(wver);
    sfree(buildinfo_text);
}

static void minimized_session_free(MinimizedSessionItem *item) {
    sfree(item->title);
    sfree(item->session_name);
}

static const wchar_t *minimized_session_title(
    const MinimizedSessionItem *item) {
    return item->title && item->title[0] ? item->title : L"PuTTY session";
}

static bool minimized_session_has_name(const MinimizedSessionItem *item) {
    return item->session_name && item->session_name[0];
}

static int find_minimized_session(HWND hwnd) {
    for (int i = 0; i < g_minimized_session_count; i++) {
        if (g_minimized_sessions[i].hwnd == hwnd)
            return i;
    }
    return -1;
}

static void remove_minimized_session_at(int index) {
    if (index < 0 || index >= g_minimized_session_count)
        return;

    minimized_session_free(&g_minimized_sessions[index]);
    memmove(g_minimized_sessions + index, g_minimized_sessions + index + 1,
            (g_minimized_session_count - index - 1) *
            sizeof(*g_minimized_sessions));
    g_minimized_session_count--;
}

static void free_minimized_sessions(void) {
    for (int i = 0; i < g_minimized_session_count; i++)
        minimized_session_free(&g_minimized_sessions[i]);
    sfree(g_minimized_sessions);
    g_minimized_sessions = NULL;
    g_minimized_session_count = 0;
    g_minimized_session_capacity = 0;
}

static void prune_minimized_sessions(void) {
    for (int i = 0; i < g_minimized_session_count ;) {
        if (!IsWindow(g_minimized_sessions[i].hwnd))
            remove_minimized_session_at(i);
        else
            i++;
    }
}

static bool wcsistr(const wchar_t *haystack, const wchar_t *needle) {
    size_t haystack_chars = wcslen(haystack);
    size_t needle_chars = wcslen(needle);

    if (needle_chars == 0)
        return true;

    for (size_t i = 0; i + needle_chars <= haystack_chars; i++) {
        if (CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                           haystack + i, (int)needle_chars,
                           needle, (int)needle_chars) == CSTR_EQUAL)
            return true;
    }
    return false;
}

static bool is_search_space(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

static wchar_t *dup_trimmed_wcs(const wchar_t *text) {
    const wchar_t *start = text, *end;
    wchar_t *ret;
    size_t len;

    while (*start && is_search_space(*start))
        start++;
    end = start + wcslen(start);
    while (end > start && is_search_space(end[-1]))
        end--;

    len = end - start;
    ret = snewn(len + 1, wchar_t);
    memcpy(ret, start, len * sizeof(wchar_t));
    ret[len] = L'\0';
    return ret;
}

static bool is_ascii_digit_w(wchar_t ch) {
    return ch >= L'0' && ch <= L'9';
}

static bool is_ascii_alpha_w(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

static bool is_ascii_alnum_w(wchar_t ch) {
    return is_ascii_alpha_w(ch) || is_ascii_digit_w(ch);
}

static bool is_ascii_hex_w(wchar_t ch) {
    return is_ascii_digit_w(ch) ||
        (ch >= L'A' && ch <= L'F') || (ch >= L'a' && ch <= L'f');
}

static bool is_username_char(wchar_t ch) {
    return is_ascii_alnum_w(ch) || ch == L'.' || ch == L'_' || ch == L'-';
}

static bool is_username_text(const wchar_t *text, size_t len) {
    if (!len)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (!is_username_char(text[i]))
            return false;
    }
    return true;
}

static bool is_ipv4_address_text(const wchar_t *text) {
    int parts = 0;
    const wchar_t *p = text;

    while (*p) {
        int digits = 0;
        int value = 0;

        while (is_ascii_digit_w(*p)) {
            value = value * 10 + (*p - L'0');
            digits++;
            if (digits > 3 || value > 255)
                return false;
            p++;
        }
        if (!digits)
            return false;

        parts++;
        if (*p == L'.') {
            p++;
            continue;
        }
        break;
    }

    return *p == L'\0' && parts == 4;
}

static bool is_ipv6_address_text(const wchar_t *text) {
    bool saw_colon = false, saw_hex = false, prev_colon = false;
    int chunks = 0, chunk_digits = 0, double_colons = 0;

    for (const wchar_t *p = text; *p; p++) {
        if (is_ascii_hex_w(*p)) {
            saw_hex = true;
            chunk_digits++;
            if (chunk_digits > 4)
                return false;
            prev_colon = false;
        } else if (*p == L':') {
            saw_colon = true;
            if (prev_colon && ++double_colons > 1)
                return false;
            if (chunk_digits) {
                chunks++;
                chunk_digits = 0;
            }
            prev_colon = true;
        } else {
            return false;
        }
    }

    if (prev_colon && double_colons == 0)
        return false;
    if (chunk_digits)
        chunks++;
    return saw_colon && saw_hex && chunks <= 8 &&
        ((double_colons == 1 && chunks < 8) ||
         (double_colons == 0 && chunks == 8));
}

static bool is_fqdn_text(const wchar_t *text) {
    size_t len = wcslen(text);
    int label_len = 0;
    bool saw_dot = false, final_label_has_alpha = false;
    wchar_t prev = L'\0';

    if (!len || len > 253)
        return false;
    if (text[len - 1] == L'.')
        len--;
    if (!len)
        return false;

    for (size_t i = 0; i < len; i++) {
        wchar_t ch = text[i];

        if (ch == L'.') {
            if (!label_len || prev == L'-')
                return false;
            saw_dot = true;
            label_len = 0;
            final_label_has_alpha = false;
            prev = ch;
            continue;
        }

        if (!is_ascii_alnum_w(ch) && ch != L'-')
            return false;
        if (!label_len && ch == L'-')
            return false;
        if (++label_len > 63)
            return false;
        if (is_ascii_alpha_w(ch))
            final_label_has_alpha = true;
        prev = ch;
    }

    return saw_dot && label_len && prev != L'-' && final_label_has_alpha;
}

static const wchar_t *host_part_from_target(const wchar_t *text) {
    const wchar_t *at = wcsrchr(text, L'@');

    if (!at)
        return text;
    if (at == text || !at[1] || !is_username_text(text, at - text))
        return NULL;
    return at + 1;
}

static bool is_launchable_host_text(const wchar_t *text) {
    const wchar_t *host = host_part_from_target(text);

    if (!host)
        return false;
    return is_ipv4_address_text(host) ||
        is_ipv6_address_text(host) ||
        is_fqdn_text(host);
}

static bool raw_sessions_include_exact_wide(
    const SessionList *raw, const wchar_t *name) {
    for (int i = 0; i < raw->count; i++) {
        wchar_t *wname = dup_mb_to_wc(CP_UTF8, raw->items[i].full_name);
        bool equal = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                                    wname, -1, name, -1) == CSTR_EQUAL;
        sfree(wname);
        if (equal)
            return true;
    }
    return false;
}

static bool minimized_session_matches_filter(
    const MinimizedSessionItem *item, const wchar_t *filter) {
    if (!filter || !filter[0])
        return true;
    if (wcsistr(minimized_session_title(item), filter))
        return true;
    return minimized_session_has_name(item) &&
        wcsistr(item->session_name, filter);
}

static void add_or_update_minimized_session(
    HWND hwnd, const wchar_t *title, const wchar_t *session_name) {
    int index;

    if (!IsWindow(hwnd))
        return;

    index = find_minimized_session(hwnd);
    if (index < 0) {
        if (g_minimized_session_count == g_minimized_session_capacity) {
            g_minimized_session_capacity = g_minimized_session_capacity ?
                g_minimized_session_capacity * 2 : 32;
            g_minimized_sessions = sresize(
                g_minimized_sessions, g_minimized_session_capacity,
                MinimizedSessionItem);
        }
        index = g_minimized_session_count++;
        memset(&g_minimized_sessions[index], 0,
               sizeof(g_minimized_sessions[index]));
        g_minimized_sessions[index].hwnd = hwnd;
    } else {
        minimized_session_free(&g_minimized_sessions[index]);
    }

    g_minimized_sessions[index].title = dupwcs(title ? title : L"");
    g_minimized_sessions[index].session_name =
        dupwcs(session_name ? session_name : L"");

    if (g_hwndMinimized && IsWindowVisible(g_hwndMinimized))
        update_minimized_view();
}

static bool handle_minimized_copydata(const COPYDATASTRUCT *cds) {
    const PuttyLauncherMinimizedSessionCopyData *payload;
    const wchar_t *title, *session_name;
    size_t header_bytes, payload_chars, needed_bytes;

    if (!cds || cds->dwData != PUTTY_LAUNCHER_COPYDATA_MINIMIZED_SESSION ||
        !cds->lpData)
        return false;

    header_bytes = offsetof(PuttyLauncherMinimizedSessionCopyData, strings);
    if (cds->cbData < header_bytes)
        return false;

    payload = (const PuttyLauncherMinimizedSessionCopyData *)cds->lpData;
    if ((size_t)payload->title_chars > (size_t)-1 - 2 ||
        (size_t)payload->session_name_chars >
        (size_t)-1 - (size_t)payload->title_chars - 2)
        return false;

    payload_chars =
        (size_t)payload->title_chars + 1 + payload->session_name_chars + 1;
    if (payload_chars > ((size_t)-1 - header_bytes) / sizeof(wchar_t))
        return false;

    needed_bytes = header_bytes + payload_chars * sizeof(wchar_t);

    if (needed_bytes > cds->cbData)
        return false;

    title = payload->strings;
    session_name = title + payload->title_chars + 1;
    if (title[payload->title_chars] != L'\0' ||
        session_name[payload->session_name_chars] != L'\0')
        return false;

    add_or_update_minimized_session(payload->hwnd, title, session_name);
    return true;
}

static void copy_text_to_clipboard(HWND owner, const wchar_t *text) {
    size_t bytes;
    HGLOBAL data;
    wchar_t *target;

    if (!text)
        return;

    bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!data)
        return;

    target = (wchar_t *)GlobalLock(data);
    if (!target) {
        GlobalFree(data);
        return;
    }

    memcpy(target, text, bytes);
    GlobalUnlock(data);

    if (OpenClipboard(owner)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, data))
            GlobalFree(data);
        CloseClipboard();
    } else {
        GlobalFree(data);
    }
}

static HWND get_selected_minimized_hwnd(void) {
    int sel;
    LRESULT data;

    if (!g_hwndMinList)
        return NULL;

    sel = (int)SendMessage(g_hwndMinList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR)
        return NULL;

    data = SendMessage(g_hwndMinList, LB_GETITEMDATA, sel, 0);
    return data == LB_ERR ? NULL : (HWND)data;
}

static void restore_minimized_session(HWND session_hwnd) {
    int index;

    prune_minimized_sessions();
    index = find_minimized_session(session_hwnd);
    if (index < 0) {
        update_minimized_view();
        return;
    }

    ShowWindow(g_minimized_sessions[index].hwnd, SW_RESTORE);
    SetForegroundWindow(g_minimized_sessions[index].hwnd);
    remove_minimized_session_at(index);
    update_minimized_view();
    if (g_hwndMinimized)
        ShowWindow(g_hwndMinimized, SW_HIDE);
}

static void restore_selected_minimized_session(void) {
    HWND session_hwnd = get_selected_minimized_hwnd();
    if (session_hwnd)
        restore_minimized_session(session_hwnd);
}

static void restore_all_minimized_sessions(void) {
    HWND restored_hwnd = NULL;

    prune_minimized_sessions();
    while (g_minimized_session_count > 0) {
        HWND hwnd = g_minimized_sessions[0].hwnd;
        ShowWindow(hwnd, SW_RESTORE);
        restored_hwnd = hwnd;
        remove_minimized_session_at(0);
    }

    if (restored_hwnd)
        SetForegroundWindow(restored_hwnd);
    update_minimized_view();
    if (g_hwndMinimized)
        ShowWindow(g_hwndMinimized, SW_HIDE);
}

static void close_minimized_session(HWND session_hwnd) {
    int index;

    prune_minimized_sessions();
    index = find_minimized_session(session_hwnd);
    if (index >= 0)
        PostMessage(g_minimized_sessions[index].hwnd, WM_CLOSE, 0, 0);
    update_minimized_view();
}

static void close_all_minimized_sessions(void) {
    int result;

    prune_minimized_sessions();
    if (g_minimized_session_count == 0) {
        update_minimized_view();
        return;
    }

    result = MessageBoxW(
        g_hwndMinimized,
        L"Close all minimized PuTTY sessions?",
        L"PuTTY Launcher",
        MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2);
    if (result != IDOK)
        return;

    for (int i = 0; i < g_minimized_session_count; i++)
        PostMessage(g_minimized_sessions[i].hwnd, WM_CLOSE, 0, 0);
    update_minimized_view();
}

typedef struct {
    int posted;
} MinimizeAllToTrayContext;

static bool is_putty_terminal_window(HWND hwnd) {
    wchar_t classname[64];

    if (!GetClassNameW(hwnd, classname, lenof(classname)))
        return false;

    return !wcscmp(classname, L"PuTTY") ||
        !wcscmp(classname, L"PuTTY.ansi");
}

static BOOL CALLBACK minimize_all_to_tray_proc(HWND hwnd, LPARAM lParam) {
    MinimizeAllToTrayContext *ctx = (MinimizeAllToTrayContext *)lParam;

    if (!IsWindowVisible(hwnd) || !is_putty_terminal_window(hwnd))
        return TRUE;
    if (find_minimized_session(hwnd) >= 0)
        return TRUE;

    if (PostMessage(hwnd, WM_SYSCOMMAND,
                    PUTTY_SYSCOMMAND_MINIMIZE_TO_TRAY, 0))
        ctx->posted++;
    return TRUE;
}

static void minimize_all_putty_windows_to_tray(void) {
    MinimizeAllToTrayContext ctx;

    memset(&ctx, 0, sizeof(ctx));
    prune_minimized_sessions();
    EnumWindows(minimize_all_to_tray_proc, (LPARAM)&ctx);

    if (!ctx.posted)
        update_minimized_view();
}

static void show_minimized_context_menu(HWND list, POINT pt) {
    HWND session_hwnd = get_selected_minimized_hwnd();
    int index;
    HMENU menu;
    UINT cmd;

    (void)list;

    prune_minimized_sessions();
    index = find_minimized_session(session_hwnd);
    if (index < 0) {
        update_minimized_view();
        return;
    }

    menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDC_MIN_CTX_RESTORE, L"Restore");
    AppendMenuW(menu, MF_STRING, IDC_MIN_CTX_CLOSE, L"Close session");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDC_MIN_CTX_COPY_TITLE, L"Copy title");
    if (minimized_session_has_name(&g_minimized_sessions[index])) {
        AppendMenuW(menu, MF_STRING, IDC_MIN_CTX_COPY_SESSION_NAME,
                    L"Copy session name");
    }

    SetForegroundWindow(g_hwndMinimized);
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                         pt.x, pt.y, 0, g_hwndMinimized, NULL);
    DestroyMenu(menu);

    switch (cmd) {
      case IDC_MIN_CTX_RESTORE:
        restore_minimized_session(session_hwnd);
        break;
      case IDC_MIN_CTX_CLOSE:
        close_minimized_session(session_hwnd);
        break;
      case IDC_MIN_CTX_COPY_TITLE:
        index = find_minimized_session(session_hwnd);
        if (index >= 0)
            copy_text_to_clipboard(
                g_hwndMinimized,
                minimized_session_title(&g_minimized_sessions[index]));
        break;
      case IDC_MIN_CTX_COPY_SESSION_NAME:
        index = find_minimized_session(session_hwnd);
        if (index >= 0 && minimized_session_has_name(
                &g_minimized_sessions[index]))
            copy_text_to_clipboard(
                g_hwndMinimized, g_minimized_sessions[index].session_name);
        break;
    }
}

static bool select_minimized_session_at_point(LPARAM lParam) {
    POINT pt;
    LRESULT hit;
    int index, count;

    if (!g_hwndMinList)
        return false;

    pt.x = (int)(short)LOWORD(lParam);
    pt.y = (int)(short)HIWORD(lParam);
    hit = SendMessage(g_hwndMinList, LB_ITEMFROMPOINT, 0,
                      MAKELPARAM(pt.x, pt.y));
    index = LOWORD(hit);
    count = (int)SendMessage(g_hwndMinList, LB_GETCOUNT, 0, 0);
    if (HIWORD(hit) || index < 0 || index >= count)
        return false;

    if (!SendMessage(g_hwndMinList, LB_GETITEMDATA, index, 0))
        return false;

    SendMessage(g_hwndMinList, LB_SETCURSEL, index, 0);
    return true;
}

static bool selected_minimized_session_point(POINT *pt) {
    int sel;
    RECT rect;

    sel = (int)SendMessage(g_hwndMinList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR ||
        SendMessage(g_hwndMinList, LB_GETITEMRECT, sel, (LPARAM)&rect) == LB_ERR)
        return false;

    pt->x = rect.left;
    pt->y = rect.bottom;
    ClientToScreen(g_hwndMinList, pt);
    return true;
}

static void add_host_search_item(
    SessionList *view, const SessionList *raw, const wchar_t *filter) {
    wchar_t *host = dup_trimmed_wcs(filter);

    if (is_launchable_host_text(host) &&
        !raw_sessions_include_exact_wide(raw, host)) {
        char *host_utf8 = dup_wc_to_mb(CP_UTF8, host, "");
        sl_add_host(view, host_utf8);
        sfree(host_utf8);
    }

    sfree(host);
}

static bool get_putty_exe_path(wchar_t *path, size_t pathlen) {
    static const wchar_t suffix[] = L"\\putty.exe";
    DWORD got;
    wchar_t *p;

    got = GetModuleFileNameW(NULL, path, (DWORD)pathlen);
    if (!got || got >= pathlen)
        return false;

    p = wcsrchr(path, L'\\');
    if (p)
        *p = L'\0';

    if (wcslen(path) + lenof(suffix) > pathlen)
        return false;
    wcscat(path, suffix);
    return true;
}

static void launch_putty_with_session(const char *session_name, bool edit_mode) {
    wchar_t path[MAX_PATH];
    wchar_t cmd[MAX_PATH * 2];
    STARTUPINFOW si_proc = {sizeof(si_proc)};
    PROCESS_INFORMATION pi = {0};

    if (!get_putty_exe_path(path, lenof(path)))
        return;

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

static void launch_putty_with_host(const char *target) {
    wchar_t path[MAX_PATH];
    wchar_t cmd[MAX_PATH * 3];
    wchar_t *wide_target, *host, *user = NULL;
    STARTUPINFOW si_proc = {sizeof(si_proc)};
    PROCESS_INFORMATION pi = {0};

    if (!get_putty_exe_path(path, lenof(path)))
        return;

    wide_target = dup_mb_to_wc(CP_UTF8, target);
    wchar_t *at = wcsrchr(wide_target, L'@');
    if (at) {
        *at = L'\0';
        user = wide_target;
        host = at + 1;
        if (!is_username_text(user, wcslen(user)) ||
            !is_launchable_host_text(host)) {
            sfree(wide_target);
            return;
        }
        swprintf(cmd, lenof(cmd), L"\"%ls\" -l \"%ls\" \"%ls\"",
                 path, user, host);
    } else {
        host = wide_target;
        if (!is_launchable_host_text(host)) {
            sfree(wide_target);
            return;
        }
        swprintf(cmd, lenof(cmd), L"\"%ls\" \"%ls\"", path, host);
    }
    sfree(wide_target);

    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL,
                       &si_proc, &pi)) {
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
        add_host_search_item(&view, &raw, filter);
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
        else if (view.items[i].is_host) swprintf(buf, lenof(buf),
                                                 L"Connect to %ls", wd);
        else if (view.items[i].tag) swprintf(buf, lenof(buf), L"%ls [%ls]", wd, view.items[i].tag);
        else swprintf(buf, lenof(buf), L"%ls", wd);
        
        int idx = (int)SendMessageW(g_hwndList, LB_ADDSTRING, 0, (LPARAM)buf);
        SessionItem *save = snew(SessionItem);
        save->is_dir = view.items[i].is_dir;
        save->is_host = view.items[i].is_host;
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

static void update_minimized_view(void) {
    wchar_t filter[MAX_PATH] = {0};
    int added = 0;

    if (!g_hwndMinList)
        return;

    if (g_hwndMinSearch)
        GetWindowTextW(g_hwndMinSearch, filter, MAX_PATH);

    prune_minimized_sessions();
    SendMessage(g_hwndMinList, LB_RESETCONTENT, 0, 0);

    for (int i = 0; i < g_minimized_session_count; i++) {
        MinimizedSessionItem *item = &g_minimized_sessions[i];
        wchar_t *display;
        int idx;

        if (!minimized_session_matches_filter(item, filter))
            continue;

        if (minimized_session_has_name(item)) {
            display = dupwcscat(minimized_session_title(item), L" [",
                                item->session_name, L"]");
        } else {
            display = dupwcs(minimized_session_title(item));
        }

        idx = (int)SendMessageW(g_hwndMinList, LB_ADDSTRING, 0,
                                (LPARAM)display);
        if (idx != LB_ERR) {
            SendMessage(g_hwndMinList, LB_SETITEMDATA, idx,
                        (LPARAM)item->hwnd);
            added++;
        }
        sfree(display);
    }

    if (added > 0) {
        SendMessage(g_hwndMinList, LB_SETCURSEL, 0, 0);
    } else {
        const wchar_t *message = g_minimized_session_count ?
            L"No matching minimized PuTTY sessions" :
            L"No minimized PuTTY sessions";
        int idx = (int)SendMessageW(g_hwndMinList, LB_ADDSTRING, 0,
                                    (LPARAM)message);
        if (idx != LB_ERR)
            SendMessage(g_hwndMinList, LB_SETITEMDATA, idx, 0);
        SendMessage(g_hwndMinList, LB_SETCURSEL, (WPARAM)-1, 0);
    }

    if (g_hwndMinRestoreAll)
        EnableWindow(g_hwndMinRestoreAll,
                     g_minimized_session_count > 0 ? TRUE : FALSE);
    if (g_hwndMinCloseAll)
        EnableWindow(g_hwndMinCloseAll,
                     g_minimized_session_count > 0 ? TRUE : FALSE);
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
    } else if (si->is_host) {
        launch_putty_with_host(si->full_name);
    } else {
        launch_putty_with_session(si->full_name, false);
    }
}

static void handle_load_button(void) {
    SessionItem *si = get_selected_item();
    if (!si || si->is_dir || si->is_host)
        return;

    launch_putty_with_session(si->full_name, true);
}

static void handle_new_button(void) {
    launch_putty_with_session(NULL, false);
}

static void update_button_state(void) {
    SessionItem *si = get_selected_item();
    EnableWindow(g_hwndLoad, (si && !si->is_dir && !si->is_host) ?
                 TRUE : FALSE);
    EnableWindow(g_hwndNew, TRUE);
}

static void handle_list_selection_change(void) {
    update_button_state();
}

static void reset_and_show_launcher(HWND hwnd) {
    if (g_hwndMinimized)
        ShowWindow(g_hwndMinimized, SW_HIDE);

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

static void reset_and_show_minimized_sessions(HWND hwnd) {
    POINT pt;

    (void)hwnd;

    if (!g_hwndMinimized)
        return;

    ShowWindow(g_hwndMain, SW_HIDE);
    SetWindowTextW(g_hwndMinSearch, L"");
    update_minimized_view();

    GetCursorPos(&pt);
    SetWindowPos(g_hwndMinimized, HWND_TOPMOST, pt.x - 210, pt.y - 510,
                 420, 500, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hwndMinimized);
    SetFocus(g_hwndMinSearch);
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
            return 0;
        case WM_COPYDATA:
            return handle_minimized_copydata((COPYDATASTRUCT *)lParam) ? 1 : 0;
        case WM_TRAY:
            if (lParam == WM_RBUTTONUP) {
                reset_and_show_minimized_sessions(hwnd);
            } else if (lParam == WM_LBUTTONUP) {
                reset_and_show_launcher(hwnd);
            }
            return 0;
        case WM_DESTROY:
            clear_list_data();
            free_minimized_sessions();
            Shell_NotifyIconW(NIM_DELETE, &gnid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT call_minimized_list_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam) {
    if (g_minListProcOrig)
        return CallWindowProcW(g_minListProcOrig, hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK MinimizedListProc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONUP: {
            LRESULT result = call_minimized_list_proc(
                hwnd, msg, wParam, lParam);
            restore_selected_minimized_session();
            return result;
        }
        case WM_RBUTTONDOWN:
            if (select_minimized_session_at_point(lParam)) {
                SetFocus(hwnd);
                return 0;
            }
            break;
        case WM_RBUTTONUP:
            if (select_minimized_session_at_point(lParam)) {
                POINT pt;
                pt.x = (int)(short)LOWORD(lParam);
                pt.y = (int)(short)HIWORD(lParam);
                ClientToScreen(hwnd, &pt);
                show_minimized_context_menu(hwnd, pt);
                return 0;
            }
            break;
        case WM_CONTEXTMENU: {
            POINT pt;
            if (lParam == (LPARAM)-1) {
                if (!selected_minimized_session_point(&pt))
                    return 0;
            } else {
                POINT client;
                pt.x = (int)(short)LOWORD(lParam);
                pt.y = (int)(short)HIWORD(lParam);
                client = pt;
                ScreenToClient(hwnd, &client);
                if (!select_minimized_session_at_point(
                        MAKELPARAM(client.x, client.y)))
                    return 0;
            }
            show_minimized_context_menu(hwnd, pt);
            return 0;
        }
    }
    return call_minimized_list_proc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK MinimizedWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_hwndMinSearch = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                5, 5, 410, 25, hwnd, (HMENU)IDC_MIN_SEARCH_EDIT, ghInst,
                NULL);
            g_hwndMinList = CreateWindowExW(
                0, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                WS_TABSTOP | LBS_NOTIFY | LBS_HASSTRINGS,
                5, 35, 410, 335, hwnd, (HMENU)IDC_MIN_SESSION_LIST, ghInst,
                NULL);
            g_hwndMinimizeAll = CreateWindowExW(
                0, L"BUTTON", L"Minimize all to tray",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                5, 375, 410, 35, hwnd, (HMENU)IDC_MINIMIZE_ALL_TO_TRAY, ghInst,
                NULL);
            g_hwndMinRestoreAll = CreateWindowExW(
                0, L"BUTTON", L"Restore all",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                5, 415, 200, 35, hwnd, (HMENU)IDC_MIN_RESTORE_ALL, ghInst,
                NULL);
            g_hwndMinCloseAll = CreateWindowExW(
                0, L"BUTTON", L"Close all...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                215, 415, 200, 35, hwnd, (HMENU)IDC_MIN_CLOSE_ALL, ghInst,
                NULL);
            g_hwndMinAbout = CreateWindowExW(
                0, L"BUTTON", L"About",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                5, 455, 200, 35, hwnd, (HMENU)IDC_TRAY_ABOUT, ghInst, NULL);
            g_hwndMinExit = CreateWindowExW(
                0, L"BUTTON", L"Exit",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                215, 455, 200, 35, hwnd, (HMENU)IDC_TRAY_EXIT, ghInst, NULL);
            SendMessage(g_hwndMinSearch, WM_SETFONT,
                        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SendMessage(g_hwndMinList, WM_SETFONT,
                        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SendMessage(g_hwndMinimizeAll, WM_SETFONT,
                        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SendMessage(g_hwndMinRestoreAll, WM_SETFONT,
                        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SendMessage(g_hwndMinCloseAll, WM_SETFONT,
                        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SendMessage(g_hwndMinAbout, WM_SETFONT,
                        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SendMessage(g_hwndMinExit, WM_SETFONT,
                        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            g_minListProcOrig = (WNDPROC)SetWindowLongPtr(
                g_hwndMinList, GWLP_WNDPROC, (LONG_PTR)MinimizedListProc);
            update_minimized_view();
            return 0;
        case WM_SIZE:
            if (g_hwndMinSearch && g_hwndMinList &&
                g_hwndMinimizeAll && g_hwndMinRestoreAll &&
                g_hwndMinCloseAll && g_hwndMinAbout && g_hwndMinExit) {
                int width = LOWORD(lParam);
                int height = HIWORD(lParam);
                int button_width = (width - 15) / 2;
                MoveWindow(g_hwndMinSearch, 5, 5, width - 10, 25, TRUE);
                MoveWindow(g_hwndMinList, 5, 35, width - 10, height - 165,
                           TRUE);
                MoveWindow(g_hwndMinimizeAll, 5, height - 120, width - 10, 35,
                           TRUE);
                MoveWindow(g_hwndMinRestoreAll, 5, height - 80, button_width,
                           35, TRUE);
                MoveWindow(g_hwndMinCloseAll, 10 + button_width, height - 80,
                           width - 15 - button_width, 35,
                           TRUE);
                MoveWindow(g_hwndMinAbout, 5, height - 40, button_width, 35,
                           TRUE);
                MoveWindow(g_hwndMinExit, 10 + button_width, height - 40,
                           width - 15 - button_width, 35, TRUE);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE)
                ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_MIN_SEARCH_EDIT &&
                HIWORD(wParam) == EN_CHANGE)
                update_minimized_view();
            if (LOWORD(wParam) == IDC_MIN_SESSION_LIST &&
                HIWORD(wParam) == LBN_DBLCLK)
                restore_selected_minimized_session();
            if (LOWORD(wParam) == IDC_MINIMIZE_ALL_TO_TRAY)
                minimize_all_putty_windows_to_tray();
            if (LOWORD(wParam) == IDC_MIN_RESTORE_ALL)
                restore_all_minimized_sessions();
            if (LOWORD(wParam) == IDC_MIN_CLOSE_ALL)
                close_all_minimized_sessions();
            if (LOWORD(wParam) == IDC_TRAY_ABOUT)
                show_about_box(hwnd);
            if (LOWORD(wParam) == IDC_TRAY_EXIT)
                DestroyWindow(g_hwndMain);
            return 0;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
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

    WNDCLASSW minwc = {0}; minwc.lpfnWndProc = MinimizedWndProc;
    minwc.hInstance = hInst;
    minwc.hCursor = LoadCursor(NULL, IDC_ARROW);
    minwc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    minwc.lpszClassName = g_min_wndclass; RegisterClassW(&minwc);

    g_hwndMain = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, g_wndclass, L"PuTTY Launcher", WS_POPUP | WS_BORDER, 0, 0, 350, 500, NULL, NULL, hInst, NULL);
    g_hwndMinimized = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                      g_min_wndclass,
                                      L"Minimized PuTTY Sessions",
                                      WS_POPUP | WS_BORDER, 0, 0, 420, 500,
                                      NULL, NULL, hInst, NULL);
    
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
            } else if (msg.hwnd == g_hwndMinSearch) {
                if (msg.wParam == VK_DOWN) {
                    int count = (int)SendMessage(g_hwndMinList, LB_GETCOUNT, 0, 0);
                    if (count > 0) {
                        SetFocus(g_hwndMinList);
                        SendMessage(g_hwndMinList, LB_SETCURSEL, 0, 0);
                    }
                    continue;
                }
                if (msg.wParam == VK_UP) {
                    int count = (int)SendMessage(g_hwndMinList, LB_GETCOUNT, 0, 0);
                    if (count > 0) {
                        SetFocus(g_hwndMinList);
                        SendMessage(g_hwndMinList, LB_SETCURSEL, count - 1, 0);
                    }
                    continue;
                }
                if (msg.wParam == VK_RETURN) {
                    restore_selected_minimized_session();
                    continue;
                }
            } else if (msg.hwnd == g_hwndMinList) {
                if (msg.wParam == VK_RETURN) {
                    restore_selected_minimized_session();
                    continue;
                }
                if (msg.wParam == VK_UP &&
                    SendMessage(g_hwndMinList, LB_GETCURSEL, 0, 0) == 0) {
                    SetFocus(g_hwndMinSearch);
                    continue;
                }
            }
            if (msg.wParam == VK_ESCAPE) {
                if (g_hwndMinimized && IsWindowVisible(g_hwndMinimized))
                    ShowWindow(g_hwndMinimized, SW_HIDE);
                else
                    ShowWindow(g_hwndMain, SW_HIDE);
                continue;
            }
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (hM) { ReleaseMutex(hM); CloseHandle(hM); }
    return 0;
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s) { return wWinMain(h, p, GetCommandLineW(), s); }
