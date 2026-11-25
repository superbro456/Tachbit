/* FuturaOS: поправки для сборки LLVM/libc++ поверх его libc */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void abort(void) __attribute__((noreturn));
void exit(int) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
#ifndef _POSIX_TIMERS
#define _POSIX_TIMERS 200809L   /* clock_gettime(CLOCK_MONOTONIC) у него есть */
#endif
#ifndef EX_IOERR
#define EX_IOERR 74      /* sysexits.h у него нет */
#endif
#ifdef __cplusplus
#define restrict __restrict   /* его spawn.h пишет restrict, в C++ такого слова нет */
#endif
