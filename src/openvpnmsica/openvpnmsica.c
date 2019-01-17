/*
 *  openvpnmsica -- Custom Action DLL to provide OpenVPN-specific support to MSI packages
 *
 *  Copyright (C) 2018 Simon Rozman <simon@rozman.si>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#elif defined(_MSC_VER)
#include <config-msvc.h>
#endif

#include "openvpnmsica.h"
#include "msica_op.h"
#include "msiex.h"

#include "../tapctl/basic.h"
#include "../tapctl/error.h"
#include "../tapctl/tap.h"

#include <windows.h>
#include <malloc.h>
#include <memory.h>
#include <msiquery.h>
#include <shlwapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "shlwapi.lib")
#endif
#include <stdbool.h>
#include <stdlib.h>
#include <tchar.h>


/**
 * Local constants
 */

#define MSICA_INTERFACE_TICK_SIZE (16*1024) /** Amount of tick space to reserve for one TAP/TUN interface creation/deletition. */


/**
 * Cleanup actions
 */
static const struct {
    LPCTSTR szName;               /** Name of the cleanup action. This name is appended to the deferred custom action name (e.g. "InstallTAPInterfaces" >> "InstallTAPInterfacesCommit"). */
    TCHAR szSuffix[3];            /** Two-character suffix to append to the cleanup operation sequence filename */
} openvpnmsica_cleanup_action_seqs[MSICA_CLEANUP_ACTION_COUNT] =
{
    { TEXT("Commit"  ), TEXT("cm") }, /* MSICA_CLEANUP_ACTION_COMMIT   */
    { TEXT("Rollback"), TEXT("rb") }, /* MSICA_CLEANUP_ACTION_ROLLBACK */
};


/**
 * Creates a new sequence file in the current user's temporary folder and sets MSI property
 * to its absolute path.
 *
 * @param hInstall      Handle to the installation provided to the DLL custom action
 *
 * @param szProperty    MSI property name to set to the absolute path of the sequence file.
 *
 * @param szFilename    String of minimum MAXPATH+1 characters where the zero-terminated
 *                      file absolute path is stored.
 *
 * @return ERROR_SUCCESS on success; An error code otherwise
 */
static DWORD
openvpnmsica_setup_sequence_filename(
    _In_                     MSIHANDLE hInstall,
    _In_z_                   LPCTSTR   szProperty,
    _Out_z_cap_(MAXPATH + 1) LPTSTR    szFilename)
{
    DWORD dwResult;

    if (szFilename == NULL)
        return ERROR_BAD_ARGUMENTS;

    /* Generate a random filename in the temporary folder. */
    if (GetTempPath(MAX_PATH + 1, szFilename) == 0)
    {
        dwResult = GetLastError();
        msg(M_NONFATAL | M_ERRNO, "%s: GetTempPath failed", __FUNCTION__);
        return dwResult;
    }
    if (GetTempFileName(szFilename, szProperty, 0, szFilename) == 0)
    {
        dwResult = GetLastError();
        msg(M_NONFATAL | M_ERRNO, "%s: GetTempFileName failed", __FUNCTION__);
        return dwResult;
    }

    /* Store sequence filename to property for deferred custom action. */
    dwResult = MsiSetProperty(hInstall, szProperty, szFilename);
    if (dwResult != ERROR_SUCCESS)
    {
        SetLastError(dwResult); /* MSDN does not mention MsiSetProperty() to set GetLastError(). But we do have an error code. Set last error manually. */
        msg(M_NONFATAL | M_ERRNO, "%s: MsiSetProperty(\"%"PRIsLPTSTR"\") failed", __FUNCTION__, szProperty);
        return dwResult;
    }

    /* Generate and store cleanup operation sequence filenames to properties. */
    LPTSTR szExtension = PathFindExtension(szFilename);
    TCHAR szFilenameEx[MAX_PATH + 1/*dash*/ + 2/*suffix*/ + 1/*terminator*/];
    size_t len_property_name = _tcslen(szProperty);
    for (size_t i = 0; i < MSICA_CLEANUP_ACTION_COUNT; i++)
    {
        size_t len_action_name_z = _tcslen(openvpnmsica_cleanup_action_seqs[i].szName) + 1;
        TCHAR *szPropertyEx = (TCHAR*)malloc((len_property_name + len_action_name_z) * sizeof(TCHAR));
        memcpy(szPropertyEx                    , szProperty                         , len_property_name * sizeof(TCHAR));
        memcpy(szPropertyEx + len_property_name, openvpnmsica_cleanup_action_seqs[i].szName, len_action_name_z * sizeof(TCHAR));
        _stprintf_s(
            szFilenameEx, _countof(szFilenameEx),
            TEXT("%.*s-%.2s%s"),
            (int)(szExtension - szFilename), szFilename,
            openvpnmsica_cleanup_action_seqs[i].szSuffix,
            szExtension);
        dwResult = MsiSetProperty(hInstall, szPropertyEx, szFilenameEx);
        if (dwResult != ERROR_SUCCESS)
        {
            SetLastError(dwResult); /* MSDN does not mention MsiSetProperty() to set GetLastError(). But we do have an error code. Set last error manually. */
            msg(M_NONFATAL | M_ERRNO, "%s: MsiSetProperty(\"%"PRIsLPTSTR"\") failed", __FUNCTION__, szPropertyEx);
            free(szPropertyEx);
            return dwResult;
        }
        free(szPropertyEx);
    }

    return ERROR_SUCCESS;
}


UINT __stdcall
FindTAPInterfaces(_In_ MSIHANDLE hInstall)
{
#ifdef _MSC_VER
#pragma comment(linker, DLLEXP_EXPORT)
#endif

#ifdef _DEBUG
    MessageBox(NULL, TEXT("Attach debugger!"), TEXT(__FUNCTION__) TEXT(" v")  TEXT(PACKAGE_VERSION), MB_OK);
#endif

    UINT uiResult;
    BOOL bIsCoInitialized = SUCCEEDED(CoInitialize(NULL));

    /* Set MSI session handle in TLS. */
    struct openvpnmsica_tls_data *s = (struct openvpnmsica_tls_data *)TlsGetValue(openvpnmsica_tlsidx_session);
    s->hInstall = hInstall;

    /* Get available network interfaces. */
    struct tap_interface_node *pInterfaceList = NULL;
    uiResult = tap_list_interfaces(NULL, &pInterfaceList);
    if (uiResult != ERROR_SUCCESS)
        goto cleanup_CoInitialize;

    /* Enumerate interfaces. */
    struct interface_node
    {
        const struct tap_interface_node *iface;
        struct interface_node *next;
    } *interfaces_head = NULL, *interfaces_tail = NULL;
    size_t interface_count = 0;
    MSIHANDLE hRecord = MsiCreateRecord(1);
    for (struct tap_interface_node *pInterface = pInterfaceList; pInterface; pInterface = pInterface->pNext)
    {
        for (LPCTSTR hwid = pInterface->szzHardwareIDs; hwid[0]; hwid += _tcslen(hwid) + 1)
        {
            if (_tcsicmp(hwid, TEXT(TAP_WIN_COMPONENT_ID)) == 0 ||
                _tcsicmp(hwid, TEXT("root\\") TEXT(TAP_WIN_COMPONENT_ID)) == 0)
            {
                /* TAP interface found. */

                /* Report the GUID of the interface to installer. */
                LPOLESTR szInterfaceId = NULL;
                StringFromIID((REFIID)&pInterface->guid, &szInterfaceId);
                MsiRecordSetString(hRecord, 1, szInterfaceId);
                MsiProcessMessage(hInstall, INSTALLMESSAGE_ACTIONDATA, hRecord);
                CoTaskMemFree(szInterfaceId);

                /* Append interface to the list. */
                struct interface_node *node = (struct interface_node*)malloc(sizeof(struct interface_node));
                node->iface = pInterface;
                node->next = NULL;
                if (interfaces_head)
                    interfaces_tail = interfaces_tail->next = node;
                else
                    interfaces_head = interfaces_tail = node;
                interface_count++;
                break;
            }
        }
    }
    MsiCloseHandle(hRecord);

    if (interface_count)
    {
        /* Prepare semicolon delimited list of TAP interface ID(s). */
        LPTSTR
            szTAPInterfaces = (LPTSTR)malloc(interface_count * (38/*GUID*/ + 1/*separator/terminator*/) * sizeof(TCHAR)),
            szTAPInterfacesTail = szTAPInterfaces;
        while (interfaces_head)
        {
            LPOLESTR szInterfaceId = NULL;
            StringFromIID((REFIID)&interfaces_head->iface->guid, &szInterfaceId);
            memcpy(szTAPInterfacesTail, szInterfaceId, 38 * sizeof(TCHAR));
            szTAPInterfacesTail += 38;
            CoTaskMemFree(szInterfaceId);
            szTAPInterfacesTail[0] = interfaces_head->next ? TEXT(';') : 0;
            szTAPInterfacesTail++;

            struct interface_node *p = interfaces_head;
            interfaces_head = interfaces_head->next;
            free(p);
        }

        /* Set Installer TAPINTERFACES property. */
        uiResult = MsiSetProperty(hInstall, TEXT("TAPINTERFACES"), szTAPInterfaces);
        if (uiResult != ERROR_SUCCESS)
        {
            SetLastError(uiResult); /* MSDN does not mention MsiSetProperty() to set GetLastError(). But we do have an error code. Set last error manually. */
            msg(M_NONFATAL | M_ERRNO, "%s: MsiSetProperty(\"TAPINTERFACES\") failed", __FUNCTION__);
            goto cleanup_szTAPInterfaces;
        }

    cleanup_szTAPInterfaces:
        free(szTAPInterfaces);
    }
    else
        uiResult = ERROR_SUCCESS;

    tap_free_interface_list(pInterfaceList);
cleanup_CoInitialize:
    if (bIsCoInitialized) CoUninitialize();
    return uiResult;
}


UINT __stdcall
EvaluateTAPInterfaces(_In_ MSIHANDLE hInstall)
{
#ifdef _MSC_VER
#pragma comment(linker, DLLEXP_EXPORT)
#endif

#ifdef _DEBUG
    MessageBox(NULL, TEXT("Attach debugger!"), TEXT(__FUNCTION__) TEXT(" v")  TEXT(PACKAGE_VERSION), MB_OK);
#endif

    UINT uiResult;
    BOOL bIsCoInitialized = SUCCEEDED(CoInitialize(NULL));

    /* Set MSI session handle in TLS. */
    struct openvpnmsica_tls_data *s = (struct openvpnmsica_tls_data *)TlsGetValue(openvpnmsica_tlsidx_session);
    s->hInstall = hInstall;

    /* List of deferred custom actions EvaluateTAPInterfaces prepares operation sequence for. */
    static const LPCTSTR szActionNames[] =
    {
        TEXT("InstallTAPInterfaces"),
        TEXT("UninstallTAPInterfaces"),
    };
    struct msica_op_seq exec_seq[_countof(szActionNames)];
    for (size_t i = 0; i < _countof(szActionNames); i++)
        msica_op_seq_init(&exec_seq[i]);

    {
        /* Check and store the rollback enabled state. */
        TCHAR szValue[128];
        DWORD dwLength = _countof(szValue);
        bool enable_rollback = MsiGetProperty(hInstall, TEXT("RollbackDisabled"), szValue, &dwLength) == ERROR_SUCCESS ?
            _ttoi(szValue) || _totlower(szValue[0]) == TEXT('y') ? false : true :
            true;
        for (size_t i = 0; i < _countof(szActionNames); i++)
            msica_op_seq_add_tail(
                &exec_seq[i],
                msica_op_create_bool(
                    msica_op_rollback_enable,
                    0,
                    NULL,
                    enable_rollback));
    }

    /* Open MSI database. */
    MSIHANDLE hDatabase = MsiGetActiveDatabase(hInstall);
    if (hDatabase == 0)
    {
        msg(M_NONFATAL, "%s: MsiGetActiveDatabase failed", __FUNCTION__);
        uiResult = ERROR_INVALID_HANDLE; goto cleanup_exec_seq;
    }

    /* Check if TAPInterface table exists. If it doesn't exist, there's nothing to do. */
    switch (MsiDatabaseIsTablePersistent(hDatabase, TEXT("TAPInterface")))
    {
    case MSICONDITION_FALSE:
    case MSICONDITION_TRUE : break;
    default:
        uiResult = ERROR_SUCCESS;
        goto cleanup_hDatabase;
    }

    /* Prepare a query to get a list/view of interfaces. */
    MSIHANDLE hViewST = 0;
    LPCTSTR szQuery = TEXT("SELECT `Interface`,`DisplayName`,`Condition`,`Component_` FROM `TAPInterface`");
    uiResult = MsiDatabaseOpenView(hDatabase, szQuery, &hViewST);
    if (uiResult != ERROR_SUCCESS)
    {
        SetLastError(uiResult); /* MSDN does not mention MsiDatabaseOpenView() to set GetLastError(). But we do have an error code. Set last error manually. */
        msg(M_NONFATAL | M_ERRNO, "%s: MsiDatabaseOpenView(\"%"PRIsLPTSTR"\") failed", __FUNCTION__, szQuery);
        goto cleanup_hDatabase;
    }

    /* Execute query! */
    uiResult = MsiViewExecute(hViewST, 0);
    if (uiResult != ERROR_SUCCESS)
    {
        SetLastError(uiResult); /* MSDN does not mention MsiViewExecute() to set GetLastError(). But we do have an error code. Set last error manually. */
        msg(M_NONFATAL | M_ERRNO, "%s: MsiViewExecute(\"%"PRIsLPTSTR"\") failed", __FUNCTION__, szQuery);
        goto cleanup_hViewST;
    }

    /* Create a record to report progress with. */
    MSIHANDLE hRecordProg = MsiCreateRecord(2);
    if (!hRecordProg)
    {
        uiResult = ERROR_INVALID_HANDLE;
        msg(M_NONFATAL, "%s: MsiCreateRecord failed", __FUNCTION__);
        goto cleanup_hViewST_close;
    }

    for (;;)
    {
        /* Fetch one record from the view. */
        MSIHANDLE hRecord = 0;
        uiResult = MsiViewFetch(hViewST, &hRecord);
        if (uiResult == ERROR_NO_MORE_ITEMS) {
            uiResult = ERROR_SUCCESS;
            break;
        }
        else if (uiResult != ERROR_SUCCESS)
        {
            SetLastError(uiResult); /* MSDN does not mention MsiViewFetch() to set GetLastError(). But we do have an error code. Set last error manually. */
            msg(M_NONFATAL | M_ERRNO, "%s: MsiViewFetch failed", __FUNCTION__);
            goto cleanup_hRecordProg;
        }

        INSTALLSTATE iInstalled, iAction;
        {
            /* Read interface component ID (`Component_` is field #4). */
            LPTSTR szValue = NULL;
            uiResult = msi_get_record_string(hRecord, 4, &szValue);
            if (uiResult != ERROR_SUCCESS) goto cleanup_hRecord;

            /* Get the component state. */
            uiResult = MsiGetComponentState(hInstall, szValue, &iInstalled, &iAction);
            if (uiResult != ERROR_SUCCESS)
            {
                SetLastError(uiResult); /* MSDN does not mention MsiGetComponentState() to set GetLastError(). But we do have an error code. Set last error manually. */
                msg(M_NONFATAL | M_ERRNO, "%s: MsiGetComponentState(\"%"PRIsLPTSTR"\") failed", __FUNCTION__, szValue);
                free(szValue);
                goto cleanup_hRecord;
            }
            free(szValue);
        }

        /* Get interface display name (`DisplayName` is field #2). */
        LPTSTR szDisplayName = NULL;
        uiResult = msi_format_field(hInstall, hRecord, 2, &szDisplayName);
        if (uiResult != ERROR_SUCCESS)
            goto cleanup_hRecord;

        if (iAction > INSTALLSTATE_BROKEN)
        {
            if (iAction >= INSTALLSTATE_LOCAL) {
                /* Read and evaluate interface condition (`Condition` is field #3). */
                LPTSTR szValue = NULL;
                uiResult = msi_get_record_string(hRecord, 3, &szValue);
                if (uiResult != ERROR_SUCCESS) goto cleanup_szDisplayName;
#ifdef __GNUC__
/*
 * warning: enumeration value ‘MSICONDITION_TRUE’ not handled in switch
 * warning: enumeration value ‘MSICONDITION_NONE’ not handled in switch
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
#endif
                switch (MsiEvaluateCondition(hInstall, szValue))
                {
                case MSICONDITION_FALSE:
                    free(szValue);
                    goto cleanup_szDisplayName;
                case MSICONDITION_ERROR:
                    uiResult = ERROR_INVALID_FIELD;
                    msg(M_NONFATAL | M_ERRNO, "%s: MsiEvaluateCondition(\"%"PRIsLPTSTR"\") failed", __FUNCTION__, szValue);
                    free(szValue);
                    goto cleanup_szDisplayName;
                }
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
                free(szValue);

                /* Component is or should be installed. Schedule interface creation. */
                msica_op_seq_add_tail(
                    &exec_seq[0],
                    msica_op_create_string(
                        msica_op_tap_interface_create,
                        MSICA_INTERFACE_TICK_SIZE,
                        NULL,
                        szDisplayName));
            }
            else
            {
                /* Component is installed, but should be degraded to advertised/removed. Schedule interface deletition. */
                msica_op_seq_add_tail(
                    &exec_seq[1],
                    msica_op_create_string(
                        msica_op_tap_interface_delete_by_name,
                        MSICA_INTERFACE_TICK_SIZE,
                        NULL,
                        szDisplayName));
            }

            /* The amount of tick space to add for each interface to progress indicator. */
            MsiRecordSetInteger(hRecordProg, 1, 3 /* OP3 = Add ticks to the expected total number of progress of the progress bar */);
            MsiRecordSetInteger(hRecordProg, 2, MSICA_INTERFACE_TICK_SIZE);
            if (MsiProcessMessage(hInstall, INSTALLMESSAGE_PROGRESS, hRecordProg) == IDCANCEL)
            {
                uiResult = ERROR_INSTALL_USEREXIT;
                goto cleanup_szDisplayName;
            }
        }

    cleanup_szDisplayName:
        free(szDisplayName);
    cleanup_hRecord:
        MsiCloseHandle(hRecord);
        if (uiResult != ERROR_SUCCESS)
            goto cleanup_hRecordProg;
    }

    /*
    Write sequence files.
    The InstallTAPInterfaces and UninstallTAPInterfaces are deferred custom actions, thus all this information
    will be unavailable to them. Therefore save all required operations and their info to sequence files.
    */
    TCHAR szSeqFilename[_countof(szActionNames)][MAX_PATH + 1];
    for (size_t i = 0; i < _countof(szActionNames); i++)
        szSeqFilename[i][0] = 0;
    for (size_t i = 0; i < _countof(szActionNames); i++)
    {
        uiResult = openvpnmsica_setup_sequence_filename(hInstall, szActionNames[i], szSeqFilename[i]);
        if (uiResult != ERROR_SUCCESS)
            goto cleanup_szSeqFilename;
        HANDLE hSeqFile = CreateFile(
            szSeqFilename[i],
            GENERIC_WRITE,
            FILE_SHARE_READ,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            NULL);
        if (hSeqFile == INVALID_HANDLE_VALUE)
        {
            uiResult = GetLastError();
            msg(M_NONFATAL | M_ERRNO, "%s: CreateFile(\"%.*"PRIsLPTSTR"\") failed", __FUNCTION__, _countof(szSeqFilename[i]), szSeqFilename[i]);
            goto cleanup_szSeqFilename;
        }
        uiResult = msica_op_seq_save(&exec_seq[i], hSeqFile);
        CloseHandle(hSeqFile);
        if (uiResult != ERROR_SUCCESS)
            goto cleanup_szSeqFilename;
    }

    uiResult = ERROR_SUCCESS;

cleanup_szSeqFilename:
    if (uiResult != ERROR_SUCCESS)
    {
        /* Clean-up sequence files. */
        for (size_t i = _countof(szActionNames); i--;)
            if (szSeqFilename[i][0])
                DeleteFile(szSeqFilename[i]);
    }
cleanup_hRecordProg:
    MsiCloseHandle(hRecordProg);
cleanup_hViewST_close:
    MsiViewClose(hViewST);
cleanup_hViewST:
    MsiCloseHandle(hViewST);
cleanup_hDatabase:
    MsiCloseHandle(hDatabase);
cleanup_exec_seq:
    for (size_t i = 0; i < _countof(szActionNames); i++)
        msica_op_seq_free(&exec_seq[i]);
    if (bIsCoInitialized) CoUninitialize();
    return uiResult;
}


UINT __stdcall
ProcessDeferredAction(_In_ MSIHANDLE hInstall)
{
#ifdef _MSC_VER
#pragma comment(linker, DLLEXP_EXPORT)
#endif

#ifdef _DEBUG
    MessageBox(NULL, TEXT("Attach debugger!"), TEXT(__FUNCTION__) TEXT(" v")  TEXT(PACKAGE_VERSION), MB_OK);
#endif

    UINT uiResult;
    BOOL bIsCoInitialized = SUCCEEDED(CoInitialize(NULL));

    /* Set MSI session handle in TLS. */
    struct openvpnmsica_tls_data *s = (struct openvpnmsica_tls_data *)TlsGetValue(openvpnmsica_tlsidx_session);
    s->hInstall = hInstall;

    BOOL bIsCleanup = MsiGetMode(hInstall, MSIRUNMODE_COMMIT) || MsiGetMode(hInstall, MSIRUNMODE_ROLLBACK);

    /* Get sequence filename and open the file. */
    LPTSTR szSeqFilename = NULL;
    uiResult = msi_get_string(hInstall, TEXT("CustomActionData"), &szSeqFilename);
    if (uiResult != ERROR_SUCCESS)
        goto cleanup_CoInitialize;
    struct msica_op_seq seq = { .head = NULL, .tail = NULL };
    {
        HANDLE hSeqFile = CreateFile(
            szSeqFilename,
            GENERIC_READ,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            NULL);
        if (hSeqFile == INVALID_HANDLE_VALUE)
        {
            uiResult = GetLastError();
            if (uiResult == ERROR_FILE_NOT_FOUND && bIsCleanup)
            {
                /*
                Sequence file not found and this is rollback/commit action. Either of the following scenarios are possible:
                - The delayed action failed to save the rollback/commit sequence to file. The delayed action performed cleanup itself. No further operation is required.
                - Somebody removed the rollback/commit file between delayed action and rollback/commit action. No further operation is possible.
                */
                uiResult = ERROR_SUCCESS;
                goto cleanup_szSeqFilename;
            }
            msg(M_NONFATAL | M_ERRNO, "%s: CreateFile(\"%"PRIsLPTSTR"\") failed", __FUNCTION__, szSeqFilename);
            goto cleanup_szSeqFilename;
        }

        /* Load sequence. */
        uiResult = msica_op_seq_load(&seq, hSeqFile);
        CloseHandle(hSeqFile);
        if (uiResult != ERROR_SUCCESS)
            goto cleanup_seq;
    }

    /* Prepare session context. */
    struct msica_session session;
    openvpnmsica_session_init(
        &session,
        hInstall,
        bIsCleanup, /* In case of commit/rollback, continue sequence on error, to do as much cleanup as possible. */
        false);

    /* Execute sequence. */
    uiResult = msica_op_seq_process(&seq, &session);
    if (!bIsCleanup)
    {
        /*
        Save cleanup scripts of delayed action regardless of action's execution status.
        Rollback action MUST be scheduled in InstallExecuteSequence before this action! Otherwise cleanup won't be performed in case this action execution failed.
        */
        DWORD dwResultEx; /* Don't overwrite uiResult. */
        LPCTSTR szExtension = PathFindExtension(szSeqFilename);
        TCHAR szFilenameEx[MAX_PATH + 1/*dash*/ + 2/*suffix*/ + 1/*terminator*/];
        for (size_t i = 0; i < MSICA_CLEANUP_ACTION_COUNT; i++)
        {
            _stprintf_s(
                szFilenameEx, _countof(szFilenameEx),
                TEXT("%.*s-%.2s%s"),
                (int)(szExtension - szSeqFilename), szSeqFilename,
                openvpnmsica_cleanup_action_seqs[i].szSuffix,
                szExtension);

            /* After commit, delete rollback file. After rollback, delete commit file. */
            msica_op_seq_add_tail(
                &session.seq_cleanup[MSICA_CLEANUP_ACTION_COUNT - 1 - i],
                msica_op_create_string(
                    msica_op_file_delete,
                    0,
                    NULL,
                    szFilenameEx));
        }
        for (size_t i = 0; i < MSICA_CLEANUP_ACTION_COUNT; i++)
        {
            _stprintf_s(
                szFilenameEx, _countof(szFilenameEx),
                TEXT("%.*s-%.2s%s"),
                (int)(szExtension - szSeqFilename), szSeqFilename,
                openvpnmsica_cleanup_action_seqs[i].szSuffix,
                szExtension);

            /* Save the cleanup sequence file. */
            HANDLE hSeqFile = CreateFile(
                szFilenameEx,
                GENERIC_WRITE,
                FILE_SHARE_READ,
                NULL,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                NULL);
            if (hSeqFile == INVALID_HANDLE_VALUE)
            {
                dwResultEx = GetLastError();
                msg(M_NONFATAL | M_ERRNO, "%s: CreateFile(\"%.*"PRIsLPTSTR"\") failed", __FUNCTION__, _countof(szFilenameEx), szFilenameEx);
                goto cleanup_session;
            }
            dwResultEx = msica_op_seq_save(&session.seq_cleanup[i], hSeqFile);
            CloseHandle(hSeqFile);
            if (dwResultEx != ERROR_SUCCESS)
                goto cleanup_session;
        }

    cleanup_session:
        if (dwResultEx != ERROR_SUCCESS)
        {
            /* The commit and/or rollback scripts were not written to file successfully. Perform the cleanup immediately. */
            struct msica_session session_cleanup;
            openvpnmsica_session_init(
                &session_cleanup,
                hInstall,
                true,
                false);
            msica_op_seq_process(&session.seq_cleanup[MSICA_CLEANUP_ACTION_ROLLBACK], &session_cleanup);

            szExtension = PathFindExtension(szSeqFilename);
            for (size_t i = 0; i < MSICA_CLEANUP_ACTION_COUNT; i++)
            {
                _stprintf_s(
                    szFilenameEx, _countof(szFilenameEx),
                    TEXT("%.*s-%.2s%s"),
                    (int)(szExtension - szSeqFilename), szSeqFilename,
                    openvpnmsica_cleanup_action_seqs[i].szSuffix,
                    szExtension);
                DeleteFile(szFilenameEx);
            }
        }
    }
    else
    {
        /* No cleanup after cleanup support. */
        uiResult = ERROR_SUCCESS;
    }

    for (size_t i = MSICA_CLEANUP_ACTION_COUNT; i--;)
        msica_op_seq_free(&session.seq_cleanup[i]);
    DeleteFile(szSeqFilename);
cleanup_seq:
    msica_op_seq_free(&seq);
cleanup_szSeqFilename:
    free(szSeqFilename);
cleanup_CoInitialize:
    if (bIsCoInitialized) CoUninitialize();
    return uiResult;
}
