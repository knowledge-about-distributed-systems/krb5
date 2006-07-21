/*
 * lib/krb5/ccache/cc_mslsa.c
 *
 * Copyright 2003,2004 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 * 
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 * 
 * Copyright 2000 by Carnegie Mellon University
 *
 * All Rights Reserved
 * 
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted,
 * provided that the above copyright notice appear in all copies and that
 * both that copyright notice and this permission notice appear in
 * supporting documentation, and that the name of Carnegie Mellon
 * University not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission.
 * 
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE FOR
 * ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Implementation of microsoft windows lsa read-only credentials cache
 */

#ifdef _WIN32
#define UNICODE
#define _UNICODE

#include "k5-int.h"
#include "com_err.h"
#include "cc-int.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <conio.h>
#include <time.h>
#define SECURITY_WIN32
#include <security.h>
#include <ntsecapi.h>
#include <ntstatus.h>

#define MAX_MSG_SIZE 256
#define MAX_MSPRINC_SIZE 1024

/* THREAD SAFETY 
 * The functions is_windows_2000(), is_windows_xp(), and 
 * does_retrieve_ticket_cache_ticket() contain static variables to cache 
 * the responses of the tests being performed.  There is no harm in the 
 * test being performed more than once since the result will always be the 
 * same.
 */

static BOOL 
is_windows_2000 (void)
{
   static BOOL fChecked = FALSE;
   static BOOL fIsWin2K = FALSE;

   if (!fChecked)
   {
       OSVERSIONINFO Version;

       memset (&Version, 0x00, sizeof(Version));
       Version.dwOSVersionInfoSize = sizeof(Version);

       if (GetVersionEx (&Version))
       {
           if (Version.dwPlatformId == VER_PLATFORM_WIN32_NT &&
                Version.dwMajorVersion >= 5)
               fIsWin2K = TRUE;
       }
       fChecked = TRUE;
   }

   return fIsWin2K;
}

static BOOL 
is_windows_xp (void)
{
   static BOOL fChecked = FALSE;
   static BOOL fIsWinXP = FALSE;

   if (!fChecked)
   {
       OSVERSIONINFO Version;

       memset (&Version, 0x00, sizeof(Version));
       Version.dwOSVersionInfoSize = sizeof(Version);

       if (GetVersionEx (&Version))
       {
           if (Version.dwPlatformId == VER_PLATFORM_WIN32_NT &&
                (Version.dwMajorVersion > 5 ||
                 Version.dwMajorVersion == 5 && Version.dwMinorVersion >= 1) )
               fIsWinXP = TRUE;
       }
       fChecked = TRUE;
   }

   return fIsWinXP;
}

typedef BOOL (WINAPI *LPFN_ISWOW64PROCESS) (HANDLE, PBOOL);

static BOOL
is_broken_wow64(void)
{
    static BOOL fChecked = FALSE;
    static BOOL fIsBrokenWow64 = FALSE;

    if (!fChecked)
    {
	BOOL isWow64 = FALSE;
	OSVERSIONINFO Version;
	HANDLE h1 = NULL;
	LPFN_ISWOW64PROCESS fnIsWow64Process = NULL;
		
	h1 = GetModuleHandle(L"kernel32.dll");
	fnIsWow64Process = 
	    (LPFN_ISWOW64PROCESS)GetProcAddress(h1, "IsWow64Process");

	/* If we don't find the fnIsWow64Process function then we 
	 * are not running in a broken Wow64 
	 */
	if (fnIsWow64Process) {
	    memset (&Version, 0x00, sizeof(Version));
	    Version.dwOSVersionInfoSize = sizeof(Version);

	    if (fnIsWow64Process(GetCurrentProcess(), &isWow64) && 
		GetVersionEx (&Version)) {
		if (isWow64 && 
		    Version.dwPlatformId == VER_PLATFORM_WIN32_NT &&
		    Version.dwMajorVersion < 6)
		    fIsBrokenWow64 = TRUE;
	    } 
	}
	fChecked = TRUE;
    }

    return fIsBrokenWow64;
}

/* This flag is only supported by versions of Windows which have obtained
 * a code change from Microsoft.   When the code change is installed,
 * setting this flag will cause all retrieved credentials to be stored 
 * in the LSA cache.
 */
#ifndef KERB_RETRIEVE_TICKET_CACHE_TICKET
#define KERB_RETRIEVE_TICKET_CACHE_TICKET  0x20
#endif

static VOID
ShowWinError(LPSTR szAPI, DWORD dwError)
{

    // TODO - Write errors to event log so that scripts that don't
    // check for errors will still get something in the event log

    // This code is completely unsafe for use on non-English systems
    // Any call to this function will result in the FormatMessage
    // call failing and the program terminating.  This might have
    // been acceptable when this code was part of ms2mit.exe as
    // a standalone executable but it is not appropriate for a library

#ifdef COMMENT
    WCHAR szMsgBuf[MAX_MSG_SIZE];
    DWORD dwRes;

    printf("Error calling function %s: %lu\n", szAPI, dwError);

    dwRes = FormatMessage (
        FORMAT_MESSAGE_FROM_SYSTEM,
        NULL,
        dwError,
        MAKELANGID (LANG_ENGLISH, SUBLANG_ENGLISH_US),
        szMsgBuf,
        MAX_MSG_SIZE,
        NULL);
    if (0 == dwRes) {
        printf("FormatMessage failed with %d\n", GetLastError());
        ExitProcess(EXIT_FAILURE);
    }

    printf("%S",szMsgBuf);
#endif /* COMMENT */
}

static VOID
ShowLsaError(LPSTR szAPI, NTSTATUS Status)
{
    //
    // Convert the NTSTATUS to Winerror. Then call ShowWinError().
    //
    ShowWinError(szAPI, LsaNtStatusToWinError(Status));
}

static BOOL
WINAPI
UnicodeToANSI(LPTSTR lpInputString, LPSTR lpszOutputString, int nOutStringLen)
{
    CPINFO CodePageInfo;

    GetCPInfo(CP_ACP, &CodePageInfo);

    if (CodePageInfo.MaxCharSize > 1) {
        // Only supporting non-Unicode strings
        int reqLen = WideCharToMultiByte(CP_ACP, 0, (LPCWSTR) lpInputString, -1,
                                         NULL, 0, NULL, NULL);
        if ( reqLen > nOutStringLen) 
        {
            return FALSE;
        } else {
            WideCharToMultiByte(CP_ACP, 0, (LPCWSTR) lpInputString, -1,
                                lpszOutputString, nOutStringLen, NULL, NULL);
        }
    } 
    else if (((LPBYTE) lpInputString)[1] == '\0')
    {
        // Looks like unicode, better translate it
        WideCharToMultiByte(CP_ACP, 0, (LPCWSTR) lpInputString, -1,
                            lpszOutputString, nOutStringLen, NULL, NULL);
    }
    else
        lstrcpyA(lpszOutputString, (LPSTR) lpInputString);

    return TRUE;
}  // UnicodeToANSI

static VOID
WINAPI
ANSIToUnicode(LPSTR lpInputString, LPTSTR lpszOutputString, int nOutStringLen)
{

    CPINFO CodePageInfo;

    lstrcpy(lpszOutputString, (LPTSTR) lpInputString);

    GetCPInfo(CP_ACP, &CodePageInfo);

    if (CodePageInfo.MaxCharSize > 1 || ((LPBYTE) lpInputString)[1] != '\0')
    {
        // Looks like ANSI or MultiByte, better translate it
        MultiByteToWideChar(CP_ACP, 0, (LPCSTR) lpInputString, -1,
                            (LPWSTR) lpszOutputString, nOutStringLen);
    }
    else
        lstrcpy(lpszOutputString, (LPTSTR) lpInputString);
}  // ANSIToUnicode


static void
MITPrincToMSPrinc(krb5_context context, krb5_principal principal, UNICODE_STRING * msprinc)
{
    char *aname = NULL;

    if (!krb5_unparse_name(context, principal, &aname)) {
        msprinc->Length = strlen(aname) * sizeof(WCHAR);
        if ( msprinc->Length <= msprinc->MaximumLength )
            ANSIToUnicode(aname, msprinc->Buffer, msprinc->MaximumLength);
        else 
            msprinc->Length = 0;
        krb5_free_unparsed_name(context,aname);
    }
}

static BOOL
UnicodeStringToMITPrinc(UNICODE_STRING *service, WCHAR *realm, krb5_context context, 
                        krb5_principal *principal)
{
    WCHAR princbuf[512];
    char aname[512];

    princbuf[0]=0;
    wcsncpy(princbuf, service->Buffer, service->Length/sizeof(WCHAR));
    princbuf[service->Length/sizeof(WCHAR)]=0;
    wcscat(princbuf, L"@");
    wcscat(princbuf, realm);
    if (UnicodeToANSI(princbuf, aname, sizeof(aname))) {
        krb5_parse_name(context, aname, principal);
        return TRUE;
    }
    return FALSE;
}


static BOOL
KerbExternalNameToMITPrinc(KERB_EXTERNAL_NAME *msprinc, WCHAR *realm, krb5_context context, 
                           krb5_principal *principal)
{
    WCHAR princbuf[512],tmpbuf[128];
    char aname[512];
    USHORT i;
    princbuf[0]=0;
    for (i=0;i<msprinc->NameCount;i++) {
        wcsncpy(tmpbuf, msprinc->Names[i].Buffer,
                msprinc->Names[i].Length/sizeof(WCHAR));
        tmpbuf[msprinc->Names[i].Length/sizeof(WCHAR)]=0;
        if (princbuf[0])
            wcscat(princbuf, L"/");
        wcscat(princbuf, tmpbuf);
    }
    wcscat(princbuf, L"@");
    wcscat(princbuf, realm);
    if (UnicodeToANSI(princbuf, aname, sizeof(aname))) {
        krb5_parse_name(context, aname, principal);
        return TRUE;
    }
    return FALSE;
}

static time_t
FileTimeToUnixTime(LARGE_INTEGER *ltime)
{
    FILETIME filetime, localfiletime;
    SYSTEMTIME systime;
    struct tm utime;
    filetime.dwLowDateTime=ltime->LowPart;
    filetime.dwHighDateTime=ltime->HighPart;
    FileTimeToLocalFileTime(&filetime, &localfiletime);
    FileTimeToSystemTime(&localfiletime, &systime);
    utime.tm_sec=systime.wSecond;
    utime.tm_min=systime.wMinute;
    utime.tm_hour=systime.wHour;
    utime.tm_mday=systime.wDay;
    utime.tm_mon=systime.wMonth-1;
    utime.tm_year=systime.wYear-1900;
    utime.tm_isdst=-1;
    return(mktime(&utime));
}

static void
MSSessionKeyToMITKeyblock(KERB_CRYPTO_KEY *mskey, krb5_context context, krb5_keyblock *keyblock)
{
    krb5_keyblock tmpblock;
    tmpblock.magic=KV5M_KEYBLOCK;
    tmpblock.enctype=mskey->KeyType;
    tmpblock.length=mskey->Length;
    tmpblock.contents=mskey->Value;
    krb5_copy_keyblock_contents(context, &tmpblock, keyblock);
}


static void
MSFlagsToMITFlags(ULONG msflags, ULONG *mitflags)
{
    *mitflags=msflags;
}

static BOOL
MSTicketToMITTicket(KERB_EXTERNAL_TICKET *msticket, krb5_context context, krb5_data *ticket)
{
    krb5_data tmpdata, *newdata = 0;
    krb5_error_code rc;

    tmpdata.magic=KV5M_DATA;
    tmpdata.length=msticket->EncodedTicketSize;
    tmpdata.data=msticket->EncodedTicket;

    // this is ugly and will break krb5_free_data() 
    // now that this is being done within the library it won't break krb5_free_data()
    rc = krb5_copy_data(context, &tmpdata, &newdata);
    if (rc)
        return FALSE;
    
    memcpy(ticket, newdata, sizeof(krb5_data));
    krb5_xfree(newdata);
    return TRUE;
}

/*
 * PreserveInitialTicketIdentity()
 *
 * This will find the "PreserveInitialTicketIdentity" key in the registry.  
 * Returns 1 to preserve and 0 to not.
 */

static DWORD
PreserveInitialTicketIdentity(void)
{
    HKEY hKey;
    DWORD size = sizeof(DWORD);
    DWORD type = REG_DWORD;
    const char *key_path = "Software\\MIT\\Kerberos5";
    const char *value_name = "PreserveInitialTicketIdentity";
    DWORD retval = 1;     /* default to Preserve */

    if (RegOpenKeyExA(HKEY_CURRENT_USER, key_path, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        goto syskey;
    if (RegQueryValueExA(hKey, value_name, 0, &type, (LPBYTE)&retval, &size) != ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        goto syskey;
    }
    RegCloseKey(hKey);
    goto done;

  syskey:
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_path, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        goto done;
    if (RegQueryValueExA(hKey, value_name, 0, &type, (LPBYTE)&retval, &size) != ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        goto done;
    }
    RegCloseKey(hKey);

  done:
    return retval;
}


static BOOL
MSCredToMITCred(KERB_EXTERNAL_TICKET *msticket, UNICODE_STRING ClientRealm, 
                krb5_context context, krb5_creds *creds)
{
    WCHAR wrealm[128];
    ZeroMemory(creds, sizeof(krb5_creds));
    creds->magic=KV5M_CREDS;

    // construct Client Principal
    wcsncpy(wrealm, ClientRealm.Buffer, ClientRealm.Length/sizeof(WCHAR));
    wrealm[ClientRealm.Length/sizeof(WCHAR)]=0;
    if (!KerbExternalNameToMITPrinc(msticket->ClientName, wrealm, context, &creds->client))
        return FALSE;

    // construct Service Principal
    wcsncpy(wrealm, msticket->DomainName.Buffer,
            msticket->DomainName.Length/sizeof(WCHAR));
    wrealm[msticket->DomainName.Length/sizeof(WCHAR)]=0;
    if (!KerbExternalNameToMITPrinc(msticket->ServiceName, wrealm, context, &creds->server))
        return FALSE;
    MSSessionKeyToMITKeyblock(&msticket->SessionKey, context, 
                              &creds->keyblock);
    MSFlagsToMITFlags(msticket->TicketFlags, &creds->ticket_flags);
    creds->times.starttime=FileTimeToUnixTime(&msticket->StartTime);
    creds->times.endtime=FileTimeToUnixTime(&msticket->EndTime);
    creds->times.renew_till=FileTimeToUnixTime(&msticket->RenewUntil);
    creds->addresses = NULL;

    return MSTicketToMITTicket(msticket, context, &creds->ticket);
}

static BOOL
PackageConnectLookup(HANDLE *pLogonHandle, ULONG *pPackageId)
{
    LSA_STRING Name;
    NTSTATUS Status;

    Status = LsaConnectUntrusted(
        pLogonHandle
        );

    if (FAILED(Status))
    {
        ShowLsaError("LsaConnectUntrusted", Status);
        return FALSE;
    }

    Name.Buffer = MICROSOFT_KERBEROS_NAME_A;
    Name.Length = strlen(Name.Buffer);
    Name.MaximumLength = Name.Length + 1;

    Status = LsaLookupAuthenticationPackage(
        *pLogonHandle,
        &Name,
        pPackageId
        );

    if (FAILED(Status))
    {
        ShowLsaError("LsaLookupAuthenticationPackage", Status);
        return FALSE;
    }

    return TRUE;

}

static BOOL 
does_retrieve_ticket_cache_ticket (void)
{
   static BOOL fChecked = FALSE;
   static BOOL fCachesTicket = FALSE;

   if (!fChecked)
   {
       NTSTATUS Status = 0;
       NTSTATUS SubStatus = 0;
       HANDLE LogonHandle;
       ULONG  PackageId;
       ULONG RequestSize;
       PKERB_RETRIEVE_TKT_REQUEST pTicketRequest = NULL;
       PKERB_RETRIEVE_TKT_RESPONSE pTicketResponse = NULL;
       ULONG ResponseSize;

       RequestSize = sizeof(*pTicketRequest) + 1;

       if (!PackageConnectLookup(&LogonHandle, &PackageId))
           return FALSE;

       pTicketRequest = (PKERB_RETRIEVE_TKT_REQUEST) LocalAlloc(LMEM_ZEROINIT, RequestSize);
       if (!pTicketRequest) {
           CloseHandle(LogonHandle);
           return FALSE;
       }

       pTicketRequest->MessageType = KerbRetrieveEncodedTicketMessage;
       pTicketRequest->LogonId.LowPart = 0;
       pTicketRequest->LogonId.HighPart = 0;
       pTicketRequest->TargetName.Length = 0;
       pTicketRequest->TargetName.MaximumLength = 0;
       pTicketRequest->TargetName.Buffer = (PWSTR) (pTicketRequest + 1);
       pTicketRequest->CacheOptions = 
           KERB_RETRIEVE_TICKET_DONT_USE_CACHE | KERB_RETRIEVE_TICKET_CACHE_TICKET;
       pTicketRequest->EncryptionType = 0;
       pTicketRequest->TicketFlags = 0;

       Status = LsaCallAuthenticationPackage( LogonHandle,
                                              PackageId,
                                              pTicketRequest,
                                              RequestSize,
                                              &pTicketResponse,
                                              &ResponseSize,
                                              &SubStatus
                                              );                                             

       LocalFree(pTicketRequest);
       CloseHandle(LogonHandle);

       if (FAILED(Status) || FAILED(SubStatus)) {
           if ( SubStatus == STATUS_NOT_SUPPORTED )
               /* The combination of the two CacheOption flags 
                * is not supported; therefore, the new flag is supported 
                */
               fCachesTicket = TRUE;
       }
       fChecked = TRUE;
   }

   return fCachesTicket;
}

static DWORD
ConcatenateUnicodeStrings(UNICODE_STRING *pTarget, UNICODE_STRING Source1, UNICODE_STRING Source2)
{
    //
    // The buffers for Source1 and Source2 cannot overlap pTarget's
    // buffer.  Source1.Length + Source2.Length must be <= 0xFFFF,
    // otherwise we overflow...
    //

    USHORT TotalSize = Source1.Length + Source2.Length;
    PBYTE buffer = (PBYTE) pTarget->Buffer;

    if (TotalSize > pTarget->MaximumLength)
        return ERROR_INSUFFICIENT_BUFFER;

    if ( pTarget->Buffer != Source1.Buffer )
        memcpy(buffer, Source1.Buffer, Source1.Length);
    memcpy(buffer + Source1.Length, Source2.Buffer, Source2.Length);

    pTarget->Length = TotalSize;
    return ERROR_SUCCESS;
}

static BOOL
get_STRING_from_registry(HKEY hBaseKey, char * key, char * value, char * outbuf, DWORD  outlen)
{
    HKEY hKey;
    DWORD dwCount;
    LONG rc;

	if (!outbuf || outlen == 0)
		return FALSE;

    rc = RegOpenKeyExA(hBaseKey, key, 0, KEY_QUERY_VALUE, &hKey);
    if (rc)
        return FALSE;

    dwCount = outlen;
    rc = RegQueryValueExA(hKey, value, 0, 0, (LPBYTE) outbuf, &dwCount);
    RegCloseKey(hKey);

    return rc?FALSE:TRUE;
}

static BOOL
GetSecurityLogonSessionData(PSECURITY_LOGON_SESSION_DATA * ppSessionData)
{
    NTSTATUS Status = 0;
    HANDLE  TokenHandle;
    TOKEN_STATISTICS Stats;
    DWORD   ReqLen;
    BOOL    Success;

    if (!ppSessionData)
        return FALSE;
    *ppSessionData = NULL;

    Success = OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &TokenHandle );
    if ( !Success )
        return FALSE;

    Success = GetTokenInformation( TokenHandle, TokenStatistics, &Stats, sizeof(TOKEN_STATISTICS), &ReqLen );
    CloseHandle( TokenHandle );
    if ( !Success )
        return FALSE;

    Status = LsaGetLogonSessionData( &Stats.AuthenticationId, ppSessionData );
    if ( FAILED(Status) || !ppSessionData )
        return FALSE;

    return TRUE;
}

//
// IsKerberosLogon() does not validate whether or not there are valid tickets in the 
// cache.  It validates whether or not it is reasonable to assume that if we 
// attempted to retrieve valid tickets we could do so.  Microsoft does not 
// automatically renew expired tickets.  Therefore, the cache could contain
// expired or invalid tickets.  Microsoft also caches the user's password 
// and will use it to retrieve new TGTs if the cache is empty and tickets
// are requested.

static BOOL
IsKerberosLogon(VOID)
{
    PSECURITY_LOGON_SESSION_DATA pSessionData = NULL;
    BOOL    Success = FALSE;

    if ( GetSecurityLogonSessionData(&pSessionData) ) {
        if ( pSessionData->AuthenticationPackage.Buffer ) {
            WCHAR buffer[256];
            WCHAR *usBuffer;
            int usLength;

            Success = FALSE;
            usBuffer = (pSessionData->AuthenticationPackage).Buffer;
            usLength = (pSessionData->AuthenticationPackage).Length;
            if (usLength < 256)
            {
                lstrcpynW (buffer, usBuffer, usLength);
                lstrcatW (buffer,L"");
                if ( !lstrcmpW(L"Kerberos",buffer) )
                    Success = TRUE;
            }
        }
        LsaFreeReturnBuffer(pSessionData);
    }
    return Success;
}

static DWORD
ConstructTicketRequest(UNICODE_STRING DomainName, PKERB_RETRIEVE_TKT_REQUEST * outRequest, ULONG * outSize)
{
    DWORD Error;
    UNICODE_STRING TargetPrefix;
    USHORT TargetSize;
    ULONG RequestSize;
    PKERB_RETRIEVE_TKT_REQUEST pTicketRequest = NULL;

    *outRequest = NULL;
    *outSize = 0;

    //
    // Set up the "krbtgt/" target prefix into a UNICODE_STRING so we
    // can easily concatenate it later.
    //

    TargetPrefix.Buffer = L"krbtgt/";
    TargetPrefix.Length = wcslen(TargetPrefix.Buffer) * sizeof(WCHAR);
    TargetPrefix.MaximumLength = TargetPrefix.Length;

    //
    // We will need to concatenate the "krbtgt/" prefix and the 
    // Logon Session's DnsDomainName into our request's target name.
    //
    // Therefore, first compute the necessary buffer size for that.
    //
    // Note that we might theoretically have integer overflow.
    //

    TargetSize = TargetPrefix.Length + DomainName.Length;

    //
    // The ticket request buffer needs to be a single buffer.  That buffer
    // needs to include the buffer for the target name.
    //

    RequestSize = sizeof(*pTicketRequest) + TargetSize;

    //
    // Allocate the request buffer and make sure it's zero-filled.
    //

    pTicketRequest = (PKERB_RETRIEVE_TKT_REQUEST) LocalAlloc(LMEM_ZEROINIT, RequestSize);
    if (!pTicketRequest)
        return GetLastError();

    //
    // Concatenate the target prefix with the previous reponse's
    // target domain.
    //

    pTicketRequest->TargetName.Length = 0;
    pTicketRequest->TargetName.MaximumLength = TargetSize;
    pTicketRequest->TargetName.Buffer = (PWSTR) (pTicketRequest + 1);
    Error = ConcatenateUnicodeStrings(&(pTicketRequest->TargetName),
                                        TargetPrefix,
                                        DomainName);
    *outRequest = pTicketRequest;
    *outSize    = RequestSize;
    return Error;
}

static BOOL
PurgeAllTickets(HANDLE LogonHandle, ULONG  PackageId)
{
    NTSTATUS Status = 0;
    NTSTATUS SubStatus = 0;
    KERB_PURGE_TKT_CACHE_REQUEST PurgeRequest;

    PurgeRequest.MessageType = KerbPurgeTicketCacheMessage;
    PurgeRequest.LogonId.LowPart = 0;
    PurgeRequest.LogonId.HighPart = 0;
    PurgeRequest.ServerName.Buffer = L"";
    PurgeRequest.ServerName.Length = 0;
    PurgeRequest.ServerName.MaximumLength = 0;
    PurgeRequest.RealmName.Buffer = L"";
    PurgeRequest.RealmName.Length = 0;
    PurgeRequest.RealmName.MaximumLength = 0;
    Status = LsaCallAuthenticationPackage(LogonHandle,
                                           PackageId,
                                           &PurgeRequest,
                                           sizeof(PurgeRequest),
                                           NULL,
                                           NULL,
                                           &SubStatus
                                           );
    if (FAILED(Status) || FAILED(SubStatus))
        return FALSE;
    return TRUE;
}

static BOOL
PurgeTicket2000( HANDLE LogonHandle, ULONG  PackageId, 
                 krb5_context context, krb5_creds *cred )
{
    NTSTATUS Status = 0;
    NTSTATUS SubStatus = 0;
    KERB_PURGE_TKT_CACHE_REQUEST * pPurgeRequest;
    DWORD dwRequestLen = sizeof(KERB_PURGE_TKT_CACHE_REQUEST) + 2048;
    char * sname = NULL, * srealm = NULL;

    if (krb5_unparse_name(context, cred->server, &sname))
        return FALSE;

    pPurgeRequest = malloc(dwRequestLen);
    if ( pPurgeRequest == NULL )
        return FALSE;
    memset(pPurgeRequest, 0, dwRequestLen);

    srealm = strrchr(sname, '@');
    *srealm = '\0';
    srealm++;

    pPurgeRequest->MessageType = KerbPurgeTicketCacheMessage;
    pPurgeRequest->LogonId.LowPart = 0;
    pPurgeRequest->LogonId.HighPart = 0;
    pPurgeRequest->ServerName.Buffer = (PWSTR)(((CHAR *)pPurgeRequest)+sizeof(KERB_PURGE_TKT_CACHE_REQUEST));
    pPurgeRequest->ServerName.Length = strlen(sname)*sizeof(WCHAR);
    pPurgeRequest->ServerName.MaximumLength = 256;
    ANSIToUnicode(sname, pPurgeRequest->ServerName.Buffer,
                  pPurgeRequest->ServerName.MaximumLength);
    pPurgeRequest->RealmName.Buffer = (PWSTR)(((CHAR *)pPurgeRequest)+sizeof(KERB_PURGE_TKT_CACHE_REQUEST)+512);
    pPurgeRequest->RealmName.Length = strlen(srealm)*sizeof(WCHAR);
    pPurgeRequest->RealmName.MaximumLength = 256;
    ANSIToUnicode(srealm, pPurgeRequest->RealmName.Buffer,
                  pPurgeRequest->RealmName.MaximumLength);

    Status = LsaCallAuthenticationPackage( LogonHandle,
                                           PackageId,
                                           pPurgeRequest,
                                           dwRequestLen,
                                           NULL,
                                           NULL,
                                           &SubStatus
                                           );
    free(pPurgeRequest);
    krb5_free_unparsed_name(context, sname);

    if (FAILED(Status) || FAILED(SubStatus))
        return FALSE;

    return TRUE;
}


static BOOL
PurgeTicketXP( HANDLE LogonHandle, ULONG  PackageId, 
               krb5_context context, krb5_flags flags, krb5_creds *cred)
{
    NTSTATUS Status = 0;
    NTSTATUS SubStatus = 0;
    KERB_PURGE_TKT_CACHE_EX_REQUEST * pPurgeRequest;
    DWORD dwRequestLen = sizeof(KERB_PURGE_TKT_CACHE_EX_REQUEST) + 4096;
    char * cname = NULL, * crealm = NULL;
    char * sname = NULL, * srealm = NULL;

    if (krb5_unparse_name(context, cred->client, &cname))
        return FALSE;
    
    if (krb5_unparse_name(context, cred->server, &sname)) {
        krb5_free_unparsed_name(context, cname);
        return FALSE;
    }

    pPurgeRequest = malloc(dwRequestLen);
    if ( pPurgeRequest == NULL )
        return FALSE;
    memset(pPurgeRequest, 0, dwRequestLen);

    crealm = strrchr(cname, '@');
    *crealm = '\0';
    crealm++;

    srealm = strrchr(sname, '@');
    *srealm = '\0';
    srealm++;

    pPurgeRequest->MessageType = KerbPurgeTicketCacheExMessage;
    pPurgeRequest->LogonId.LowPart = 0;
    pPurgeRequest->LogonId.HighPart = 0;
    pPurgeRequest->Flags = 0;
    pPurgeRequest->TicketTemplate.ClientName.Buffer = (PWSTR)((CHAR *)pPurgeRequest + sizeof(KERB_PURGE_TKT_CACHE_EX_REQUEST));
    pPurgeRequest->TicketTemplate.ClientName.Length = strlen(cname)*sizeof(WCHAR);
    pPurgeRequest->TicketTemplate.ClientName.MaximumLength = 256;
    ANSIToUnicode(cname, pPurgeRequest->TicketTemplate.ClientName.Buffer,
                  pPurgeRequest->TicketTemplate.ClientName.MaximumLength);

    pPurgeRequest->TicketTemplate.ClientRealm.Buffer = (PWSTR)(((CHAR *)pPurgeRequest)+sizeof(KERB_PURGE_TKT_CACHE_EX_REQUEST) + 512);
    pPurgeRequest->TicketTemplate.ClientRealm.Length = strlen(crealm)*sizeof(WCHAR);
    pPurgeRequest->TicketTemplate.ClientRealm.MaximumLength = 256;
    ANSIToUnicode(crealm, pPurgeRequest->TicketTemplate.ClientRealm.Buffer,
                  pPurgeRequest->TicketTemplate.ClientRealm.MaximumLength);

    pPurgeRequest->TicketTemplate.ServerName.Buffer = (PWSTR)(((CHAR *)pPurgeRequest)+sizeof(KERB_PURGE_TKT_CACHE_EX_REQUEST) + 1024);
    pPurgeRequest->TicketTemplate.ServerName.Length = strlen(sname)*sizeof(WCHAR);
    pPurgeRequest->TicketTemplate.ServerName.MaximumLength = 256;
    ANSIToUnicode(sname, pPurgeRequest->TicketTemplate.ServerName.Buffer,
                  pPurgeRequest->TicketTemplate.ServerName.MaximumLength);

    pPurgeRequest->TicketTemplate.ServerRealm.Buffer = (PWSTR)(((CHAR *)pPurgeRequest)+sizeof(KERB_PURGE_TKT_CACHE_EX_REQUEST) + 1536);
    pPurgeRequest->TicketTemplate.ServerRealm.Length = strlen(srealm)*sizeof(WCHAR);
    pPurgeRequest->TicketTemplate.ServerRealm.MaximumLength = 256;
    ANSIToUnicode(srealm, pPurgeRequest->TicketTemplate.ServerRealm.Buffer,
                  pPurgeRequest->TicketTemplate.ServerRealm.MaximumLength);

    pPurgeRequest->TicketTemplate.StartTime;
    pPurgeRequest->TicketTemplate.EndTime;
    pPurgeRequest->TicketTemplate.RenewTime;
    pPurgeRequest->TicketTemplate.EncryptionType = cred->keyblock.enctype;
    pPurgeRequest->TicketTemplate.TicketFlags = flags;

    Status = LsaCallAuthenticationPackage( LogonHandle,
                                           PackageId,
                                           pPurgeRequest,
                                           dwRequestLen,
                                           NULL,
                                           NULL,
                                           &SubStatus
                                           );
    free(pPurgeRequest);
    krb5_free_unparsed_name(context,cname);
    krb5_free_unparsed_name(context,sname);

    if (FAILED(Status) || FAILED(SubStatus))
        return FALSE;
    return TRUE;
}

/* 
 * A simple function to determine if there is an exact match between two tickets
 * We rely on the fact that the external tickets contain the raw Kerberos ticket.
 * If the EncodedTicket fields match, the KERB_EXTERNAL_TICKETs must be the same.
 */
static BOOL
KerbExternalTicketMatch( PKERB_EXTERNAL_TICKET one, PKERB_EXTERNAL_TICKET two )
{
    if ( one->EncodedTicketSize != two->EncodedTicketSize )
        return FALSE;

    if ( memcmp(one->EncodedTicket, two->EncodedTicket, one->EncodedTicketSize) )
         return FALSE;

    return TRUE;
}

krb5_boolean
krb5_is_permitted_tgs_enctype(krb5_context context, krb5_const_principal princ, krb5_enctype etype)
{
    krb5_enctype *list, *ptr;
    krb5_boolean ret;

    if (krb5_get_tgs_ktypes(context, princ, &list))
        return(0);
    
    ret = 0;

    for (ptr = list; *ptr; ptr++)
	if (*ptr == etype)
	    ret = 1;

    krb5_free_ktypes (context, list);

    return(ret);
}

#define ENABLE_PURGING 1
// to allow the purging of expired tickets from LSA cache.  This is necessary
// to force the retrieval of new TGTs.  Microsoft does not appear to retrieve
// new tickets when they expire.  Instead they continue to accept the expired
// tickets.  This is safe to do because the LSA purges its cache when it 
// retrieves a new TGT (ms calls this renew) but not when it renews the TGT
// (ms calls this refresh).

static BOOL
GetMSTGT(krb5_context context, HANDLE LogonHandle, ULONG PackageId, KERB_EXTERNAL_TICKET **ticket, BOOL enforce_tgs_enctypes)
{
    //
    // INVARIANTS:
    //
    //   (FAILED(Status) || FAILED(SubStatus)) ==> error
    //   bIsLsaError ==> LsaCallAuthenticationPackage() error
    //

    BOOL bIsLsaError = FALSE;
    NTSTATUS Status = 0;
    NTSTATUS SubStatus = 0;
    DWORD   Error;

    KERB_QUERY_TKT_CACHE_REQUEST CacheRequest;
    PKERB_RETRIEVE_TKT_REQUEST pTicketRequest = NULL;
    PKERB_RETRIEVE_TKT_RESPONSE pTicketResponse = NULL;
    ULONG RequestSize;
    ULONG ResponseSize;
#ifdef ENABLE_PURGING
    int    purge_cache = 0;
#endif /* ENABLE_PURGING */
    int    ignore_cache = 0;
    krb5_enctype *etype_list = NULL, *ptr = NULL, etype = 0;

    memset(&CacheRequest, 0, sizeof(KERB_QUERY_TKT_CACHE_REQUEST));
    CacheRequest.MessageType = KerbRetrieveTicketMessage;
    CacheRequest.LogonId.LowPart = 0;
    CacheRequest.LogonId.HighPart = 0;

    Status = LsaCallAuthenticationPackage(
        LogonHandle,
        PackageId,
        &CacheRequest,
        sizeof(CacheRequest),
        &pTicketResponse,
        &ResponseSize,
        &SubStatus
        );

    if (FAILED(Status))
    {
        // if the call to LsaCallAuthenticationPackage failed we cannot
        // perform any queries most likely because the Kerberos package 
        // is not available or we do not have access
        bIsLsaError = TRUE;
        goto cleanup;
    }

    if (FAILED(SubStatus)) {
        PSECURITY_LOGON_SESSION_DATA pSessionData = NULL;
        BOOL    Success = FALSE;
        OSVERSIONINFOEX verinfo;
        int supported = 0;

        // SubStatus 0x8009030E is not documented.  However, it appears
        // to mean there is no TGT
        if (SubStatus != 0x8009030E) {
            bIsLsaError = TRUE;
            goto cleanup;
        }

        verinfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
        GetVersionEx((OSVERSIONINFO *)&verinfo);
        supported = (verinfo.dwMajorVersion > 5) || 
            (verinfo.dwMajorVersion == 5 && verinfo.dwMinorVersion >= 1);

        // If we could not get a TGT from the cache we won't know what the
        // Kerberos Domain should have been.  On Windows XP and 2003 Server
        // we can extract it from the Security Logon Session Data.  However,
        // the required fields are not supported on Windows 2000.  :(
        if ( supported && GetSecurityLogonSessionData(&pSessionData) ) {
            if ( pSessionData->DnsDomainName.Buffer ) {
                Error = ConstructTicketRequest(pSessionData->DnsDomainName,
                                                &pTicketRequest, &RequestSize);
                LsaFreeReturnBuffer(pSessionData);
                if ( Error )
                    goto cleanup;
            } else {
                LsaFreeReturnBuffer(pSessionData);
                bIsLsaError = TRUE;
                goto cleanup;
            }
        } else {
            CHAR  UserDnsDomain[256];
            WCHAR UnicodeUserDnsDomain[256];
            UNICODE_STRING wrapper;
            if ( !get_STRING_from_registry(HKEY_CURRENT_USER,
                                          "Volatile Environment",
                                          "USERDNSDOMAIN",
                                           UserDnsDomain,
                                           sizeof(UserDnsDomain)
                                           ) )
            {
                goto cleanup;
            }

            ANSIToUnicode(UserDnsDomain,UnicodeUserDnsDomain,256);
            wrapper.Buffer = UnicodeUserDnsDomain;
            wrapper.Length = wcslen(UnicodeUserDnsDomain) * sizeof(WCHAR);
            wrapper.MaximumLength = 256;

            Error = ConstructTicketRequest(wrapper,
                                             &pTicketRequest, &RequestSize);
            if ( Error )
                goto cleanup;
        }
    } else {
        /* We have succeeded in obtaining a credential from the cache. 
         * Assuming the enctype is one that we support and the ticket
         * has not expired and is not marked invalid we will use it.
         * Otherwise, we must create a new ticket request and obtain
         * a credential we can use. 
         */

#ifdef PURGE_ALL
        purge_cache = 1;
#else
        /* Check Supported Enctypes */
        if ( !enforce_tgs_enctypes ||
             pTicketResponse->Ticket.SessionKey.KeyType == KERB_ETYPE_NULL ||
             krb5_is_permitted_tgs_enctype(context, NULL, pTicketResponse->Ticket.SessionKey.KeyType) ) {
            FILETIME Now, MinLife, EndTime, LocalEndTime;
            __int64  temp;
            // FILETIME is in units of 100 nano-seconds
            // If obtained tickets are either expired or have a lifetime
            // less than 20 minutes, retry ...
            GetSystemTimeAsFileTime(&Now);
            EndTime.dwLowDateTime=pTicketResponse->Ticket.EndTime.LowPart;
            EndTime.dwHighDateTime=pTicketResponse->Ticket.EndTime.HighPart;
            FileTimeToLocalFileTime(&EndTime, &LocalEndTime);
            temp = Now.dwHighDateTime;
            temp <<= 32;
            temp = Now.dwLowDateTime;
            temp += 1200 * 10000;
            MinLife.dwHighDateTime = (DWORD)((temp >> 32) & 0xFFFFFFFF);
            MinLife.dwLowDateTime = (DWORD)(temp & 0xFFFFFFFF);
            if (CompareFileTime(&MinLife, &LocalEndTime) >= 0) {
#ifdef ENABLE_PURGING
                purge_cache = 1;
#else
                ignore_cache = 1;
#endif /* ENABLE_PURGING */
            }
            if (pTicketResponse->Ticket.TicketFlags & KERB_TICKET_FLAGS_invalid) {
                ignore_cache = 1;   // invalid, need to attempt a TGT request
            }
            goto cleanup;           // we have a valid ticket, all done
        } else {
            // not supported
            ignore_cache = 1;
        }
#endif /* PURGE_ALL */

        Error = ConstructTicketRequest(pTicketResponse->Ticket.TargetDomainName,
                                        &pTicketRequest, &RequestSize);
        if ( Error ) {
            goto cleanup;
        }

        //
        // Free the previous response buffer so we can get the new response.
        //

        if ( pTicketResponse ) {
            memset(pTicketResponse,0,sizeof(KERB_RETRIEVE_TKT_RESPONSE));
            LsaFreeReturnBuffer(pTicketResponse);
            pTicketResponse = NULL;
        }

#ifdef ENABLE_PURGING
        if ( purge_cache ) {
            //
            // Purge the existing tickets which we cannot use so new ones can 
            // be requested.  It is not possible to purge just the TGT.  All
            // service tickets must be purged.
            //
            PurgeAllTickets(LogonHandle, PackageId);
        }
#endif /* ENABLE_PURGING */
    }
    
    //
    // Intialize the request of the request.
    //

    pTicketRequest->MessageType = KerbRetrieveEncodedTicketMessage;
    pTicketRequest->LogonId.LowPart = 0;
    pTicketRequest->LogonId.HighPart = 0;
    // Note: pTicketRequest->TargetName set up above
#ifdef ENABLE_PURGING
    pTicketRequest->CacheOptions = ((ignore_cache || !purge_cache) ? 
                                     KERB_RETRIEVE_TICKET_DONT_USE_CACHE : 0L);
#else
    pTicketRequest->CacheOptions = (ignore_cache ? KERB_RETRIEVE_TICKET_DONT_USE_CACHE : 0L);
#endif /* ENABLE_PURGING */
    pTicketRequest->TicketFlags = 0L;
    pTicketRequest->EncryptionType = 0L;

    Status = LsaCallAuthenticationPackage( LogonHandle,
                                           PackageId,
                                           pTicketRequest,
                                           RequestSize,
                                           &pTicketResponse,
                                           &ResponseSize,
                                           &SubStatus
                                           );

    if (FAILED(Status) || FAILED(SubStatus))
    {
        bIsLsaError = TRUE;
        goto cleanup;
    }

    //
    // Check to make sure the new tickets we received are of a type we support
    //

    /* Check Supported Enctypes */
    if ( !enforce_tgs_enctypes ||
         krb5_is_permitted_tgs_enctype(context, NULL, pTicketResponse->Ticket.SessionKey.KeyType) ) {
        goto cleanup;       // we have a valid ticket, all done
    }

    if (krb5_get_tgs_ktypes(context, NULL, &etype_list)) {
        ptr = etype_list = NULL;
        etype = ENCTYPE_DES_CBC_CRC;
    } else {
        ptr = etype_list + 1;
        etype = *etype_list;
    }

    while ( etype ) {
        // Try once more but this time specify the Encryption Type
        // (This will not store the retrieved tickets in the LSA cache unless
        // 0 is supported.)
        pTicketRequest->EncryptionType = etype;
        pTicketRequest->CacheOptions = 0;
        if ( does_retrieve_ticket_cache_ticket() )
            pTicketRequest->CacheOptions |= KERB_RETRIEVE_TICKET_CACHE_TICKET;

        if ( pTicketResponse ) {
            memset(pTicketResponse,0,sizeof(KERB_RETRIEVE_TKT_RESPONSE));
            LsaFreeReturnBuffer(pTicketResponse);
            pTicketResponse = NULL;
        }

        Status = LsaCallAuthenticationPackage( LogonHandle,
                                               PackageId,
                                               pTicketRequest,
                                               RequestSize,
                                               &pTicketResponse,
                                               &ResponseSize,
                                               &SubStatus
                                               );

        if (FAILED(Status) || FAILED(SubStatus))
        {
            bIsLsaError = TRUE;
            goto cleanup;
        }

        if ( pTicketResponse->Ticket.SessionKey.KeyType == etype && 
             (!enforce_tgs_enctypes ||
             krb5_is_permitted_tgs_enctype(context, NULL, pTicketResponse->Ticket.SessionKey.KeyType)) ) {
            goto cleanup;       // we have a valid ticket, all done
        }

        if ( ptr ) {
            etype = *ptr++;
        } else {
            etype = 0;
        }
    }

  cleanup:
    if ( etype_list )
        krb5_free_ktypes(context, etype_list);

    if ( pTicketRequest )
        LocalFree(pTicketRequest);

    if (FAILED(Status) || FAILED(SubStatus))
    {
        if (bIsLsaError)
        {
            // XXX - Will be fixed later
            if (FAILED(Status))
                ShowLsaError("LsaCallAuthenticationPackage", Status);
            if (FAILED(SubStatus))
                ShowLsaError("LsaCallAuthenticationPackage", SubStatus);
        }
        else
        {
            ShowWinError("GetMSTGT", Status);
        }

        if (pTicketResponse) {
            memset(pTicketResponse,0,sizeof(KERB_RETRIEVE_TKT_RESPONSE));
            LsaFreeReturnBuffer(pTicketResponse);
            pTicketResponse = NULL;
        }
        return(FALSE);
    }

    *ticket = &(pTicketResponse->Ticket);
    return(TRUE);
}

static BOOL
GetQueryTktCacheResponseW2K( HANDLE LogonHandle, ULONG PackageId,
                             PKERB_QUERY_TKT_CACHE_RESPONSE * ppResponse)
{
    NTSTATUS Status = 0;
    NTSTATUS SubStatus = 0;

    KERB_QUERY_TKT_CACHE_REQUEST CacheRequest;
    PKERB_QUERY_TKT_CACHE_RESPONSE pQueryResponse = NULL;
    ULONG ResponseSize;
    
    CacheRequest.MessageType = KerbQueryTicketCacheMessage;
    CacheRequest.LogonId.LowPart = 0;
    CacheRequest.LogonId.HighPart = 0;

    Status = LsaCallAuthenticationPackage(
        LogonHandle,
        PackageId,
        &CacheRequest,
        sizeof(CacheRequest),
        &pQueryResponse,
        &ResponseSize,
        &SubStatus
        );

    if ( !(FAILED(Status) || FAILED(SubStatus)) ) {
        *ppResponse = pQueryResponse;
        return TRUE;
    }

    return FALSE;
}

static BOOL
GetQueryTktCacheResponseXP( HANDLE LogonHandle, ULONG PackageId,
                            PKERB_QUERY_TKT_CACHE_EX_RESPONSE * ppResponse)
{
    NTSTATUS Status = 0;
    NTSTATUS SubStatus = 0;

    KERB_QUERY_TKT_CACHE_REQUEST CacheRequest;
    PKERB_QUERY_TKT_CACHE_EX_RESPONSE pQueryResponse = NULL;
    ULONG ResponseSize;
    
    CacheRequest.MessageType = KerbQueryTicketCacheExMessage;
    CacheRequest.LogonId.LowPart = 0;
    CacheRequest.LogonId.HighPart = 0;

    Status = LsaCallAuthenticationPackage(
        LogonHandle,
        PackageId,
        &CacheRequest,
        sizeof(CacheRequest),
        &pQueryResponse,
        &ResponseSize,
        &SubStatus
        );

    if ( !(FAILED(Status) || FAILED(SubStatus)) ) {
        *ppResponse = pQueryResponse;
        return TRUE;
    }

    return FALSE;
}

static BOOL
GetMSCacheTicketFromMITCred( HANDLE LogonHandle, ULONG PackageId,
                             krb5_context context, krb5_creds *creds, 
                             PKERB_EXTERNAL_TICKET *ticket)
{
    NTSTATUS Status = 0;
    NTSTATUS SubStatus = 0;
    ULONG RequestSize;
    PKERB_RETRIEVE_TKT_REQUEST pTicketRequest = NULL;
    PKERB_RETRIEVE_TKT_RESPONSE pTicketResponse = NULL;
    ULONG ResponseSize;

    RequestSize = sizeof(*pTicketRequest) + MAX_MSPRINC_SIZE;

    pTicketRequest = (PKERB_RETRIEVE_TKT_REQUEST) LocalAlloc(LMEM_ZEROINIT, RequestSize);
    if (!pTicketRequest)
        return FALSE;

    pTicketRequest->MessageType = KerbRetrieveEncodedTicketMessage;
    pTicketRequest->LogonId.LowPart = 0;
    pTicketRequest->LogonId.HighPart = 0;

    pTicketRequest->TargetName.Length = 0;
    pTicketRequest->TargetName.MaximumLength = MAX_MSPRINC_SIZE;
    pTicketRequest->TargetName.Buffer = (PWSTR) (pTicketRequest + 1);
    MITPrincToMSPrinc(context, creds->server, &pTicketRequest->TargetName);
    pTicketRequest->CacheOptions = 0;
    if ( does_retrieve_ticket_cache_ticket() )
        pTicketRequest->CacheOptions |= KERB_RETRIEVE_TICKET_CACHE_TICKET;
    pTicketRequest->TicketFlags = creds->ticket_flags;
    pTicketRequest->EncryptionType = creds->keyblock.enctype;

    Status = LsaCallAuthenticationPackage( LogonHandle,
                                           PackageId,
                                           pTicketRequest,
                                           RequestSize,
                                           &pTicketResponse,
                                           &ResponseSize,
                                           &SubStatus
                                           );

    LocalFree(pTicketRequest);

    if (FAILED(Status) || FAILED(SubStatus))
        return(FALSE);

    /* otherwise return ticket */
    *ticket = &(pTicketResponse->Ticket);
    return(TRUE);
}

static BOOL
GetMSCacheTicketFromCacheInfoW2K( HANDLE LogonHandle, ULONG PackageId,
                  PKERB_TICKET_CACHE_INFO tktinfo, PKERB_EXTERNAL_TICKET *ticket)
{
    NTSTATUS Status = 0;
    NTSTATUS SubStatus = 0;
    ULONG RequestSize;
    PKERB_RETRIEVE_TKT_REQUEST pTicketRequest = NULL;
    PKERB_RETRIEVE_TKT_RESPONSE pTicketResponse = NULL;
    ULONG ResponseSize;

    RequestSize = sizeof(*pTicketRequest) + tktinfo->ServerName.Length;

    pTicketRequest = (PKERB_RETRIEVE_TKT_REQUEST) LocalAlloc(LMEM_ZEROINIT, RequestSize);
    if (!pTicketRequest)
        return FALSE;

    pTicketRequest->MessageType = KerbRetrieveEncodedTicketMessage;
    pTicketRequest->LogonId.LowPart = 0;
    pTicketRequest->LogonId.HighPart = 0;
    pTicketRequest->TargetName.Length = tktinfo->ServerName.Length;
    pTicketRequest->TargetName.MaximumLength = tktinfo->ServerName.Length;
    pTicketRequest->TargetName.Buffer = (PWSTR) (pTicketRequest + 1);
    memcpy(pTicketRequest->TargetName.Buffer,tktinfo->ServerName.Buffer, tktinfo->ServerName.Length);
    pTicketRequest->CacheOptions = 0;
    if ( does_retrieve_ticket_cache_ticket() )
        pTicketRequest->CacheOptions |= KERB_RETRIEVE_TICKET_CACHE_TICKET;
    pTicketRequest->EncryptionType = tktinfo->EncryptionType;
    pTicketRequest->TicketFlags = 0;
    if ( tktinfo->TicketFlags & KERB_TICKET_FLAGS_forwardable )
        pTicketRequest->TicketFlags |= KDC_OPT_FORWARDABLE;
    if ( tktinfo->TicketFlags & KERB_TICKET_FLAGS_forwarded )
        pTicketRequest->TicketFlags |= KDC_OPT_FORWARDED;
    if ( tktinfo->TicketFlags & KERB_TICKET_FLAGS_proxiable )
        pTicketRequest->TicketFlags |= KDC_OPT_PROXIABLE;
    if ( tktinfo->TicketFlags & KERB_TICKET_FLAGS_renewable )
        pTicketRequest->TicketFlags |= KDC_OPT_RENEWABLE;

    Status = LsaCallAuthenticationPackage(
        LogonHandle,
        PackageId,
        pTicketRequest,
        RequestSize,
        &pTicketResponse,
        &ResponseSize,
        &SubStatus
        );

    LocalFree(pTicketRequest);

    if (FAILED(Status) || FAILED(SubStatus))
        return(FALSE);
    
    /* otherwise return ticket */
    *ticket = &(pTicketResponse->Ticket);

    /* set the initial flag if we were attempting to retrieve one
     * because Windows won't necessarily return the initial ticket
     * to us.
     */
    if ( tktinfo->TicketFlags & KERB_TICKET_FLAGS_initial )
        (*ticket)->TicketFlags |= KERB_TICKET_FLAGS_initial;

    return(TRUE);
}

static BOOL
GetMSCacheTicketFromCacheInfoXP( HANDLE LogonHandle, ULONG PackageId,
                  PKERB_TICKET_CACHE_INFO_EX tktinfo, PKERB_EXTERNAL_TICKET *ticket)
{
    NTSTATUS Status = 0;
    NTSTATUS SubStatus = 0;
    ULONG RequestSize;
    PKERB_RETRIEVE_TKT_REQUEST pTicketRequest = NULL;
    PKERB_RETRIEVE_TKT_RESPONSE pTicketResponse = NULL;
    ULONG ResponseSize;

    RequestSize = sizeof(*pTicketRequest) + tktinfo->ServerName.Length;

    pTicketRequest = (PKERB_RETRIEVE_TKT_REQUEST) LocalAlloc(LMEM_ZEROINIT, RequestSize);
    if (!pTicketRequest)
        return FALSE;

    pTicketRequest->MessageType = KerbRetrieveEncodedTicketMessage;
    pTicketRequest->LogonId.LowPart = 0;
    pTicketRequest->LogonId.HighPart = 0;
    pTicketRequest->TargetName.Length = tktinfo->ServerName.Length;
    pTicketRequest->TargetName.MaximumLength = tktinfo->ServerName.Length;
    pTicketRequest->TargetName.Buffer = (PWSTR) (pTicketRequest + 1);
    memcpy(pTicketRequest->TargetName.Buffer,tktinfo->ServerName.Buffer, tktinfo->ServerName.Length);
    pTicketRequest->CacheOptions = 0;
    pTicketRequest->EncryptionType = tktinfo->EncryptionType;
    pTicketRequest->TicketFlags = 0;
    if ( tktinfo->TicketFlags & KERB_TICKET_FLAGS_forwardable )
        pTicketRequest->TicketFlags |= KDC_OPT_FORWARDABLE;
    if ( tktinfo->TicketFlags & KERB_TICKET_FLAGS_forwarded )
        pTicketRequest->TicketFlags |= KDC_OPT_FORWARDED;
    if ( tktinfo->TicketFlags & KERB_TICKET_FLAGS_proxiable )
        pTicketRequest->TicketFlags |= KDC_OPT_PROXIABLE;
    if ( tktinfo->TicketFlags & KERB_TICKET_FLAGS_renewable )
        pTicketRequest->TicketFlags |= KDC_OPT_RENEWABLE;

    Status = LsaCallAuthenticationPackage(
        LogonHandle,
        PackageId,
        pTicketRequest,
        RequestSize,
        &pTicketResponse,
        &ResponseSize,
        &SubStatus
        );

    LocalFree(pTicketRequest);

    if (FAILED(Status) || FAILED(SubStatus))
        return(FALSE);
    
    /* otherwise return ticket */
    *ticket = &(pTicketResponse->Ticket);
    
    /* set the initial flag if we were attempting to retrieve one
     * because Windows won't necessarily return the initial ticket
     * to us.
     */
    if ( tktinfo->TicketFlags & KERB_TICKET_FLAGS_initial )
        (*ticket)->TicketFlags |= KERB_TICKET_FLAGS_initial;

    return(TRUE);
}

static krb5_error_code KRB5_CALLCONV krb5_lcc_close
        (krb5_context, krb5_ccache id);

static krb5_error_code KRB5_CALLCONV krb5_lcc_destroy
        (krb5_context, krb5_ccache id);

static krb5_error_code KRB5_CALLCONV krb5_lcc_end_seq_get
        (krb5_context, krb5_ccache id, krb5_cc_cursor *cursor);

static krb5_error_code KRB5_CALLCONV krb5_lcc_generate_new
        (krb5_context, krb5_ccache *id);

static const char * KRB5_CALLCONV krb5_lcc_get_name
        (krb5_context, krb5_ccache id);

static krb5_error_code KRB5_CALLCONV krb5_lcc_get_principal
        (krb5_context, krb5_ccache id, krb5_principal *princ);

static krb5_error_code KRB5_CALLCONV krb5_lcc_initialize
        (krb5_context, krb5_ccache id, krb5_principal princ);

static krb5_error_code KRB5_CALLCONV krb5_lcc_next_cred
        (krb5_context, krb5_ccache id, krb5_cc_cursor *cursor,
	 krb5_creds *creds);

static krb5_error_code KRB5_CALLCONV krb5_lcc_resolve
        (krb5_context, krb5_ccache *id, const char *residual);

static krb5_error_code KRB5_CALLCONV krb5_lcc_retrieve
        (krb5_context, krb5_ccache id, krb5_flags whichfields,
	 krb5_creds *mcreds, krb5_creds *creds);

static krb5_error_code KRB5_CALLCONV krb5_lcc_start_seq_get
        (krb5_context, krb5_ccache id, krb5_cc_cursor *cursor);

static krb5_error_code KRB5_CALLCONV krb5_lcc_store
        (krb5_context, krb5_ccache id, krb5_creds *creds);

static krb5_error_code KRB5_CALLCONV krb5_lcc_set_flags
        (krb5_context, krb5_ccache id, krb5_flags flags);

extern const krb5_cc_ops krb5_lcc_ops;

krb5_error_code krb5_change_cache (void);

krb5_boolean
krb5int_cc_creds_match_request(krb5_context, krb5_flags whichfields, krb5_creds *mcreds, krb5_creds *creds);

#define KRB5_OK 0

typedef struct _krb5_lcc_data {
    HANDLE LogonHandle;
    ULONG  PackageId;
    char * cc_name;
    krb5_principal princ;
    krb5_flags flags;
} krb5_lcc_data;

typedef struct _krb5_lcc_cursor {
    union {
        PKERB_QUERY_TKT_CACHE_RESPONSE w2k;
        PKERB_QUERY_TKT_CACHE_EX_RESPONSE xp;
    } response;
    unsigned int index;
    PKERB_EXTERNAL_TICKET mstgt;
} krb5_lcc_cursor;


/*
 * Requires:
 * residual is ignored
 *
 * Modifies:
 * id
 * 
 * Effects:
 * Acccess the MS Kerberos LSA cache in the current logon session
 * Ignore the residual.
 * 
 * Returns:
 * A filled in krb5_ccache structure "id".
 *
 * Errors:
 * KRB5_CC_NOMEM - there was insufficient memory to allocate the
 * 
 * 		krb5_ccache.  id is undefined.
 * permission errors
 */
static krb5_error_code KRB5_CALLCONV
krb5_lcc_resolve (krb5_context context, krb5_ccache *id, const char *residual)
{
    krb5_ccache lid;
    krb5_lcc_data *data;
    HANDLE LogonHandle;
    ULONG  PackageId;
    KERB_EXTERNAL_TICKET *msticket;
    krb5_error_code retval = KRB5_OK;

    if (!is_windows_2000() || is_broken_wow64())
        return KRB5_FCC_NOFILE;

    if (!PackageConnectLookup(&LogonHandle, &PackageId))
        return KRB5_FCC_NOFILE;

    lid = (krb5_ccache) malloc(sizeof(struct _krb5_ccache));
    if (lid == NULL) {
        CloseHandle(LogonHandle);
        return KRB5_CC_NOMEM;
    }

    lid->ops = &krb5_lcc_ops;

    lid->data = (krb5_pointer) malloc(sizeof(krb5_lcc_data));
    if (lid->data == NULL) {
        krb5_xfree(lid);
        CloseHandle(LogonHandle);
        return KRB5_CC_NOMEM;
    }

    lid->magic = KV5M_CCACHE;
    data = (krb5_lcc_data *)lid->data;    
    data->LogonHandle = LogonHandle;
    data->PackageId = PackageId;
    data->princ = 0;

    data->cc_name = (char *)malloc(strlen(residual)+1);
    if (data->cc_name == NULL) {
        krb5_xfree(lid->data);
        krb5_xfree(lid);
        CloseHandle(LogonHandle);
        return KRB5_CC_NOMEM;
    }
    strcpy(data->cc_name, residual);

    /*
     * we must obtain a tgt from the cache in order to determine the principal
     */
    if (GetMSTGT(context, data->LogonHandle, data->PackageId, &msticket, FALSE)) {
        /* convert the ticket */
        krb5_creds creds;
        if (!MSCredToMITCred(msticket, msticket->DomainName, context, &creds))
            retval = KRB5_FCC_INTERNAL;
        LsaFreeReturnBuffer(msticket);

        if (retval == KRB5_OK)
            krb5_copy_principal(context, creds.client, &data->princ);
        krb5_free_cred_contents(context,&creds);
    } else if (!does_retrieve_ticket_cache_ticket()) {
        krb5_xfree(data->cc_name);
        krb5_xfree(lid->data);
        krb5_xfree(lid);
        CloseHandle(LogonHandle);
        return KRB5_FCC_NOFILE;
    }

    /*
     * other routines will get errors on open, and callers must expect them,
     * if cache is non-existent/unusable 
     */
    *id = lid;
    return retval;
}

/*
*  return success although we do not do anything
*  We should delete all tickets belonging to the specified principal
*/

static krb5_error_code KRB5_CALLCONV
krb5_lcc_remove_cred(krb5_context context, krb5_ccache id, krb5_flags flags,
                     krb5_creds *creds);

static krb5_error_code KRB5_CALLCONV
krb5_lcc_initialize(krb5_context context, krb5_ccache id, krb5_principal princ)
{
    krb5_cc_cursor cursor;
    krb5_error_code code;
    krb5_creds cred;

    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    code = krb5_cc_start_seq_get(context, id, &cursor);
    if (code) {
        if (code == KRB5_CC_NOTFOUND)
            return KRB5_OK;
        return code;
    }

    while ( !(code = krb5_cc_next_cred(context, id, &cursor, &cred)) )
    {
        if ( krb5_principal_compare(context, princ, cred.client) ) {
            code = krb5_lcc_remove_cred(context, id, 0, &cred);
        }
        krb5_free_cred_contents(context, &cred);
    }

    if (code == KRB5_CC_END || code == KRB5_CC_NOTFOUND)
    {
        krb5_cc_end_seq_get(context, id, &cursor);
        return KRB5_OK;
    }
    return code;
}

/*
 * Modifies:
 * id
 *
 * Effects:
 * Closes the microsoft lsa cache, invalidates the id, and frees any resources
 * associated with the cache.
 */
static krb5_error_code KRB5_CALLCONV
krb5_lcc_close(krb5_context context, krb5_ccache id)
{
    register int closeval = KRB5_OK;
    register krb5_lcc_data *data;
    
    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    if (id) {
        data = (krb5_lcc_data *) id->data;

        if (data) {
            CloseHandle(data->LogonHandle);
            krb5_xfree(data);
        }
        krb5_xfree(id);
    }
    return closeval;
}

/*
 * Effects:
 * Destroys the contents of id.
 *
 * Errors:
 * system errors
 */
static krb5_error_code KRB5_CALLCONV
krb5_lcc_destroy(krb5_context context, krb5_ccache id)
{
    register krb5_lcc_data *data;
    
    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    if (id) { 
        data = (krb5_lcc_data *) id->data;

        return PurgeAllTickets(data->LogonHandle, data->PackageId) ? KRB5_OK : KRB5_FCC_INTERNAL;
    }   
    return KRB5_FCC_INTERNAL;
}

/*
 * Effects:
 * Prepares for a sequential search of the credentials cache.
 * Returns a krb5_cc_cursor to be used with krb5_lcc_next_cred and
 * krb5_lcc_end_seq_get.
 *
 * If the cache is modified between the time of this call and the time
 * of the final krb5_lcc_end_seq_get, the results are undefined.
 *
 * Errors:
 * KRB5_CC_NOMEM
 * KRB5_FCC_INTERNAL - system errors
 */
static krb5_error_code KRB5_CALLCONV
krb5_lcc_start_seq_get(krb5_context context, krb5_ccache id, krb5_cc_cursor *cursor)
{
    krb5_lcc_cursor *lcursor;
    krb5_lcc_data *data = (krb5_lcc_data *)id->data;

    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    lcursor = (krb5_lcc_cursor *) malloc(sizeof(krb5_lcc_cursor));
    if (lcursor == NULL) {
        *cursor = 0;
        return KRB5_CC_NOMEM;
    }

    /*
     * obtain a tgt to refresh the ccache in case the ticket is expired
     */
    if (!GetMSTGT(context, data->LogonHandle, data->PackageId, &lcursor->mstgt, TRUE)) {
        free(lcursor);
        *cursor = 0;
        return KRB5_CC_NOTFOUND;
    }

    if ( is_windows_xp() ) {
        if ( !GetQueryTktCacheResponseXP(data->LogonHandle, data->PackageId, &lcursor->response.xp) ) {
            LsaFreeReturnBuffer(lcursor->mstgt);
            free(lcursor);
            *cursor = 0;
            return KRB5_FCC_INTERNAL;
        }
    } else {
        if ( !GetQueryTktCacheResponseW2K(data->LogonHandle, data->PackageId, &lcursor->response.w2k) ) {
            LsaFreeReturnBuffer(lcursor->mstgt);
            free(lcursor);
            *cursor = 0;
            return KRB5_FCC_INTERNAL;
        }
    }
    lcursor->index = 0;
    *cursor = (krb5_cc_cursor) lcursor;
    return KRB5_OK;
}


/*
 * Requires:
 * cursor is a krb5_cc_cursor originally obtained from
 * krb5_lcc_start_seq_get.
 *
 * Modifes:
 * cursor
 * 
 * Effects:
 * Fills in creds with the TGT obtained from the MS LSA
 *
 * The cursor is updated to indicate TGT retrieval
 *
 * Errors:
 * KRB5_CC_END
 * KRB5_FCC_INTERNAL - system errors
 */
static krb5_error_code KRB5_CALLCONV
krb5_lcc_next_cred(krb5_context context, krb5_ccache id, krb5_cc_cursor *cursor, krb5_creds *creds)
{
    krb5_lcc_cursor *lcursor = (krb5_lcc_cursor *) *cursor;
    krb5_lcc_data *data;
    KERB_EXTERNAL_TICKET *msticket;
    krb5_error_code  retval = KRB5_OK;

    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    data = (krb5_lcc_data *)id->data;

  next_cred:
    if ( is_windows_xp() ) {
        if ( lcursor->index >= lcursor->response.xp->CountOfTickets ) {
            if (retval == KRB5_OK)
                return KRB5_CC_END;
            else {
                LsaFreeReturnBuffer(lcursor->mstgt);
                LsaFreeReturnBuffer(lcursor->response.xp);
                free(*cursor);
                *cursor = 0;
                return retval;
            }
        }

        if (!GetMSCacheTicketFromCacheInfoXP(data->LogonHandle, data->PackageId,
                                            &lcursor->response.xp->Tickets[lcursor->index++],&msticket)) {
            retval = KRB5_FCC_INTERNAL;
            goto next_cred;
        }
    } else {
        if ( lcursor->index >= lcursor->response.w2k->CountOfTickets ) {
            if (retval == KRB5_OK)
                return KRB5_CC_END;
            else {
                LsaFreeReturnBuffer(lcursor->mstgt);
                LsaFreeReturnBuffer(lcursor->response.w2k);
                free(*cursor);
                *cursor = 0;
                return retval;
            }
        }

        if (!GetMSCacheTicketFromCacheInfoW2K(data->LogonHandle, data->PackageId,
                                            &lcursor->response.w2k->Tickets[lcursor->index++],&msticket)) {
            retval = KRB5_FCC_INTERNAL;
            goto next_cred;
        }
    }

    /* Don't return tickets with NULL Session Keys */
    if ( msticket->SessionKey.KeyType == KERB_ETYPE_NULL) {
        LsaFreeReturnBuffer(msticket);
        goto next_cred;
    }

    /* convert the ticket */
    if ( is_windows_xp() ) {
        if (!MSCredToMITCred(msticket, lcursor->response.xp->Tickets[lcursor->index-1].ClientRealm, context, creds))
            retval = KRB5_FCC_INTERNAL;
    } else {
        if (!MSCredToMITCred(msticket, lcursor->mstgt->DomainName, context, creds))
            retval = KRB5_FCC_INTERNAL;
    }
    LsaFreeReturnBuffer(msticket);
    return retval;
}

/*
 * Requires:
 * cursor is a krb5_cc_cursor originally obtained from
 * krb5_lcc_start_seq_get.
 *
 * Modifies:
 * id, cursor
 *
 * Effects:
 * Finishes sequential processing of the file credentials ccache id,
 * and invalidates the cursor (it must never be used after this call).
 */
/* ARGSUSED */
static krb5_error_code KRB5_CALLCONV
krb5_lcc_end_seq_get(krb5_context context, krb5_ccache id, krb5_cc_cursor *cursor)
{
    krb5_lcc_cursor *lcursor = (krb5_lcc_cursor *) *cursor;

    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    if ( lcursor ) {
        LsaFreeReturnBuffer(lcursor->mstgt);
        if ( is_windows_xp() )
            LsaFreeReturnBuffer(lcursor->response.xp);
        else
            LsaFreeReturnBuffer(lcursor->response.w2k);
        free(*cursor);
    }
    *cursor = 0;

    return KRB5_OK;
}


/*
 * Errors:
 * KRB5_CC_READONLY - not supported
 */
static krb5_error_code KRB5_CALLCONV
krb5_lcc_generate_new (krb5_context context, krb5_ccache *id)
{
    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    return KRB5_CC_READONLY;
}

/*
 * Requires:
 * id is a ms lsa credential cache
 * 
 * Returns:
 *   The ccname specified during the krb5_lcc_resolve call
 */
static const char * KRB5_CALLCONV
krb5_lcc_get_name (krb5_context context, krb5_ccache id)
{

    if (!is_windows_2000())
        return "";

    if ( !id )
        return "";

    return (char *) ((krb5_lcc_data *) id->data)->cc_name;
}

/*
 * Modifies:
 * id, princ
 *
 * Effects:
 * Retrieves the primary principal from id, as set with
 * krb5_lcc_initialize.  The principal is returned is allocated
 * storage that must be freed by the caller via krb5_free_principal.
 *
 * Errors:
 * system errors
 * KRB5_CC_NOT_KTYPE
 */
static krb5_error_code KRB5_CALLCONV
krb5_lcc_get_principal(krb5_context context, krb5_ccache id, krb5_principal *princ)
{
    krb5_lcc_data *data = (krb5_lcc_data *)id->data;

    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    /* obtain principal */
    if (data->princ)
        return krb5_copy_principal(context, data->princ, princ);
    else {
        /*
         * we must obtain a tgt from the cache in order to determine the principal
         */
        KERB_EXTERNAL_TICKET *msticket;
        if (GetMSTGT(context, data->LogonHandle, data->PackageId, &msticket, FALSE)) {
            /* convert the ticket */
            krb5_creds creds;
            if (!MSCredToMITCred(msticket, msticket->DomainName, context, &creds))
            {
                LsaFreeReturnBuffer(msticket);
                return KRB5_FCC_INTERNAL;
            }
            LsaFreeReturnBuffer(msticket);

            krb5_copy_principal(context, creds.client, &data->princ);
            krb5_free_cred_contents(context,&creds);
            return krb5_copy_principal(context, data->princ, princ);
        }    
    }
    return KRB5_CC_NOTFOUND;
}

     
static krb5_error_code KRB5_CALLCONV
krb5_lcc_retrieve(krb5_context context, krb5_ccache id, krb5_flags whichfields, 
                  krb5_creds *mcreds, krb5_creds *creds)
{
    krb5_error_code kret = KRB5_OK;
    krb5_lcc_data *data = (krb5_lcc_data *)id->data;
    KERB_EXTERNAL_TICKET *msticket = 0, *mstgt = 0, *mstmp = 0;
    krb5_creds * mcreds_noflags = 0;
    krb5_creds   fetchcreds;

    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    memset(&fetchcreds, 0, sizeof(krb5_creds));

    /* first try to find out if we have an existing ticket which meets the requirements */
    kret = krb5_cc_retrieve_cred_default (context, id, whichfields, mcreds, creds);
    if ( !kret )
        return KRB5_OK;
    
    /* if not, we must try to get a ticket without specifying any flags or etypes */
    kret = krb5_copy_creds(context, mcreds, &mcreds_noflags);
    if (kret)
        goto cleanup;
    mcreds_noflags->ticket_flags = 0;
    mcreds_noflags->keyblock.enctype = 0;

    if (!GetMSCacheTicketFromMITCred(data->LogonHandle, data->PackageId, context, mcreds_noflags, &msticket)) {
        kret = KRB5_CC_NOTFOUND;
        goto cleanup;
    }

    /* try again to find out if we have an existing ticket which meets the requirements */
    kret = krb5_cc_retrieve_cred_default (context, id, whichfields, mcreds, creds);
    if ( !kret )
        goto cleanup;

    /* if not, obtain a ticket using the request flags and enctype even though it may not
     * be stored in the LSA cache for future use.
     */
    if ( msticket ) {
        LsaFreeReturnBuffer(msticket);
        msticket = 0;
    }

    if (!GetMSCacheTicketFromMITCred(data->LogonHandle, data->PackageId, context, mcreds, &msticket)) {
        kret = KRB5_CC_NOTFOUND;
        goto cleanup;
    }

    /* convert the ticket */
    if ( !is_windows_xp() || !does_retrieve_ticket_cache_ticket() ) {
        if ( PreserveInitialTicketIdentity() )
            GetMSTGT(context, data->LogonHandle, data->PackageId, &mstgt, FALSE);

        if (!MSCredToMITCred(msticket, mstgt ? mstgt->DomainName : msticket->DomainName, context, &fetchcreds))
        {
            kret = KRB5_FCC_INTERNAL;
            goto cleanup;
        }
    } else {
        /* We can obtain the correct client realm for a ticket by walking the
         * cache contents until we find the matching service ticket.
         */
        PKERB_QUERY_TKT_CACHE_EX_RESPONSE pResponse = 0;
        unsigned int i;

        if (!GetQueryTktCacheResponseXP( data->LogonHandle, data->PackageId, &pResponse)) {
            kret = KRB5_FCC_INTERNAL;
            goto cleanup;
        }

        for ( i=0; i<pResponse->CountOfTickets; i++ ) {
            if (!GetMSCacheTicketFromCacheInfoXP(data->LogonHandle, data->PackageId,
                                                  &pResponse->Tickets[i],&mstmp)) {
                continue;
            }

            if ( KerbExternalTicketMatch(msticket,mstmp) )
                break;

            LsaFreeReturnBuffer(mstmp);
            mstmp = 0;
        }

        if (!MSCredToMITCred(msticket, mstmp ? pResponse->Tickets[i].ClientRealm : msticket->DomainName, context, &fetchcreds))
        {
            LsaFreeReturnBuffer(pResponse);
            kret = KRB5_FCC_INTERNAL;
            goto cleanup;
        }
        LsaFreeReturnBuffer(pResponse);
    }


    /* check to see if this ticket matches the request using logic from
     * krb5_cc_retrieve_cred_default()
     */
    if ( krb5int_cc_creds_match_request(context, whichfields, mcreds, &fetchcreds) ) {
        *creds = fetchcreds;
    } else {
        krb5_free_cred_contents(context, &fetchcreds);
        kret = KRB5_CC_NOTFOUND;
    }

  cleanup:
    if ( mstmp )
        LsaFreeReturnBuffer(mstmp);
    if ( mstgt )
        LsaFreeReturnBuffer(mstgt);
    if ( msticket )
        LsaFreeReturnBuffer(msticket);
    if ( mcreds_noflags )
        krb5_free_creds(context, mcreds_noflags);
    return kret;
}


/*
 * We can't write to the MS LSA cache.  So we request the cache to obtain a ticket for the same
 * principal in the hope that next time the application requires a ticket for the service it
 * is attempt to store, the retrieved ticket will be good enough.
 *
 * Errors:
 * KRB5_CC_READONLY - not supported
 */
static krb5_error_code KRB5_CALLCONV
krb5_lcc_store(krb5_context context, krb5_ccache id, krb5_creds *creds)
{
    krb5_error_code kret = KRB5_OK;
    krb5_lcc_data *data = (krb5_lcc_data *)id->data;
    KERB_EXTERNAL_TICKET *msticket = 0, *msticket2 = 0;
    krb5_creds * creds_noflags = 0;

    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    /* If not, lets try to obtain a matching ticket from the KDC */
    if ( creds->ticket_flags != 0 && creds->keyblock.enctype != 0 ) {
        /* if not, we must try to get a ticket without specifying any flags or etypes */
        kret = krb5_copy_creds(context, creds, &creds_noflags);
        if (kret == 0) {
            creds_noflags->ticket_flags = 0;
            creds_noflags->keyblock.enctype = 0;

            GetMSCacheTicketFromMITCred(data->LogonHandle, data->PackageId, context, creds_noflags, &msticket2);
            krb5_free_creds(context, creds_noflags);
        }
    }

    GetMSCacheTicketFromMITCred(data->LogonHandle, data->PackageId, context, creds, &msticket);
    if (msticket || msticket2) {
        if (msticket)
            LsaFreeReturnBuffer(msticket);
        if (msticket2)
            LsaFreeReturnBuffer(msticket2);
        return KRB5_OK;
    }
    return KRB5_CC_READONLY;
}

/* 
 * Individual credentials can be implemented differently depending
 * on the operating system version.  (undocumented.)
 * 
 * Errors:
 *    KRB5_CC_READONLY: 
 */
static krb5_error_code KRB5_CALLCONV
krb5_lcc_remove_cred(krb5_context context, krb5_ccache id, krb5_flags flags,
                     krb5_creds *creds)
{
    krb5_lcc_data *data = (krb5_lcc_data *)id->data;

    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    if (!is_windows_xp()) {
        if ( PurgeTicket2000( data->LogonHandle, data->PackageId, context, creds) )
            return KRB5_OK;
    } else {
        if ( PurgeTicketXP( data->LogonHandle, data->PackageId, context, flags, creds) )
            return KRB5_OK;
    }

    return KRB5_CC_READONLY;
}


/*
 * Effects:
 *   Set
 */
static krb5_error_code KRB5_CALLCONV
krb5_lcc_set_flags(krb5_context context, krb5_ccache id, krb5_flags flags)
{
    krb5_lcc_data *data = (krb5_lcc_data *)id->data;

    if (!is_windows_2000())
        return KRB5_FCC_NOFILE;

    data->flags = flags;
    return KRB5_OK;
}

const krb5_cc_ops krb5_lcc_ops = {
     0,
     "MSLSA",
     krb5_lcc_get_name,
     krb5_lcc_resolve,
     krb5_lcc_generate_new,
     krb5_lcc_initialize,
     krb5_lcc_destroy,
     krb5_lcc_close,
     krb5_lcc_store,
     krb5_lcc_retrieve,
     krb5_lcc_get_principal,
     krb5_lcc_start_seq_get,
     krb5_lcc_next_cred,
     krb5_lcc_end_seq_get,
     krb5_lcc_remove_cred,
     krb5_lcc_set_flags
};
#endif /* _WIN32 */
