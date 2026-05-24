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

/*
 * Utility functions. This file contains implementations that are specific
 * to Windows.
 */

/* Having a header included outside of the ifdef block prevents the compilation
 * unit from becoming empty, which is disallowed by pedantic ISO C. */
#include "pyi_global.h"

#ifdef _WIN32

#include <windows.h>
#include <process.h> /* _getpid */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sddl.h> /* ConvertStringSecurityDescriptorToSecurityDescriptorW */

/* PyInstaller headers. */
#include "pyi_utils.h"
#include "pyi_path.h"
#include "pyi_main.h"


/**********************************************************************\
 *                  Environment variable management                   *
\**********************************************************************/
char *
pyi_getenv(const char *variable)
{
    wchar_t *variable_w;
    wchar_t value[PYI_PATH_MAX];
    wchar_t expanded_value[PYI_PATH_MAX];
    DWORD rc;

    /* Convert the variable name from UTF-8 to wide-char */
    variable_w = pyi_win32_utf8_to_wcs(variable, NULL, 0);

    /* Retrieve environment variable */
    rc = GetEnvironmentVariableW(variable_w, value, PYI_PATH_MAX);
    if (rc >= PYI_PATH_MAX) {
        return NULL; /* Insufficient buffer size */
    }
    if (rc == 0) {
        return NULL; /* Variable unavailable */
    }

    /* Expand environment variables within the environment variable's
     * value */
    rc = ExpandEnvironmentStringsW(value, expanded_value, PYI_PATH_MAX);
    if (rc >= PYI_PATH_MAX) {
        return NULL; /* Insufficient buffer size */
    }
    if (rc == 0) {
        return NULL; /* Error during expansion */
    }

    /* Convert to UTF-8 and return */
    return pyi_win32_wcs_to_utf8(expanded_value, NULL, 0);
}

int
pyi_setenv(const char *variable, const char *value)
{
    int rc;
    wchar_t *variable_w;
    wchar_t *value_w;

    /* Convert from UTF-8 to wide-char */
    variable_w = pyi_win32_utf8_to_wcs(variable, NULL, 0);
    value_w = pyi_win32_utf8_to_wcs(value, NULL, 0);

    /* `SetEnvironmentVariableW` updates only the value in the process
     * environment block, while _wputenv_s updates the value in the CRT
     * block AND calls `SetEnvironmentVariableW` to update the process
     * environment block.
     *
     * Therefore, in order for modification to be visible to other CRT
     * functions (for example, `_wtempnam`), we must use `_wputenv_s`. */
    rc = _wputenv_s(variable_w, value_w);

    free(variable_w);
    free(value_w);

    return rc;
}

int
pyi_unsetenv(const char *variable)
{
    int rc;
    wchar_t *variable_w;

    /* Convert from UTF-8 to wide-char */
    variable_w = pyi_win32_utf8_to_wcs(variable, NULL, 0);

    /* See the comment in `pyi_setenv`. As per MSDN, "You can remove a
     * variable from the environment by specifying an empty string (that
     * is, "") for value_string. */
    rc = _wputenv_s(variable_w, L"");

    free(variable_w);

    return rc;
}


/**********************************************************************\
 *         Temporary application top-level directory (onefile)        *
\**********************************************************************/
/* Resolve the temporary directory specified by user via runtime_tmpdir
 * option, and create corresponding directory tree. */
static wchar_t *
_pyi_create_runtime_tmpdir(const char *runtime_tmpdir)
{
    wchar_t *runtime_tmpdir_w;
    wchar_t runtime_tmpdir_expanded[PYI_PATH_MAX];
    wchar_t *runtime_tmpdir_abspath;
    wchar_t *subpath_cursor;
    wchar_t directory_tree_path[PYI_PATH_MAX];
    DWORD rc;

    /* Convert UTF-8 path to wide-char */
    runtime_tmpdir_w = pyi_win32_utf8_to_wcs(runtime_tmpdir, NULL, 0);
    if (!runtime_tmpdir_w) {
        PYI_ERROR_W(L"LOADER: failed to convert runtime-tmpdir to a wide string.\n");
        return NULL;
    }

    /* Expand environment variables like %LOCALAPPDATA% */
    rc = ExpandEnvironmentStringsW(runtime_tmpdir_w, runtime_tmpdir_expanded, PYI_PATH_MAX);
    free(runtime_tmpdir_w);
    if (!rc) {
        PYI_ERROR_W(L"LOADER: failed to expand environment variables in the runtime-tmpdir.\n");
        return NULL;
    }

    /* Check if runtime-tmpdir is just a disk drive root (e.g., "c:\").
     * If so, validate the drive's existence, and return the path as-is.
     * There is no path to resolve (and calling `_wfullpath()` would end
     * up returning current working directory on the current drive), and
     * there is no directory structure to create */
    if (pyi_win32_is_drive_root(runtime_tmpdir_expanded)) {
        int drive_type = 0;
        PYI_DEBUG_W(L"LOADER: expanded runtime-tmpdir is a drive root: %ls\n", runtime_tmpdir_expanded);

        /* Ensure the given volume name is valid, i.e., includes the
         * backslash as per Windows path naming conventions:
         * https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file
         * The `pyi_win32_is_drive_root` check ensures that the path is
         * either two or three characters long, so the third character
         * is either the backslash or terminating NUL. */
        if (runtime_tmpdir_expanded[2] != L'\\') {
            PYI_DEBUG_W(L"LOADER: appending backslash to the given drive root %ls\n", runtime_tmpdir_expanded);
            wcscat(runtime_tmpdir_expanded, L"\\");
        }

        /* Now ensure that the drive actually exists - raise error on
         * DRIVE_UNKNOWN (0) and DRIVE_NO_ROOT_DIR (1) */
        drive_type = GetDriveTypeW(runtime_tmpdir_expanded);
        if (drive_type <= DRIVE_NO_ROOT_DIR) {
            PYI_ERROR_W(L"LOADER: runtime-tmpdir points to non-existent drive %ls (type: %d)!\n", runtime_tmpdir_expanded, drive_type);
            return NULL;
        }

        return _wcsdup(runtime_tmpdir_expanded);
    }

    /* Resolve absolute path */
    runtime_tmpdir_abspath = _wfullpath(NULL, runtime_tmpdir_expanded, PYI_PATH_MAX);
    if (!runtime_tmpdir_abspath) {
        PYI_ERROR_W(L"LOADER: failed to obtain the absolute path of the runtime-tmpdir.\n");
        return NULL;
    }

    PYI_DEBUG_W(L"LOADER: absolute runtime-tmpdir is %ls\n", runtime_tmpdir_abspath);

    /* Recursively create the directory structure
     *
     * NOTE: we call CreateDirectoryW without security descriptor for
     * this part of directory tree, as it might be shared by application
     * instances ran by different users. Only the last component (the
     * actual _MEIXXXXXX directory), created by the caller, uses security
     * descriptor to restrict access to current user.
     *
     * NOTE2: we ignore errors returned by CreateDirectoryW; if we
     * actually fail to create (a part of) directory tree here, we will
     * catch the error in the caller when trying to create the final
     * temporary directory component. */
    for(subpath_cursor = wcschr(runtime_tmpdir_abspath, L'\\'); subpath_cursor != NULL; subpath_cursor = wcschr(++subpath_cursor, L'\\')) {
        int subpath_length = (int)(subpath_cursor - runtime_tmpdir_abspath);

        _snwprintf(directory_tree_path, PYI_PATH_MAX, L"%.*s", subpath_length, runtime_tmpdir_abspath);
        PYI_DEBUG_W(L"LOADER: creating runtime-tmpdir path component: %ls\n", directory_tree_path);
        CreateDirectoryW(directory_tree_path, NULL);
    }

    /* Run once more on full path, to handle cases when path did not end
     * with separator. Here, we also explicitly check that the call
     * succeeded or failed with ERROR_ALREADY_EXISTS, to properly report
     * errors in creation of temporary directory tree. */
    PYI_DEBUG_W(L"LOADER: creating runtime-tmpdir path: %ls\n", runtime_tmpdir_abspath);
    if (CreateDirectoryW(runtime_tmpdir_abspath, NULL) == 0) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            PYI_WINERROR_W(L"CreateDirectory", L"LOADER: failed to create runtime-tmpdir path %ls!\n", runtime_tmpdir_abspath);
            free(runtime_tmpdir_abspath);
            return NULL;
        }
    }

    return runtime_tmpdir_abspath;
}

int
pyi_create_temporary_application_directory(struct PYI_CONTEXT *pyi_ctx)
{
    char *original_tmp_value = NULL;
    wchar_t prefix[16];
    wchar_t tempdir_path[PYI_PATH_MAX];
    int ret = 0;
    int i;

    /* If user specified the temporary directory via runtime_tmpdir
     * option, resolve it, create it, and store the path to TMP
     * environment variable to have`GetTempPathW` use it. */
    if (pyi_ctx->runtime_tmpdir != NULL) {
        wchar_t *runtime_tmpdir_w;
        DWORD rc;

        /* Retrieve original value of TMP environment variable, so we
         * can restore it at the very end of this function. */
        original_tmp_value = pyi_getenv("TMP");

        /* Resolve and create directory specified via the runtime_tmpdir
         * option. */
        runtime_tmpdir_w = _pyi_create_runtime_tmpdir(pyi_ctx->runtime_tmpdir);
        if (runtime_tmpdir_w == NULL) {
            free(original_tmp_value);
            return -1;
        }

        /* Store the path in the TMP environment variable. */
        rc = _wputenv_s(L"TMP", runtime_tmpdir_w);
        free(runtime_tmpdir_w);
        if (rc) {
            PYI_ERROR_W(L"LOADER: failed to set the TMP environment variable.\n");
            free(original_tmp_value);
            return -1;
        }

        PYI_DEBUG_W(L"LOADER: successfully resolved the specified runtime-tmpdir\n");
    }

    /* Retrieve temporary directory */
    GetTempPathW(PYI_PATH_MAX, tempdir_path);
    PYI_DEBUG_W(L"LOADER: attempting to create temporary application directory under %ls\n", tempdir_path);

    /* Create _MEI + PID prefix */
    swprintf(prefix, 16, L"_MEI%d", _getpid());

    /* Windows does not have a race-free function to create a temporary
     * directory. Thus, we rely on _tempnam, and simply try several times
     * to avoid stupid race conditions. */
    for (i = 0; i < 5; i++) {
        wchar_t *application_home_dir_w = _wtempnam(tempdir_path, prefix);

        /* Try creating the directory. Use `CreateDirectoryW` with security
         * descriptor to limit access to current user. The functon returns
         * 0 on failure. */
        if (CreateDirectoryW(application_home_dir_w, pyi_ctx->security_attr) == 0) {
            free(application_home_dir_w);
            ret = -1; /* In case we reached max. retries */
        } else {
            /* Convert path to UTF-8 and store it in main context structure */
            if (pyi_win32_wcs_to_utf8(application_home_dir_w, pyi_ctx->application_home_dir, PYI_PATH_MAX) == NULL) {
                PYI_ERROR_W(L"LOADER: length of teporary directory path exceeds maximum path length!\n");
                ret = -1;
            } else {
                ret = 0; /* Re-set to zero in case this is not first iteration. */
            }
            free(application_home_dir_w);
            break;
        }
    }

    /* If we modified TMP environment variable due to runtime_tmpdir
     * option, restore the environment variable to its original state. */
    if (pyi_ctx->runtime_tmpdir != NULL) {
        if (original_tmp_value!= NULL) {
            pyi_setenv("TMP", original_tmp_value);
            free(original_tmp_value);
        } else {
            pyi_unsetenv("TMP");
        }
    }

    return ret;
}


/**********************************************************************\
 *                  Recursive removal of a directory                  *
\**********************************************************************/
/* The actual implementation with wide-char path */
static int
_pyi_recursive_rmdir(const wchar_t *dir_path)
{
    int dir_path_length;
    int buffer_size;
    wchar_t entry_path[PYI_PATH_MAX];
    HANDLE handle;
    WIN32_FIND_DATAW entry_info;

    /* Copy the directory path, and append separator and a wildcard for
     * the `FindFirstFileW()` call. Store the length of the directory
     * path plus the separator; this allows us to re-use the same buffer
     * for constructing entries' full paths, by overwriting only the
     * part of the string that follows the path separator that we added. */
    dir_path_length = _snwprintf(entry_path, PYI_PATH_MAX, L"%s\\*", dir_path);
    if (dir_path_length >= PYI_PATH_MAX) {
        return -1;
    }
    dir_path_length--; /* Ignore the wildcard at the end */
    buffer_size = PYI_PATH_MAX - dir_path_length; /* Remaining buffer size */

    /* Start the search by looking for first entry */
    handle = FindFirstFileW(entry_path, &entry_info);
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    do {
        /* Skip . and .. */
        if (wcscmp(entry_info.cFileName, L".") == 0 || wcscmp(entry_info.cFileName, L"..") == 0) {
            continue;
        }

        /* Construct the full path, by overwriting the part of string
         * that starts after path directory and separator. */
        if (_snwprintf(entry_path + dir_path_length, buffer_size, L"%s", entry_info.cFileName) >= buffer_size) {
            continue;
        }

        /* Deteremine the type of entry, and remove it. On errors, emit
         * debug messages to simplify debugging, and keep going on. We
         * want to remove everything we can; if we fail to remove an
         * entry here, we will also fail to remove the top-level
         * directory, and will return error there and then.
         * NOTE: do NOT emit warning or error messages here, as those
         * use dialogs in windowed/noconsole mode, and we don't want to
         * spam user with those! */
        if (entry_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* Avoid recursing into symlinked directories */
            unsigned char is_symlink = 0;

            if (entry_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (entry_info.dwReserved0 == IO_REPARSE_TAG_SYMLINK) {
                    is_symlink = 1;
                }
            }

            if (is_symlink) {
                /* Remove only the symlink itself; return value is 1 on
                 * success, 0 on failure. */
                if (RemoveDirectoryW(entry_path) == 0) {
                    PYI_DEBUG_W(L"LOADER: failed to remove directory symbolic link: %ls\n", entry_path);
                }
            } else {
                /* Recurse into directory; return value is 0 on success,
                 * -1 on failure. */
                if (_pyi_recursive_rmdir(entry_path) < 0) {
                    PYI_DEBUG_W(L"LOADER: failed to remove directory: %ls\n", entry_path);
                }
            }
        } else {
            /* Delete file (or symlink to a file); return value is 1 on
             * success, 0 on failure. */
            if (DeleteFileW(entry_path) == 0) {
                PYI_DEBUG_W(L"LOADER: failed to remove file: %ls\n", entry_path);
            }
        }
    } while (FindNextFileW(handle, &entry_info) != 0);

    FindClose(handle);

    /* Finally, remove the directory */
    return RemoveDirectoryW(dir_path) != 1 ? -1 : 0; /* false/true-> -1/0 */
}


/* For now, the caller is supplying narrow-char path in  UTF-8 encoding. */
int
pyi_recursive_rmdir(const char *dir_path)
{
    wchar_t dir_path_w[PYI_PATH_MAX];
    pyi_win32_utf8_to_wcs(dir_path, dir_path_w, PYI_PATH_MAX);
    return _pyi_recursive_rmdir(dir_path_w);
}


/**********************************************************************\
 *             Security descriptor for temporary directory            *
\**********************************************************************/
/* Retrieve the SID for the specified token information class from the current process.
 *
 * At the moment, TokenUser and TokenAppContainerSid are supported.
 *
 * Used in a compatibility work-around for wine, which at the time of writing
 * (version 5.0.2) does not properly support SID S-1-3-4 (directory owner),
 * and therefore user's actual SID must be used instead.
 *
 * Returns a copy of SID string on success, NULL on failure (or if the SID is unavailable
 * or zero-length). The returned string must be freed using LocalFree().
 */
static wchar_t *
_pyi_win32_get_sid(TOKEN_INFORMATION_CLASS token_information_class)
{
    HANDLE process_token = INVALID_HANDLE_VALUE;
    DWORD token_info_size = 0;
    void *token_info = NULL;
    wchar_t *sid = NULL;

    /* Get access token for the calling process */
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token)) {
        goto cleanup;
    }

    /* Query buffer size and allocate buffer */
    if (!GetTokenInformation(process_token, token_information_class, NULL, 0, &token_info_size)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            goto cleanup;
        }
    }
    if (token_info_size == 0) {
        /* As per MSDN, in the Microsoft implementation, if number or size is zero, calloc
         * returns a pointer to an allocated block of non-zero size. An attempt to read or
         * write through the returned pointer leads to undefined behavior. */
        goto cleanup;
    }
    token_info = calloc(1, token_info_size);
    if (!token_info) {
        goto cleanup;
    }

    /* Get token information */
    if (!GetTokenInformation(process_token, token_information_class, token_info, token_info_size, &token_info_size)) {
        goto cleanup;
    }

    /* Convert SID to string */
    switch (token_information_class)
    {
        case TokenUser: {
            PTOKEN_USER user_info = (PTOKEN_USER)token_info;
            ConvertSidToStringSidW(user_info->User.Sid, &sid);
            break;
        }
        case TokenAppContainerSid: {
            PTOKEN_APPCONTAINER_INFORMATION app_container_info = (PTOKEN_APPCONTAINER_INFORMATION)token_info;
            ConvertSidToStringSidW(app_container_info->TokenAppContainer, &sid);
            break;
        }
        default: {
            /* Unsupoorted token information class */
            break;
        }
    }

    /* Cleanup */
cleanup:
    free(token_info);
    if (process_token != INVALID_HANDLE_VALUE) {
        CloseHandle(process_token);
    }

    return sid;
}

/* Initialize security descriptor applied to application's temporary directory and its
 * sub-directories.
 */
SECURITY_ATTRIBUTES *
pyi_win32_initialize_security_descriptor()
{
    SECURITY_ATTRIBUTES *security_attr;
    LPVOID lpSecurityDescriptor;
    wchar_t *user_sid;
    wchar_t *app_container_sid;
    wchar_t security_descriptor_str[PYI_PATH_MAX];
    int ret;

    /* Resolve user's SID for compatibility with wine */
    user_sid = _pyi_win32_get_sid(TokenUser);

    /* If program is running within an AppContainer, the app container SID has to be added to
     * the DACL, otherwise our process will not have access to the temporary directory. */
    app_container_sid = _pyi_win32_get_sid(TokenAppContainerSid); /* NULL when not running in AppContainer */

    /* DACL descriptor D:dacl_flags(string_ace1)(string_ace2)
     * with ACE string:
     * ace_type;ace_flags;rights;object_guid;inherit_object_guid;account_sid;(resource_attribute)
     * - ace_type = SDDL_ACCESS_ALLOWED (A)
     * - rights = SDDL_FILE_ALL (FA)
     * - account_sid = current user (queried SID)
     */
    if (app_container_sid) {
        ret = _snwprintf(
            security_descriptor_str,
            PYI_PATH_MAX,
            L"D:(A;;FA;;;%s)(A;;FA;;;%s)",
            user_sid ? user_sid : L"S-1-3-4",
            app_container_sid);
    } else {
        ret = _snwprintf(
            security_descriptor_str,
            PYI_PATH_MAX,
            L"D:(A;;FA;;;%s)",
            user_sid ? user_sid : L"S-1-3-4");
    }

    LocalFree(user_sid); /* Must be freed using LocalFree() */
    LocalFree(app_container_sid); /* Must be freed using LocalFree() */

    if (ret >= PYI_PATH_MAX) {
        PYI_WARNING_W(L"Security descriptor string length exceeds PYI_PATH_MAX!\n");
        return NULL;
    }

    /* Convert security descriptor string to security descriptor, and
     * store it in the SECURITY_ATTRIBUTES structure. */
    PYI_DEBUG_W(L"LOADER: initializing security descriptor from string: %ls\n", security_descriptor_str);
    ret = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        security_descriptor_str,
        SDDL_REVISION_1,
        &lpSecurityDescriptor,
        NULL);
    if (ret == 0) {
        return NULL;
    }

    /* Allocate SECURITY_ATTRIBUTES and fill it in. */
    security_attr = calloc(1, sizeof(SECURITY_ATTRIBUTES));
    security_attr->nLength = sizeof(SECURITY_ATTRIBUTES);
    security_attr->bInheritHandle = FALSE;
    security_attr->lpSecurityDescriptor = lpSecurityDescriptor;

    return security_attr;
}

/* Free security descriptor applied to application's temporary directory and its
 * sub-directories.
 */
void
pyi_win32_free_security_descriptor(SECURITY_ATTRIBUTES **security_attr_ref)
{
    SECURITY_ATTRIBUTES *security_attr = *security_attr_ref;

    security_attr_ref = NULL;

    /* Free security descriptor */
    LocalFree(security_attr->lpSecurityDescriptor);

    /* Free the structure itself */
    free(security_attr);
}


/**********************************************************************\
 *      Console minimization/hiding (console-enabled build only)      *
\**********************************************************************/
#if !defined(WINDOWED)

/* Helper that hides or minimizes the console window if it is owned by
 * the process. The show_cmd argument is passed to the ShowWindow call,
 * and should be either SW_HIDE or SW_SHOWMINNOACTIVE.
 */
static void pyi_win32_adjust_console(int show_cmd)
{
    HWND hConsole = GetConsoleWindow();
    if (hConsole != NULL) {
        DWORD dwProcessId = GetCurrentProcessId();
        DWORD dwConsoleProcessId;
        int i;

        if (GetWindowThreadProcessId(hConsole, &dwConsoleProcessId) == 0) {
            return; /* Window handle is invalid */
        }

        if (dwProcessId != dwConsoleProcessId) {
            /* We do not own console window (i.e., were launched from existing console) */
            PYI_DEBUG_W(L"LOADER: console window not owned by application - skipping adjustment.\n");
            return;
        }

        PYI_DEBUG_W(L"LOADER: console window is owned by application - calling ShowWindow() with nCmdShow=%d...\n", show_cmd);

        /* If Windows Terminal is used as the default terminal app (which
         * is the case on contemporary Windows 11 systems), we need to
         * ensure that its window was shown before we submit request
         * to hide/minimize it (sidenote: both translate into minimization).
         * Otherwise, the request ends up being ignored. ShowWindow()
         * returns the previous status of the window; non-zero if it
         * was visible, zero if it was not visible - use that to catch
         * transition from visible state. As a safety mechanism, perform
         * up to five attempts with 100 ms delay, so that in the worst
         * case, we end up delaying the program by half a second. */
        for (i = 0; i < 5; i++) {
            BOOL status;

            status = ShowWindow(hConsole, show_cmd);
            if (status != 0) {
                PYI_DEBUG_W(L"LOADER: console window transitioned from non-hidden to hidden on attempt #%i.\n", i + 1);
                return;
            }
            Sleep(100);
        }

        PYI_DEBUG_W(L"LOADER: console window failed to transition from non-hidden to hidden in %i attempts!\n", i);
    }
}

void pyi_win32_hide_console()
{
    pyi_win32_adjust_console(SW_HIDE);
}

void pyi_win32_minimize_console()
{
    pyi_win32_adjust_console(SW_SHOWMINNOACTIVE);
}

#endif /* !defined(WINDOWED) */


#endif /* ifdef _WIN32 */
