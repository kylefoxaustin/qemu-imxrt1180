/*
 * NETC switch FDB / NTMP command-BD test (Cortex-M33).
 *
 * Exercises the switch's L2 forwarding-database the way the fsl_netc_switch
 * driver does: over the NETC Table Management Protocol (NTMP) command BD ring.
 * A command is a 32-byte BD written into a ring in memory; ringing the producer
 * index CBDRPIR is the doorbell, and hardware signals completion by advancing
 * the consumer index CBDRCIR to match (there is NO busy bit -- CIR==PIR IS the
 * completion, per NETC_CmdBDSendCommand).  This test drives that ring directly
 * and checks a full FDB round-trip:
 *
 *   AddAndQuery  a {MAC, FID} -> portBitmap entry, get its hardware entry_id
 *   Query        the entry back by exact key -> the SAME portBitmap/FID
 *   Delete       the entry by its entry_id
 *   Query        again -> zero matches (it is gone)
 *
 * WHY A ROUND-TRIP, NOT A "COMMAND ACCEPTED" CHECK: acking the doorbell (advancing
 * CBDRCIR) with no table behind it would let the driver's spin exit and look like
 * success, while every query returned nothing -- a silent lie the driver cannot
 * see.  So we require the data we PUT IN to come BACK OUT, and to be GONE after a
 * delete: the model must actually store and retrieve the entry, not just ack.
 *
 * The BD ring and the request/response data buffer live in OCRAM: the M33 DTCM at
 * 0x20000000 is a per-core view no bus master (and so no NETC DMA) can see, but in
 * OCRAM the CPU and DMA addresses are identical.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* SW0 (switch) @ 0x60A0_0000; command BD ring 0 registers at +0x800. */
#define SW0_BASE   0x60A00000u
#define CBDRMR   (*(volatile uint32_t *)(SW0_BASE + 0x800))
#define CBDRBAR0 (*(volatile uint32_t *)(SW0_BASE + 0x810))
#define CBDRBAR1 (*(volatile uint32_t *)(SW0_BASE + 0x814))
#define CBDRPIR  (*(volatile uint32_t *)(SW0_BASE + 0x818))
#define CBDRCIR  (*(volatile uint32_t *)(SW0_BASE + 0x81C))
#define CBDRLENR (*(volatile uint32_t *)(SW0_BASE + 0x820))
#define CBDRMR_EN 0x80000000u

/* DMA-visible buffers in OCRAM1 (0x2048_4000): ring 128B-aligned, data buf 16B. */
#define RING_BASE 0x20484000u          /* 8 BDs x 32 = 256 bytes */
#define DBUF_BASE 0x20484100u          /* request/response data buffer (48 bytes) */
#define RING_LEN  8u

/* NTMP table IDs / commands / access modes. */
#define TB_FDB       15u
#define TB_VF        18u               /* VLAN filter table */
#define CMD_DELETE   0x1u
#define CMD_QUERY    0x4u
#define CMD_ADDQUERY 0xCu              /* Add | Query */
#define ACC_ENTRYID  0u
#define ACC_EXACTKEY 1u

/* Set to 0 if any command's doorbell/completion (CBDRCIR advancing to the
 * producer index) ever fails to arrive -- the driver would hang forever there. */
static int g_complete_ok = 1;

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

void reset_handler(void);

__attribute__((section(".vectors"), used))
void (* const vt[])(void) = {
    [0] = (void (*)(void))STACK_TOP,
    [1] = reset_handler,
};

static volatile uint32_t *bd_of(unsigned idx)
{
    return (volatile uint32_t *)(RING_BASE + idx * 32u);
}

/* Fill the request buffer with an FDB exact-key {mac, fid} + a cfge portBitmap. */
static void fill_key(const uint8_t mac[6], uint32_t fid, uint32_t port_bitmap)
{
    volatile uint8_t *b = (volatile uint8_t *)DBUF_BASE;
    for (int i = 0; i < 48; i++) {
        b[i] = 0;
    }
    for (int i = 0; i < 6; i++) {
        b[4 + i] = mac[i];                 /* keye.macAddr @ +4 */
    }
    *(volatile uint32_t *)(DBUF_BASE + 12) = fid & 0xFFF;    /* keye.fid @ +12 */
    *(volatile uint32_t *)(DBUF_BASE + 36) = port_bitmap;   /* cfge.portBitmap @ +36 */
}

/* Fill the request buffer with a VLAN-filter exact-key {vid} + cfge {membership, fid}. */
static void fill_vlan(uint32_t vid, uint32_t membership, uint32_t fid)
{
    volatile uint8_t *b = (volatile uint8_t *)DBUF_BASE;
    for (int i = 0; i < 48; i++) {
        b[i] = 0;
    }
    *(volatile uint32_t *)(DBUF_BASE + 4)  = vid & 0xFFF;         /* keye.vid @ +4 */
    *(volatile uint32_t *)(DBUF_BASE + 8)  = membership & 0xFFFFFF; /* cfge.portMembership @ +8 */
    *(volatile uint32_t *)(DBUF_BASE + 12) = fid & 0xFFF;         /* cfge.fid @ +12 */
}

/* Submit one command BD at ring slot `idx`, ring the doorbell, wait for the model
 * to advance CBDRCIR, and return resp.error; *nmatch gets resp.numMatched. */
static uint32_t submit(unsigned idx, unsigned reqLen, unsigned resLen,
                       unsigned cmd, unsigned acc, unsigned tableId, uint32_t *nmatch)
{
    volatile uint32_t *bd = bd_of(idx);
    bd[0] = DBUF_BASE;                              /* req.addr low  */
    bd[1] = 0;                                      /* req.addr high */
    bd[2] = (resLen & 0xFFFFFu) | (reqLen << 20);   /* resLength | reqLength */
    bd[3] = (cmd & 0xF) | ((acc & 0x3) << 12) | ((tableId & 0xFF) << 16);
    bd[4] = bd[5] = bd[6] = bd[7] = 0;

    uint32_t pir = (idx + 1) % RING_LEN;            /* producer index, wrapped */
    CBDRPIR = pir;                                  /* doorbell */

    uint32_t guard = 0;
    while ((CBDRCIR & 0x3FFu) != pir) {             /* completion = CIR==PIR */
        if (++guard > 2000000u) { g_complete_ok = 0; break; }
    }
    uint32_t resp = bd[3];                          /* resp dword@12 (HW wrote it) */
    if (nmatch) { *nmatch = resp & 0xFFFFu; }
    return (resp >> 16) & 0xFFFu;                   /* resp.error */
}

void reset_handler(void)
{
    int ok = 1;
    static const uint8_t mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    const uint32_t fid = 5, portmap = 0x6;         /* forward to ports 1 and 2 */
    uint32_t nmatch, err, entry_id;

    /* Bring up command BD ring 0: base, length, enable. */
    CBDRBAR0 = RING_BASE;
    CBDRBAR1 = 0;
    CBDRLENR = RING_LEN;
    CBDRMR   = CBDRMR_EN;

    /* 1) Add the entry and query it back in one command (cmd = Add|Query). */
    fill_key(mac, fid, portmap);
    err = submit(0, 48, 36, CMD_ADDQUERY, ACC_EXACTKEY, TB_FDB, &nmatch);
    entry_id = *(volatile uint32_t *)(DBUF_BASE + 4);   /* rsp.entryID @ +4 */
    if (err != 0 || nmatch != 1) { ok = 0; }

    /* 2) Query by exact key -> the config we put in must come back out. */
    fill_key(mac, fid, 0);                         /* portBitmap 0: prove it is READ */
    err = submit(1, 48, 36, CMD_QUERY, ACC_EXACTKEY, TB_FDB, &nmatch);
    uint32_t got_port = *(volatile uint32_t *)(DBUF_BASE + 20);  /* rsp.cfge.portBitmap */
    uint32_t got_fid  = *(volatile uint32_t *)(DBUF_BASE + 16);  /* rsp.keye.fid       */
    uint32_t got_eid  = *(volatile uint32_t *)(DBUF_BASE + 4);   /* rsp.entryID        */
    if (err != 0 || nmatch != 1) { ok = 0; }
    if (got_port != portmap) { ok = 0; }
    if (got_fid != fid) { ok = 0; }
    if (got_eid != entry_id) { ok = 0; }

    /* 3) Delete by the hardware entry_id. */
    for (int i = 0; i < 48; i++) { ((volatile uint8_t *)DBUF_BASE)[i] = 0; }
    *(volatile uint32_t *)(DBUF_BASE + 4) = entry_id;           /* req.entryID @ +4 */
    err = submit(2, 48, 36, CMD_DELETE, ACC_ENTRYID, TB_FDB, &nmatch);
    if (err != 0 || nmatch != 1) { ok = 0; }

    /* 4) Query again -> the entry is gone (zero matches). */
    fill_key(mac, fid, 0);
    err = submit(3, 48, 36, CMD_QUERY, ACC_EXACTKEY, TB_FDB, &nmatch);
    if (nmatch != 0) { ok = 0; }                   /* still present -> delete failed */

    /* ---- VLAN filter table (tableId 18): the same NTMP round-trip ---- */
    const uint32_t vid = 100, membership = 0x7, vfid = 42;   /* VID 100 -> FID 42 */
    uint32_t ventry;

    /* 5) Add a VLAN entry {vid -> portMembership, fid} and query it back. */
    fill_vlan(vid, membership, vfid);
    err = submit(4, 24, 28, CMD_ADDQUERY, ACC_EXACTKEY, TB_VF, &nmatch);
    ventry = *(volatile uint32_t *)(DBUF_BASE + 4);   /* rsp.entryID @ +4 */
    if (err != 0 || nmatch != 1) { ok = 0; }

    /* 6) Query by VID -> membership and FID must come back out. */
    fill_vlan(vid, 0, 0);                          /* zero them: prove they are READ */
    err = submit(5, 24, 28, CMD_QUERY, ACC_EXACTKEY, TB_VF, &nmatch);
    uint32_t got_mem  = *(volatile uint32_t *)(DBUF_BASE + 12) & 0xFFFFFF; /* rsp.cfge.portMembership */
    uint32_t got_vfid = *(volatile uint32_t *)(DBUF_BASE + 16) & 0xFFF;    /* rsp.cfge.fid          */
    uint32_t got_vid  = *(volatile uint32_t *)(DBUF_BASE + 8) & 0xFFF;     /* rsp.keye.vid          */
    if (err != 0 || nmatch != 1) { ok = 0; }
    if (got_mem != membership) { ok = 0; }
    if (got_vfid != vfid) { ok = 0; }
    if (got_vid != vid) { ok = 0; }

    /* 7) Delete the VLAN entry by its entry_id, then query -> zero matches. */
    for (int i = 0; i < 48; i++) { ((volatile uint8_t *)DBUF_BASE)[i] = 0; }
    *(volatile uint32_t *)(DBUF_BASE + 4) = ventry;
    err = submit(6, 24, 28, CMD_DELETE, ACC_ENTRYID, TB_VF, &nmatch);
    if (err != 0 || nmatch != 1) { ok = 0; }
    fill_vlan(vid, 0, 0);
    err = submit(7, 24, 28, CMD_QUERY, ACC_EXACTKEY, TB_VF, &nmatch);
    if (nmatch != 0) { ok = 0; }

    if (!g_complete_ok) { ok = 0; }                /* a doorbell never completed */

    if (ok) {
        puts_("NETC-FDB: PASS - FDB add/query/delete round-trip over the NTMP command BD ring\r\n");
        puts_("NETC-FDB: PASS - queried portBitmap/FID/entry_id match what was programmed\r\n");
        puts_("NETC-FDB: PASS - entry is gone after delete (zero matches)\r\n");
        puts_("NETC-VF:  PASS - VLAN filter add/query/delete round-trip (VID->membership/FID)\r\n");
    } else {
        puts_("NETC-FDB: FAIL - FDB/VLAN round-trip mismatch (see which assert)\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
