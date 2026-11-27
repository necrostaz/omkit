/* The optics probe's handing of work to its worker task (opticsdrv.c), as plain C: the request and its sequence words,
 * and the step's wait with its limits. The waits, the clock and the job's cancel check come from the caller, so the host
 * test (tests/host/test_optwork.c) runs the very code with a worker of its own. The includer defines u32.
 *
 * The words: req (the step writes it), done (the worker, after its last access to the picture), stop and abandon (the
 * step; the worker looks at both between its parts and touches no buffer after either). One request at a time: the job
 * task runs one job at a time, and a step returns only when the worker has answered - or after give_up_us. A request
 * given up (abandon) stays the worker's until it answers it: no new one is posted before that (ow_post gives 0), so
 * nothing clears the abandon word while the worker may still be on the old request's buffers. */
#ifndef OMK_OPTWORK_H
#define OMK_OPTWORK_H

enum { OW_CANCEL = 1, OW_LATE = 2, OW_ABANDON = 4 };

typedef struct {
    volatile u32 req, done, stop, abandon;
} ow_chan_t;

typedef struct {
    void (*wait_done)(void *ctx, u32 ms);   /* the step's wait on the worker's "done" flag, at most ms, the flag cleared */
    int (*cancelled)(void *ctx);            /* the job's cancel check: 1 - cancelled */
    u32 (*since)(void *ctx);                /* us since the request was posted */
    void (*note)(void *ctx, u32 state);     /* the state changed (OW_*): for the crumbs; may be 0 */
    void *ctx;
} ow_env_t;

/* 1 - the worker has not answered the last request yet (a request given up after give_up_us, or one under way) */
static inline int ow_busy(const ow_chan_t *c)
{
    return c->done != c->req;
}

/* The step: a new request; its sequence number (never 0), or 0 - the worker has not answered the last one (ow_busy):
 * nothing posted, stop and abandon left as they are, the caller goes on without the worker */
static inline u32 ow_post(ow_chan_t *c)
{
    if (ow_busy(c))
        return 0;
    c->stop = 0;
    c->abandon = 0;
    if (++c->req == 0)                      /* 0 means "not posted": the sequence skips it when it wraps */
        c->req = 1;
    return c->req;
}

/* The step: wait for the answer to request seq, slice_ms a wait. The job cancelled - stop asked, the wait goes on; no
 * answer after late_us - stop asked, the wait goes on (the worker may still touch the picture: the step must not
 * return); after give_up_us - abandon, and back. A done flag set by an older answer is told by the sequence. OW_* */
static inline u32 ow_wait(ow_chan_t *c, u32 seq, const ow_env_t *e, u32 slice_ms, u32 late_us, u32 give_up_us)
{
    u32 st = 0, el;

    for (;;) {
        e->wait_done(e->ctx, slice_ms);
        if (c->done == seq)
            return st;
        el = e->since(e->ctx);
        if (!(st & OW_CANCEL) && e->cancelled(e->ctx)) {
            c->stop = 1;
            st |= OW_CANCEL;
            if (e->note)
                e->note(e->ctx, st);
        }
        if (!(st & OW_LATE) && el >= late_us) {
            c->stop = 1;
            st |= OW_LATE;
            if (e->note)
                e->note(e->ctx, st);
        }
        if (el >= give_up_us) {
            c->abandon = 1;
            st |= OW_ABANDON;
            if (e->note)
                e->note(e->ctx, st);
            return st;
        }
    }
}

/* The worker: a request not answered yet - 1 and its number in *seq */
static inline int ow_take(const ow_chan_t *c, u32 *seq)
{
    *seq = c->req;
    return *seq != c->done;
}

/* The worker: the answer, after its last access to the picture */
static inline void ow_answer(ow_chan_t *c, u32 seq)
{
    c->done = seq;
}

#endif
