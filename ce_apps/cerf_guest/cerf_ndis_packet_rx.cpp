#include "cerf_ndis.h"
#include "cerf_ndis_miniport.h"
#include "cerf_ndis_packet_rx.h"
#include "cerf_debug_log.h"

#include "../../cerf/peripherals/cerf_virt/cerf_virt_addr_map.h"
#include "../../cerf/peripherals/cerf_virt/cerf_virt_nic_regs.h"

#define CERF_MP_RX_SLOTS      16
#define CERF_MP_RX_SLOT_BYTES (CerfVirt::kNicSlotSize - CerfVirt::kNicSlotPayloadOff)

#define CERF_NDIS_PACKET_TAIL 12

typedef void (*PFN_PacketIndicate)(void* adapter, void** packets, UINT count);

typedef struct {
    void*  packet;
    UCHAR* data;
    LONG   busy;
} CerfRxSlot;

static void*      s_rx_adapter;
static void*      s_pkt_pool;
static void*      s_buf_pool;
static CerfRxSlot s_rx_slot[CERF_MP_RX_SLOTS];
static BOOL       s_rx_ready;

static UCHAR* CerfRxOob(void* packet) {
    return (UCHAR*)packet +
           *(USHORT*)((UCHAR*)packet + CERF_NDIS_PACKET_OOB_OFF);
}

static void CerfRxReclaimSlot(const CerfNdisApi* api, int i) {
    void* packet = s_rx_slot[i].packet;
    void* buf;

    if (!packet) return;
    buf = *(void**)((UCHAR*)packet + CERF_NDIS_PACKET_HEAD);
    if (buf && api && api->FreeBuffer) api->FreeBuffer(buf);
    *(void**)((UCHAR*)packet + CERF_NDIS_PACKET_HEAD) = 0;
    *(void**)((UCHAR*)packet + CERF_NDIS_PACKET_TAIL) = 0;
    *(ULONG*)(CerfRxOob(packet) + CERF_NDIS_OOB_STATUS) = 0;
    InterlockedExchange((LONG*)&s_rx_slot[i].busy, 0);
}

static PFN_PacketIndicate CerfRxIndicateHandler(void) {
    ULONG ethdb = CerfMpEthDbOff();

    if (!s_rx_adapter || !ethdb) return 0;
    return *(PFN_PacketIndicate*)((UCHAR*)s_rx_adapter + ethdb +
                                  CERF_NDIS_BLOCK_PKTINDICATE_FROM_ETHDB);
}

BOOL CerfNdisPacketRxInit(void* adapter) {
    const CerfNdisApi* api = CerfNdisResolve();
    NDIS_STATUS st = CERF_NDIS_STATUS_FAILURE;
    int i;

    if (s_rx_ready) return TRUE;
    if (!api || !adapter) return FALSE;
    if (!api->AllocatePacketPool || !api->AllocatePacket ||
        !api->AllocateBufferPool || !api->AllocateBuffer) {
        CERF_LOG("ndis: packet pools unavailable - legacy receive only");
        return FALSE;
    }

    if (CerfMpOsMajor() < 7) {
        CERF_LOG_X("ndis: legacy receive path for os major", CerfMpOsMajor());
        return FALSE;
    }

    s_rx_adapter = adapter;

    api->AllocatePacketPool(&st, &s_pkt_pool, CERF_MP_RX_SLOTS, 0);
    if (st != CERF_NDIS_STATUS_SUCCESS || !s_pkt_pool) {
        CERF_LOG_X("ndis: packet pool alloc failed", (ULONG)st);
        s_rx_adapter = 0;
        return FALSE;
    }
    api->AllocateBufferPool(&st, &s_buf_pool, CERF_MP_RX_SLOTS);
    if (st != CERF_NDIS_STATUS_SUCCESS) {
        CERF_LOG_X("ndis: buffer pool alloc failed", (ULONG)st);
        if (api->FreePacketPool) api->FreePacketPool(s_pkt_pool);
        s_pkt_pool   = 0;
        s_buf_pool   = 0;
        s_rx_adapter = 0;
        return FALSE;
    }

    for (i = 0; i < CERF_MP_RX_SLOTS; ++i) {
        s_rx_slot[i].data = (UCHAR*)LocalAlloc(LMEM_FIXED, CERF_MP_RX_SLOT_BYTES);
        if (!s_rx_slot[i].data) break;
        api->AllocatePacket(&st, &s_rx_slot[i].packet, s_pkt_pool);
        if (st != CERF_NDIS_STATUS_SUCCESS || !s_rx_slot[i].packet) break;
        s_rx_slot[i].busy = 0;
    }
    if (i == 0) {
        CERF_LOG("ndis: no receive packets allocated");
        if (s_rx_slot[0].data) {
            LocalFree(s_rx_slot[0].data);
            s_rx_slot[0].data = 0;
        }
        if (api->FreePacketPool) api->FreePacketPool(s_pkt_pool);
        if (api->FreeBufferPool) api->FreeBufferPool(s_buf_pool);
        s_pkt_pool   = 0;
        s_buf_pool   = 0;
        s_rx_adapter = 0;
        return FALSE;
    }

    s_rx_ready = TRUE;
    CERF_LOG_X("ndis: packet receive ready, slots", (ULONG)i);
    return TRUE;
}

BOOL CerfNdisPacketRxReady(void) { return s_rx_ready; }

BOOL CerfNdisPacketRxIndicate(const UCHAR* frame, ULONG len) {
    const CerfNdisApi* api = CerfNdisResolve();
    PFN_PacketIndicate indicate;
    NDIS_STATUS st = CERF_NDIS_STATUS_FAILURE;
    void* buf = 0;
    void* one[1];
    int i;

    if (!s_rx_ready || !api || !frame) return FALSE;
    if (len == 0 || len > CERF_MP_RX_SLOT_BYTES) {
        CERF_LOG_X("ndis: packet rx rejected, len out of range", len);
        return FALSE;
    }

    indicate = CerfRxIndicateHandler();
    if (!indicate) {
        CERF_LOG("ndis: packet rx has no indicate handler on the block");
        return FALSE;
    }

    for (i = 0; i < CERF_MP_RX_SLOTS; ++i) {
        if (s_rx_slot[i].packet &&
            InterlockedExchange((LONG*)&s_rx_slot[i].busy, 1) == 0)
            break;
    }
    if (i == CERF_MP_RX_SLOTS) {
        CERF_LOG("ndis: packet rx table exhausted - all slots still held by NDIS");
        return FALSE;
    }

    memcpy(s_rx_slot[i].data, frame, len);
    api->AllocateBuffer(&st, &buf, s_buf_pool, s_rx_slot[i].data, len);
    if (st != CERF_NDIS_STATUS_SUCCESS || !buf) {
        CERF_LOG_X("ndis: packet rx buffer alloc failed", (ULONG)st);
        InterlockedExchange((LONG*)&s_rx_slot[i].busy, 0);
        return FALSE;
    }

    {
        UCHAR* p = (UCHAR*)s_rx_slot[i].packet;
        *(void**)(p + CERF_NDIS_PACKET_HEAD)      = buf;
        *(void**)(p + CERF_NDIS_PACKET_TAIL)      = buf;
        *(ULONG*)(p + CERF_NDIS_PACKET_TOTALLEN)  = len;
        *(ULONG*)(p + CERF_NDIS_PACKET_COUNT)     = 1;
        *(ULONG*)(p + CERF_NDIS_PACKET_PHYSCOUNT) = 0;
        *(p + CERF_NDIS_PACKET_VALIDCOUNTS)       = 1;
    }

    *(ULONG*)(CerfRxOob(s_rx_slot[i].packet) + CERF_NDIS_OOB_STATUS) = 0;
    one[0] = s_rx_slot[i].packet;
    indicate(s_rx_adapter, one, 1);

    {
        ULONG oob_status =
            *(ULONG*)(CerfRxOob(s_rx_slot[i].packet) + CERF_NDIS_OOB_STATUS);
        CERF_LOG_X_DEV("ndis: packet rx oob status", oob_status);
        if (oob_status == 0)
            CerfRxReclaimSlot(api, i);
    }
    return TRUE;
}

void CerfNdisPacketRxReturn(void* packet) {
    const CerfNdisApi* api = CerfNdisResolve();
    int i;

    if (!packet || !s_rx_ready) return;

    for (i = 0; i < CERF_MP_RX_SLOTS; ++i) {
        if (s_rx_slot[i].packet == packet) break;
    }
    if (i == CERF_MP_RX_SLOTS) return;

    CerfRxReclaimSlot(api, i);
}

void CerfNdisPacketRxShutdown(BOOL free_resources) {
    const CerfNdisApi* api = CerfNdisResolve();
    int i;
    int outstanding = 0;

    s_rx_ready = FALSE;
    s_rx_adapter = 0;
    if (!free_resources) {
        CERF_LOG("ndis: receive thread still live - packet pool retained");
        return;
    }

    for (i = 0; i < CERF_MP_RX_SLOTS; ++i) {
        if (s_rx_slot[i].busy) { ++outstanding; continue; }
        if (s_rx_slot[i].packet) {
            void* buf = *(void**)((UCHAR*)s_rx_slot[i].packet +
                                  CERF_NDIS_PACKET_HEAD);
            if (buf && api && api->FreeBuffer) api->FreeBuffer(buf);
            if (api && api->FreePacket) api->FreePacket(s_rx_slot[i].packet);
            s_rx_slot[i].packet = 0;
        }
        if (s_rx_slot[i].data) {
            LocalFree(s_rx_slot[i].data);
            s_rx_slot[i].data = 0;
        }
    }

    if (outstanding) {
        CERF_LOG_X("ndis: halt with packets still held by NDIS, pools retained",
                   (ULONG)outstanding);
        return;
    }

    if (api && api->FreePacketPool && s_pkt_pool) api->FreePacketPool(s_pkt_pool);
    if (api && api->FreeBufferPool && s_buf_pool) api->FreeBufferPool(s_buf_pool);
    s_pkt_pool = 0;
    s_buf_pool = 0;
}
