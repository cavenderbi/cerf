#include "cerf_ndis.h"
#include "cerf_ndis_miniport.h"
#include "cerf_ndis_packet_rx.h"
#include "cerf_debug_log.h"
#include "cerf_regs_map.h"

#include "../../cerf/peripherals/cerf_virt/cerf_virt_addr_map.h"
#include "../../cerf/peripherals/cerf_virt/cerf_virt_nic_regs.h"

#define CERF_MP_ETH_HDR                 14

typedef struct {
    UCHAR  MajorNdisVersion;
    UCHAR  MinorNdisVersion;
    USHORT Filler;
    UINT   Reserved;
    PVOID  CheckForHangHandler;
    PVOID  DisableInterruptHandler;
    PVOID  EnableInterruptHandler;
    PVOID  HaltHandler;
    PVOID  HandleInterruptHandler;
    PVOID  InitializeHandler;
    PVOID  ISRHandler;
    PVOID  QueryInformationHandler;
    PVOID  ReconfigureHandler;
    PVOID  ResetHandler;
    PVOID  SendHandler;
    PVOID  SetInformationHandler;
    PVOID  TransferDataHandler;
    PVOID  ReturnPacketHandler;
    PVOID  SendPacketsHandler;
    PVOID  AllocateCompleteHandler;
} CerfMiniportChars40;

static NDIS_HANDLE     s_wrapper;
static NDIS_HANDLE     s_adapter;
static volatile ULONG* s_nic;
static volatile UCHAR* s_stage;
static UCHAR           s_mac[6];
static ULONG           s_filter;
static ULONG           s_lookahead;
static ULONG           s_rx_consumed;
static HANDLE          s_rx_thread;
static volatile LONG   s_rx_stop;
static UCHAR           s_rx_hdr[CERF_MP_ETH_HDR];
static UCHAR*          s_rx_body;
static ULONG           s_rx_body_len;

extern "C" BOOL CerfMpRegistered(void) { return s_wrapper != 0; }

extern "C" const UCHAR* CerfMpMac(void) { return s_mac; }
extern "C" ULONG CerfMpFilterGet(void) { return s_filter; }
extern "C" void  CerfMpFilterSet(ULONG v) { s_filter = v; }
extern "C" ULONG CerfMpLookaheadGet(void) { return s_lookahead; }
extern "C" void  CerfMpLookaheadSet(ULONG v) { s_lookahead = v; }
extern "C" ULONG CerfMpLinkUp(void) {
    return s_nic && s_nic[CerfVirt::kNicRegLinkUp / 4] ? 1u : 0u;
}

static volatile ULONG* s_nic_page;

static volatile ULONG* CerfMpMapNic(void) {
    volatile ULONG* nic;

    if (s_nic_page) return s_nic_page;
    nic = (volatile ULONG*)CerfMapRegsPage(
        g_CerfVirtBase + CerfVirt::kNicRegsOffset, CerfVirt::kNicRegsSize);
    if (!nic) return 0;
    if (nic[CerfVirt::kNicRegMagic / 4] != CerfVirt::kNicMagic) {
        VirtualFree((LPVOID)nic, 0, MEM_RELEASE);
        return 0;
    }
    s_nic_page = nic;
    return nic;
}

extern "C" BOOL CerfMpNicChannelPresent(void) { return CerfMpMapNic() != 0; }

static ULONG s_ethdb_off;
static ULONG s_block_off;
static ULONG s_os_major;
static ULONG s_os_minor;
static int   s_layout_resolved;

extern "C" ULONG CerfMpEthDbOff(void) { return s_ethdb_off; }
extern "C" ULONG CerfMpOsMajor(void)  { return s_os_major; }

static void CerfMpResolveLayout(void) {
    OSVERSIONINFOW ovi;

    if (s_layout_resolved) return;
    s_layout_resolved = 1;

    memset(&ovi, 0, sizeof(ovi));
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    if (!GetVersionExW(&ovi)) {
        CERF_LOG_X("ndis: GetVersionExW failed gle", GetLastError());
        return;
    }
    s_os_major = ovi.dwMajorVersion;
    s_os_minor = ovi.dwMinorVersion;

    if (s_os_major == 2 && s_os_minor < 11) {
        s_ethdb_off = CERF_NDIS_MINIPORT_ETHDB_ALT;
        s_block_off = CERF_NDIS_FILTER_BLOCK_CE2;
    } else if (s_os_major <= 3) {
        s_ethdb_off = CERF_NDIS_MINIPORT_ETHDB;
        s_block_off = CERF_NDIS_FILTER_BLOCK_CE2;
    } else if (s_os_major <= 6) {
        s_ethdb_off = CERF_NDIS_MINIPORT_ETHDB;
        s_block_off = CERF_NDIS_FILTER_BLOCK_CE5;
    } else {
        s_ethdb_off = CERF_NDIS_MINIPORT_ETHDB;
        s_block_off = CERF_NDIS_FILTER_BLOCK_CE7;
    }
    CERF_LOG_X("ndis: os major", s_os_major);
    CERF_LOG_X("ndis: os minor", s_os_minor);
    CERF_LOG_X("ndis: EthDB offset", s_ethdb_off);
    CERF_LOG_X("ndis: filter block offset", s_block_off);
}

static void* CerfMpFilter(void) {
    void* filter;

    if (!s_adapter) return 0;
    CerfMpResolveLayout();
    if (!s_ethdb_off) return 0;

    filter = *(void**)((UCHAR*)s_adapter + s_ethdb_off);
    if (!filter) return 0;

    if (*(ULONG*)((UCHAR*)filter + s_block_off) != (ULONG)s_adapter) {
        CERF_LOG_X("ndis: Eth filter back-pointer mismatch at offset", s_block_off);
        return 0;
    }
    return filter;
}

static ULONG CerfMpMaxFrame(void) {
    return CerfVirt::kNicSlotSize - CerfVirt::kNicSlotPayloadOff;
}


extern "C" NDIS_STATUS CerfMpSend(NDIS_HANDLE adapter, void* packet) {
    const CerfNdisApi* api = CerfNdisResolve();
    ULONG  total, wseq, rseq, off;
    void*  buf;
    UCHAR* slot;

    (void)adapter;
    if (!api || !s_nic || !s_stage || !packet) {
        CERF_LOG("ndis: send with no channel");
        return CERF_NDIS_STATUS_FAILURE;
    }

    if (!*((UCHAR*)packet + CERF_NDIS_PACKET_VALIDCOUNTS)) {
        void* b   = *(void**)((UCHAR*)packet + CERF_NDIS_PACKET_HEAD);
        ULONG sum = 0;
        ULONG cnt = 0;
        while (b) {
            sum += *(ULONG*)((UCHAR*)b + CERF_NDIS_BUFFER_BYTECOUNT);
            ++cnt;
            b = *(void**)((UCHAR*)b + CERF_NDIS_BUFFER_NEXT);
        }
        *(ULONG*)((UCHAR*)packet + CERF_NDIS_PACKET_COUNT)     = cnt;
        *(ULONG*)((UCHAR*)packet + CERF_NDIS_PACKET_TOTALLEN)  = sum;
        *(ULONG*)((UCHAR*)packet + CERF_NDIS_PACKET_PHYSCOUNT) = 0;
        *((UCHAR*)packet + CERF_NDIS_PACKET_VALIDCOUNTS)       = 1;
    }

    total = *(ULONG*)((UCHAR*)packet + CERF_NDIS_PACKET_TOTALLEN);
    buf   = *(void**)((UCHAR*)packet + CERF_NDIS_PACKET_HEAD);
    if (total == 0 || total > CerfMpMaxFrame()) {
        CERF_LOG_X("ndis: send rejected, total len", total);
        return CERF_NDIS_STATUS_FAILURE;
    }

    wseq = s_nic[CerfVirt::kNicRegTxWriteSeq / 4];
    rseq = s_nic[CerfVirt::kNicRegTxReadSeq / 4];
    if (wseq - rseq >= CerfVirt::kNicTxSlots) {
        CERF_LOG_X("ndis: send rejected, tx ring full at", wseq);
        return CERF_NDIS_STATUS_FAILURE;
    }

    slot = (UCHAR*)s_stage + CerfVirt::kNicTxBufOffset +
           (wseq % CerfVirt::kNicTxSlots) * CerfVirt::kNicSlotSize;

    off = 0;
    while (buf && off < total) {
        void* va  = 0;
        UINT  n   = 0;
        api->QueryBuffer(buf, &va, &n);
        if (va && n) {
            if (off + n > total) n = total - off;
            memcpy(slot + CerfVirt::kNicSlotPayloadOff + off, va, n);
            off += n;
        }
        buf = *(void**)((UCHAR*)buf + CERF_NDIS_BUFFER_NEXT);
    }

    if (off != total) {
        CERF_LOG_X("ndis: send rejected, short buffer walk got", off);
        return CERF_NDIS_STATUS_FAILURE;
    }

    *(ULONG*)(slot + CerfVirt::kNicSlotLenOff) = off;
    s_nic[CerfVirt::kNicRegTxWriteSeq / 4] = wseq + 1;
    return CERF_NDIS_STATUS_SUCCESS;
}

extern "C" NDIS_STATUS CerfMpTransferData(void* packet, UINT* transferred,
                                          NDIS_HANDLE adapter, NDIS_HANDLE ctx,
                                          UINT offset, UINT want) {
    const CerfNdisApi* api = CerfNdisResolve();
    void* buf;
    UINT  done = 0;

    (void)adapter; (void)ctx;
    *transferred = 0;
    CERF_LOG_X_DEV("ndis: transferdata offset", offset);
    CERF_LOG_X_DEV("ndis: transferdata want", want);
    if (!api || !packet || !s_rx_body) return CERF_NDIS_STATUS_FAILURE;
    if (offset > s_rx_body_len) return CERF_NDIS_STATUS_FAILURE;
    if (offset + want > s_rx_body_len) want = s_rx_body_len - offset;

    buf = *(void**)((UCHAR*)packet + CERF_NDIS_PACKET_HEAD);
    while (buf && done < want) {
        void* va = 0;
        UINT  n  = 0;
        api->QueryBuffer(buf, &va, &n);
        if (va && n) {
            if (done + n > want) n = want - done;
            memcpy(va, s_rx_body + offset + done, n);
            done += n;
        }
        buf = *(void**)((UCHAR*)buf + CERF_NDIS_BUFFER_NEXT);
    }
    *transferred = done;
    return CERF_NDIS_STATUS_SUCCESS;
}

static DWORD WINAPI CerfMpRxThread(LPVOID unused) {
    const CerfNdisApi* api = CerfNdisResolve();
    (void)unused;
    if (!api) return 0;

    while (!s_rx_stop) {
        ULONG wseq = s_nic[CerfVirt::kNicRegRxWriteSeq / 4];
        while (s_rx_consumed != wseq) {
            UCHAR* slot;
            ULONG  len;
            void*  filter;

            slot = (UCHAR*)s_stage + CerfVirt::kNicRxBufOffset +
                   (s_rx_consumed % CerfVirt::kNicRxSlots) * CerfVirt::kNicSlotSize;
            len = *(ULONG*)(slot + CerfVirt::kNicSlotLenOff);

            filter = CerfMpFilter();

            CERF_LOG_X_DEV("ndis: rx slot len", len);

            if (CerfNdisPacketRxReady() &&
                len >= CERF_MP_ETH_HDR && len <= CerfMpMaxFrame()) {
                if (CerfNdisPacketRxIndicate(slot + CerfVirt::kNicSlotPayloadOff,
                                             len)) {
                    ++s_rx_consumed;
                    continue;
                }
                CERF_LOG_X("ndis: packet indicate failed, trying legacy, len", len);
            }

            if (filter && len >= CERF_MP_ETH_HDR && len <= CerfMpMaxFrame()) {
                memcpy(s_rx_hdr, slot + CerfVirt::kNicSlotPayloadOff, CERF_MP_ETH_HDR);
                s_rx_body     = slot + CerfVirt::kNicSlotPayloadOff + CERF_MP_ETH_HDR;
                s_rx_body_len = len - CERF_MP_ETH_HDR;

                void* lock = 0;
                if (s_block_off == CERF_NDIS_FILTER_BLOCK_CE2 &&
                    api->AcquireSpinLock && api->ReleaseSpinLock) {
                    ULONG block = *(ULONG*)((UCHAR*)filter + s_block_off);
                    lock = (void*)(block + CERF_NDIS_BLOCK_SPINLOCK_CE2);
                    api->AcquireSpinLock(lock);
                }

                api->EthIndicateReceive(filter, (NDIS_HANDLE)&s_rx_body,
                                        (char*)s_rx_hdr, s_rx_hdr, CERF_MP_ETH_HDR,
                                        s_rx_body, s_rx_body_len, s_rx_body_len);
                api->EthIndicateReceiveComplete(filter);

                if (lock) api->ReleaseSpinLock(lock);
                s_rx_body = 0;
                CERF_LOG_X_DEV("ndis: rx indicated body len", s_rx_body_len);
            } else {
                CERF_LOG_X("ndis: rx frame dropped, no consumer, len", len);
            }
            ++s_rx_consumed;
        }
        s_nic[CerfVirt::kNicRegRxReadSeq / 4] = s_rx_consumed;
        Sleep(5);
    }
    return 0;
}

extern "C" NDIS_STATUS CerfMpInitialize(NDIS_STATUS* open_status, UINT* medium_index,
                                        ULONG* medium_array, UINT medium_count,
                                        NDIS_HANDLE adapter, NDIS_HANDLE config) {
    const CerfNdisApi* api = CerfNdisResolve();
    UINT i;
    ULONG w0, w1;

    (void)config;
    *open_status = CERF_NDIS_STATUS_SUCCESS;
    if (!api) {
        CERF_LOG("ndis: initialize with no ndis api");
        return CERF_NDIS_STATUS_FAILURE;
    }

    for (i = 0; i < medium_count; ++i)
        if (medium_array[i] == CERF_NDIS_MEDIUM_802_3) break;
    if (i == medium_count) {
        *open_status = CERF_NDIS_STATUS_UNSUPPORTED_MEDIA;
        return CERF_NDIS_STATUS_UNSUPPORTED_MEDIA;
    }
    *medium_index = i;

    s_nic = CerfMpMapNic();
    if (!s_nic) {
        CERF_LOG("ndis: cerf_virt NIC channel absent");
        return CERF_NDIS_STATUS_FAILURE;
    }
    if (!s_stage) {
        s_stage = (volatile UCHAR*)CerfMapRegsPage(
            g_CerfVirtBase + CerfVirt::kNicStageOffset, CerfVirt::kNicStageSize);
    }
    if (!s_stage) return CERF_NDIS_STATUS_FAILURE;

    w0 = s_nic[CerfVirt::kNicRegMacWord0 / 4];
    w1 = s_nic[CerfVirt::kNicRegMacWord1 / 4];
    s_mac[0] = (UCHAR)(w0);       s_mac[1] = (UCHAR)(w0 >> 8);
    s_mac[2] = (UCHAR)(w0 >> 16); s_mac[3] = (UCHAR)(w0 >> 24);
    s_mac[4] = (UCHAR)(w1);       s_mac[5] = (UCHAR)(w1 >> 8);

    s_adapter     = adapter;
    s_lookahead   = CERF_MP_FRAME_MAX;
    s_rx_consumed = s_nic[CerfVirt::kNicRegRxWriteSeq / 4];

    CerfMpResolveLayout();

    api->MSetAttributesEx(adapter, adapter, 0, 0, 0);
    CerfNdisPacketRxInit(adapter);

    if (api->MIndicateStatus) {
        api->MIndicateStatus(adapter, CERF_NDIS_STATUS_MEDIA_CONNECT, 0, 0);
        if (api->MIndicateStatusComplete) api->MIndicateStatusComplete(adapter);
    }

    s_rx_stop   = 0;
    s_rx_thread = CreateThread(0, 0, CerfMpRxThread, 0, 0, 0);

    CERF_LOG_X("ndis: miniport initialized, medium index", i);
    return CERF_NDIS_STATUS_SUCCESS;
}

extern "C" void CerfMpHalt(NDIS_HANDLE adapter) {
    BOOL rx_exited = TRUE;

    (void)adapter;
    s_rx_stop = 1;
    if (s_rx_thread) {
        rx_exited = WaitForSingleObject(s_rx_thread, 2000) == WAIT_OBJECT_0;
        CloseHandle(s_rx_thread);
        s_rx_thread = 0;
    }
    s_adapter = 0;
    CerfNdisPacketRxShutdown(rx_exited);
    CERF_LOG("ndis: miniport halted");
}

extern "C" NDIS_STATUS CerfMpReset(BOOLEAN* addressing_reset, NDIS_HANDLE adapter) {
    (void)adapter;
    *addressing_reset = FALSE;
    return CERF_NDIS_STATUS_SUCCESS;
}

extern "C" void CerfMpNoOp(NDIS_HANDLE adapter) { (void)adapter; }

extern "C" void CerfMpReturnPacket(NDIS_HANDLE adapter, void* packet) {
    (void)adapter;
    CerfNdisPacketRxReturn(packet);
}

extern "C" NDIS_STATUS DriverEntry(void* driver_object, void* registry_path) {
    const CerfNdisApi* api = CerfNdisResolve();
    static CerfMiniportChars40 chars;
    NDIS_STATUS st;

    CERF_LOG_INIT(CERF_LOG_CH_SHARED_FOLDERS);
    if (!api) return CERF_NDIS_STATUS_FAILURE;

    memset(&chars, 0, sizeof(chars));
    chars.MajorNdisVersion        = 4;
    chars.MinorNdisVersion        = 0;
    chars.DisableInterruptHandler = (PVOID)CerfMpNoOp;
    chars.EnableInterruptHandler  = (PVOID)CerfMpNoOp;
    chars.HaltHandler             = (PVOID)CerfMpHalt;
    chars.HandleInterruptHandler  = (PVOID)CerfMpNoOp;
    chars.InitializeHandler       = (PVOID)CerfMpInitialize;
    chars.ISRHandler              = 0;
    chars.QueryInformationHandler = (PVOID)CerfMpQueryInformation;
    chars.ResetHandler            = (PVOID)CerfMpReset;
    chars.SendHandler             = (PVOID)CerfMpSend;
    chars.SetInformationHandler   = (PVOID)CerfMpSetInformation;
    chars.TransferDataHandler     = (PVOID)CerfMpTransferData;
    chars.ReturnPacketHandler     = (PVOID)CerfMpReturnPacket;

    api->InitializeWrapper(&s_wrapper, driver_object, registry_path, 0);
    st = api->MRegisterMiniport(s_wrapper, &chars, sizeof(chars));
    if (st != CERF_NDIS_STATUS_SUCCESS) {
        CERF_LOG_X("ndis: NdisMRegisterMiniport failed", (ULONG)st);
        api->TerminateWrapper(s_wrapper, 0);
        s_wrapper = 0;
        return st;
    }
    CERF_LOG("ndis: miniport registered");
    return CERF_NDIS_STATUS_SUCCESS;
}
