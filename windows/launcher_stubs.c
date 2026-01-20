#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

void nonfatal(const char *fmt, ...)
{
    /* можно лог/MessageBox, но достаточно молча */
    (void)fmt;
}

void modalfatalbox(const char *fmt, ...)
{
    /* минимум: показать MessageBox и выйти */
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    MessageBoxA(NULL, buf, "PuTTY launcher fatal", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

void clear_jumplist(void) {}
void remove_session_from_jumplist(const char *sessionname) { (void)sessionname; }
