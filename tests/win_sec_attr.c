/*
    Copyright (c) 2015 Timothee "TTimo" Besset  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.


*/

#include "../src/grid.h"
#include "../src/pair.h"
#include "../src/pubsub.h"
#include "../src/ipc.h"

#include "testutil.h"

#include <AccCtrl.h>
#include <Sddl.h>
#include <Aclapi.h>

/*  Windows only. Custom SECURITY_ATTRIBUTES on a socket. */

#define PIPE_NAME "win_sec_attr.ipc"
#define SOCKET_ADDRESS "ipc://" PIPE_NAME

int main ()
{
    int sb;
    int sc;
    SECURITY_ATTRIBUTES sec;
    BOOL ret;
    SID SIDAuthUsers;
    DWORD SIDSize;
    EXPLICIT_ACCESS xa;
    PACL pACL;
    DWORD ret2;
    int ret3;
    void * void_ret_value = NULL;
    size_t void_ret_size = sizeof(void_ret_value);
    HANDLE pipeHandle = NULL;
    PSID pSidOwner = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;

    BOOL equal = FALSE;
    BOOL bret = FALSE;
    ACL* dacl = NULL;
    PACE_HEADER ace = NULL;
    PACCESS_ALLOWED_ACE allowed_ace = NULL;
    PSID the_sid = NULL;
    SECURITY_DESCRIPTOR* sd = NULL;

    sc = test_socket (AF_SP, GRID_PAIR);
    test_connect (sc, SOCKET_ADDRESS);

    sb = test_socket (AF_SP, GRID_PAIR);

    memset (&sec, 0, sizeof(sec));
    sec.lpSecurityDescriptor = (PSECURITY_DESCRIPTOR)malloc (SECURITY_DESCRIPTOR_MIN_LENGTH);
    ret = InitializeSecurityDescriptor (sec.lpSecurityDescriptor, SECURITY_DESCRIPTOR_REVISION);
    grid_assert (ret);

    SIDSize = sizeof (SIDAuthUsers);
    ret = CreateWellKnownSid (WinAuthenticatedUserSid, NULL, &SIDAuthUsers, &SIDSize);
    grid_assert (ret);

    xa.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
    xa.grfAccessMode = SET_ACCESS;
    xa.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    xa.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    xa.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    xa.Trustee.ptstrName  = (LPSTR) &SIDAuthUsers;
    ret2 = SetEntriesInAcl (1, &xa, NULL, &pACL);
    grid_assert (ret2 == ERROR_SUCCESS);

    ret = SetSecurityDescriptorDacl (sec.lpSecurityDescriptor, TRUE, pACL, FALSE);
    grid_assert (ret);

    sec.nLength = sizeof(sec);
    sec.bInheritHandle = TRUE;

    ret3 = grid_setsockopt (sb, GRID_IPC, GRID_IPC_SEC_ATTR, (void*)&sec, sizeof(&sec));
    grid_assert (ret3 == 0);
    test_bind (sb, SOCKET_ADDRESS);

    grid_sleep (200);

    test_send (sc, "0123456789012345678901234567890123456789");
    test_recv (sb, "0123456789012345678901234567890123456789");

    ret3 = grid_getsockopt(sb, GRID_IPC, GRID_IPC_SEC_ATTR, &void_ret_value, &void_ret_size);
    grid_assert(ret3 == 0);
    grid_assert(void_ret_value == &sec);


    // verify that the pipe has the same security descriptor that we set by comparing the ace of the kernel object
    // to the one we created it with
    pipeHandle = CreateFileA(
        "\\\\.\\\\pipe\\" PIPE_NAME,
        READ_CONTROL,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL
        );

    grid_assert(pipeHandle != INVALID_HANDLE_VALUE);


    ret2 = GetSecurityInfo(pipeHandle, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd);

    grid_assert(ret2 == ERROR_SUCCESS);
    grid_assert(1 == dacl->AceCount);

    bret = GetAce(dacl, 0, &ace);

    grid_assert(bret == TRUE);
    grid_assert(ace->AceType == ACCESS_ALLOWED_ACE_TYPE);

    allowed_ace = (PACCESS_ALLOWED_ACE)ace;
    the_sid = (PSID)&(allowed_ace->SidStart);

    grid_assert(IsValidSid(the_sid));

    equal = EqualSid((PSID)&(allowed_ace->SidStart), &SIDAuthUsers);
    grid_assert(equal);
    LocalFree(dacl);

    test_close (sc);
    test_close (sb);

    LocalFree (pACL);
    
    free (sec.lpSecurityDescriptor);

    return 0;
}
