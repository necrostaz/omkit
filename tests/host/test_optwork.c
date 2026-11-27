/* Host test of the optics probe's handing of work to its worker task (src/platform/om5m2/fw1100/optwork.h, the code the
 * step and the worker run): a worker of the test answers after a number of the step's waits, a clock moves by the wait's
 * length. Cases: an answer; a wake-up with another number in done (a flag left set, an answer to another request); the
 * job cancelled (stop asked, the step waits on for the answer); a late worker (stop at the late limit, the step waits
 * on); a worker that never answers (abandon at the give-up limit, and only then back); no new request while the worker
 * has not answered the one given up (the abandon word kept), and one again after its late answer; the sequence's wrap;
 * the worker's view of requests. Prints "ok", or what failed. */
#include <stdio.h>
#include "omk_types.h"
#include "optwork.h"

#define SLICE_MS 50
#define LATE_US 2000000
#define GIVE_UP_US 10000000

static int failed;

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "%s\n", what);
        failed = 1;
    }
}

typedef struct {
    ow_chan_t *c;
    u32 seq;                                /* the request being waited for */
    u32 now_us;                             /* the clock since the request */
    u32 waits;                              /* the step's waits so far */
    u32 answer_at;                          /* the worker answers at this wait (0 - never) */
    u32 junk_at;                            /* at this wait done holds another number (0 - never) */
    u32 cancel_at;                          /* the job is cancelled from this wait on (0 - never) */
    u32 stop_at;                            /* the wait at which the worker first saw stop (0 - not) */
    u32 notes, cancels;
} sim_t;

static void sim_wait(void *x, u32 ms)
{
    sim_t *s = x;

    s->now_us += ms * 1000;
    s->waits++;
    if (s->c->stop && !s->stop_at)
        s->stop_at = s->waits;
    if (s->junk_at && s->waits == s->junk_at)
        s->c->done = s->seq + 7;
    if (s->answer_at && s->waits == s->answer_at)
        ow_answer(s->c, s->seq);
}

static int sim_cancelled(void *x)
{
    sim_t *s = x;

    s->cancels++;
    return s->cancel_at && s->waits >= s->cancel_at;
}

static u32 sim_since(void *x)
{
    return ((sim_t *)x)->now_us;
}

static void sim_note(void *x, u32 st)
{
    (void)st;
    ((sim_t *)x)->notes++;
}

static u32 run(sim_t *s, ow_chan_t *c)
{
    ow_env_t e = {sim_wait, sim_cancelled, sim_since, sim_note, s};

    s->c = c;
    s->seq = ow_post(c);
    return ow_wait(c, s->seq, &e, SLICE_MS, LATE_US, GIVE_UP_US);
}

int main(void)
{
    ow_chan_t c = {0, 0, 0, 0};
    sim_t s;
    u32 st, t, i;

    /* the worker sees a request until it answers */
    check(!ow_take(&c, &t), "no request at first");
    c.req = 1;
    check(ow_take(&c, &t) && t == 1, "a request is seen");
    ow_answer(&c, 1);
    check(!ow_take(&c, &t), "answered: nothing new");

    /* an answer at the third wait */
    s = (sim_t){0};
    s.answer_at = 3;
    st = run(&s, &c);
    check(st == 0 && s.waits == 3 && c.done == s.seq && !c.stop && !c.abandon && !s.notes, "an answer: 3 waits, no state");

    /* another number in done at the second wait: the step waits on for its own */
    s = (sim_t){0};
    s.junk_at = 2;
    s.answer_at = 5;
    st = run(&s, &c);
    check(st == 0 && s.waits == 5, "another number in done: not taken");

    /* the job cancelled at the 4th wait: stop asked once, the step waits for the answer at the 20th */
    s = (sim_t){0};
    s.cancel_at = 4;
    s.answer_at = 20;
    st = run(&s, &c);
    check(st == OW_CANCEL && s.waits == 20 && c.stop && !c.abandon && s.stop_at == 5 && s.notes == 1,
          "cancelled: stop, the answer awaited");

    /* a late worker: stop at 2 s (40 waits), the answer at 3 s - awaited */
    s = (sim_t){0};
    s.answer_at = 60;
    st = run(&s, &c);
    check(st == OW_LATE && s.waits == 60 && c.stop && !c.abandon && s.stop_at == 41 && s.notes == 1,
          "late: stop at the limit, the answer awaited");

    /* a new request clears stop and abandon */
    s = (sim_t){0};
    s.answer_at = 1;
    st = run(&s, &c);
    check(st == 0 && !c.stop && !c.abandon, "a new request: stop and abandon cleared");

    /* no answer at all: abandon at 10 s (200 waits), not before */
    s = (sim_t){0};
    st = run(&s, &c);
    check(st == (OW_LATE | OW_ABANDON) && s.waits == GIVE_UP_US / 1000 / SLICE_MS && c.abandon && c.stop && s.notes == 2,
          "never answered: abandon at the give-up limit");
    check(ow_take(&c, &t) && t == s.seq, "abandoned: the worker still sees the request (it checks abandon itself)");

    /* the next job while the worker is still on the request given up: nothing posted, abandon kept */
    t = c.req;
    check(ow_busy(&c) && ow_post(&c) == 0 && c.req == t && c.abandon && c.stop,
          "given up and not answered: no new request, abandon and stop kept");
    /* the worker wakes, sees abandon, answers the old number: the next job is posted, the words cleared */
    ow_answer(&c, t);
    check(!ow_busy(&c) && !ow_take(&c, &i), "the late answer: the channel free");
    s = (sim_t){0};
    s.answer_at = 2;
    st = run(&s, &c);
    check(st == 0 && s.seq == t + 1 && s.waits == 2 && !c.stop && !c.abandon, "after the late answer: a new request");

    /* the sequence wraps past 0 (0 means "not posted") */
    c.req = c.done = 0xffffffffu;
    check(ow_post(&c) == 1 && c.req == 1 && ow_busy(&c), "the sequence skips 0");
    ow_answer(&c, 1);

    if (!failed)
        printf("ok\n");
    return failed;
}
