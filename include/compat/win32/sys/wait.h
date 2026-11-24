/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/compat/win32/sys/wait.h - MSVC emulation of sys/wait.h
 */

#ifndef OPENAVX_COMPAT_WIN32_SYS_WAIT_H
#define OPENAVX_COMPAT_WIN32_SYS_WAIT_H

#ifdef _WIN32
#ifndef WIFEXITED
#define WIFEXITED(s) 1
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(s) (s)
#endif
#ifndef WTERMSIG
#define WTERMSIG(s) 0
#endif
#ifndef WIFSIGNALED
#define WIFSIGNALED(s) 0
#endif
#endif /* _WIN32 */

#endif /* OPENAVX_COMPAT_WIN32_SYS_WAIT_H */
