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

#ifndef PYI_MAIN_H
#define PYI_MAIN_H

#include "pyi_global.h"
#include <stddef.h>

struct ARCHIVE;
struct DYLIB_PYTHON;


/* Console hiding/minimization options. Windows only. */
#if defined(_WIN32) && !defined(WINDOWED)

/* bootloader option strings */
#define HIDE_CONSOLE_OPTION_HIDE_EARLY "hide-early"
#define HIDE_CONSOLE_OPTION_HIDE_LATE "hide-late"
#define HIDE_CONSOLE_OPTION_MINIMIZE_EARLY "minimize-early"
#define HIDE_CONSOLE_OPTION_MINIMIZE_LATE "minimize-late"

/* values used in PYI_CONTEXT field */
enum PYI_HIDE_CONSOLE
{
    PYI_HIDE_CONSOLE_UNUSED = 0,
    PYI_HIDE_CONSOLE_HIDE_EARLY = 1,
    PYI_HIDE_CONSOLE_HIDE_LATE = 2,
    PYI_HIDE_CONSOLE_MINIMIZE_EARLY = 3,
    PYI_HIDE_CONSOLE_MINIMIZE_LATE = 4
};

#endif


struct EXE_BUFFER
{
    /* Address to the buffer containing the executable */
    unsigned char *address;

    /* Size of the buffer */
    size_t size;
};

struct PYI_CONTEXT
{
    /* Command line arguments passed to the application.
     *
     * On Windows, these are wide-char (UTF-16) strings, which can be
     * directly passed into python's configuration structure.
     *
     * On POSIX systems, the strings are in local 8-bit encoding, and we
     * will need to convert them to wide-char strings when setting up
     * python's configuration structure. But in POSIX codepath, the 8-bit
     * strings from `argv` are also used in other places, for example,
     * when trying to resolve the executable's true location, and when
     * spawning child process in onefile mode. */
    int argc;
    wchar_t **argv_w;

    /* Contains the address and size of the buffer that holds our pyinstaller exe (tool) */
    struct EXE_BUFFER *exe_buffer;

    /* Fully resolved path to the executable */
    char executable_filename[PYI_PATH_MAX];

    /* Fully resolved path to the main PKG archive */
    char archive_filename[PYI_PATH_MAX];

    /* Main PKG archive - Extracted from umodified pyinstaller exe on buffer */
    struct ARCHIVE *archive;

    /* Main PKG archive - Extracted from modified pyinstaller exe on disk */
    struct ARCHIVE *archive_disk;

    /* Flag indicating whether the application's main PKG archive has
     * onefile semantics or not (i.e., needs to extract files to
     * temporary directory). */
    unsigned char is_onefile;

    /* Application's top-level directory (sys._MEIPASS), where the data
     * and shared libraries are. For applications with onefile semantics,
     * this is ephemeral temporary directory where application unpacked
     * itself. */
    char application_home_dir[PYI_PATH_MAX];

    /* Structure encapsulating loaded python shared library and pointers
     * to imported functions. */
    struct DYLIB_PYTHON *dylib_python;

    /* Strict unpack mode for onefile builds. This flag is dynamically
     * controlled by `PYINSTALLER_STRICT_UNPACK_MODE` environment variable
     * (enabled by a value different from 0). If enabled, extraction of
     * onefile builds (either splash screen dependencies, or main archive
     * unpacking) fails if trying to overwrite an existing file. Otherwise,
     * a warning is displayed on stderr. This is primarily used for
     * run-time detection of duplicated resources in onefile archives on
     * PyInstaller's CI. */
    unsigned char strict_unpack_mode;

#if defined(_WIN32)
    /* Security attributes structure with security descriptor that limits
     * the access to created directory to the user. Used in onefile mode
     * with `CreateDirectoryW` when creating the application's temporary
     * top-level directory and its sub-directories.
     *
     * Must be explicitly initialized by calling
     * `pyi_win32_initialize_security_descriptor`, and freed by calling
     * `pyi_win32_free_security_descriptor`. */
    SECURITY_ATTRIBUTES *security_attr;
#endif

    /**
     * Runtime options
     */

    /* Run-time temporary directory path in onefile builds. If this
     * option is not specified, the OS-configured temporary directory
     * is used.
     *
     * NOTE: if non-NULL, the pointer points at the TOC buffer entry in
     * the `archive` structure! */
    const char *runtime_tmpdir;

    /* Contents sub-directory in onedir builds.
     *
     * NOTE: if non-NULL, the pointer points at the TOC buffer entry in
     * the `archive` structure! */
    const char *contents_subdirectory;

    /* Console hiding/minimization options for Windows console builds. */
#if defined(_WIN32) && !defined(WINDOWED)
    unsigned char hide_console;
#endif

    /* Disable traceback in the unhandled exception message in
     * windowed/noconsole builds (unhandled exception dialog in
     * Windows noconsole builds, syslog message in macOS .app
     * bundles) */
#if defined(WINDOWED)
    unsigned char disable_windowed_traceback;
#endif

    /**
     * Flag indicating that colleted python shared library was built
     * with --disable-gil / Py_GIL_DISABLED. Used to select correct
     * PyConfig structure layout, which contains additional `enable_gil`
     * field. */
    unsigned char nogil_enabled;
};

extern struct PYI_CONTEXT *const global_pyi_ctx;


int pyi_main(struct PYI_CONTEXT *pyi_ctx);


#endif /* PYI_MAIN_H */
