#pragma once
#include <stdio.h>
#include <stdarg.h>
struct HostLog {
    void log(const char* pfx, const char* fmt, va_list ap) {
        fprintf(stderr, "[%s] ", pfx); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    }
    void errorln(const char* f, ...)   { va_list a; va_start(a, f); log("E", f, a); va_end(a); }
    void warningln(const char* f, ...) { va_list a; va_start(a, f); log("W", f, a); va_end(a); }
    void infoln(const char* f, ...)    { va_list a; va_start(a, f); log("I", f, a); va_end(a); }
};
extern HostLog Log;
