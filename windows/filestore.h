/*
 * filestore.h: file-based alternative to the Windows registry storage
 * backend.
 */

#ifndef PUTTY_WIN_FILESTORE_H
#define PUTTY_WIN_FILESTORE_H

#include "putty.h"
#include "storage.h"

/* Decide whether file storage is enabled for this process. */
bool win_use_file_storage(void);

/*
 * Return UTF-8 path to the root directory for file storage.
 * Caller must free.
 */
char *win_file_storage_root(void);

/* Global settings stored in root\putty.ini. */
Filename *win_load_winscp_path(void);
bool win_save_winscp_path(const Filename *path);

/* Session settings */
settings_w *fs_open_settings_w(const char *sessionname, char **errmsg);
void fs_write_setting_s(settings_w *handle, const char *key, const char *value);
void fs_write_setting_i(settings_w *handle, const char *key, int value);
void fs_write_setting_filename(settings_w *handle, const char *key, Filename *value);
void fs_write_setting_fontspec(settings_w *handle, const char *key, FontSpec *font);
void fs_close_settings_w(settings_w *handle);

settings_r *fs_open_settings_r(const char *sessionname);
char *fs_read_setting_s(settings_r *handle, const char *key);
int fs_read_setting_i(settings_r *handle, const char *key, int defvalue);
Filename *fs_read_setting_filename(settings_r *handle, const char *key);
FontSpec *fs_read_setting_fontspec(settings_r *handle, const char *key);
void fs_close_settings_r(settings_r *handle);

void fs_del_settings(const char *sessionname);
settings_e *fs_enum_settings_start(void);
bool fs_enum_settings_next(settings_e *handle, strbuf *out);
void fs_enum_settings_finish(settings_e *handle);

/* Host keys */
int fs_check_stored_host_key(const char *hostname, int port,
                             const char *keytype, const char *key);
void fs_store_host_key(Seat *seat, const char *hostname, int port,
                       const char *keytype, const char *key);

/* Host certification authorities */
host_ca_enum *fs_enum_host_ca_start(void);
bool fs_enum_host_ca_next(host_ca_enum *handle, strbuf *out);
void fs_enum_host_ca_finish(host_ca_enum *handle);
host_ca *fs_host_ca_load(const char *name);
char *fs_host_ca_save(host_ca *hca);
char *fs_host_ca_delete(const char *name);

/* Random seed */
void fs_read_random_seed(noise_consumer_t consumer);
void fs_write_random_seed(void *data, int len);

/* Cleanup */
void fs_cleanup_all(void);

#endif
