#include "cerf_fs_driver.h"

#include <windows.h>

#define CERF_FS_MAX_FIND 40

static unsigned char g_tid_used[CERF_FS_MAX_FIND];

static unsigned long AllocTid(void) {
    unsigned long i;
    for (i = 0; i < CERF_FS_MAX_FIND; ++i)
        if (!g_tid_used[i]) { g_tid_used[i] = 1; return i; }
    return 0xFFFFFFFFu;
}
static void FreeTid(unsigned long tid) {
    if (tid < CERF_FS_MAX_FIND) g_tid_used[tid] = 0;
}

static void HostFindClose(unsigned long tid) {
    CerfFsServerPB* pb = CerfFsPb();
    pb->fFindTransactionID = tid;
    CerfFsCall(pb, CERF_FS_OP_FIND_CLOSE);
}

static void FillFindData(WIN32_FIND_DATAW* fd, CerfFsServerPB* pb) {
    int n;
    memset(fd, 0, sizeof(*fd));
    fd->dwFileAttributes = pb->fFileAttributes;
    CerfFsDosToFiletime(pb->fFileCreateTimeDate, &fd->ftCreationTime);
    CerfFsDosToFiletime(pb->fFileTimeDate, &fd->ftLastWriteTime);
    fd->ftLastAccessTime = fd->ftLastWriteTime;
    fd->nFileSizeLow = pb->fSize;
    /* devemu_wm5 nk.exe 0x8009A12C OpenExe: oe.bIsOID =
       GetFileInformationByHandle(hf, &bhfi) && bhfi.dwOID != 0xFFFFFFFF. */
    fd->dwOID = (DWORD)-1;
    n = pb->u.fNameLength / sizeof(WCHAR);
    if (n > MAX_PATH - 1) n = MAX_PATH - 1;
    memcpy(fd->cFileName, pb->u.fName, n * sizeof(WCHAR));
    fd->cFileName[n] = 0;
}

HANDLE CerfFsFindFirstFileW(CerfVol* vol, HANDLE hProc, PCWSTR spec,
                            PWIN32_FIND_DATAW fd) {
    CerfFsServerPB* pb = CerfFsPb();
    unsigned long tid, e;
    HANDLE h = INVALID_HANDLE_VALUE;
    CerfFind* sc;
    int n;
    (void)vol;

    CerfFsLock();
    tid = AllocTid();
    if (tid == 0xFFFFFFFFu) {
        CerfFsUnlock();
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return INVALID_HANDLE_VALUE;
    }
    pb->fIndex = 0;
    pb->fFindTransactionID = tid;
    n = lstrlenW(spec);
    if (n > (int)CERF_FS_MAX_LFN) n = (int)CERF_FS_MAX_LFN;
    memcpy(pb->u.fName, spec, n * sizeof(WCHAR));
    pb->u.fName[n] = 0;
    pb->u.fNameLength = (unsigned short)(n * sizeof(WCHAR));

    e = CerfFsCall(pb, CERF_FS_OP_GET_INFO);
    if (e == CERF_FS_OK) {
        FillFindData(fd, pb);
        sc = (CerfFind*)LocalAlloc(LPTR, sizeof(CerfFind));
        if (sc) {
            sc->tid = tid;
            h = CerfFsMakeHandle(CerfFsFindApi(), sc, hProc);
            if (h == INVALID_HANDLE_VALUE) {
                HostFindClose(tid);
                FreeTid(tid);
                LocalFree(sc);
                e = ERROR_OUTOFMEMORY;
            }
        } else {
            HostFindClose(tid);
            FreeTid(tid);
            e = ERROR_OUTOFMEMORY;
        }
    } else {
        FreeTid(tid);
    }
    CerfFsUnlock();
    if (h == INVALID_HANDLE_VALUE) CerfFsResultToBool(e);
    return h;
}

BOOL CerfFsFindNextFileW(CerfFind* s, PWIN32_FIND_DATAW fd) {
    CerfFsServerPB* pb = CerfFsPb();
    unsigned long e;
    CerfFsLock();
    pb->fIndex = 1;
    pb->fFindTransactionID = s->tid;
    e = CerfFsCall(pb, CERF_FS_OP_GET_INFO);
    if (e == CERF_FS_OK) FillFindData(fd, pb);
    CerfFsUnlock();
    return CerfFsResultToBool(e);
}

BOOL CerfFsFindClose(CerfFind* s) {
    CerfFsLock();
    HostFindClose(s->tid);
    FreeTid(s->tid);
    CerfFsUnlock();
    LocalFree(s);
    return TRUE;
}
