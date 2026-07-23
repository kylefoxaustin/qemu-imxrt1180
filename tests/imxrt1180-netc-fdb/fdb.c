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
#define TXRING_BASE  0x20484200u       /* ENETC SI0 TX BD ring (8 x 16 = 128B)   */
#define TXFRAME_BASE 0x20484300u       /* a frame to inject through the switch    */

/* ENETC0 SI0 TX ring registers (@ 0x60B0_8010..) + the switch ingress path. */
#define TB_BAR0 (*(volatile uint32_t *)(0x60B08010u))
#define TB_BAR1 (*(volatile uint32_t *)(0x60B08014u))
#define TB_PIR  (*(volatile uint32_t *)(0x60B08018u))   /* producer index = TX "go" */
#define TB_CIR  (*(volatile uint32_t *)(0x60B0801Cu))
#define TB_LENR (*(volatile uint32_t *)(0x60B08020u))
#define MSIX0_CTRL (*(volatile uint32_t *)(0x60BF000Cu)) /* TX MSI-X vector 0 control */

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

/* Fill the request buffer with an FDB search-by-criteria for a given portBitmap
 * (netc_tb_fdb_search_criteria_t: resumeEntryId@4, cfge.portBitmap@20, cfgeMc@34). */
#define ACC_SEARCH   2u
#define CFGEMC_PORTBMP 2u
static void fill_search(uint32_t port_bitmap)
{
    volatile uint8_t *b = (volatile uint8_t *)DBUF_BASE;
    for (int i = 0; i < 48; i++) {
        b[i] = 0;
    }
    *(volatile uint32_t *)(DBUF_BASE + 4)  = 0xFFFFFFFFu;      /* resumeEntryId = start */
    *(volatile uint32_t *)(DBUF_BASE + 20) = port_bitmap;      /* cfge.portBitmap       */
    ((volatile uint8_t *)DBUF_BASE)[34]    = CFGEMC_PORTBMP;   /* cfgeMc = MatchPortBitmap */
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

    /*
     * ---- Source-MAC learning ----
     * Inject a frame through the switch (via the ENETC SI TX ring) with a known
     * source MAC.  The switch must LEARN it: a dynamic FDB entry mapping that MAC
     * to the CPU/management port (bit 4).  We then read that entry back over NTMP
     * -- proving the switch populated its own database from live traffic, not just
     * from driver-programmed static entries.
     */
    static const uint8_t src[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    volatile uint8_t *fr = (volatile uint8_t *)TXFRAME_BASE;
    for (int i = 0; i < 6; i++)  { fr[i] = 0xFF; }        /* dest = broadcast */
    for (int i = 0; i < 6; i++)  { fr[6 + i] = src[i]; }  /* source MAC       */
    fr[12] = 0x88; fr[13] = 0xB6;                         /* ethertype        */
    for (int i = 14; i < 64; i++) { fr[i] = 0x5A; }

    volatile uint32_t *txbd = (volatile uint32_t *)TXRING_BASE;
    txbd[0] = TXFRAME_BASE;                               /* BD.addr    */
    txbd[1] = 0;
    *(volatile uint16_t *)(TXRING_BASE + 10) = 64;        /* BD.frameLen @ +10 */

    MSIX0_CTRL = 1;                                       /* mask TX MSI-X (no completion DMA) */
    TB_BAR0 = TXRING_BASE;
    TB_BAR1 = 0;
    TB_CIR  = 0;
    TB_LENR = 8;
    TB_PIR  = 1;                                          /* GO: switch ingress learns src */

    fill_key(src, 0, 0);                                  /* query FDB by {src, fid 0} */
    /* The 8 FDB/VLAN commands wrapped the consumer index back to 0, so the next
     * command reuses ring slot 0 (CBDRCIR == 0). */
    err = submit(0, 48, 36, CMD_QUERY, ACC_EXACTKEY, TB_FDB, &nmatch);
    uint32_t learned_port = *(volatile uint32_t *)(DBUF_BASE + 20);  /* rsp.cfge.portBitmap */
    uint32_t learned_cfge = *(volatile uint32_t *)(DBUF_BASE + 24);  /* rsp.cfge flags      */
    int learn_ok = 1;
    if (nmatch != 1) { learn_ok = 0; }                   /* not learned at all */
    if (learned_port != (1u << 4)) { learn_ok = 0; }     /* wrong port (must be CPU port 4) */
    if (((learned_cfge >> 11) & 1) == 0) { learn_ok = 0; } /* not marked dynamic */
    if (!learn_ok) { ok = 0; }

    /*
     * ---- FDB search by criteria (SWT_BridgeSearchFDBTableEntry) ----
     * Add an entry with a distinctive portBitmap, then SEARCH the FDB for that
     * portBitmap and confirm the search returns that entry (MAC + entry_id).
     * (Ring slot 0 was used by the learning query above -> CBDRCIR wrapped to 1,
     * so we continue at slots 1, 2.)
     */
    static const uint8_t W[6] = { 0x02, 0x77, 0x77, 0x77, 0x77, 0x77 };
    const uint32_t wport = 0x5;                    /* ports 0 and 2 */
    fill_key(W, 7, wport);                          /* fid 7 -> W on portBitmap 0x5 */
    err = submit(1, 48, 36, CMD_ADDQUERY, ACC_EXACTKEY, TB_FDB, &nmatch);
    uint32_t wentry = *(volatile uint32_t *)(DBUF_BASE + 4);
    if (err != 0 || nmatch != 1) { ok = 0; }

    fill_search(wport);
    err = submit(2, 48, 36, CMD_QUERY, ACC_SEARCH, TB_FDB, &nmatch);
    uint32_t s_entry = *(volatile uint32_t *)(DBUF_BASE + 4);   /* rsp.entryID  */
    uint32_t s_port  = *(volatile uint32_t *)(DBUF_BASE + 20) & 0xFFFFFF; /* rsp.cfge.portBitmap */
    volatile uint8_t *sm = (volatile uint8_t *)(DBUF_BASE + 8); /* rsp.keye.macAddr */
    int search_ok = (nmatch == 1) && (s_entry == wentry) && (s_port == wport) &&
                    (sm[0] == W[0]) && (sm[5] == W[5]);
    if (!search_ok) { ok = 0; }

    if (!g_complete_ok) { ok = 0; }                /* a doorbell never completed */

    if (ok) {
        puts_("NETC-FDB: PASS - FDB add/query/delete round-trip over the NTMP command BD ring\r\n");
        puts_("NETC-FDB: PASS - queried portBitmap/FID/entry_id match what was programmed\r\n");
        puts_("NETC-FDB: PASS - entry is gone after delete (zero matches)\r\n");
        puts_("NETC-VF:  PASS - VLAN filter add/query/delete round-trip (VID->membership/FID)\r\n");
        puts_("NETC-LRN: PASS - switch learned an injected frame's src MAC (dynamic FDB, CPU port)\r\n");
        puts_("NETC-SRCH: PASS - FDB search-by-portBitmap returns the matching entry\r\n");
    } else {
        puts_("NETC-FDB: FAIL - FDB/VLAN/learning mismatch (see which assert)\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
