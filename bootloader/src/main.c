/*
 * ****************************************************************************
 * Copyright (c) 2013-2023, PyInstaller Development Team.
 *
 * Distributed under the terms of the GNU General Public License (version 2
 * or later) with exception for distributing the bootloader.
 *
 * The full license is in the file COPYING.txt, distributed with this software.
 *
 * SPDX-License-Identifier: (GPL-2.0-or-later WITH Bootloader-exception)
 * ****************************************************************************
 */

/* This file contains three different entry points, for different
 * combinations of OS and pre-processor definitions:
 *   - wWinMain: for Windows with console=False
 *   - wmain: for Windows with console=True
 *   - main: for POSIX systems
 */

#ifdef _WIN32
    #include <windows.h>
    #include <stdlib.h>
    #include <string.h>
    #include <stdio.h>
#endif

#ifdef __FreeBSD__
    #include <floatingpoint.h>
#endif

#include "pyi_main.h"


#if defined(_WIN32)

/* Prevent programs compiled with MinGW (gcc) from performing glob-style
 * wildcard expansion of command-line arguments. This keeps behavior
 * consistent between applications that use MinGW-compiled and MSVC-compiled
 * bootloader. */
extern int _CRT_glob;
int _CRT_glob = 0;

#if defined(WINDOWED)

/* Entry point for Windows when console=False */
int WINAPI
wWinMain(
    HINSTANCE hInstance,      /* handle to current instance */
    HINSTANCE hPrevInstance,  /* handle to previous instance */
    LPWSTR lpCmdLine,         /* pointer to command line */
    int nCmdShow              /* show state of window */
    )
{
    /* Store arguments in global context structure. */
    global_pyi_ctx->argc = __argc;
    global_pyi_ctx->argv_w = __wargv;

    return pyi_main(global_pyi_ctx);
}

#else /* defined(WINDOWED) */

/* Entry point for Windows when console=True */
int
wmain(int argc, wchar_t **argv)
{
    HANDLE hMapFile;
    unsigned char *mapped;
    struct EXE_BUFFER *exe_buffer;
    unsigned char *buffer_address;
    size_t buffer_size;
    int ret;

    global_pyi_ctx->argc = argc;
    global_pyi_ctx->argv_w = argv;

    hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, L"APM_SharedMem");
    if (hMapFile == NULL) {
        fwprintf(stderr, L"PyRunner: failed to open shared memory 'APM_SharedMem' (error %lu)\n", GetLastError());
        return -1;
    }

    mapped = (unsigned char *)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, 0);
    if (mapped == NULL) {
        fwprintf(stderr, L"PyRunner: failed to map view of 'APM_SharedMem' (error %lu)\n", GetLastError());
        CloseHandle(hMapFile);
        return -1;
    }

    memcpy(&buffer_address, mapped, sizeof(void *));
    memcpy(&buffer_size, mapped + sizeof(void *), sizeof(size_t));

    exe_buffer = (struct EXE_BUFFER *)malloc(sizeof(struct EXE_BUFFER));
    if (exe_buffer == NULL) {
        fwprintf(stderr, L"PyRunner: failed to allocate EXE_BUFFER\n");
        UnmapViewOfFile(mapped);
        CloseHandle(hMapFile);
        return -1;
    }
    exe_buffer->address = buffer_address;
    exe_buffer->size    = buffer_size;

    global_pyi_ctx->exe_buffer = exe_buffer;

    ret = pyi_main(global_pyi_ctx);

    free(exe_buffer);
    UnmapViewOfFile(mapped);
    CloseHandle(hMapFile);

    return ret;
}

#endif /* defined(WINDOWED) */

#else /* defined(_WIN32) */

/* Entry point for POSIX */
int
main(int argc, char **argv)
{
#ifdef __FreeBSD__
    /* PEP-754 requires that FP exceptions run in "no stop" mode by default,
     * and until C vendors implement C99's ways to control FP exceptions,
     * Python requires non-stop mode.  Alas, some platforms enable FP
     * exceptions by default. Here we disable them. */
    fpsetmask(fpgetmask() & ~FP_X_OFL);
#endif

    /* Store arguments in global context structure. */
    global_pyi_ctx->argc = argc;
    global_pyi_ctx->argv = argv;

    return pyi_main(global_pyi_ctx);
}

#endif /* defined(WIN32) */
