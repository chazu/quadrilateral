#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quad/kernels.h"
#include "quad/world.h"
#include "test.h"

/* native.rate: a kernel that moves a stock at the rate of a cell
 * (docs/adr/0020). These worlds come from the C API. tests/test_elab.c has
 * the same kernel from source. */

#define OK(x) CHECK((x) == QW_OK)
#define TICKS(seconds) ((int64_t) (seconds) * 1000000)

static const QInterval ANY = { Q_NINF, Q_INF };

/* A tank that leaks: the outflow is volume / empties. The volume is in
 * mm^3 (scale -9), the outflow in the same for each second, and the time
 * constant `empties` in seconds. */
static QWorld *tank(double volume_m3, int64_t empties_s, int64_t tol_e9, QkKernel **kernel)
{
    QWorld *w = qw_new(1, -6);
    QwKind volume, flow, time, ratio;
    QwPhen p;
    QnCell v, f, e;
    OK(qw_kind(w, "volume", "m^3", -9, true, &volume));
    OK(qw_kind(w, "flow", "m^3/s", -9, false, &flow));
    OK(qw_kind(w, "time", "s", -6, false, &time));
    OK(qw_kind(w, "ratio", "", -9, false, &ratio));
    OK(qw_phenomenon(w, "tank", &p));
    OK(qw_cell(w, "tank.volume", volume, qi_point((int64_t) llround(volume_m3 * 1e9)), &v));
    OK(qw_cell(w, "tank.empties", time, qi_point(TICKS(empties_s)), &e));
    OK(qw_derived_cell(w, "tank.outflow", flow, qi_top(), &f));
    OK(qw_cell(w, "tank.tolerance", ratio, qi_point(tol_e9), NULL));
    CHECK(qn_quotient(qw_net(w), f, v, e, NULL) == QN_OK);
    const QkSlot slots[] = {
        { "outflow", QK_IN, "m^3/s", -9, ANY },
        { "tolerance", QK_IN, "", -9, { 1, 1000000000 } },
        { "volume", QK_STOCK, "m^3", -9, ANY },
    };
    const QkRateTransfer leak = { "leak", 0, 1, 2, QK_NONE };
    *kernel = qk_rate_new(slots, 3, &leak, 1);
    CHECK(*kernel != NULL);
    if (*kernel) OK(qw_attach(w, p, *kernel));
    return w;
}

static double volume_of(QWorld *w) { return (double) qw_get(w, "tank.volume").lo * 1e-9; }

/* A rate in proportion to its stock: the result follows the exponential
 * within the tolerance, and the step becomes tolerance × time constant. */
void test_rate_decay(void)
{
    QkKernel *k;
    QWorld *w = tank(2.0, 600, 10000000, &k);          /* 2 m^3, 600 s, 1 % */
    if (k) {
        for (int t = 600; t <= 3000; t += 600) {
            OK(qw_run(w, TICKS(t)));
            double exact = 2.0 * exp(-(double) t / 600.0), got = volume_of(w);
            /* the error of the explicit method grows with the number of time constants */
            CHECK(fabs(got - exact) <= 0.01 * (t / 600) * exact);
            CHECK(got > 0 && got < exact);                                 /* explicit: it leaks a little too fast */
        }
        /* about 100 steps for each time constant, and the short steps at the start */
        uint64_t steps = qw_boundaries(w);
        CHECK(steps > 450 && steps < 620);
        /* at each later time, one step is 1 % of 600 s */
        uint64_t before = qw_boundaries(w);
        OK(qw_run(w, TICKS(3060)));
        CHECK(qw_boundaries(w) - before >= 10 && qw_boundaries(w) - before <= 11);
        /* a smaller tolerance gives a smaller error */
        QkKernel *k2;
        QWorld *fine = tank(2.0, 600, 1000000, &k2);                       /* 0.1 % */
        OK(qw_run(fine, TICKS(600)));
        OK(qw_run(w, TICKS(3060)));
        double exact = 2.0 * exp(-1.0);
        CHECK(fabs(volume_of(fine) - exact) <= 0.001 * exact);
        qw_free(fine);
        qk_rate_free(k2);
    }
    qw_free(w);
    qk_rate_free(k);
}

/* The tank never goes below zero, and a run has the same hash each time. */
void test_rate_decay_long(void)
{
    QkKernel *k, *k2;
    QWorld *w = tank(0.001, 60, 50000000, &k), *again = tank(0.001, 60, 50000000, &k2);     /* 1 litre, 60 s, 5 % */
    for (int t = 60; t <= 3600; t += 60) {
        OK(qw_run(w, TICKS(t)));
        OK(qw_run(again, TICKS(t)));
        CHECK(qw_get(w, "tank.volume").lo >= 0);
        CHECK(qw_hash(w) == qw_hash(again));
    }
    CHECK(qw_get(w, "tank.volume").lo < 100);          /* 60 time constants: less than 100 mm^3 */
    qw_free(w);
    qw_free(again);
    qk_rate_free(k);
    qk_rate_free(k2);
}

/* A heater and two rooms. power: a constant rate from outside into a.
 * flow: a rate between a and b that a law gives from their difference. */
typedef struct { QWorld *w; QkKernel *k; QnCell a, b, power; } Rooms;

static Rooms rooms(int64_t power_mw, int64_t per_s_e9)
{
    Rooms r = { qw_new(7, -6), NULL, 0, 0, 0 };
    QWorld *w = r.w;
    QwKind energy, gap, powerk, rate, ratio;
    QwPhen pa, pb, mover;
    QnCell diff, k, flow;
    OK(qw_kind(w, "energy", "J", -3, true, &energy));
    OK(qw_kind(w, "power", "W", -3, false, &powerk));
    OK(qw_anonymous_kind(w, "J", -3, &gap));
    OK(qw_kind(w, "rate", "s^-1", -9, false, &rate));
    OK(qw_kind(w, "ratio", "", -9, false, &ratio));
    OK(qw_phenomenon(w, "a", &pa));
    OK(qw_phenomenon(w, "b", &pb));
    OK(qw_phenomenon(w, "mover", &mover));
    OK(qw_cell(w, "a.energy", energy, qi_point(5000000), &r.a));       /* 5000 J */
    OK(qw_cell(w, "b.energy", energy, qi_point(1000000), &r.b));       /* 1000 J */
    OK(qw_cell(w, "mover.power", powerk, qi_point(power_mw), &r.power));
    OK(qw_cell(w, "mover.k", rate, qi_point(per_s_e9), &k));
    OK(qw_derived_cell(w, "mover.diff", gap, qi_top(), &diff));
    OK(qw_derived_cell(w, "mover.flow", powerk, qi_top(), &flow));
    OK(qw_cell(w, "mover.tolerance", ratio, qi_point(10000000), NULL));
    OK(qw_connect(w, mover, "hot", "a"));
    OK(qw_connect(w, mover, "cold", "b"));
    CHECK(qn_difference(qw_net(w), diff, r.a, r.b, NULL) == QN_OK);    /* diff = a - b */
    CHECK(qn_product(qw_net(w), flow, k, diff, NULL) == QN_OK);        /* flow = k × diff */
    const QkSlot slots[] = {
        { "power", QK_IN, "W", -3, ANY },
        { "flow", QK_IN, "W", -3, ANY },
        { "tolerance", QK_IN, "", -9, { 1, 1000000000 } },
        { "hot.energy", QK_LINK, "J", -3, ANY },
        { "cold.energy", QK_LINK, "J", -3, ANY },
    };
    const QkRateTransfer moves[] = {
        { "heat", 0, 2, QK_NONE, 3 },          /* from outside into a */
        { "level", 1, 2, 3, 4 },               /* from a to b */
    };
    r.k = qk_rate_new(slots, 5, moves, 2);
    CHECK(r.k != NULL);
    if (r.k) {
        OK(qw_attach(w, mover, r.k));
        OK(qw_adopt_kernel(w, r.k, qk_rate_free));     /* the world frees the kernel */
    }
    return r;
}

/* A constant rate is exact at each time, with steps of any length. The sum
 * of the two stocks changes only by what comes from outside. */
void test_rate_constant_and_conserved(void)
{
    Rooms r = rooms(1500000, 2000000);                 /* 1500 W, and 0.002 for each second (500 s) */
    QWorld *w = r.w;
    if (r.k) {
        const int times[] = { 1, 7, 60, 61, 500, 2000, 10000 };
        for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
            OK(qw_run(w, TICKS(times[i])));
            int64_t sum = qw_get(w, "a.energy").lo + qw_get(w, "b.energy").lo;
            CHECK_I64(sum, 6000000 + (int64_t) 1500000 * times[i]);        /* 6000 J + 1500 J for each second */
        }
        /* the two rooms come to the same level: a is above b by power / (2 k) = 375000 J */
        int64_t gap = qw_get(w, "a.energy").lo - qw_get(w, "b.energy").lo;
        CHECK(llabs(gap - 375000000) <= 3750000);
    }
    qw_free(w);

    /* with no heater, the transfer between the rooms conserves the sum exactly, and it turns around with the sign */
    r = rooms(0, 2000000);
    w = r.w;
    if (r.k) {
        OK(qw_run(w, TICKS(250)));
        CHECK_I64(qw_get(w, "a.energy").lo + qw_get(w, "b.energy").lo, 6000000);
        CHECK(qw_get(w, "a.energy").lo < 5000000 && qw_get(w, "b.energy").lo > 1000000);
        /* an input makes b the fuller one: the rate is negative, and the quantity goes from b to a */
        OK(qw_input(w, "mover.k", qi_point(-2000000)));
        int64_t a0 = qw_get(w, "a.energy").lo;
        OK(qw_run(w, TICKS(260)));
        CHECK(qw_get(w, "a.energy").lo > a0);
        CHECK_I64(qw_get(w, "a.energy").lo + qw_get(w, "b.energy").lo, 6000000);
    }
    qw_free(w);
}

/* A rate that is too small for one quantum in a step still moves the right
 * amount over time: the kernel keeps the rest. */
void test_rate_carry(void)
{
    /* 1 mW into a stock of whole joules, with short steps from a second kernel-free input schedule */
    QWorld *w = qw_new(1, -6);
    QwKind energy, powerk, ratio;
    QwPhen p;
    OK(qw_kind(w, "energy", "J", 0, true, &energy));
    OK(qw_kind(w, "power", "W", -3, false, &powerk));
    OK(qw_kind(w, "ratio", "", -9, false, &ratio));
    OK(qw_phenomenon(w, "p", &p));
    OK(qw_cell(w, "p.energy", energy, qi_point(0), NULL));
    OK(qw_cell(w, "p.power", powerk, qi_point(1), NULL));              /* 1 mW */
    OK(qw_cell(w, "p.tolerance", ratio, qi_point(10000000), NULL));
    const QkSlot slots[] = {
        { "power", QK_IN, "W", -3, ANY },
        { "tolerance", QK_IN, "", -9, { 1, 1000000000 } },
        { "energy", QK_STOCK, "J", 0, ANY },
    };
    const QkRateTransfer in = { "in", 0, 1, QK_NONE, 2 };
    QkKernel *k = qk_rate_new(slots, 3, &in, 1);
    CHECK(k != NULL);
    if (k) {
        OK(qw_attach(w, p, k));
        for (int t = 1; t <= 5000; t += 357) OK(qw_run(w, TICKS(t)));     /* many stops, each below one joule */
        OK(qw_run(w, TICKS(5000)));
        CHECK_I64(qw_get(w, "p.energy").lo, 5);                            /* 1 mW × 5000 s */
        OK(qw_run(w, TICKS(5999)));
        CHECK_I64(qw_get(w, "p.energy").lo, 5);
        OK(qw_run(w, TICKS(6000)));
        CHECK_I64(qw_get(w, "p.energy").lo, 6);
    }
    qw_free(w);
    qk_rate_free(k);
}

/* A tap and a leak on one tank: the level comes to inflow × empties and
 * stays there. Near the balance each rate changes little, so the rule of the
 * tolerance alone gives steps longer than the time constant, and the level
 * goes above and below the balance. The kernel sees the outflow go up and
 * down in turn and caps the step. */
void test_rate_balance(void)
{
    QWorld *w = qw_new(1, -6);
    QwKind volume, flow, time, ratio;
    QwPhen p;
    QnCell v, f, e;
    OK(qw_kind(w, "volume", "m^3", -9, true, &volume));
    OK(qw_kind(w, "flow", "m^3/s", -15, false, &flow));
    OK(qw_kind(w, "time", "s", -6, false, &time));
    OK(qw_kind(w, "ratio", "", -9, false, &ratio));
    OK(qw_phenomenon(w, "tank", &p));
    OK(qw_cell(w, "tank.volume", volume, qi_point(0), &v));
    OK(qw_cell(w, "tank.empties", time, qi_point(TICKS(100)), &e));
    OK(qw_cell(w, "tank.inflow", flow, qi_point(2000000000000), NULL));       /* 0.002 m^3/s */
    OK(qw_derived_cell(w, "tank.outflow", flow, qi_top(), &f));
    OK(qw_cell(w, "tank.tolerance", ratio, qi_point(10000000), NULL));
    CHECK(qn_quotient(qw_net(w), f, v, e, NULL) == QN_OK);
    const QkSlot slots[] = {
        { "inflow", QK_IN, "m^3/s", -15, ANY },
        { "outflow", QK_IN, "m^3/s", -15, ANY },
        { "tolerance", QK_IN, "", -9, { 1, 1000000000 } },
        { "volume", QK_STOCK, "m^3", -9, ANY },
    };
    const QkRateTransfer moves[] = { { "tap", 0, 2, QK_NONE, 3 }, { "leak", 1, 2, 3, QK_NONE } };
    QkKernel *k = qk_rate_new(slots, 4, moves, 2);
    CHECK(k != NULL);
    if (k) {
        OK(qw_attach(w, p, k));
        OK(qw_adopt_kernel(w, k, qk_rate_free));
        /* one long run, with no stop that could hide a long step */
        OK(qw_run(w, TICKS(2000)));                                    /* 20 time constants */
        CHECK(llabs(qw_get(w, "tank.volume").lo - 200000000) <= 200000);       /* 0.2 m^3, within 0.1 % */
        uint64_t steps = qw_boundaries(w);
        OK(qw_run(w, TICKS(100000)));                                  /* 1000 time constants */
        CHECK(llabs(qw_get(w, "tank.volume").lo - 200000000) <= 20);
        /* at the balance, a step stays near the time constant: it does not grow, and it does not collapse */
        uint64_t more = qw_boundaries(w) - steps;
        CHECK(more > 400 && more < 6000);
    }
    qw_free(w);
}

/* A rate with no bound stops the run, and the message names the transfer.
 * A range uses its middle. */
void test_rate_unknown(void)
{
    Rooms r = rooms(1000, 0);
    QWorld *w = r.w;
    if (r.k) {
        OK(qw_input(w, "mover.power", (QInterval) { 1000, 3000 }));         /* 1 W to 3 W: the middle is 2 W */
        OK(qw_run(w, TICKS(100)));
        CHECK_I64(qw_get(w, "a.energy").lo, 5000000 + 200000);
        OK(qw_input(w, "mover.power", (QInterval) { 0, Q_INF }));
        CHECK(qw_run(w, TICKS(200)) == QW_EKERNEL);
        CHECK(strstr(qw_error(w), "the rate of the interaction heat has no bound") != NULL);
    }
    qw_free(w);
}

/* The contract: what qk_rate_new refuses. */
void test_rate_contract(void)
{
    const QkSlot slots[] = {
        { "power", QK_IN, "W", -3, ANY },
        { "tolerance", QK_IN, "", -9, { 1, 1000000000 } },
        { "energy", QK_STOCK, "J", 0, ANY },
    };
    const QkRateTransfer good = { "in", 0, 1, QK_NONE, 2 };
    QkKernel *k = qk_rate_new(slots, 3, &good, 1);
    CHECK(k != NULL);
    if (k) {
        CHECK(!strcmp(k->name, "native.rate") && k->nslots == 3 && k->ntransfers == 1);
        CHECK(!k->transfers[0].conserved && k->transfers[0].from == QK_NONE && k->transfers[0].to == 2);
        CHECK((k->flags & QK_EVOLVE) && (k->flags & QK_PORTABLE) && !(k->flags & QK_STATELESS));
        CHECK(k->save && k->restore);
    }
    qk_rate_free(k);
    const QkRateTransfer both = { "move", 0, 1, 2, 2 };
    k = qk_rate_new(slots, 3, &both, 1);
    CHECK(k && k->transfers[0].conserved);
    qk_rate_free(k);
    const QkRateTransfer bad[] = {
        { "none", 0, 1, QK_NONE, QK_NONE },    /* no stock */
        { "rate", 3, 1, QK_NONE, 2 },          /* the rate is no slot */
        { "tol", 0, 9, QK_NONE, 2 },           /* the tolerance is no slot */
        { "stock", 0, 1, 2, 5 },               /* the sink is no slot */
        { "from", 0, 1, 7, 2 },                /* the source is no slot */
        { "role", 0, 2, QK_NONE, 2 },          /* the tolerance is a stock */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(qk_rate_new(slots, 3, &bad[i], 1) == NULL);
    CHECK(qk_rate_new(slots, 0, &good, 1) == NULL);
    CHECK(qk_rate_new(slots, 3, &good, 0) == NULL);
    qk_rate_free(NULL);
}
