/* The colour tables of the ISP replaced with prepared ones: an OM5M2 ISPB block
 * (OMKit/docs/formats/isp.md, section 5) goes into the colour structure S of every development job at the entry of the
 * register writer, before S is copied to the registers and the tables to the pipe's RAM.
 *
 * H2 0x6037c519 - the writer's entry (a0 register base, a1 = S, a3 = ctx, d3 the job's register mask): census; with an
 *    override on, the S fields; at a new slow generation for this job's persistent S (ext, ctx+56) ctx+108 |= 3 - the
 *    loader reloads the gamma and the sector tables.
 * H1 0x603e8fba - the colour builder's entry (a0 = ctx): at a new slow generation for this ext ctx+60 = -1, a full rebuild
 *    from the stock values first (the builders keep unchanged parts, so our values would stay otherwise).
 * Both run on any CPU in the firmware's job task, under the pipe's hardware claim: they touch only the arena block below
 * (through its uncached address), the job's own S/ctx/B (cached, on the same CPU) and the stack. The core task prepares
 * everything. Two classes of change:
 *  - fast: the matrix, Cb/Cr, the sector records and mask, the tone - what the writer copies to the registers every job
 *    and the tone, which the firmware's loader 0x6037ccd2 takes on every call. The descriptor H2 copies is a two-copy
 *    latch with a sequence counter (the hooks copy the stable copy and check the counter, 3 tries at most; the core task
 *    never waits); the tone goes through a ring of TONE_RING tables, a new one at most every FAST_MIN_US, so a table is
 *    rewritten (TONE_RING - 1) x FAST_MIN_US after it was replaced - far past the reader's window, H2 to the end of the
 *    loader in the same dispatcher call 0x6037bd59 (well under 2 ms). No reload and no rebuild: the next job has it.
 *  - slow: the gamma and the sector weight tables (G1..G5), which the loader reads only when told (ctx+108): two table
 *    sets, a new one at most every MIN_CHANGE_US; the slow generation tells the hooks to reload and rebuild.
 * The grain mask (core 1.21): the look's grain on the monochrome profile's list (graindrv.c) has a mask development, stage
 * 0x900, whose tone curve the firmware replaces for key 12 only (the tone selector 0x603e80d2: ROM 0x60545fde). For the
 * jobs of the grain's key at that stage the writer puts our mask table at S+2608 instead (the stock never gives key 2 the
 * stage 0x900), and puts the S's tone back on its next pass if nothing else replaced our table there.
 * Nothing is ever freed: an ext may keep our pointers until its next build. The matrix: the block's F multiplies the camera's own matrix of the frame, S+80 = F * S+80 (Q7, rows sum to 128) -
 * the camera keeps its white balance and its matrix for the light, the look adds its own. */
#include "omk_drv.h"
#include "omk_core.h"
#include "omk_log.h"
#include "omk_mem.h"

#define H2_SITE 0x6037c519
#define H1_SITE 0x603e8fba
#define CPUS 3
#define TRACK 16                            /* persistent S of jobs remembered per CPU */
#define RING 32                             /* census entries per CPU */
#define MRING 8                             /* still matrices remembered per CPU */
#define MIN_CHANGE_US 1000000               /* one slow change a second at most (see above) */
#define FAST_MIN_US 40000                   /* a fast change every 40 ms at most: a tone table is reused 120 ms later */
#define TONE_RING 4
#define MASK_RING 4                         /* grain mask tables, a new one at most every FAST_MIN_US (as the tones) */
#define MASK_KEEP 4                         /* S whose tone the mask replaced, per CPU */
#define MASK_STAGE 0x900
#define TONE_LINEAR 0x6053fd42              /* ROM: round(i * 1023 / 255), the tone of code 2000..2003 - the fallback */

/* ISPB OM5M2 layout 1 */
#define ISPB_SIZE 5064
#define P_SAT 8
#define P_GAMMA 2
#define P_TONE 4
#define P_SECTORS 16
#define P_MATRIX 1
#define P_ALL (P_MATRIX | P_SAT | P_GAMMA | P_TONE | P_SECTORS)
#define FULL_TURN 32767                     /* the widest hue window: the whole circle (15-bit field) */

/* The colour structure S and the job */
#define S_FLAGS 4                           /* u32: bit 0x100 - the tone curve off */
#define S_MATRIX 80                         /* s16[9] Q7, rows sum to 128: the camera's matrix of this frame (AWB) */
#define S_CB 124
#define S_CR 126
#define S_RECORDS 688                       /* 12 x 24 bytes */
#define S_MASK 976                          /* u32 */
#define S_GAMMA 2596                        /* 3 pointers R, G, B */
#define S_TONE 2608
#define S_LUT 3124                          /* 10 pointers, see LUT_SLOT */
#define CTX_B 4
#define B_STAGE 12                          /* u16, set by the steps' callbacks (0x603903c9: 0x900) */
#define CTX_CODE 52
#define CTX_EXT 56
#define CTX_CHANGED 60
#define CTX_RELOAD 108
#define B_TYPE 16                           /* u8 (movbu (16,a1) at 0x6037a835, 0x603e2d75) */
#define B_PIPE 17                           /* u8: 1 - pipe A, 2 - pipe B, set by the step (0x603965c5, 0x60395582) */
#define B_SUB 28                            /* u16, the subtype in the high byte */
#define B_KIND_LIVE 0x01                    /* the low byte of B+28 in live view (0x07 in a still, T0 census) */
#define SCOPE_LIVE 1
#define SCOPE_STILLS 2
#define B_LOG 1808                          /* u8: not 0 - a log gamma */
#define B_HLG 1809                          /* u8: not 0 - HLG */

typedef struct {
    u16 gamma[3][256];
    u32 g1[2][128], g2[2][128];             /* lo/hi halves: g1 H(chroma)/H(hue), g2 C(luma)/C(chroma) */
    u16 g3[2][128], g4[2][128], g5[2][128]; /* C(hue), Y, H(luma) */
} tables_t;

typedef struct {
    u32 present;
    s16 mat[9];                             /* F, Q7, rows sum to 128 */
    u16 cb, cr;
    u32 mask;
    u8 rec[12 * 24];
    u32 gamma[3], tone, lut[10];
    u32 sgen;                               /* the slow generation of the gamma and lut tables */
} desc_t;

typedef struct {
    u32 ext, gen;
    u32 mat;                                /* 1: ours holds what we wrote into this ext's S+80, orig the camera's */
    s16 orig[9], ours[9];
} track_t;

typedef struct {
    u32 ticks, base, type, code, ext, changed, mask, pad;
} census_t;

typedef struct {
    u32 ticks, type, code;
    s16 m[9];
    u16 cb, cr;
    u32 gamma[3], tone, flags;              /* the stock curves' addresses (ROM tables) and S+4 */
    u32 stage;
} smat_t;

typedef struct {
    u32 S, tone, flags;                     /* S 0 - free */
} mkeep_t;

typedef struct {
    u32 gen;                                /* changes published; 0 - never overridden: the hooks do nothing but the census */
    u32 active;                             /* override on */
    u32 sgen;                               /* slow generation: the hooks reload and rebuild when it changes */
    u32 seq;                                /* the latch: odd - desc[1] is the stable copy, even - desc[0] */
    u32 census;
    u32 scope;                              /* bit 0 live view jobs, bit 1 still jobs (the rest of the development jobs) */
    u32 h2[CPUS], h1[CPUS], patched[CPUS], forced[CPUS], rebuilt[CPUS], ring_n[CPUS], torn[CPUS];
    desc_t desc[2];
    track_t track[CPUS][TRACK];
    u32 track_next[CPUS];
    census_t ring[CPUS][RING];
    u32 mat_n[CPUS];
    smat_t mat[CPUS][MRING];                /* the camera's own matrix and Cb/Cr gains of still jobs */
    u32 mask, mask_key;                     /* the grain mask table (0 - off) and the key of its jobs */
    u32 masked[CPUS], restored[CPUS], lost[CPUS];
    mkeep_t keep[CPUS][MASK_KEEP];
    u32 keep_next[CPUS];
    u32 evicted[CPUS];                      /* track(): a remembered S gave its place to a new one (all TRACK in use) */
} isp_t;

/* Only through slot(): the uncached alias; word 1 - the arena block (isp_t), word 2 - the tables (tables_t[2]), word 3 -
 * the tone ring (u16[TONE_RING][256]), word 4 - the mask ring (u16[MASK_RING][256]) */
static u32 isp_slot[16] __attribute__((aligned(64), section(".data.cpu_shared")));
static int installed;
static u32 last_slow, last_fast, slow_crc, slow_present, tables_cur, tone_next, fast_n, slow_n;
static desc_t now_desc;                     /* core task: the published descriptor, the base of the next change */
static u16 now_tone[256];
static u32 tone_seq, mask_next, last_mask;    /* core task */

static volatile u32 *slot(void)
{
    return (volatile u32 *)((u32)isp_slot + DRV_UNCACHED);
}

static volatile isp_t *isp(void)
{
    return (volatile isp_t *)slot()[1];
}

void drv_tramp_isp_w(void);                 /* thunks.s */
void drv_tramp_isp_b(void);

static volatile track_t *track(volatile isp_t *s, u32 cpu, u32 ext, int add)
{
    u32 i;

    for (i = 0; i < TRACK; i++)
        if (s->track[cpu][i].ext == ext)
            return &s->track[cpu][i];
    if (!add)
        return 0;
    i = s->track_next[cpu]++ % TRACK;       /* full: the oldest goes; it is forced once more if it comes back, harmless */
    if (s->track[cpu][i].ext)
        s->evicted[cpu]++;                  /* counted for the log: more than TRACK persistent S on this CPU */
    s->track[cpu][i].ext = ext;
    s->track[cpu][i].gen = 0;
    s->track[cpu][i].mat = 0;
    return &s->track[cpu][i];
}

/* S+80 = F * the camera's matrix, Q7, the rows brought back to 128 on the diagonal (as the firmware's 0x603e4f0c).
 * A persistent S (ext) keeps what we wrote: when its S+80 is still ours, the camera's matrix is the one remembered -
 * otherwise F would be applied again on every job */
static void put_matrix(volatile isp_t *s, u32 cpu, u32 ext, volatile desc_t *d, u8 *S)
{
    s16 *m = (s16 *)(S + S_MATRIX);
    volatile track_t *t = ext ? track(s, cpu, ext, 1) : 0;
    s16 base[9];
    int r, c, k, acc, sum;
    u32 i, same = t && t->mat;

    for (i = 0; same && i < 9; i++)
        same = m[i] == t->ours[i];
    for (i = 0; i < 9; i++)
        base[i] = same ? t->orig[i] : m[i];
    for (r = 0; r < 3; r++) {
        sum = 0;
        for (c = 0; c < 3; c++) {
            acc = 0;
            for (k = 0; k < 3; k++)
                acc += d->mat[r * 3 + k] * base[k * 3 + c];
            acc = acc >= 0 ? (acc + 64) >> 7 : -((-acc + 64) >> 7);
            m[r * 3 + c] = (s16)acc;
            sum += acc;
        }
        m[r * 3 + r] += (s16)(128 - sum);
    }
    if (t) {
        for (i = 0; i < 9; i++) {
            t->orig[i] = base[i];
            t->ours[i] = m[i];
        }
        t->mat = 1;
    }
}

static u32 cpu_now(void)
{
    u32 c = drv_cpu();

    return c < CPUS ? c : 0;
}

#define CHANGED_ALL 0xe3ffffffu               /* the stock "everything changed" of a new ext (bits 26..28 are other things) */

/* Does this job get our tables: a development job of a picture mode (Natural is code 2; ART and scene codes are
 * 0x01vv00nn / 0x0200nnnn, 2000..2003 have branches of their own), no log gamma (the log branch 0x603e2e67 takes
 * B+1808 1..3), no HLG, and in the scope. type: B+16 | B+17 << 8 | B+28 << 16 */
static int eligible(volatile isp_t *s, const u8 *B, u32 type, u32 code)
{
    return s->active && B && ((type & 0xff) == 0 || (type & 0xff) == 16) && code < 100 && !B[B_LOG] && !B[B_HLG] &&
           (s->scope & ((type >> 16 & 0xff) == B_KIND_LIVE ? SCOPE_LIVE : SCOPE_STILLS)) != 0;
}

static u32 job_type(const u8 *B)
{
    return B ? (u32)B[B_TYPE] | (u32)B[B_PIPE] << 8 | (u32)*(const u16 *)(B + B_SUB) << 16 : 0xffffffffu;
}

/* Is this tone pointer one of our grain mask tables */
static int is_mask(u32 p)
{
    u32 ring = slot()[4];

    return ring && p >= ring && p < ring + MASK_RING * 512;
}

/* The grain mask at the writer's entry (after the look's tables): the mask pass of the grain's jobs gets our table as its
 * tone, the S's own tone kept; on another pass of an S that still holds our table, that tone goes back (the firmware's
 * builder rebuilds S+2608 only when its change mask says so) */
static void grain_mask(volatile isp_t *s, u32 cpu, u8 *S, const u8 *B, u32 code)
{
    volatile mkeep_t *k;
    u32 tab = s->mask, cur = *(u32 *)(S + S_TONE), i;

    if (tab && B && code == s->mask_key && B[B_TYPE] == 0 && *(const u16 *)(B + B_STAGE) == MASK_STAGE) {
        if (!is_mask(cur)) {
            for (i = 0; i < MASK_KEEP && s->keep[cpu][i].S != (u32)S; i++)
                ;
            k = &s->keep[cpu][i < MASK_KEEP ? i : s->keep_next[cpu]++ % MASK_KEEP];
            k->S = (u32)S;
            k->tone = cur;
            k->flags = *(u32 *)(S + S_FLAGS) & 0x100;
        }
        *(u32 *)(S + S_TONE) = tab;
        *(u32 *)(S + S_FLAGS) &= ~0x100u;
        s->masked[cpu]++;
        return;
    }
    if (!is_mask(cur))
        return;
    for (i = 0; i < MASK_KEEP && s->keep[cpu][i].S != (u32)S; i++)
        ;
    if (i < MASK_KEEP) {
        k = &s->keep[cpu][i];
        *(u32 *)(S + S_TONE) = k->tone;
        *(u32 *)(S + S_FLAGS) = (*(u32 *)(S + S_FLAGS) & ~0x100u) | k->flags;
        k->S = 0;
        s->restored[cpu]++;
    } else {
        *(u32 *)(S + S_TONE) = TONE_LINEAR;   /* never expected: the keep ring is per CPU and holds 4 */
        *(u32 *)(S + S_FLAGS) &= ~0x100u;
        s->lost[cpu]++;
    }
}

/* H2 (thunks.s): the writer's entry */
void drv_isp_writer(u32 base, u8 *S, u8 *ctx, u32 mask)
{
    volatile isp_t *s = isp();
    volatile track_t *e;
    volatile desc_t *d;
    u8 *B;
    u32 cpu, ext, gen, sgen, seq, type, code, i, n, want, did = 0;
    s16 mat0[9];

    if (!s || !S || !ctx)
        return;
    cpu = cpu_now();
    s->h2[cpu]++;
    B = *(u8 **)(ctx + CTX_B);
    ext = *(u32 *)(ctx + CTX_EXT);
    code = *(u32 *)(ctx + CTX_CODE);
    type = job_type(B);
#ifdef OMK_DEBUG                            /* the census and the stills' own matrices, for the log */
    if (s->census) {
        volatile census_t *c = &s->ring[cpu][s->ring_n[cpu]++ % RING];

        c->ticks = drv_ticks();
        c->base = base;
        c->type = type;
        c->code = code;
        c->ext = ext;
        c->changed = *(u32 *)(ctx + CTX_CHANGED);
        c->mask = mask;
    }
    /* The camera's matrix of a still development job, as the builder made it - core never writes it, so this is the
     * input the replaced tables get */
    if (B && ((type & 0xff) == 0 || (type & 0xff) == 16) && code < 100 && (type >> 16 & 0xff) != B_KIND_LIVE) {
        volatile smat_t *m = &s->mat[cpu][s->mat_n[cpu]++ % MRING];

        m->ticks = drv_ticks();
        m->type = type;
        m->code = code;
        for (i = 0; i < 9; i++)
            m->m[i] = ((const s16 *)(S + S_MATRIX))[i];
        m->cb = *(u16 *)(S + S_CB);
        m->cr = *(u16 *)(S + S_CR);
        for (i = 0; i < 3; i++)
            m->gamma[i] = ((u32 *)(S + S_GAMMA))[i];
        m->tone = *(u32 *)(S + S_TONE);
        m->flags = *(u32 *)(S + S_FLAGS);
        m->stage = *(const u16 *)(B + B_STAGE);
    }
#else
    (void)base;
    (void)mask;
#endif
    gen = s->gen;
    sgen = s->sgen;
    if (gen && eligible(s, B, type, code)) {
        /* The latch: the stable copy, then the counter again - the core task may have moved on meanwhile (3 tries; a
         * job that still saw it move keeps the last copy, the next job takes the new one). The matrix is F x the job's
         * own: a try again starts from the job's, not from F x it - for S from the pool (ext 0), which has no track to
         * tell; with a track put_matrix sees its own result and takes the original */
        for (i = 0; i < 9; i++)
            mat0[i] = ((const s16 *)(S + S_MATRIX))[i];
        for (n = 0; n < 3; n++) {
            seq = s->seq;
            d = &s->desc[seq & 1];
            if (n && !ext)
                for (i = 0; i < 9; i++)
                    ((s16 *)(S + S_MATRIX))[i] = mat0[i];
            if (d->present & P_MATRIX)
                put_matrix(s, cpu, ext, d, S);
            if (d->present & P_SAT) {
                *(u16 *)(S + S_CB) = d->cb;
                *(u16 *)(S + S_CR) = d->cr;
            }
            if (d->present & P_SECTORS) {
                for (i = 0; i < sizeof(d->rec) / 4; i++)
                    ((u32 *)(S + S_RECORDS))[i] = ((volatile u32 *)d->rec)[i];
                *(u32 *)(S + S_MASK) = d->mask;
                for (i = 0; i < 10; i++)
                    ((u32 *)(S + S_LUT))[i] = d->lut[i];
            }
            if (d->present & P_GAMMA)
                for (i = 0; i < 3; i++)
                    ((u32 *)(S + S_GAMMA))[i] = d->gamma[i];
            if (d->present & P_TONE) {
                *(u32 *)(S + S_TONE) = d->tone;
                *(u32 *)(S + S_FLAGS) &= ~0x100u;
            }
            sgen = d->sgen;
            if (s->seq == seq)
                break;
        }
        if (n == 3)
            s->torn[cpu]++;
        s->patched[cpu]++;
        did = 1;
    }
    grain_mask(s, cpu, S, B, code);         /* after the look's tone: at the mask pass the mask wins */
    if (!gen || !ext)
        return;                             /* a pool S: rebuilt and fully reloaded every job anyway */
    /* The pipe reloads the gamma and the sector tables only when they change: when this ext gets our tables of a new
     * slow generation, or goes back to stock ones. A fast change needs no reload: the writer and the loader take it
     * anyway. An ext we never touched is left alone, whatever the generation (in 0.90 live view was forced to reload on
     * every change although the scope was stills only) */
    e = track(s, cpu, ext, did);
    if (!e)
        return;
    want = sgen << 1 | did;
    if (e->gen != want && (did || (e->gen & 1))) {
        *(u32 *)(ctx + CTX_RELOAD) |= 3;
        s->forced[cpu]++;
    }
    e->gen = want;
}

/* H1 (thunks.s): the colour builder's entry */
void drv_isp_builder(u8 *ctx)
{
    volatile isp_t *s = isp();
    volatile track_t *e;
    u32 cpu, ext;
    u8 *B;

    if (!s || !ctx)
        return;
    cpu = cpu_now();
    s->h1[cpu]++;
    ext = *(u32 *)(ctx + CTX_EXT);
    if (!s->gen || !ext)
        return;
    /* Only an ext that holds our values and must lose some of them is rebuilt from the stock ones: it goes back to
     * stock, or a new slow generation may leave out parts the old one had. One we patch now needs nothing - the writer
     * puts our parts over whatever the builder made; one we never touched is not ours to rebuild */
    e = track(s, cpu, ext, 0);
    if (!e || !(e->gen & 1))
        return;
    B = *(u8 **)(ctx + CTX_B);
    if (e->gen >> 1 != s->sgen || !eligible(s, B, job_type(B), *(u32 *)(ctx + CTX_CODE))) {
        *(u32 *)(ctx + CTX_CHANGED) |= CHANGED_ALL;
        s->rebuilt[cpu]++;
    }
}

/* Core task: the hooks and the block. 0, or the step that failed */
int drv_isp_install(void)
{
    static const u8 h2_site[5] = {0xf1, 0xd5, 0xf1, 0xd3, 0x4e};   /* mov a1,d1; mov a0,d3; mov d3,(sp) (6th byte apart) */
    static const u8 h1_site[5] = {0xf1, 0xd3, 0xf1, 0xef, 0x53};   /* mov a0,d3; mov d3,a3; inc4 a3 */
    u8 *blk, *tab, *tone, *mask;
    int rc;

    if (installed)
        return 0;
    if (*(volatile u8 *)(H2_SITE + 5 + DRV_UNCACHED) != 0x00)
        return 1;
    blk = mem_alloc(sizeof(isp_t));
    tab = mem_alloc(2 * sizeof(tables_t) + 64);
    tone = mem_alloc(TONE_RING * 512 + 64);
    mask = mem_alloc(MASK_RING * 512 + 64);
    if (!blk || !tab || !tone || !mask)
        return 2;
    mem_set(blk, 0, sizeof(isp_t));
    ((isp_t *)blk)->scope = SCOPE_STILLS;   /* live view only on request: a stepped test curve there broke the autofocus */
    tab += -(u32)tab & 63;                  /* 64-byte lines for the loader's reads */
    mem_set(tab, 0, 2 * sizeof(tables_t));
    tone += -(u32)tone & 63;
    mem_set(tone, 0, TONE_RING * 512);
    mask += -(u32)mask & 63;
    mem_set(mask, 0, MASK_RING * 512);
    slot()[4] = (u32)mask;
    slot()[3] = (u32)tone;
    slot()[2] = (u32)tab;
    slot()[1] = (u32)blk;                   /* before any hook can run */
    rc = drv_code_jump(H1_SITE, h1_site, (void *)drv_tramp_isp_b);
    if (rc)
        return 10 + rc;
    rc = drv_code_jump(H2_SITE, h2_site, (void *)drv_tramp_isp_w);
    if (rc)
        return 20 + rc;
    installed = 1;
    return 0;
}

#ifdef OMK_DEBUG
void drv_isp_census(int on)
{
    volatile isp_t *s = isp();

    if (s)
        s->census = on != 0;
}
#endif

/* Which jobs get the tables: SCOPE_* bits; 0 - none. Read at every job (eligible), so it takes effect with the next job */
void drv_isp_scope(u32 scope)
{
    volatile isp_t *s = isp();

    if (s)
        s->scope = scope & (SCOPE_LIVE | SCOPE_STILLS);
}

static u16 get16(const u8 *p)
{
    return p[0] | p[1] << 8;
}

/* A profile node of the .isp block (s16 Q14, 16384 = 1.0) as the hardware holds it: 1/64 steps, -64..63 */
static int node64(const u8 *p)
{
    int v = ((s16)get16(p) + 128) >> 8;

    return v < -64 ? -64 : v > 63 ? 63 : v;
}

/* One profile word as the firmware builds them (0x60383ccf, 0x60384b60): the node value (7-bit signed, 64 = 1.0) in the
 * high byte, the slope to the next node in the low byte; the hardware interpolates (hi + lo * f) / 64 inside the node
 * (camera experiment S1). The last node slopes to 0 at the window's edge, as in the ROM tables -
 * or, for a hue profile over the whole circle, to node 0. pr: the 16 nodes */
static u16 word(const u8 *pr, u32 n, int wrap)
{
    int hi = node64(pr + 2 * n), next = n < 15 ? node64(pr + 2 * n + 2) : wrap ? node64(pr) : 0;

    return (u16)((hi & 0x7f) << 8 | ((next - hi) & 0xff));
}

/* The latch: the hooks copy the stable copy (the other one is being written), never both written at once */
static void latch_put(volatile isp_t *s, const desc_t *n)
{
    u32 i;

    s->seq++;                               /* odd: the hooks take desc[1] */
    for (i = 0; i < sizeof(desc_t) / 4; i++)
        ((volatile u32 *)&s->desc[0])[i] = ((const u32 *)n)[i];
    s->seq++;                               /* even: desc[0] */
    for (i = 0; i < sizeof(desc_t) / 4; i++)
        ((volatile u32 *)&s->desc[1])[i] = ((const u32 *)n)[i];
}

/* The block's checks: the sector windows inside the hardware fields, the matrix rows, the profile node values. 0 or -1 */
static int block_bad(const u8 *b, u32 present)
{
    u32 k, n;

    /* windows of the enabled sectors: h0 and hw 15 bit, the others 10 bit - a width of 1024 reads as 0, the window off
     * (camera experiments Y1-Y3) */
    for (k = 0; k < 12; k++) {
        const u8 *r = b + 2088 + 24 * k;

        if ((get16(b + 2080) >> k & 1) && (get16(r) > FULL_TURN || get16(r + 2) > FULL_TURN || get16(r + 4) > 1023 ||
                                           get16(r + 6) > 1023 || get16(r + 8) > 1023 || get16(r + 10) > 1023))
            return -1;
    }
    /* the look's matrix F: Q7, every row sums to 128 (white stays white) */
    if (present & P_MATRIX)
        for (k = 0; k < 3; k++)
            if ((s16)get16(b + 8 + 6 * k) + (s16)get16(b + 10 + 6 * k) + (s16)get16(b + 12 + 6 * k) != 128)
                return -1;
    /* weight profiles: node values s16 Q14 within -1.0..16383/16384 (the hardware holds 1/64 steps up to 63/64) */
    for (k = 0; k < 12 * 7 * 16; k++) {
        n = get16(b + 2376 + 2 * k);
        if (n >= 16384 && n < 49152)
            return -1;
    }
    return 0;
}

/* The slow tables of the block into the table set `set`: the gamma and the sector weight words */
static void slow_tables(const u8 *b, u32 set, desc_t *nd)
{
    volatile tables_t *t = (volatile tables_t *)(slot()[2] + set * sizeof(tables_t));
    u32 c, k, n, half, at;
    const u8 *pr;
    int wrap;

    for (c = 0; c < 3; c++)
        for (k = 0; k < 256; k++) {
            n = get16(b + 32 + 512 * c + 2 * k);
            t->gamma[c][k] = n > 4095 ? 4095 : n;
        }
    for (half = 0; half < 2; half++)
        for (at = 0; at < 128; at++) {
            t->g1[half][at] = t->g2[half][at] = 0;
            t->g3[half][at] = t->g4[half][at] = t->g5[half][at] = 0;
        }
    /* per sector 7 x 16 nodes: H/C chroma, H/C hue, H/C luma, Y. G1 = H(chroma) | H(hue) << 16,
     * G2 = C(luma) | C(chroma) << 16, G3 = C(hue), G4 = Y, G5 = H(luma) (camera experiments E3, E4:
     * up to core 0.95 the hue and luma H profiles were swapped); up to core 0.96 the
     * values went to the hardware as they were, the low byte a stray slope */
    for (k = 0; k < 12; k++) {
        pr = b + 2376 + 224 * k;
        half = k / 6;
        wrap = get16(b + 2088 + 24 * k + 2) == FULL_TURN;
        for (n = 0; n < 16; n++) {
            at = (k % 6) * 16 + n;
            t->g1[half][at] = word(pr, n, wrap) | (u32)word(pr + 64, n, wrap) << 16;
            t->g2[half][at] = word(pr + 160, n, 0) | (u32)word(pr + 32, n, 0) << 16;
            t->g3[half][at] = word(pr + 96, n, 0);
            t->g4[half][at] = word(pr + 192, n, 0);
            t->g5[half][at] = word(pr + 128, n, wrap);
        }
    }
    for (c = 0; c < 3; c++)
        nd->gamma[c] = (u32)t->gamma[c];
    nd->lut[0] = (u32)t->g1[0];             /* S+3124, 3128, 3132, 3136, 3140, 3144, 3148, 3152, 3156, 3160 */
    nd->lut[1] = (u32)t->g1[1];
    nd->lut[2] = (u32)t->g2[0];
    nd->lut[3] = (u32)t->g3[0];
    nd->lut[4] = (u32)t->g2[1];
    nd->lut[5] = (u32)t->g3[1];
    nd->lut[6] = (u32)t->g4[0];
    nd->lut[7] = (u32)t->g4[1];
    nd->lut[8] = (u32)t->g5[0];
    nd->lut[9] = (u32)t->g5[1];
}

/* Core task: publish an ISPB block (0 - stock again). 0 - published with new slow tables (or back to stock), 1 - a fast
 * change (the same gamma, weights and parts: no reload), 2 - nothing changed; -1 a bad block; -2 too soon (the slow
 * class: MIN_CHANGE_US after the last slow change; the fast class: FAST_MIN_US); -3 no service */
int drv_isp_set(const u8 *b, u32 size)
{
    volatile isp_t *s = isp();
    desc_t nd;
    u32 k, n, present, crc, slow, same;
    volatile u16 *tone;

    if (!installed || !s)
        return -3;
    if (!b) {
        if (s->gen && last_slow && drv_ticks_since(last_slow) < MIN_CHANGE_US)
            return -2;
        s->active = 0;
        tone_seq++;                         /* the grain mask: the identity tone now */
        s->sgen++;                          /* the hooks reload and rebuild: back to stock */
        s->gen++;
        last_slow = drv_ticks() | 1;
        slow_crc = 0;
        slow_n++;
        return 0;
    }
    if (size != ISPB_SIZE || get16(b) != 1)
        return -1;
    present = get16(b + 2) & P_ALL;
    if (block_bad(b, present))
        return -1;
    crc = crc32_update(crc32_update(0, b + 32, 3 * 512), b + 2376, 12 * 7 * 16 * 2);
    slow = !s->active || crc != slow_crc || present != slow_present;
    k = slow ? last_slow : last_fast;       /* 0 - no change within its window (drv_isp_service) */
    if (s->gen && k && drv_ticks_since(k) < (slow ? MIN_CHANGE_US : FAST_MIN_US))
        return -2;
    mem_copy(&nd, &now_desc, sizeof(nd));
    for (k = 0; k < 9; k++)
        nd.mat[k] = (s16)get16(b + 8 + 2 * k);
    nd.present = present;
    nd.cb = get16(b + 28);
    nd.cr = get16(b + 30);
    nd.mask = get16(b + 2080) & 0x0fff;
    mem_copy(nd.rec, b + 2088, sizeof(nd.rec));
    /* the tone: a new table of the ring when it changed */
    same = nd.tone != 0;
    for (k = 0; k < 256 && same; k++) {
        n = get16(b + 1568 + 2 * k);
        same = now_tone[k] == (n > 1023 ? 1023 : n);
    }
    if (!same) {
        tone = (volatile u16 *)(slot()[3] + tone_next * 512);
        tone_next = (tone_next + 1) % TONE_RING;
        for (k = 0; k < 256; k++) {
            n = get16(b + 1568 + 2 * k);
            now_tone[k] = n > 1023 ? 1023 : n;
            tone[k] = now_tone[k];
        }
        nd.tone = (u32)tone;
        tone_seq++;
    }
    if (!s->active || (present & P_TONE) != (now_desc.present & P_TONE))
        tone_seq++;
    if (slow) {
        tables_cur ^= 1;                    /* the set not in use (the one before the last slow change) */
        slow_tables(b, tables_cur, &nd);
        nd.sgen = s->sgen + 1;
        s->sgen = nd.sgen;                  /* before the latch: a builder that sees it rebuilds, at worst once more */
        slow_crc = crc;
        slow_present = present;
        last_slow = drv_ticks() | 1;
        slow_n++;
        if (present & P_MATRIX)
            log_printf(OMK_LOG_INFO, "isp: look matrix %d %d %d / %d %d %d / %d %d %d", nd.mat[0], nd.mat[1], nd.mat[2],
                       nd.mat[3], nd.mat[4], nd.mat[5], nd.mat[6], nd.mat[7], nd.mat[8]);
    } else {
        for (k = 0; k < sizeof(desc_t) / 4 && ((u32 *)&nd)[k] == ((u32 *)&now_desc)[k]; k++)
            ;
        if (k == sizeof(desc_t) / 4)
            return 2;
        fast_n++;
    }
    latch_put(s, &nd);
    mem_copy(&now_desc, &nd, sizeof(nd));
    s->active = 1;
    last_fast = drv_ticks() | 1;
    s->gen++;                               /* last: the hooks see a complete set */
    return slow ? 0 : 1;
}

u32 drv_isp_tone_seq(void)
{
    return tone_seq;
}

/* Core task: see omk_drv.h. The mask table: w through the look's tone (10 bits; its 8-bit Y = (t + 2) / 4), into the next
 * table of the ring - not one the hooks may still read: a table is rewritten (MASK_RING - 1) x FAST_MIN_US after it was
 * replaced, far past the writer-to-loader window */
int drv_isp_grain_mask(u32 key, const u16 *w)
{
    volatile isp_t *s = isp();
    volatile u16 *tab;
    u32 i, y, tone = s && s->active && (now_desc.present & P_TONE);

    if (!installed || !s)
        return -3;
    if (!w) {
        s->mask = 0;
        return 0;
    }
    if (last_mask && drv_ticks_since(last_mask) < FAST_MIN_US)
        return -2;
    tab = (volatile u16 *)(slot()[4] + mask_next * 512);
    mask_next = (mask_next + 1) % MASK_RING;
    for (i = 0; i < 256; i++) {
        y = tone ? ((u32)now_tone[i] + 2) >> 2 : i;
        y = w[y > 255 ? 255 : y];
        tab[i] = (u16)(y > 1023 ? 1023 : y);
    }
    s->mask_key = key;
    s->mask = (u32)tab;                     /* last: the writer sees a complete table */
    last_mask = drv_ticks() | 1;
    return 0;
}

/* Core task: the pacing stamps; in a debug build also the census and the counters to the log, every 5 s */
void drv_isp_service(u32 now)
{
#ifdef OMK_DEBUG
    static u32 last_ms, seen_h2[CPUS], seen_h1[CPUS], seen_ring[CPUS], seen_mat[CPUS], seen_slow, seen_fast;
    static u32 seen_evicted[CPUS];
    volatile isp_t *s = isp();
    u32 cpu, i, n, told = 0;
#else
    (void)now;
#endif

    /* The pacing stamps (drv_ticks | 1; 0 - none) end here once their windows are over: left in place they would say
     * "too soon" again for the window's length every turn of the 30-bit counter (1073 s). Called at every core loop
     * while a module owns the service */
    if (last_slow && drv_ticks_since(last_slow) >= MIN_CHANGE_US)
        last_slow = 0;
    if (last_fast && drv_ticks_since(last_fast) >= FAST_MIN_US)
        last_fast = 0;
    if (last_mask && drv_ticks_since(last_mask) >= FAST_MIN_US)
        last_mask = 0;
#ifdef OMK_DEBUG
    if (!s || now - last_ms < 5000)
        return;
    last_ms = now;
    for (cpu = 0; cpu < CPUS; cpu++) {
        n = s->mat_n[cpu] - seen_mat[cpu];
        seen_mat[cpu] = s->mat_n[cpu];
        for (i = n < MRING ? n : MRING; i > 0; i--) {   /* oldest first */
            volatile smat_t *m = &s->mat[cpu][(s->mat_n[cpu] - i) % MRING];

            log_printf(OMK_LOG_DEBUG, "isp: still matrix cpu %u t %u type %08x code %u: %d %d %d / %d %d %d / %d %d %d, "
                       "cb %u cr %u", cpu, m->ticks, m->type, m->code, m->m[0], m->m[1], m->m[2], m->m[3], m->m[4],
                       m->m[5], m->m[6], m->m[7], m->m[8], m->cb, m->cr);
            log_printf(OMK_LOG_DEBUG, "isp: still curves cpu %u t %u stage %04x: gamma %08x %08x %08x tone %08x flags %08x",
                       cpu, m->ticks, m->stage, m->gamma[0], m->gamma[1], m->gamma[2], m->tone, m->flags);
        }
    }
    for (cpu = 0; cpu < CPUS; cpu++) {
        if (s->h2[cpu] == seen_h2[cpu] && s->h1[cpu] == seen_h1[cpu])
            continue;
        seen_h2[cpu] = s->h2[cpu];
        seen_h1[cpu] = s->h1[cpu];
        log_printf(OMK_LOG_DEBUG, "isp: cpu %u writer %u builder %u patched %u reloads %u rebuilds %u torn %u (gen %u: "
                   "%u slow, %u fast, %s)", cpu, s->h2[cpu], s->h1[cpu], s->patched[cpu], s->forced[cpu], s->rebuilt[cpu],
                   s->torn[cpu], s->gen, slow_n, fast_n, s->active ? "on" : "off");
        if (s->evicted[cpu] != seen_evicted[cpu]) {
            seen_evicted[cpu] = s->evicted[cpu];
            log_printf(OMK_LOG_INFO, "isp: cpu %u: %u remembered colour structures gave way to newer ones (%u are kept a "
                       "CPU; one that comes back is reloaded once more)", cpu, seen_evicted[cpu], TRACK);
        }
        if (s->masked[cpu] || s->restored[cpu] || s->lost[cpu])
            log_printf(OMK_LOG_INFO, "isp: cpu %u grain mask passes %u, tone put back %u, lost %u (mask %08x)", cpu,
                       s->masked[cpu], s->restored[cpu], s->lost[cpu], s->mask);
        told = 1;
        if (s->census) {
            n = s->ring_n[cpu] - seen_ring[cpu];
            seen_ring[cpu] = s->ring_n[cpu];
            for (i = 0; i < n && i < 8; i++) {      /* the latest few: pipe, type/subtype, code, ext, changed, mask */
                volatile census_t *c = &s->ring[cpu][(s->ring_n[cpu] - 1 - i) % RING];

                log_printf(OMK_LOG_DEBUG, "isp: cpu %u t %u base %08x type %08x code %08x ext %08x chg %08x mask %08x", cpu,
                           c->ticks, c->base, c->type, c->code, c->ext, c->changed, c->mask);
            }
        }
    }
    if (!told && (slow_n != seen_slow || fast_n != seen_fast))   /* changes the hooks have not taken yet (or never: emulator) */
        log_printf(OMK_LOG_DEBUG, "isp: no hook calls, gen %u: %u slow, %u fast, %s", s->gen, slow_n, fast_n,
                   s->active ? "on" : "off");
    seen_slow = slow_n;
    seen_fast = fast_n;
#endif
}
