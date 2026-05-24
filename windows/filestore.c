/*
 * filestore.c: file-based implementation of PuTTY's storage.h
 * interface for Windows.
 *
 * Goals:
 *  - Provide a KiTTY-style 'portable' mode storing sessions, host keys,
 *    host CAs and random seed in files.
 *  - Be runtime-selectable, leaving the default registry backend intact.
 *
 * Storage layout (root directory):
 *   putty.ini                (optional marker for portable mode)
 *   sessions\<escaped>.ini   (one file per session)
 *   sshhostkeys.ini          (host key database)
 *   hostcas\<escaped>.ini    (one file per host CA record)
 *   randomseed               (RNG seed file)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>

#include "putty.h"
#include "storage.h"
#include "filestore.h"

#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <shlobj.h>   /* SHGetFolderPath, CSIDL_*, SHGFP_* */

#include <windows.h>

/* ----------------------------------------------------------------------
 * Backend selection and path handling
 */

static bool fs_inited = false;
static bool fs_enabled = false;
static char *fs_root_utf8 = NULL; /* malloced */

static bool fs_file_exists_utf8(const char *path_utf8)
{
    Filename *fn = filename_from_str(path_utf8);
    DWORD attr = GetFileAttributesW(fn->wpath);
    filename_free(fn);
    return attr != INVALID_FILE_ATTRIBUTES;
}

static void fs_mkdir_utf8(const char *path_utf8)
{
    Filename *fn = filename_from_str(path_utf8);
    CreateDirectoryW(fn->wpath, NULL);
    filename_free(fn);
}

static char *fs_dirname_utf8(const char *path_utf8)
{
    /* returns newly allocated directory component (no trailing slash) */
    char *p = dupstr(path_utf8);
    char *slash = strrchr(p, '\\');
    char *slash2 = strrchr(p, '/');
    if (slash2 && (!slash || slash2 > slash))
        slash = slash2;
    if (slash)
        *slash = '\0';
    return p;
}

static void fs_init(void)
{
    if (fs_inited)
        return;
    fs_inited = true;

    const char *env = getenv("PUTTY_STORAGE");
    if (env) {
        if (!strcmp(env, "file") || !strcmp(env, "ini") || !strcmp(env, "files"))
            fs_enabled = true;
        else if (!strcmp(env, "registry"))
            fs_enabled = false;
    }

    /* Determine executable directory */
    wchar_t wexe[MAX_PATH];
    DWORD wlen = GetModuleFileNameW(NULL, wexe, lenof(wexe));
    if (wlen == 0 || wlen >= lenof(wexe))
        wexe[0] = L'\0';
    for (int i = (int)wcslen(wexe) - 1; i >= 0; i--) {
        if (wexe[i] == L'\\' || wexe[i] == L'/') {
            wexe[i] = L'\0';
            break;
        }
    }
    char *exe_dir_utf8 = dup_wc_to_mb(CP_UTF8, wexe, NULL);

    /* Portable marker: putty.ini next to the executable */
    char *portable_marker = dupcat(exe_dir_utf8, "\\putty.ini");
    bool portable = fs_file_exists_utf8(portable_marker);

    if (!env && portable)
        fs_enabled = true;

    if (fs_enabled) {
        /* Root is exe dir for portable mode; else %APPDATA%\PuTTY */
        if (portable) {
            fs_root_utf8 = dupstr(exe_dir_utf8);
        } else {
            /* Use SHGetFolderPathA if present; fall back to env vars */
            char appdata[MAX_PATH];
            appdata[0] = '\0';
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL,
                                           SHGFP_TYPE_CURRENT, appdata))) {
                fs_root_utf8 = dupcat(appdata, "\\PuTTY");
            } else {
                const char *a = getenv("APPDATA");
                fs_root_utf8 = a ? dupcat(a, "\\PuTTY") : dupstr(exe_dir_utf8);
            }
        }
    }

    sfree(portable_marker);
    sfree(exe_dir_utf8);
}

bool win_use_file_storage(void)
{
    fs_init();
    return fs_enabled;
}

char *win_file_storage_root(void)
{
    fs_init();
    return fs_root_utf8 ? dupstr(fs_root_utf8) : NULL;
}

static char *fs_sessions_dir(void)
{
    fs_init();
    return fs_root_utf8 ? dupcat(fs_root_utf8, "\\sessions") : NULL;
}

static char *fs_hostcas_dir(void)
{
    fs_init();
    return fs_root_utf8 ? dupcat(fs_root_utf8, "\\hostcas") : NULL;
}

static char *fs_session_file(const char *sessionname)
{
    if (!sessionname || !*sessionname)
        sessionname = "Default Settings";

    char *dir = fs_sessions_dir();
    if (!dir) return NULL;

    strbuf *sb = strbuf_new();
    const char *p = sessionname;
    
    /* 1. Генерируем путь, заменяя '/' на '\' и создавая подпапки */
    while (*p) {
        const char *q = strchr(p, '/');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        
        strbuf *comp = strbuf_new();
        put_data(comp, p, len);
        strbuf *esc = strbuf_new();
        escape_registry_key(comp->s, esc);
        
        if (sb->len > 0) put_byte(sb, '\\');
        put_datapl(sb, ptrlen_from_strbuf(esc));
        
        /* Если это промежуточный сегмент (папка), создаем её */
        if (q) {
            char *sub = dupprintf("%s\\%s", dir, sb->s);
            wchar_t *wsub = dup_mb_to_wc(CP_UTF8, sub);
            CreateDirectoryW(wsub, NULL);
            sfree(wsub);
            sfree(sub);
        }

        strbuf_free(comp);
        strbuf_free(esc);
        if (!q) break;
        p = q + 1;
    }

    char *path = dupprintf("%s\\%s.ini", dir, sb->s);
    strbuf_free(sb);
    sfree(dir);
    return path;
}

static char *fs_session_file_without_ini_suffix(const char *sessionname)
{
    if (!sessionname || !*sessionname)
        return NULL;

    size_t len = strlen(sessionname);
    if (len <= 4 || _stricmp(sessionname + len - 4, ".ini"))
        return NULL;

    char *stripped = dupprintf("%.*s", (int)(len - 4), sessionname);
    char *path = fs_session_file(stripped);
    sfree(stripped);
    return path;
}

static char *fs_session_file_from_path_arg(const char *sessionname)
{
    if (!sessionname || !*sessionname)
        return NULL;

    size_t len = strlen(sessionname);
    bool pathlike =
        strchr(sessionname, '\\') || strchr(sessionname, '/') ||
        (isalpha((unsigned char)sessionname[0]) && sessionname[1] == ':') ||
        (len > 4 && !_stricmp(sessionname + len - 4, ".ini"));

    return pathlike ? dupstr(sessionname) : NULL;
}

static char *fs_hostkey_file(void)
{
    fs_init();
    return fs_root_utf8 ? dupcat(fs_root_utf8, "\\sshhostkeys.ini") : NULL;
}

static char *fs_randomseed_file(void)
{
    fs_init();
    return fs_root_utf8 ? dupcat(fs_root_utf8, "\\randomseed") : NULL;
}

static char *fs_hostca_file(const char *name)
{
    if (!name || !*name)
        return NULL;
    char *dir = fs_hostcas_dir();
    if (!dir)
        return NULL;
    strbuf *sb = strbuf_new();
    escape_registry_key(name, sb);
    char *path = dupprintf("%s\\%s.ini", dir, sb->s);
    strbuf_free(sb);
    sfree(dir);
    return path;
}

static void fs_ensure_root_dirs(void)
{
    fs_init();
    if (!fs_root_utf8)
        return;

    fs_mkdir_utf8(fs_root_utf8);
    char *sd = fs_sessions_dir();
    if (sd) {
        fs_mkdir_utf8(sd);
        sfree(sd);
    }
    char *hd = fs_hostcas_dir();
    if (hd) {
        fs_mkdir_utf8(hd);
        sfree(hd);
    }
}

/* ----------------------------------------------------------------------
 * Simple key-value file format helpers
 */

typedef struct kvpair {
    char *key;
    char *value; /* stored as UTF-8 text, already encoded if needed */
    struct kvpair *next;
} kvpair;

static kvpair *kv_find(kvpair *head, const char *key)
{
    for (kvpair *p = head; p; p = p->next)
        if (!strcmp(p->key, key))
            return p;
    return NULL;
}

static void kv_set(kvpair **phead, const char *key, const char *value)
{
    kvpair *p = kv_find(*phead, key);
    if (!p) {
        p = snew(kvpair);
        p->key = dupstr(key);
        p->value = NULL;
        p->next = *phead;
        *phead = p;
    }
    sfree(p->value);
    p->value = dupstr(value);
}

static void kv_free_all(kvpair *head)
{
    while (head) {
        kvpair *next = head->next;
        sfree(head->key);
        sfree(head->value);
        sfree(head);
        head = next;
    }
}

static char *kv_encode(const char *s)
{
    /* Encode all strings to be safe (percent-encoding) */
    strbuf *enc = percent_encode_sb(ptrlen_from_asciz(s), NULL);
    char *ret = dupstr(enc->s);
    strbuf_free(enc);
    return ret;
}

static char *kv_decode(const char *s)
{
    strbuf *dec = percent_decode_sb(ptrlen_from_asciz(s));
    char *ret = strbuf_to_str(dec);
    return ret;
}

static kvpair *kv_load_file(const char *path_utf8)
{
    Filename *fn = filename_from_str(path_utf8);
    FILE *fp = f_open(fn, "rb", false);
    filename_free(fn);
    if (!fp)
        return NULL;

    kvpair *head = NULL;
    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && (*p == ' ' || *p == '\t'))
            p++;
        /*
         * KiTTY portable session files are in the form Item\\Value\\
         * and allow percent-encoding in Value.
         *
         * For smooth migration, also accept legacy key=value files.
         */
        if (!*p || *p == '\n' || *p == '\r' || *p == '#')
            continue;

        /* trim line endings early */
        char *eol = p + strlen(p);
        while (eol > p && (eol[-1] == '\n' || eol[-1] == '\r'))
            *--eol = '\0';

        char *key = NULL;
        char *val = NULL;

        /* Prefer KiTTY format if it looks like it */
        char *bs = strchr(p, '\\');

        if (bs && eol > p && eol[-1] == '\\') {
            *bs = '\0';   /* split key and value */
            eol[-1] = '\0'; /* remove trailing backslash */
            key = p;
            val = bs + 1;
        } else {
            char *eq = strchr(p, '=');
            if (!eq)
                continue;
            *eq = '\0';
            key = p;
            val = eq + 1;
        }

        /* trim end of key */
        for (char *q = key + strlen(key); q > key && (q[-1] == ' ' || q[-1] == '\t'); q--)
            q[-1] = '\0';

        kv_set(&head, key, val);
    }

    fclose(fp);

    return head;
}

static bool kv_save_file_atomic(const char *path_utf8, kvpair *head)
{
    fs_ensure_root_dirs();

    char *dir = fs_dirname_utf8(path_utf8);
    fs_mkdir_utf8(dir);
    sfree(dir);

    char *tmp = dupcat(path_utf8, ".tmp");
    Filename *fn = filename_from_str(tmp);
    FILE *fp = f_open(fn, "wb", true);
    filename_free(fn);
    if (!fp) {
        sfree(tmp);
        return false;
    }

    for (kvpair *p = head; p; p = p->next) {
        /* Write KiTTY-compatible portable format: Item\\Value\\ */
        fprintf(fp, "%s\\%s\\\n", p->key, p->value ? p->value : "");
    }
    fclose(fp);

    Filename *fn_tmp = filename_from_str(tmp);
    Filename *fn_dst = filename_from_str(path_utf8);

    /* Replace destination atomically */
    MoveFileExW(fn_tmp->wpath, fn_dst->wpath,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

    filename_free(fn_tmp);
    filename_free(fn_dst);
    sfree(tmp);
    return true;
}

/* ----------------------------------------------------------------------
 * Sessions
 */

typedef struct fs_settings_w {
    char *path;
    kvpair *kv;
} fs_settings_w;

typedef struct fs_settings_r {
    char *path;
    kvpair *kv;
} fs_settings_r;

static bool fs_try_load_settings_file(fs_settings_r *h, char *path)
{
    if (!path)
        return false;

    kvpair *kv = kv_load_file(path);
    if (!kv) {
        sfree(path);
        return false;
    }

    sfree(h->path);
    h->path = path;
    h->kv = kv;
    return true;
}

settings_w *fs_open_settings_w(const char *sessionname, char **errmsg)
{
    *errmsg = NULL;
    fs_init();
    if (!fs_root_utf8) {
        *errmsg = dupstr("File storage root directory is unavailable");
        return NULL;
    }

    fs_settings_w *h = snew(fs_settings_w);
    h->path = fs_session_file(sessionname);
    h->kv = NULL;
    return (settings_w *)h;
}

void fs_write_setting_s(settings_w *handle, const char *key, const char *value)
{
    if (!handle)
        return;
    fs_settings_w *h = (fs_settings_w *)handle;
    char *enc = kv_encode(value ? value : "");
    kv_set(&h->kv, key, enc);
    sfree(enc);
}

void fs_write_setting_i(settings_w *handle, const char *key, int value)
{
    if (!handle)
        return;
    char buf[64];
    sprintf(buf, "%d", value);
    fs_settings_w *h = (fs_settings_w *)handle;
    kv_set(&h->kv, key, buf);
}

void fs_write_setting_filename(settings_w *handle,
                               const char *key, Filename *value)
{
    /* Preserve legacy behaviour: store cpath */
    fs_write_setting_s(handle, key, value ? value->cpath : "");
}

void fs_write_setting_fontspec(settings_w *handle,
                               const char *name, FontSpec *font)
{
    char *settingname;

    fs_write_setting_s(handle, name, font->name);
    settingname = dupcat(name, "IsBold");
    fs_write_setting_i(handle, settingname, font->isbold);
    sfree(settingname);
    settingname = dupcat(name, "CharSet");
    fs_write_setting_i(handle, settingname, font->charset);
    sfree(settingname);
    settingname = dupcat(name, "Height");
    fs_write_setting_i(handle, settingname, font->height);
    sfree(settingname);
}

void fs_close_settings_w(settings_w *handle)
{
    if (!handle)
        return;
    fs_settings_w *h = (fs_settings_w *)handle;
    if (h->path)
        kv_save_file_atomic(h->path, h->kv);
    kv_free_all(h->kv);
    sfree(h->path);
    sfree(h);
}

settings_r *fs_open_settings_r(const char *sessionname)
{
    fs_settings_r *h = snew(fs_settings_r);
    h->path = fs_session_file(sessionname);
    if (!h->path) {
        sfree(h);
        return NULL;
    }
    h->kv = kv_load_file(h->path);
    if (!h->kv) {
        if (!fs_try_load_settings_file(
                h, fs_session_file_from_path_arg(sessionname))) {
            fs_try_load_settings_file(
                h, fs_session_file_without_ini_suffix(sessionname));
        }
    }
    if (!h->kv) {
        sfree(h->path);
        sfree(h);
        return NULL;
    }
    return (settings_r *)h;
}

char *fs_read_setting_s(settings_r *handle, const char *key)
{
    if (!handle)
        return NULL;
    fs_settings_r *h = (fs_settings_r *)handle;
    kvpair *p = kv_find(h->kv, key);
    if (!p || !p->value)
        return NULL;
    return kv_decode(p->value);
}

int fs_read_setting_i(settings_r *handle, const char *key, int defvalue)
{
    if (!handle)
        return defvalue;
    fs_settings_r *h = (fs_settings_r *)handle;
    kvpair *p = kv_find(h->kv, key);
    if (!p || !p->value)
        return defvalue;
    return atoi(p->value);
}

Filename *fs_read_setting_filename(settings_r *handle, const char *name)
{
    char *tmp = fs_read_setting_s(handle, name);
    if (tmp) {
        Filename *ret = filename_from_str(tmp);
        sfree(tmp);
        return ret;
    }
    return NULL;
}

FontSpec *fs_read_setting_fontspec(settings_r *handle, const char *name)
{
    char *settingname;
    char *fontname;
    FontSpec *ret;
    int isbold, height, charset;

    fontname = fs_read_setting_s(handle, name);
    if (!fontname)
        return NULL;

    settingname = dupcat(name, "IsBold");
    isbold = fs_read_setting_i(handle, settingname, -1);
    sfree(settingname);
    if (isbold == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "CharSet");
    charset = fs_read_setting_i(handle, settingname, -1);
    sfree(settingname);
    if (charset == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "Height");
    height = fs_read_setting_i(handle, settingname, INT_MIN);
    sfree(settingname);
    if (height == INT_MIN) {
        sfree(fontname);
        return NULL;
    }

    ret = fontspec_new(fontname, isbold, height, charset);
    sfree(fontname);
    return ret;
}

void fs_close_settings_r(settings_r *handle)
{
    if (!handle)
        return;
    fs_settings_r *h = (fs_settings_r *)handle;
    kv_free_all(h->kv);
    sfree(h->path);
    sfree(h);
}

void fs_del_settings(const char *sessionname)
{
    char *path = fs_session_file(sessionname);
    if (!path)
        return;
    Filename *fn = filename_from_str(path);
    DeleteFileW(fn->wpath);
    filename_free(fn);
    sfree(path);
}

/* Вспомогательная структура уровня вложенности */
struct dir_level {
    HANDLE hfind;
    char *relpath;        /* Относительный путь в формате "folder/sub" */
    struct dir_level *next;
};

struct fs_settings_e {
    char *root;           /* Абсолютный путь к папке sessions */
    struct dir_level *stack;
    WIN32_FIND_DATAW ffd;
    bool ffd_valid;       /* Флаг: содержит ли ffd еще не обработанный элемент */
};

settings_e *fs_enum_settings_start(void)
{
    char *dir = fs_sessions_dir();
    if (!dir) return NULL;
    fs_ensure_root_dirs();

    struct fs_settings_e *e = snew(struct fs_settings_e);
    e->root = dir;
    e->stack = snew(struct dir_level);
    e->stack->relpath = dupstr("");
    e->stack->next = NULL;

    /* Начинаем поиск в корневой папке sessions */
    char *pattern = dupprintf("%s\\*", e->root);
    wchar_t *wpat = dup_mb_to_wc(CP_UTF8, pattern);
    e->stack->hfind = FindFirstFileW(wpat, &e->ffd);
    sfree(wpat);
    sfree(pattern);

    if (e->stack->hfind == INVALID_HANDLE_VALUE) {
        sfree(e->stack->relpath);
        sfree(e->stack);
        sfree(e->root);
        sfree(e);
        return NULL;
    }

    e->ffd_valid = true; /* У нас уже есть первый элемент из FindFirstFile */
    return (settings_e *)e;
}

bool fs_enum_settings_next(settings_e *handle, strbuf *out)
{
    struct fs_settings_e *e = (struct fs_settings_e *)handle;

    while (e->stack) {
        /* 1. Если текущий ffd пуст, пытаемся получить следующий элемент */
        if (!e->ffd_valid) {
            if (FindNextFileW(e->stack->hfind, &e->ffd)) {
                e->ffd_valid = true;
            } else {
                /* Файлы в текущей папке кончились — поднимаемся выше */
                FindClose(e->stack->hfind);
                struct dir_level *old = e->stack;
                e->stack = old->next;
                sfree(old->relpath);
                sfree(old);
                e->ffd_valid = false; /* Нужно дернуть FindNext в родительской папке */
                continue;
            }
        }

        /* 2. Пропускаем системные ссылки . и .. */
        if (!wcscmp(e->ffd.cFileName, L".") || !wcscmp(e->ffd.cFileName, L"..")) {
            e->ffd_valid = false;
            continue;
        }

        char *name = dup_wc_to_mb(CP_UTF8, e->ffd.cFileName, NULL);

        /* 3. Если это папка — погружаемся */
        if (e->ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            struct dir_level *new_lev = snew(struct dir_level);
            new_lev->relpath = *e->stack->relpath ? 
                               dupprintf("%s/%s", e->stack->relpath, name) : 
                               dupstr(name);

            /* Формируем путь для поиска внутри найденной папки */
            strbuf *sb_path = strbuf_new();
            put_fmt(sb_path, "%s\\", e->root);
            for (char *p = new_lev->relpath; *p; p++) 
                put_byte(sb_path, (*p == '/') ? '\\' : *p);
            put_dataz(sb_path, "\\*");

            wchar_t *wpat = dup_mb_to_wc(CP_UTF8, sb_path->s);
            HANDLE h = FindFirstFileW(wpat, &e->ffd); /* ffd теперь содержит первый файл подпапки */
            sfree(wpat);
            strbuf_free(sb_path);

            if (h != INVALID_HANDLE_VALUE) {
                new_lev->hfind = h;
                new_lev->next = e->stack;
                e->stack = new_lev;
                e->ffd_valid = true; /* Обработаем первый файл подпапки на след. итерации */
            } else {
                /* Папка пуста или недоступна */
                sfree(new_lev->relpath);
                sfree(new_lev);
                e->ffd_valid = false; 
            }
            sfree(name);
            continue;
        } 
        
        /* 4. Если это файл — проверяем расширение .ini */
        bool found = false;
        size_t nlen = strlen(name);
        if (nlen > 4 && !_stricmp(name + nlen - 4, ".ini")) {
            name[nlen - 4] = '\0';
            strbuf *unid = strbuf_new();
            unescape_registry_key(name, unid);
            
            /* Возвращаем имя в формате "Folder/SubFolder/SessionName" */
            if (*e->stack->relpath)
                put_fmt(out, "%s/%s", e->stack->relpath, unid->s);
            else
                put_datapl(out, ptrlen_from_strbuf(unid));
            
            strbuf_free(unid);
            found = true;
        }

        sfree(name);
        e->ffd_valid = false; /* Файл "потреблен" */
        if (found) return true;
    }

    return false;
}

void fs_enum_settings_finish(settings_e *handle)
{
    struct fs_settings_e *e = (struct fs_settings_e *)handle;
    while (e->stack) {
        FindClose(e->stack->hfind);
        struct dir_level *old = e->stack;
        e->stack = old->next;
        sfree(old->relpath);
        sfree(old);
    }
    sfree(e->root);
    sfree(e);
}

/* ----------------------------------------------------------------------
 * Host keys (single key-value file)
 */

static void fs_hostkey_regname(strbuf *sb, const char *hostname,
                               int port, const char *keytype)
{
    put_fmt(sb, "%s@%d:", keytype, port);
    escape_registry_key(hostname, sb);
}

int fs_check_stored_host_key(const char *hostname, int port,
                             const char *keytype, const char *key)
{
    strbuf *regname = strbuf_new();
    fs_hostkey_regname(regname, hostname, port, keytype);

    char *path = fs_hostkey_file();
    if (!path) {
        strbuf_free(regname);
        return 1;
    }

    kvpair *kv = kv_load_file(path);
    if (!kv) {
        sfree(path);
        strbuf_free(regname);
        return 1;
    }

    kvpair *p = kv_find(kv, regname->s);
    char *other = p && p->value ? kv_decode(p->value) : NULL;
    int compare = other ? strcmp(other, key) : -1;

    sfree(other);
    kv_free_all(kv);
    sfree(path);
    strbuf_free(regname);

    if (!p)
        return 1;
    else if (compare)
        return 2;
    else
        return 0;
}

void fs_store_host_key(Seat *seat, const char *hostname, int port,
                       const char *keytype, const char *key)
{
    char *path = fs_hostkey_file();
    if (!path)
        return;

    kvpair *kv = kv_load_file(path);
    strbuf *regname = strbuf_new();
    fs_hostkey_regname(regname, hostname, port, keytype);

    char *enc = kv_encode(key);
    kv_set(&kv, regname->s, enc);
    sfree(enc);
    strbuf_free(regname);

    kv_save_file_atomic(path, kv);
    kv_free_all(kv);
    sfree(path);
}

/* ----------------------------------------------------------------------
 * Host CAs (one file per record)
 */

struct fs_host_ca_enum {
    HANDLE hfind;
    WIN32_FIND_DATAW ffd;
    bool first;
    char *pattern_utf8;
};

host_ca_enum *fs_enum_host_ca_start(void)
{
    char *dir = fs_hostcas_dir();
    if (!dir)
        return NULL;
    fs_ensure_root_dirs();

    struct fs_host_ca_enum *e = snew(struct fs_host_ca_enum);
    e->pattern_utf8 = dupprintf("%s\\*.ini", dir);
    sfree(dir);

    wchar_t *wpat = dup_mb_to_wc(CP_UTF8, e->pattern_utf8);
    e->hfind = FindFirstFileW(wpat, &e->ffd);
    sfree(wpat);
    e->first = true;

    if (e->hfind == INVALID_HANDLE_VALUE) {
        sfree(e->pattern_utf8);
        sfree(e);
        return NULL;
    }

    return (host_ca_enum *)e;
}

bool fs_enum_host_ca_next(host_ca_enum *handle, strbuf *out)
{
    struct fs_host_ca_enum *e = (struct fs_host_ca_enum *)handle;
    for (;;) {
        bool ok;
        if (e->first) {
            ok = true;
            e->first = false;
        } else {
            ok = FindNextFileW(e->hfind, &e->ffd);
        }
        if (!ok)
            return false;
        if (e->ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char *name_utf8 = dup_wc_to_mb(CP_UTF8, e->ffd.cFileName, NULL);
        size_t len = strlen(name_utf8);
        if (len < 4 || _stricmp(name_utf8 + len - 4, ".ini")) {
            sfree(name_utf8);
            continue;
        }
        name_utf8[len - 4] = '\0';
        unescape_registry_key(name_utf8, out);
        sfree(name_utf8);
        return true;
    }
}

void fs_enum_host_ca_finish(host_ca_enum *handle)
{
    struct fs_host_ca_enum *e = (struct fs_host_ca_enum *)handle;
    if (e->hfind != INVALID_HANDLE_VALUE)
        FindClose(e->hfind);
    sfree(e->pattern_utf8);
    sfree(e);
}

host_ca *fs_host_ca_load(const char *name)
{
    char *path = fs_hostca_file(name);
    if (!path)
        return NULL;

    kvpair *kv = kv_load_file(path);
    if (!kv) {
        sfree(path);
        return NULL;
    }

    host_ca *hca = host_ca_new();
    hca->name = dupstr(name);

    kvpair *p;
    if ((p = kv_find(kv, "PublicKey")) && p->value) {
        char *pub = kv_decode(p->value);
        hca->ca_public_key = base64_decode_sb(ptrlen_from_asciz(pub));
        sfree(pub);
    }
    if ((p = kv_find(kv, "Validity")) && p->value) {
        char *val = kv_decode(p->value);
        hca->validity_expression = val; /* already malloced */
    }

    if ((p = kv_find(kv, "PermitRSASHA1")) && p->value)
        hca->opts.permit_rsa_sha1 = atoi(p->value);
    if ((p = kv_find(kv, "PermitRSASHA256")) && p->value)
        hca->opts.permit_rsa_sha256 = atoi(p->value);
    if ((p = kv_find(kv, "PermitRSASHA512")) && p->value)
        hca->opts.permit_rsa_sha512 = atoi(p->value);

    kv_free_all(kv);
    sfree(path);
    return hca;
}

char *fs_host_ca_save(host_ca *hca)
{
    if (!*hca->name)
        return dupstr("CA record must have a name");

    char *path = fs_hostca_file(hca->name);
    if (!path)
        return dupstr("File storage root directory is unavailable");

    kvpair *kv = NULL;

    strbuf *b64 = base64_encode_sb(ptrlen_from_strbuf(hca->ca_public_key), 0);
    char *enc_pub = kv_encode(b64->s);
    kv_set(&kv, "PublicKey", enc_pub);
    sfree(enc_pub);
    strbuf_free(b64);

    char *enc_val = kv_encode(hca->validity_expression);
    kv_set(&kv, "Validity", enc_val);
    sfree(enc_val);

    char buf[32];
    sprintf(buf, "%d", hca->opts.permit_rsa_sha1);
    kv_set(&kv, "PermitRSASHA1", buf);
    sprintf(buf, "%d", hca->opts.permit_rsa_sha256);
    kv_set(&kv, "PermitRSASHA256", buf);
    sprintf(buf, "%d", hca->opts.permit_rsa_sha512);
    kv_set(&kv, "PermitRSASHA512", buf);

    kv_save_file_atomic(path, kv);
    kv_free_all(kv);
    sfree(path);
    return NULL;
}

char *fs_host_ca_delete(const char *name)
{
    char *path = fs_hostca_file(name);
    if (!path)
        return NULL;
    Filename *fn = filename_from_str(path);
    DeleteFileW(fn->wpath);
    filename_free(fn);
    sfree(path);
    return NULL;
}

/* ----------------------------------------------------------------------
 * Random seed
 */

void fs_read_random_seed(noise_consumer_t consumer)
{
    char *path = fs_randomseed_file();
    if (!path)
        return;

    Filename *fn = filename_from_str(path);
    FILE *fp = f_open(fn, "rb", true);
    filename_free(fn);
    if (!fp) {
        sfree(path);
        return;
    }

    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        consumer(buf, (int)n);
    fclose(fp);
    sfree(path);
}

void fs_write_random_seed(void *data, int len)
{
    char *path = fs_randomseed_file();
    if (!path)
        return;
    fs_ensure_root_dirs();

    Filename *fn = filename_from_str(path);
    FILE *fp = f_open(fn, "wb", true);
    filename_free(fn);
    if (fp) {
        fwrite(data, 1, len, fp);
        fclose(fp);
    }
    sfree(path);
}

/* ----------------------------------------------------------------------
 * Cleanup
 */

static void fs_delete_tree_utf8(const char *dir_utf8)
{
    char *pattern = dupprintf("%s\\*", dir_utf8);
    wchar_t *wpat = dup_mb_to_wc(CP_UTF8, pattern);

    WIN32_FIND_DATAW ffd;
    HANDLE h = FindFirstFileW(wpat, &ffd);
    sfree(wpat);
    sfree(pattern);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (!wcscmp(ffd.cFileName, L".") || !wcscmp(ffd.cFileName, L".."))
            continue;
        char *name = dup_wc_to_mb(CP_UTF8, ffd.cFileName, NULL);
        char *path = dupprintf("%s\\%s", dir_utf8, name);
        sfree(name);

        Filename *fn = filename_from_str(path);
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            filename_free(fn);
            fs_delete_tree_utf8(path);
            fn = filename_from_str(path);
            RemoveDirectoryW(fn->wpath);
        } else {
            DeleteFileW(fn->wpath);
        }
        filename_free(fn);
        sfree(path);
    } while (FindNextFileW(h, &ffd));

    FindClose(h);
}

void fs_cleanup_all(void)
{
    fs_init();
    if (!fs_root_utf8)
        return;

    char *sd = fs_sessions_dir();
    if (sd) {
        fs_delete_tree_utf8(sd);
        Filename *fn = filename_from_str(sd);
        RemoveDirectoryW(fn->wpath);
        filename_free(fn);
        sfree(sd);
    }

    char *hc = fs_hostcas_dir();
    if (hc) {
        fs_delete_tree_utf8(hc);
        Filename *fn = filename_from_str(hc);
        RemoveDirectoryW(fn->wpath);
        filename_free(fn);
        sfree(hc);
    }

    char *hk = fs_hostkey_file();
    if (hk) {
        Filename *fn = filename_from_str(hk);
        DeleteFileW(fn->wpath);
        filename_free(fn);
        sfree(hk);
    }

    char *rs = fs_randomseed_file();
    if (rs) {
        Filename *fn = filename_from_str(rs);
        DeleteFileW(fn->wpath);
        filename_free(fn);
        sfree(rs);
    }
}
