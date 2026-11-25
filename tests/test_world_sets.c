#include <stdlib.h>
#include <string.h>

#include "quad/kernels.h"
#include "quad/world.h"
#include "test.h"

/* Set cells in the world and in kernels (ADR 0012). */

#define OK(x) CHECK((x) == QW_OK)
#define SEC 1000000LL

/* --- a door that opens and closes every period ---------------------------------- */

enum { D_STATE, D_PERIOD, D_NSLOTS };

static const QkSlot door_slots[D_NSLOTS] = {
    [D_STATE]  = { "state",  QK_OUT, "atoms", 0,  { Q_NINF, Q_INF } },
    [D_PERIOD] = { "period", QK_IN,  "s",     -6, { 1, Q_INF } },
};

typedef enum { DM_NONE, DM_WRITE_BAD_ATOM, DM_READ_AS_INTERVAL, DM_READ_SET_OF_INTERVAL } DoorMisbehave;
static DoorMisbehave door_misbehave;

struct QkInstance { const QkHost *host; int open, closed; };

static QkResult door_create(const QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out)
{
    (void) self;
    if (host->size < sizeof(QkHost)) return QK_ERROR;
    QkInstance *i = malloc(sizeof *i);
    if (!i) return QK_ERROR;
    i->host = host;
    i->open = host->atom(call, D_STATE, "open");
    i->closed = host->atom(call, D_STATE, "closed");
    if (i->open < 0 || i->closed < 0 || host->atom(call, D_STATE, "ajar") != -1) {
        free(i);
        return QK_ERROR;
    }
    *out = i;
    return QK_OK;
}

static void door_destroy(QkInstance *i) { free(i); }

static QkResult door_predict(QkInstance *i, QkCall *call, QkPrediction *out)
{
    const QkHost *h = i->host;
    out->horizon = h->now(call) + h->read(call, D_PERIOD).lo;
    out->event_lo = out->event_hi = QK_NO_TIME;
    return QK_OK;
}

/* Every kernel advances at every boundary of the shared scheduler, so the
 * door changes only at multiples of its period. */
static QkResult door_advance(QkInstance *i, QkCall *call, int64_t to)
{
    const QkHost *h = i->host;
    if (to % h->read(call, D_PERIOD).lo != 0 && door_misbehave == DM_NONE) return QK_OK;
    switch (door_misbehave) {
    case DM_WRITE_BAD_ATOM: h->write_atom(call, D_STATE, 7); return QK_OK;
    case DM_READ_AS_INTERVAL: h->read(call, D_STATE); return QK_OK;
    case DM_READ_SET_OF_INTERVAL: h->read_set(call, D_PERIOD); return QK_OK;
    default: break;
    }
    bool open = h->read_set(call, D_STATE) == (uint64_t) 1 << i->open;
    h->write_atom(call, D_STATE, (uint32_t) (open ? i->closed : i->open));
    return QK_OK;
}

static const QkKernel door_kernel = {
    .abi_version = QK_ABI_VERSION,
    .size = sizeof(QkKernel),
    .qr_version = QR_VERSION,
    .name = "test.door",
    .flags = QK_EVOLVE | QK_STATELESS | QK_PORTABLE,
    .slots = door_slots,
    .nslots = D_NSLOTS,
    .create = door_create,
    .destroy = door_destroy,
    .predict = door_predict,
    .advance = door_advance,
};

/* --- two rooms, a wall, and a door in the wall ------------------------------------ */

typedef struct { QWorld *w; QwPhen a, b, wall, door; QwKind energy, state; } House;

static void room(House *h, const char *name, int64_t cap_mJK, int64_t temp_cK, QwKind temp, QwKind cap)
{
    char path[64];
    QnCell e, c, t;
    snprintf(path, sizeof path, "%s.energy", name);
    OK(qw_cell(h->w, path, h->energy, qi_point(cap_mJK / 100 * temp_cK), &e));
    snprintf(path, sizeof path, "%s.capacity", name);
    OK(qw_cell(h->w, path, cap, qi_point(cap_mJK), &c));
    snprintf(path, sizeof path, "%s.temperature", name);
    OK(qw_cell(h->w, path, temp, qi_top(), &t));
    CHECK(qn_product(qw_net(h->w), e, c, t, NULL) == QN_OK);
}

static House make_house(bool with_door)
{
    House h;
    memset(&h, 0, sizeof h);
    h.w = qw_new(1, -6);
    QwKind temp, cap, cond, ratio, dur;
    OK(qw_kind(h.w, "energy", "J", -3, true, &h.energy));
    OK(qw_kind(h.w, "temperature", "K", -6, false, &temp));
    OK(qw_kind(h.w, "capacity", "J/K", -3, false, &cap));
    OK(qw_kind(h.w, "conductance", "W/K", -3, false, &cond));
    OK(qw_kind(h.w, "ratio", "", -9, false, &ratio));
    OK(qw_kind(h.w, "duration", "s", -6, false, &dur));
    const char *atoms[3] = { "locked", "closed", "open" };   /* not the kernel's order of lookup */
    OK(qw_enum_kind(h.w, "door_state", atoms, 3, &h.state));
    OK(qw_phenomenon(h.w, "roomA", &h.a));
    OK(qw_phenomenon(h.w, "roomB", &h.b));
    OK(qw_phenomenon(h.w, "wall", &h.wall));
    OK(qw_phenomenon(h.w, "door", &h.door));
    room(&h, "roomA", 10000000, 30315, temp, cap);
    room(&h, "roomB", 10000000, 29315, temp, cap);

    QnCell state, g;
    OK(qw_set_cell(h.w, "door.state", h.state, (uint64_t) 1 << qw_atom(h.w, h.state, "closed"), &state));
    OK(qw_cell(h.w, "door.period", dur, qi_point(100 * SEC), NULL));
    OK(qw_cell(h.w, "wall.conductance", cond, qi_top(), &g));
    QInterval table[3] = { qi_point(10000), qi_point(10000), qi_point(20000) };   /* open: 20 W/K, else 10 W/K */
    CHECK(qn_element(qw_net(h.w), g, state, table, 3) == QN_OK);
    OK(qw_cell(h.w, "wall.tolerance", ratio, qi_point(10000000), NULL));
    OK(qw_connect(h.w, h.wall, "hot", "roomA"));
    OK(qw_connect(h.w, h.wall, "cold", "roomB"));
    OK(qw_attach(h.w, h.wall, qk_conduction()));
    if (with_door) OK(qw_attach(h.w, h.door, &door_kernel));
    return h;
}

static uint64_t door_mask(House *h, const char *atom) { return (uint64_t) 1 << qw_atom(h->w, h->state, atom); }

void test_world_set_cells(void)
{
    House h = make_house(false);
    CHECK(qw_get_set(h.w, "door.state") == door_mask(&h, "closed"));
    CHECK_I64(qw_atom(h.w, h.state, "open"), 2);
    CHECK_I64(qw_atom(h.w, h.state, "ajar"), -1);
    OK(qw_run(h.w, 1 * SEC));
    CHECK_I64(qw_get(h.w, "wall.conductance").lo, 10000);

    /* an input opens the door, and the conductance follows through the element propagator */
    OK(qw_input_set(h.w, "door.state", door_mask(&h, "open")));
    CHECK_I64(qw_get(h.w, "wall.conductance").lo, 20000);

    /* errors */
    QnCell c;
    QwKind k;
    const char *dup[2] = { "a", "a" };
    CHECK(qw_enum_kind(h.w, "twice", dup, 2, &k) == QW_EDUP);
    CHECK(qw_enum_kind(h.w, "door_state", dup, 1, &k) == QW_EDUP);
    CHECK(qw_enum_kind(h.w, "none", dup, 0, &k) == QW_ETYPE);
    CHECK(qw_set_cell(h.w, "door.other", h.energy, 1, &c) == QW_ETYPE);        /* not an enumeration */
    CHECK(qw_cell(h.w, "door.other", h.state, qi_point(1), &c) == QW_ETYPE);    /* use qw_set_cell */
    CHECK(qw_set_cell(h.w, "door.other", h.state, 8, &c) == QW_ETYPE);         /* outside the universe */
    CHECK(qw_input_set(h.w, "roomA.capacity", 1) == QW_ETYPE);
    CHECK(qw_input(h.w, "door.state", qi_point(1)) == QW_ETYPE);
    CHECK(qw_get_set(h.w, "roomA.capacity") == 0);
    qw_free(h.w);
}

void test_world_door_kernel(void)
{
    door_misbehave = DM_NONE;
    House h = make_house(true), shut = make_house(false);
    int64_t total = qw_total(h.w, h.energy);
    OK(qw_run(h.w, 50 * SEC));
    CHECK(qw_get_set(h.w, "door.state") == door_mask(&h, "closed"));
    OK(qw_run(h.w, 150 * SEC));
    CHECK(qw_get_set(h.w, "door.state") == door_mask(&h, "open"));
    CHECK_I64(qw_get(h.w, "wall.conductance").lo, 20000);
    uint64_t before = qw_predicts(h.w, h.wall);
    OK(qw_run(h.w, 250 * SEC));
    CHECK(qw_get_set(h.w, "door.state") == door_mask(&h, "closed"));
    CHECK_I64(qw_get(h.w, "wall.conductance").lo, 10000);
    /* the wall read the conductance in predict, so the closing invalidated its prediction */
    CHECK(qw_predicts(h.w, h.wall) > before);
    CHECK_I64(qw_total(h.w, h.energy), total);

    /* an open door moves more heat than a door that stays shut */
    OK(qw_run(shut.w, 250 * SEC));
    CHECK(qw_get(h.w, "roomA.energy").lo < qw_get(shut.w, "roomA.energy").lo);
    qw_free(h.w);
    qw_free(shut.w);
}

void test_world_set_slot_errors(void)
{
    struct { DoorMisbehave m; const char *msg; } cases[] = {
        { DM_WRITE_BAD_ATOM, "atom" },
        { DM_READ_AS_INTERVAL, "set" },
        { DM_READ_SET_OF_INTERVAL, "not a set" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        door_misbehave = cases[i].m;
        House h = make_house(true);
        CHECK(qw_run(h.w, 150 * SEC) == QW_EKERNEL);
        CHECK(strstr(qw_error(h.w), cases[i].msg) != NULL);
        qw_free(h.w);
    }
    door_misbehave = DM_NONE;

    /* binding: an "atoms" slot needs a set cell, and an interval slot an interval cell */
    House h = make_house(false);
    QkSlot wrong[D_NSLOTS];
    memcpy(wrong, door_slots, sizeof wrong);
    wrong[D_STATE].name = "period";   /* door.period is an interval cell */
    wrong[D_PERIOD].name = "state";   /* door.state is a set cell */
    QkKernel k = door_kernel;
    k.slots = wrong;
    CHECK(qw_attach(h.w, h.door, &k) == QW_ETYPE);
    QkSlot stock[D_NSLOTS];
    memcpy(stock, door_slots, sizeof stock);
    stock[D_STATE].role = QK_STOCK;
    k.slots = stock;
    CHECK(qw_attach(h.w, h.door, &k) == QW_ETYPE);
    qw_free(h.w);
}

/* --- a kernel that watches the door ------------------------------------------------- */

static const QkSlot watch_slots[1] = { { "door.state", QK_IN, "atoms", 0, { Q_NINF, Q_INF } } };

static QkResult watch_predict(QkInstance *i, QkCall *call, QkPrediction *out)
{
    i->host->read_set(call, 0);   /* a read during predict is an assumption */
    out->horizon = i->host->now(call) + 1000 * SEC;
    out->event_lo = out->event_hi = QK_NO_TIME;
    return QK_OK;
}

static QkResult watch_advance(QkInstance *i, QkCall *call, int64_t to)
{
    (void) i;
    (void) call;
    (void) to;
    return QK_OK;
}

static QkResult watch_create(const QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out)
{
    (void) self;
    (void) call;
    QkInstance *i = calloc(1, sizeof *i);
    if (!i) return QK_ERROR;
    i->host = host;
    *out = i;
    return QK_OK;
}

static const QkKernel watch_kernel = {
    .abi_version = QK_ABI_VERSION,
    .size = sizeof(QkKernel),
    .qr_version = QR_VERSION,
    .name = "test.watch",
    .flags = QK_EVOLVE | QK_STATELESS | QK_PORTABLE,
    .slots = watch_slots,
    .nslots = 1,
    .create = watch_create,
    .destroy = door_destroy,
    .predict = watch_predict,
    .advance = watch_advance,
};

void test_world_set_assumptions_and_log(void)
{
    House h = make_house(false);
    QwPhen watcher;
    OK(qw_phenomenon(h.w, "watcher", &watcher));
    OK(qw_connect(h.w, watcher, "door", "door"));
    OK(qw_attach(h.w, watcher, &watch_kernel));
    qw_log_changes(h.w, true);
    OK(qw_run(h.w, 10 * SEC));
    uint64_t p = qw_predicts(h.w, watcher);
    CHECK_I64((int64_t) p, 1);

    /* the same atoms again: the assumption holds */
    OK(qw_input_set(h.w, "door.state", door_mask(&h, "closed")));
    OK(qw_run(h.w, 20 * SEC));
    CHECK_I64((int64_t) qw_predicts(h.w, watcher), 1);

    /* the door opens: the watcher predicts again, and the log has the set value */
    size_t before;
    qw_changes(h.w, &before);
    OK(qw_input_set(h.w, "door.state", door_mask(&h, "open")));
    size_t n;
    const QwChange *log = qw_changes(h.w, &n);
    CHECK_I64((int64_t) (n - before), 1);
    QnCell state;
    OK(qw_find(h.w, "door.state", &state));
    CHECK(log[n - 1].cell == state && log[n - 1].kind == QW_CHANGE_VALUE);
    CHECK(log[n - 1].after.lo == (int64_t) door_mask(&h, "open"));
    CHECK(log[n - 1].before.lo == (int64_t) door_mask(&h, "closed"));
    OK(qw_run(h.w, 30 * SEC));
    CHECK_I64((int64_t) qw_predicts(h.w, watcher), 2);
    qw_free(h.w);

    /* a door kernel's write is logged as a set value too */
    House d = make_house(true);
    qw_log_changes(d.w, true);
    OK(qw_run(d.w, 100 * SEC));
    log = qw_changes(d.w, &n);
    OK(qw_find(d.w, "door.state", &state));
    bool found = false;
    for (size_t i = 0; i < n; i++)
        if (log[i].cell == state) found = log[i].after.lo == (int64_t) door_mask(&d, "open");
    CHECK(found);
    qw_free(d.w);
}

/* A derived set cell (ADR 0022 section 3): a truth value that propagators
 * give. It is not state: it takes no input, and qw_hash and a snapshot do
 * not cover it. */
void test_world_derived_set_cell(void)
{
    QWorld *pair[2] = { qw_new(1, -6), qw_new(1, -6) };
    QnCell seen[2], lit[2], open[2];
    QwPhen p;
    for (int k = 0; k < 2; k++) {
        QWorld *w = pair[k];
        OK(qw_phenomenon(w, "room", &p));
        OK(qw_set_cell(w, "room.seen", qw_boolean(w), 3, &seen[k]));
        OK(qw_set_cell(w, "room.open", qw_boolean(w), 3, &open[k]));
        OK(qw_derived_set_cell(w, "room.lit", qw_boolean(w), 3, &lit[k]));
        CHECK(qn_and(qw_net(w), lit[k], seen[k], open[k]) == QN_OK);
    }
    QWorld *w = pair[0], *other = pair[1];
    /* the errors of a set cell */
    QnCell c;
    QwKind energy;
    OK(qw_kind(w, "energy", "m^2 kg s^-2", -3, true, &energy));
    CHECK(qw_derived_set_cell(w, "room.bad", energy, 1, &c) == QW_ETYPE);
    CHECK(qw_derived_set_cell(w, "room.bad", qw_boolean(w), 4, &c) == QW_ETYPE);
    CHECK(qw_derived_set_cell(w, "room.lit", qw_boolean(w), 3, &c) == QW_EDUP);
    OK(qw_derived_set_cell(w, "room.spare", qw_boolean(w), 3, NULL));
    OK(qw_kind(other, "energy", "m^2 kg s^-2", -3, true, &energy));
    OK(qw_derived_set_cell(other, "room.spare", qw_boolean(other), 3, NULL));

    OK(qw_run(w, 0));
    OK(qw_run(other, 0));
    CHECK(qw_get_set(w, "room.lit") == 3);
    CHECK(qw_input_set(w, "room.lit", 2) == QW_EOWNER);
    CHECK(strstr(qw_error(w), "'room.lit' is derived: a law gives its value") != NULL);
    uint64_t unknown = qw_hash(w);
    CHECK(qw_hash(other) == unknown);
    OK(qw_input_set(w, "room.seen", 2));
    OK(qw_input_set(w, "room.open", 2));
    CHECK(qw_get_set(w, "room.lit") == 2);
    CHECK(qw_hash(w) != unknown);

    /* a snapshot has the inputs, and the derived cell gets its value again from the propagator */
    void *bytes = NULL;
    size_t len = 0;
    OK(qw_snapshot(w, &bytes, &len));
    OK(qw_restore(other, bytes, len));
    free(bytes);
    CHECK(qw_hash(other) == qw_hash(w));
    CHECK(qw_get_set(other, "room.lit") == 2 && qw_get_set(other, "room.seen") == 2);
    OK(qw_input_set(other, "room.open", 1));
    CHECK(qw_get_set(other, "room.lit") == 1);
    qw_free(w);
    qw_free(other);
}
