/*
 * ****************************************************************************
 * Copyright (c) 2013-2024, PyInstaller Development Team.
 *
 * Distributed under the terms of the GNU General Public License (version 2
 * or later) with exception for distributing the bootloader.
 *
 * The full license is in the file COPYING.txt, distributed with this software.
 *
 * SPDX-License-Identifier: (GPL-2.0-or-later WITH Bootloader-exception)
 * ****************************************************************************
 */

/*
 * Bootloader for a packed executable.
 */

#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PyInstaller headers. */
#include "pyi_main.h"
#include "pyi_global.h"  /* PYI_PATH_MAX */
#include "pyi_path.h"
#include "pyi_archive.h"
#include "pyi_utils.h"
#include "pyi_launch.h"


/* Global PYI_CONTEXT structure used for bookkeeping of state variables.
 * Since the structure is always used, we can define as static here.
 *
 * We also define a pointer to it, which is intended for use in callbacks
 * and signal handlers that do not allow passing additional data. In
 * accordance with encapsulation principle, it is preferred that the
 * pointer to structure is passed along regular function calls.
 *
 * NOTE: per C standard, static objects are default-initialized, so
 * we do not need explicit zero-initialization.
 */
static struct PYI_CONTEXT _pyi_ctx;


/* Pointer to global PYI_CONTEXT structure. Intended for use in signal
 * handlers that have no user data / context */
struct PYI_CONTEXT *const global_pyi_ctx = &_pyi_ctx;


/* Large parts of `pyi_main` are implemented as helper functions. We
 * keep their definitions below that of `pyi_main`, in an attempt to
 * keep code organized in top-down fashion. Hence, we need forward
 * declarations here */
#if defined(LAUNCH_DEBUG)
static void _pyi_main_dump_command_line_arguments(const struct PYI_CONTEXT *pyi_ctx);
#endif

static void _pyi_main_read_runtime_options(struct PYI_CONTEXT *pyi_ctx);

static int _pyi_main_onedir_or_onefile_child(struct PYI_CONTEXT *pyi_ctx);
static int _pyi_main_onefile_parent(struct PYI_CONTEXT *pyi_ctx);

static int _pyi_main_resolve_executable(struct PYI_CONTEXT *pyi_context);
static int _pyi_main_resolve_pkg_archive(struct PYI_CONTEXT *pyi_context);
static int _pyi_main_resolve_pkg_archive_modified(struct PYI_CONTEXT *pyi_ctx);


int
pyi_main(struct PYI_CONTEXT *pyi_ctx)
{
    char *env_var_value;
    bool reset_environment;

    setbuf(stderr, (char *)NULL);

    PYI_DEBUG("PyRunner v6.19.0\n");
    PYI_DEBUG("PyInstaller Bootloader 6.x\n");

    /* In debug builds, dump the command-line arguments. */
#if defined(LAUNCH_DEBUG)
    _pyi_main_dump_command_line_arguments(pyi_ctx);
#endif

    /* Fully resolve the executable name. */

    if (_pyi_main_resolve_executable(pyi_ctx) < 0) {
        return -1;
    }
    PYI_DEBUG("LOADER: executable file on disk: %s\n", pyi_ctx->executable_filename);

    /* Resolve main PKG archive from exe on disk. */
    if (_pyi_main_resolve_pkg_archive(pyi_ctx) < 0) {
        return -1;
    }
    PYI_DEBUG("LOADER: archive file: %s\n", pyi_ctx->archive_filename);

    /* Resolve main PKG archive from exe on buffer. */
    if (_pyi_main_resolve_pkg_archive_modified(pyi_ctx) < 0) {
        return -1;
    }
    PYI_DEBUG("LOADER: archive file: %s\n", pyi_ctx->archive_filename);

    /* We can now access PKG archive via pyi_ctx->archive; for example,
     * to read run-time options */

    /* Check if archive contains extractable entries - this implies
     * that we are running in onefile mode */
    pyi_ctx->is_onefile = pyi_ctx->archive->contains_extractable_entries;
    PYI_DEBUG("LOADER: application has %s semantics...\n", pyi_ctx->is_onefile ? "onefile" : "onedir");

    /* Check if user explicitly requested environment reset via the
     * PYINSTALLER_RESET_ENVIRONMENT environment variable. In this case,
     * we unconditionally reset the environment and make this process
     * a (new) top-level process. */
    reset_environment = false;

    env_var_value = pyi_getenv("PYINSTALLER_RESET_ENVIRONMENT");
    if (env_var_value) {
        /* Only valid value is 1; anything else is ignored */
        if (strcmp(env_var_value, "1") == 0) {
            PYI_DEBUG("LOADER: explicit environment reset enabled via environment variable!\n");
            reset_environment = true;
        }

        /* Clear the environment variable, to avoid affecting child
         * processes of this process. */
        pyi_unsetenv("PYINSTALLER_RESET_ENVIRONMENT");
    }
    free(env_var_value);

    /* Check if existing PyInstaller run-time environment exists, and
     * determine whether we should inherit it or not. This is done by
     * checking _PYI_ARCHIVE_FILE environment variable:
     *  - if it is not set, there is no environment to inherit. We will
     *    still reset all PyInstaller-related environment variables, in
     *    case whoever ran the process is trying to force the program to
     *    run as independent instance by having unset _PYI_ARCHIVE_FILE.
     *  - if it is set and the contents match our archive filename,
     *    we are using the same archive/executable as the parent process,
     *    and we should inherit its environment.
     *  - if it is set and the contents differ from our archive filename,
     *    then we are a different program from the parent process, and
     *    should reset the environment. */
    if (!reset_environment) {
        reset_environment = true;
        env_var_value = pyi_getenv("_PYI_ARCHIVE_FILE");
        if (env_var_value) {
            PYI_DEBUG("LOADER: _PYI_ARCHIVE_FILE already defined: %s\n", env_var_value);
            if (strcmp(pyi_ctx->archive_filename, env_var_value) == 0) {
                PYI_DEBUG("LOADER: using same archive file as parent environment!\n");
                reset_environment = false;
            } else {
                PYI_DEBUG("LOADER: using different archive file than parent environment!\n");
            }
        } else {
            PYI_DEBUG("LOADER: _PYI_ARCHIVE_FILE not defined...\n");
        }
        free(env_var_value);
    }

    /* Perform the actual environment reset, if necessary */
    if (reset_environment) {
        /* Set the _PYI_ARCHIVE_FILE */
        pyi_setenv("_PYI_ARCHIVE_FILE", pyi_ctx->archive_filename);

        /* Clear PyInstaller environment variables */
        pyi_unsetenv("_PYI_APPLICATION_HOME_DIR");
    }

    /* Read all applicable run-time options from the PKG archive */
    _pyi_main_read_runtime_options(pyi_ctx);

    /* Early console hiding/minimization (Windows-only) */
#if defined(_WIN32) && !defined(WINDOWED)
    if (pyi_ctx->hide_console == PYI_HIDE_CONSOLE_HIDE_EARLY) {
        pyi_win32_hide_console();
    } else if (pyi_ctx->hide_console == PYI_HIDE_CONSOLE_MINIMIZE_EARLY) {
        pyi_win32_minimize_console();
    }
#endif

    /* Read the setting for strict unpack mode from corresponding
     * environment variable. */
    env_var_value = pyi_getenv("PYINSTALLER_STRICT_UNPACK_MODE"); /* strdup'd copy or NULL */
    if (env_var_value) {
        pyi_ctx->strict_unpack_mode = strcmp(env_var_value, "0") != 0;
    }
    free(env_var_value);

    /* Determine the application's top-level directory. */
    if (pyi_ctx->is_onefile) {
        /* Create the ephemeral top-level application directory. */
#if defined(_WIN32)
        PYI_DEBUG("LOADER: initializing security descriptor for temporary directory...\n");
        pyi_ctx->security_attr = pyi_win32_initialize_security_descriptor();
        if (pyi_ctx->security_attr == NULL) {
            PYI_ERROR("Failed to initialize security descriptor for temporary directory!\n");
            return -1;
        }
#endif

        PYI_DEBUG("LOADER: creating temporary directory (runtime_tmpdir=%s)...\n", pyi_ctx->runtime_tmpdir);
        if (pyi_create_temporary_application_directory(pyi_ctx) < 0) {
            PYI_ERROR("Could not create temporary directory!\n");
            return -1;
        }
        PYI_DEBUG("LOADER: created temporary directory: %s\n", pyi_ctx->application_home_dir);

        PYI_DEBUG("LOADER: setting _PYI_APPLICATION_HOME_DIR to %s\n", pyi_ctx->application_home_dir);
        if (pyi_setenv("_PYI_APPLICATION_HOME_DIR", pyi_ctx->application_home_dir) < 0) {
            PYI_ERROR("Failed to set application home directory via environment variable!\n");
            return -1;
        }
    } else {
        char executable_dir[PYI_PATH_MAX];
        pyi_path_dirname(executable_dir, pyi_ctx->executable_filename);
        if (pyi_ctx->contents_subdirectory) {
            pyi_path_join(pyi_ctx->application_home_dir, executable_dir, pyi_ctx->contents_subdirectory);
        } else {
            snprintf(pyi_ctx->application_home_dir, PYI_PATH_MAX, "%s", executable_dir);
        }
    }

    PYI_DEBUG("LOADER: application's top-level directory: %s\n", pyi_ctx->application_home_dir);

    /* Pre-emptively load system copies of VC runtime DLLs to prevent
     * bundled copies from being loaded into this process. */
    if (pyi_ctx->is_onefile) {
        const wchar_t *dll_names[] = {
            L"VCRUNTIME140.dll",
            L"VCRUNTIME140_1.dll"
        };
        int i;

        SetDllDirectoryW(NULL);
        for (i = 0; i < sizeof(dll_names) / sizeof(dll_names[0]); i++) {
            const wchar_t *dll_name = dll_names[i];
            PYI_DEBUG_W(L"LOADER: attempting to pre-load system copy of %ls...\n", dll_name);
            if (LoadLibraryExW(dll_name, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
                PYI_DEBUG_W(L"LOADER: successfully loaded system copy of %ls.\n", dll_name);
            } else {
                PYI_DEBUG_W(L"LOADER: could not load system copy of %ls.\n", dll_name);
            }
        }
    }

    /* Set the DLL search path. */
    {
        wchar_t dllpath_w[PYI_PATH_MAX];
        if (pyi_win32_utf8_to_wcs(pyi_ctx->application_home_dir, dllpath_w, PYI_PATH_MAX) == NULL) {
            PYI_ERROR("Failed to convert DLL search path!\n");
            return -1;
        }
        PYI_DEBUG_W(L"LOADER: calling SetDllDirectoryW: %ls\n", dllpath_w);
        SetDllDirectoryW(dllpath_w);
    }

    if (pyi_ctx->is_onefile) {
        return _pyi_main_onefile_parent(pyi_ctx);
    } else {
        return _pyi_main_onedir_or_onefile_child(pyi_ctx);
    }
}

#if defined(LAUNCH_DEBUG)

static void
_pyi_main_dump_command_line_arguments(const struct PYI_CONTEXT *pyi_ctx)
{
    int i;
    for (i = 0; i < pyi_ctx->argc; i++) {
        PYI_DEBUG_W(L"LOADER: argv[%d]: %ls\n", i, pyi_ctx->argv_w[i]);
    }
}

#endif /* defined(LAUNCH_DEBUG) */

static void
_pyi_main_read_runtime_options(struct PYI_CONTEXT *pyi_ctx)
{
    const struct ARCHIVE *archive = pyi_ctx->archive;
    const struct TOC_ENTRY *toc_entry;

    for (toc_entry = archive->toc; toc_entry < archive->toc_end; toc_entry = pyi_archive_next_toc_entry(archive, toc_entry)) {
        if (toc_entry->typecode != ARCHIVE_ITEM_RUNTIME_OPTION) {
            continue;
        }

        /* NOTE: option names are constants, so we use hard-coded
         * lengths as well to avoid invoking strlen() on each
         * comparison. */

        /* pyi-python-flag <value>
         *
         * Used to pass information about flags that collected python
         * shared library was built with, which might for example affect
         * the layout of PyConfig structure.
         *
         * Currently recongized flags:
         * - Py_GIL_DISABLED
         *
         * Might be specified multiple times, for each such flag. */
        if (strncmp(toc_entry->name, "pyi-python-flag", 15) == 0) {
            const char *flag_name = toc_entry->name + 16;
            if (strncmp(flag_name, "Py_GIL_DISABLED", 15) == 0) {
                pyi_ctx->nogil_enabled = 1;
            }
            continue;
        }

        /* pyi-runtime-tmpdir <value>
         *
         * Run-time temporary directory override for onefile programs. */
        if (strncmp(toc_entry->name, "pyi-runtime-tmpdir", 18) == 0) {
            pyi_ctx->runtime_tmpdir = toc_entry->name + 19;
        }

        /* pyi-contents-directory <value>
         *
         * Contents sub-directory in onedir programs. */
        if (strncmp(toc_entry->name, "pyi-contents-directory", 22) == 0) {
            pyi_ctx->contents_subdirectory = toc_entry->name + 23;
        }

        /* pyi-hide-console <value>
         *
         * Console hiding/minimization option for Windows console-enabled
         * builds. */
#if defined(_WIN32) && !defined(WINDOWED)
        if (strncmp(toc_entry->name, "pyi-hide-console", 16) == 0) {
            const char *option_value = toc_entry->name + 17;
            if (strcmp(option_value, HIDE_CONSOLE_OPTION_HIDE_EARLY) == 0) {
                pyi_ctx->hide_console = PYI_HIDE_CONSOLE_HIDE_EARLY;
            } else if (strcmp(option_value, HIDE_CONSOLE_OPTION_MINIMIZE_EARLY) == 0) {
                pyi_ctx->hide_console = PYI_HIDE_CONSOLE_MINIMIZE_EARLY;
            } else if (strcmp(option_value, HIDE_CONSOLE_OPTION_HIDE_LATE) == 0) {
                pyi_ctx->hide_console = PYI_HIDE_CONSOLE_HIDE_LATE;
            } else if (strcmp(option_value, HIDE_CONSOLE_OPTION_MINIMIZE_LATE) == 0) {
                pyi_ctx->hide_console = PYI_HIDE_CONSOLE_MINIMIZE_LATE;
            } else {
                pyi_ctx->hide_console = PYI_HIDE_CONSOLE_UNUSED;
            }
            continue;
        }
#endif

        /* pyi-disable-windowed-traceback
         *
         * Disable traceback in the unhandled exception message in
         * windowed/noconsole builds (unhandled exception dialog in
         * Windows noconsole builds, syslog message in macOS .app
         * bundles) */
#if defined(WINDOWED)
        if (strncmp(toc_entry->name, "pyi-disable-windowed-traceback", 30) == 0) {
            pyi_ctx->disable_windowed_traceback = 1;
            continue;
        }
#endif

    }
}


/**********************************************************************\
 *                  Onedir or onefile child codepath                  *
\**********************************************************************/
static int
_pyi_main_onedir_or_onefile_child(struct PYI_CONTEXT *pyi_ctx)
{
    int ret;

    /* Late console hiding/minimization; this should turn out to be a
     * no-op in child processes of onefile programs or in spawned
     * additional subprocesses using the executable, because the
     * process does not own the console. */
#if defined(_WIN32) && !defined(WINDOWED)
    if (pyi_ctx->hide_console == PYI_HIDE_CONSOLE_HIDE_LATE) {
        pyi_win32_hide_console();
    } else if (pyi_ctx->hide_console == PYI_HIDE_CONSOLE_MINIMIZE_LATE) {
        pyi_win32_minimize_console();
    }
#endif

#if defined(_WIN32) && defined(WINDOWED)
    {
        MSG msg;
        PostMessageW(NULL, 0, 0, 0);
        GetMessageW(&msg, NULL, 0, 0);
    }
#endif

    /* Main code to initialize Python and run user's code. */
    pyi_launch_initialize(pyi_ctx);
    ret = pyi_launch_execute(pyi_ctx);
    pyi_launch_finalize(pyi_ctx);

    PYI_DEBUG("LOADER: end of process reached!\n");
    return ret;
}


/**********************************************************************\
 *                      Onefile parent codepath                       *
\**********************************************************************/
static int
_pyi_main_onefile_parent(struct PYI_CONTEXT *pyi_ctx)
{
    int ret;

    /* Extract files to temporary directory */
    PYI_DEBUG("LOADER: extracting files to temporary directory...\n");
    if (pyi_launch_extract_files_from_archive(pyi_ctx) < 0) {
        PYI_DEBUG("LOADER: failed to extract files!\n");
        return -1;
    }

    /* At this point, extraction to temporary directory is complete,
     * and we can free the Windows security descriptor that was used
     * during creation of temporary directory and its sub-directories. */
#if defined(_WIN32)
    pyi_win32_free_security_descriptor(&pyi_ctx->security_attr);
#endif

    /* Late console hiding/minimization */
#if defined(_WIN32) && !defined(WINDOWED)
    if (pyi_ctx->hide_console == PYI_HIDE_CONSOLE_HIDE_LATE) {
        pyi_win32_hide_console();
    } else if (pyi_ctx->hide_console == PYI_HIDE_CONSOLE_MINIMIZE_LATE) {
        pyi_win32_minimize_console();
    }
#endif

#if defined(_WIN32) && defined(WINDOWED)
    {
        MSG msg;
        PostMessageW(NULL, 0, 0, 0);
        GetMessageW(&msg, NULL, 0, 0);
    }
#endif

    /* Run the application. */
    ret = _pyi_main_onedir_or_onefile_child(pyi_ctx);

    PYI_DEBUG("LOADER: end of process reached!\n");
    return ret;
}

/**********************************************************************\
 *                     Executable file resolution                     *
\**********************************************************************/
static int
_pyi_resolve_executable_win32(char *executable_filename)
{
    wchar_t modulename_w[PYI_PATH_MAX];

    /* GetModuleFileNameW returns an absolute, fully qualified path */
    if (!GetModuleFileNameW(NULL, modulename_w, PYI_PATH_MAX)) {
        PYI_WINERROR_W(L"GetModuleFileNameW", L"Failed to obtain executable path.\n");
        return -1;
    }

    /* If path is a symbolic link, resolve it */
    if (pyi_win32_is_symlink(modulename_w)) {
        wchar_t executable_filename_w[PYI_PATH_MAX];
        int offset = 0;

        PYI_DEBUG_W(L"LOADER: executable file %ls is a symbolic link - resolving...\n", modulename_w);

        /* Resolve */
        if (pyi_win32_realpath(modulename_w, executable_filename_w) < 0) {
            PYI_ERROR_W(L"Failed to resolve full path to executable %ls.\n", modulename_w);
            return -1;
        }

        /* Remove the extended path indicator, to avoid potential issues due
         * to its appearance in `sys.executable`, `sys._MEIPASS`, etc. */
        if (wcsncmp(L"\\\\?\\", executable_filename_w, 4) == 0) {
            offset = 4;
        }

        /* Convert to UTF-8 */
        if (!pyi_win32_wcs_to_utf8(executable_filename_w + offset, executable_filename, PYI_PATH_MAX)) {
            PYI_ERROR_W(L"Failed to convert executable path to UTF-8.\n");
            return -1;
        }
    } else {
        /* Convert to UTF-8 */
        if (!pyi_win32_wcs_to_utf8(modulename_w, executable_filename, PYI_PATH_MAX)) {
            PYI_ERROR_W(L"Failed to convert executable path to UTF-8.\n");
            return -1;
        }
    }

    return 0;
}



static int
_pyi_main_resolve_executable(struct PYI_CONTEXT *pyi_ctx)
{
    return _pyi_resolve_executable_win32(pyi_ctx->executable_filename);
}


/**********************************************************************\
 *                      Archive file resolution                       *
\**********************************************************************/
static int
_pyi_main_resolve_pkg_archive(struct PYI_CONTEXT *pyi_ctx)
{
    /* Try opening embedded archive first */
    PYI_DEBUG("LOADER: trying to load executable-embedded archive...\n");
    pyi_ctx->archive_disk = pyi_archive_open(pyi_ctx->executable_filename);
    if (pyi_ctx->archive_disk != NULL) {
        /* Copy executable filename to archive filename; we know it does not exceed PYI_PATH_MAX */
        snprintf(pyi_ctx->archive_filename, PYI_PATH_MAX, "%s", pyi_ctx->executable_filename);
        return 0;}
    else {
        return -1;}
}


static int
_pyi_main_resolve_pkg_archive_modified(struct PYI_CONTEXT *pyi_ctx)
{
    /* Try opening embedded archive first */
    PYI_DEBUG("LOADER: trying to load executable-embedded archive...\n");
    pyi_ctx->archive = pyi_archive_open_modified(pyi_ctx->exe_buffer);
    if (pyi_ctx->archive != NULL) {
        return 0;}
    else {
        return -1;}
}

