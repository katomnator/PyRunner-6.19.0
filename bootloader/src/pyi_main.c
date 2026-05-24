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

#ifdef _WIN32
    #include <windows.h>
    #include <wchar.h>
#else
    #include <unistd.h>
    #include <errno.h>
#endif

#ifdef __CYGWIN__
    #include <sys/cygwin.h>  /* cygwin_conv_path */
    #include <windows.h>  /* SetDllDirectoryW */
#endif

#include <stdio.h>  /* FILE */
#include <stdlib.h> /* calloc */
#include <string.h> /* memset */

#if defined(__APPLE__) && defined(WINDOWED)
    #include <Carbon/Carbon.h>  /* TransformProcessType */
#endif

#if defined(__APPLE__)
    #include <mach-o/dyld.h>  /* _NSGetExecutablePath() */
#endif

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

#ifdef _WIN32
    /* On Windows, both Visual C runtime and MinGW seem to buffer stderr
     * when redirected. This might cause the output to not appear at all
     * if the application crashes or is terminated, which in turn makes
     * debugging difficult. So make sure that stderr is unbuffered. */
    setbuf(stderr, (char *)NULL);
#endif  /* _WIN32 */

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
        bool is_macos_app_bundle = false;
#if defined(__APPLE__)
        size_t executable_dir_len;
#endif

        /* Determine application's top-level directory based on the
         * executable's location. */
        pyi_path_dirname(executable_dir, pyi_ctx->executable_filename);

#if defined(__APPLE__)
        executable_dir_len = strnlen(executable_dir, PYI_PATH_MAX);
        is_macos_app_bundle = executable_dir_len > 19 && strncmp(executable_dir + executable_dir_len - 19, ".app/Contents/MacOS", 19) == 0;
#endif

        if (is_macos_app_bundle) {
            /* macOS .app bundle; relocate top-level application directory
             * from Contents/MacOS directory to Contents/Frameworks */
            char contents_dir[PYI_PATH_MAX]; /* the parent Contents directory */
            pyi_path_dirname(contents_dir, executable_dir);
            pyi_path_join(pyi_ctx->application_home_dir, contents_dir, "Frameworks");
        } else {
            if (pyi_ctx->contents_subdirectory) {
                pyi_path_join(pyi_ctx->application_home_dir, executable_dir, pyi_ctx->contents_subdirectory);
            } else {
                snprintf(pyi_ctx->application_home_dir, PYI_PATH_MAX, "%s", executable_dir);
            }
        }
    }

    PYI_DEBUG("LOADER: application's top-level directory: %s\n", pyi_ctx->application_home_dir);

    /* Perform necessary modifications to library search path. Do so
     * before we start loading bundled shared libraries (i.e., before
     * trying to start the splash screen, if available). */
#if defined(_WIN32)
    /* In onefile parent process on Windows, attempt to pre-emptively
     * load system copies of VC runtime DLLs (e.g., VCRUNTIME140.dll
     * and VCRUNTIME140_1.dll). The bootloader itself has no need for
     * these DLLs - when building bootloader with MSVC, we statically
     * link both the CRT and VC runtime into the bootloader executable
     * (which allows the onefile executable to be launched on systems
     * without VC redistributable installed and without having to place
     * the VC runtime DLLs next to the executable). However, we need to
     * prevent the bundled copies from application's temporary directory
     * (which are used for example by python shared library loaded in
     * the *child* process of onefile build) from being loaded into this
     * process, because we might end up being unable to unload them and
     * thus remove the files during the cleanup..
     *
     * This issue seems to be caused by the OS, an anti-virus program,
     * or a 3rd party component injecting additional DLLs into our
     * process, and those additional DLLs depending on the VC runtime.
     * Initially, this seemed to affect only builds with splash screen
     * (see the follow-up discussion under #7106), because we need to
     * load Tcl/Tk DLLs, and those depend on the VC runtime; since we
     * unload Tcl/Tk DLLs during splash screen teardown, we free the
     * references on the VC runtime, and are usually able to also unload
     * VC runtime DLLs. This is not the case, however, if the VC runtime
     * DLLs remain locked due to injection of other 3rd party DLLs.
     * #9075 has shown that injection of 3rd party DLLs and subsequent
     * locking of VC runtime DLLs can also happen without splash screen,
     * so we now perform this pre-load in all onefile parent processes. */
    if (pyi_ctx->is_onefile) {
        const wchar_t *dll_names[] = {
            L"VCRUNTIME140.dll",
            L"VCRUNTIME140_1.dll"
        };
        int i;

        /* Avoid accidentally picking up the DLLs from another
         * (instance of) frozen application that might have launched
         * this instance. I.e., call SetDllDirectoryW(NULL) to reset
         * the search path modification that happens in the code block
         * that follows this one (and is inherited by child processes). */
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

    /* Set the DLL search path using `SetDllDirectoryW()`; the change takes
     * effect within the calling process, so we can make this call in
     * each process and regardless of onefile vs. onedir mode. */
    if (1) {
        wchar_t dllpath_w[PYI_PATH_MAX];
        if (pyi_win32_utf8_to_wcs(pyi_ctx->application_home_dir, dllpath_w, PYI_PATH_MAX) == NULL) {
            PYI_ERROR("Failed to convert DLL search path!\n");
            return -1;
        }
        PYI_DEBUG_W(L"LOADER: calling SetDllDirectoryW: %ls\n", dllpath_w);
        SetDllDirectoryW(dllpath_w);
    }
#elif defined(__CYGWIN__)
    /* Under Cygwin, `dlopen()` uses `LD_LIBRARY_PATH` environment
     * variable for library names that do not include path to the
     * library file. However, linked libraries are resolved using
     * Windows' loader, which is controlled by `SetDllDirectoryW()`.
     * Therefore, we need to modify the search path of both mechanisms.
     *
     * Failing to call `SetDllDirectoryW` results in dependencies
     * of python shared library not being resolved when running the
     * frozen application outside of the Cygwin environment.
     *
     * Failing to set `LD_LIBRARY_PATH` seems to cause segmentation
     * faults in worker processes when `multiprocessing` is used
     * (both inside and outside of the Cygwin environment). */
    if (1) {
        wchar_t dllpath_w[PYI_PATH_MAX];
        bool modify_ld_library_path;

        /* Convert POSIX path (whose root is determined by location of
         * the cygwin1.dll) into (wide-char) Windows path that can be
         * passed to `SetDllDirectoryW`. */
        if (cygwin_conv_path(CCP_POSIX_TO_WIN_W | CCP_RELATIVE, pyi_ctx->application_home_dir, dllpath_w, PYI_PATH_MAX) != 0) {
            PYI_PERROR("cygwin_conv_path", "Failed to convert DLL search path!\n");
            return -1;
        }

        /* On Cygwin, we do not have PYI_DEBUG_W macro available; so
         * use %S format to try printing the wide-char string. We can
         * be fairly certain that compiler is not MSVC, so %S does mean
         * wide-char in this context; there might still be garbled text
         * if string contains Unicode characters, but we will take the
         * risk... */
        PYI_DEBUG("LOADER: calling SetDllDirectoryW: %S\n", dllpath_w);
        SetDllDirectoryW(dllpath_w);

        /* Modify `LD_LIBRARY_PATH`, but only if we are the parent process
         * of onefile application, or main process of onedir application.
         * Their child processes will inherit the environment variable,
         * and the attempt to modify it again would result in duplicated
         * entries (and clobbered `LD_LIBRARY_PATH_ORIG`). */
        modify_ld_library_path = pyi_ctx->is_onefile;
        if (modify_ld_library_path) {
            if (pyi_utils_set_library_search_path(pyi_ctx->application_home_dir) < 0) {
                PYI_ERROR("Failed to set library search path via environment variable!\n");
                return -1;
            }
        }
    }
#elif defined(__APPLE__)
    /* No changes to library search path are required on macOS, because
     * we rewrite the library paths on collected binaries. */
#else
    /* Other POSIX OSes; set LD_LIBRARY_PATH for bundled library discovery. */
    if (pyi_utils_set_library_search_path(pyi_ctx->application_home_dir) == -1) {
        PYI_ERROR("Failed to set library search path via environment variable!\n");
        return -1;
    }
#endif

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
#if defined(_WIN32)
        PYI_DEBUG_W(L"LOADER: argv[%d]: %ls\n", i, pyi_ctx->argv_w[i]);
#else
        PYI_DEBUG("LOADER: argv[%d]: %s\n", i, pyi_ctx->argv[i]);
#endif
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
#ifdef _WIN32

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

#elif __APPLE__

static int
_pyi_resolve_executable_macos(char *executable_filename)
{
    char program_path[PYI_PATH_MAX];
    uint32_t name_length = sizeof(program_path);

    /* macOS has special function to obtain path to executable.
     * This may return a symbolic link. */
    if (_NSGetExecutablePath(program_path, &name_length) != 0) {
        PYI_ERROR("Failed to obtain executable path via _NSGetExecutablePath!\n");
        return -1;
    }

    /* Canonicalize the filename and resolve symbolic links */
    if (realpath(program_path, executable_filename) == NULL) {
        PYI_DEBUG("LOADER: failed to resolve full path for %s\n", program_path);
        return -1;
    }

    return 0;
}

#else

#if defined(__linux__)

/* Return 1 if the given executable name is in fact the ld.so dynamic loader. */
static bool
_pyi_is_ld_linux_so(const char *filename)
{
    char basename[PYI_PATH_MAX];
    int status;
    char loader_name[65] = "";
    int soversion = 0;

    pyi_path_basename(basename, filename);

    /* Match the string against ld-*.so.X. In sscanf, the %s is greedy, so
     * instead we match with character group that disallows dot (.). Also
     * limit the name length; note that the output array must be one byte
     * larger, to include the terminating NULL character. */
    status = sscanf(basename, "ld-%64[^.].so.%d", loader_name, &soversion);
    if (status != 2) {
        return false;
    }

    /* If necessary, we could further validate the loader name and soversion
     * against known patterns:
     *  - ld-linux.so.2 (glibc, x86)
     *  - ld-linux-x86-64.so.2 (glibc, x86_64)
     *  - ld-linux-x32.so.2 (glibc, x32)
     *  - ld-linux-aarch64.so.1 (glibc, aarch64)
     *  - ld-musl-x86_64.so.1 (musl, x86_64)
     *  - ...
     */

    return true;
}

#endif /* defined(__linux__) */

/* Search $PATH for the program with the given name, and return its full path. */
static bool
_pyi_find_progam_in_search_path(const char *name, char *result_path)
{
    char *search_paths = pyi_getenv("PATH"); /* returns a copy */
    char *search_path;

    if (search_paths == NULL) {
        return false;
    }

    search_path = strtok(search_paths, PYI_PATHSEPSTR);
    while (search_path != NULL) {
        if ((pyi_path_join(result_path, search_path, name) != NULL) && pyi_path_exists(result_path)) {
            free(search_paths);
            return true;
        }
        search_path = strtok(NULL, PYI_PATHSEPSTR);
    }

    free(search_paths);
    return false;
}

static int
_pyi_resolve_executable_posix(const char *argv0, char *executable_filename, char *loader_filename)
{
    /* On Linux, Cygwin, FreeBSD, and Solaris, we try /proc entry first.
     * The entry points at "true" file location, i.e., fully canonicalized
     * and with all symbolic links resolved. */
    ssize_t name_len = -1;

#if defined(__linux__) || defined(__CYGWIN__)
    name_len = readlink("/proc/self/exe", executable_filename, PYI_PATH_MAX - 1);  /* Linux, Cygwin */
#elif defined(__FreeBSD__)
    name_len = readlink("/proc/curproc/file", executable_filename, PYI_PATH_MAX - 1);  /* FreeBSD */
#elif defined(__sun)
    name_len = readlink("/proc/self/path/a.out", executable_filename, PYI_PATH_MAX - 1);  /* Solaris */
#endif

    if (name_len != -1) {
        /* Output is not yet NULL-terminated, so we need to do it using returned byte count. */
        executable_filename[name_len] = 0;
    }

    /* On linux, we might have been launched using custom ld.so dynamic loader.
     * In that case, /proc/self/exe points to the ld.so executable, and we need
     * to ignore it. */
#if defined(__linux__)
    if (_pyi_is_ld_linux_so(executable_filename) == true) {
        PYI_DEBUG("LOADER: resolved executable file %s is ld.so dynamic linker/loader - storing its name.\n", executable_filename);
        strncpy(loader_filename, executable_filename, PYI_PATH_MAX); /* both buffers are guaranteed to be PYI_PATH_MAX-sized */
        name_len = -1;
    }
#endif

    if (name_len != -1) {
        return 0;
    }

    /* We failed to resolve the executable file via /proc (or we were
     * launched via ld.so dynamic loader). Try to manually resolve the
     * program path/name given via argv[0]. */
    if (strchr(argv0, PYI_SEP)) {
        /* Absolute or relative path was given. Canonicalize it, and
         * resolve symbolic links. */
        PYI_DEBUG("LOADER: resolving program path from argv[0]: %s\n", argv0);
        if (realpath(argv0, executable_filename) == NULL) {
            PYI_DEBUG("LOADER: failed to resolve full path for %s\n", argv0);
            return -1;
        }
    } else {
        /* No path, just program name. Search $PATH for executable with
         * matching name. */
        char program_path[PYI_PATH_MAX];

        if (_pyi_find_progam_in_search_path(argv0, program_path)) {
            /* Program found in $PATH; resolve full path */
            PYI_DEBUG("LOADER: program %s found in PATH: %s. Resolving full path...\n", argv0, program_path);
            if (realpath(program_path, executable_filename) == NULL) {
                PYI_DEBUG("LOADER: failed to resolve full path for %s\n", program_path);
                return -1;
            }
        } else {
            /* Searching $PATH failed; try resolving the name as-is,
             * and hope for the best. NOTE: can we even reach this part?
             * How was the executable even launched in such case? */
            PYI_DEBUG("LOADER: could not find %s in $PATH! Attempting to resolve as-is...\n", argv0);
            if (realpath(argv0, executable_filename) == NULL) {
                PYI_DEBUG("LOADER: failed to resolve full path for %s\n", argv0);
                return -1;
            }
        }
    }

    return 0;
}

#endif


static int
_pyi_main_resolve_executable(struct PYI_CONTEXT *pyi_ctx)
{
    /* Resolve using OS-specific implementation */
#ifdef _WIN32
    return _pyi_resolve_executable_win32(pyi_ctx->executable_filename);
#elif __APPLE__
    return _pyi_resolve_executable_macos(pyi_ctx->executable_filename);
#else
    return _pyi_resolve_executable_posix(pyi_ctx->argv[0], pyi_ctx->executable_filename, pyi_ctx->dynamic_loader_filename);
#endif
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

