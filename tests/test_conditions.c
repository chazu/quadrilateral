#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quad/kernels.h"
#include "quad/world.h"
#include "test.h"

/* The conditions of transitions, the conditions of the application, and the
 * record of the calls, through the C API (docs/adr/0022). tests/test_detail.c
 * has the conditions from source. */

#define OK(x) CHECK((x) == QW_OK)
#define SEC 1000000LL
#define HAS(text, part) CHECK(strstr((text), (part)) != NULL)

enum { FALSE_ONLY = 1, TRUE_ONLY = 2, EITHER = 3 };

/* roomA is one warm lump, and a wall joins it to roomB. roomB, roomC and
 * roomD each have the models coarse and fine, a boolean `visible`, and the
 * transitions refine and abstract. roomB and roomC have the type #DetailRoom,
 * and roomD has #BigRoom. */
typedef struct {
    QWorld   *w;
    QwPhen    b, c, d;
    QkKernel *diff, *refine, *abstract;
} World;

static void lump(QWorld *w, const char *prefix, QwKind energy, QwKind cap, QwKind temp, int64_t cap_mJK, int64_t energy_mJ)
{
    char path[128];
    QnCell e, c, t;
    snprintf(path, sizeof path, "%s.energy", prefix);
    OK(qw_cell(w, path, energy, qi_point(energy_mJ), &e));
    snprintf(path, sizeof path, "%s.capacity", prefix);
    OK(qw_cell(w, path, cap, qi_point(cap_mJK), &c));
    snprintf(path, sizeof path, "%s.temperature", prefix);
    OK(qw_derived_cell(w, path, temp, qi_top(), &t));
    CHECK(qn_product(qw_net(w), e, c, t, NULL) == QN_OK);
}

static QwPhen detail_room(World *m, const char *name, const char *type, QwKind energy, QwKind cap, QwKind temp, QwKind cond,
                          QwKind ratio)
{
    QWorld *w = m->w;
    QwPhen p;
    char path[128], target[128];
    const int64_t c = 10000 * 1000;
    OK(qw_phenomenon(w, name, &p));
    OK(qw_set_type(w, p, type));
    OK(qw_model(w, p, "coarse"));
    OK(qw_model(w, p, "fine"));
    OK(qw_model_begin(w, p, "coarse"));
    snprintf(path, sizeof path, "%s.coarse", name);
    lump(w, path, energy, cap, temp, c, c / 100 * 29315);
    qw_model_end(w);
    OK(qw_model_begin(w, p, "fine"));
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "%s.fine.c%d", name, i);
        lump(w, path, energy, cap, temp, c / 4, 0);
    }
    qw_model_end(w);
    snprintf(path, sizeof path, "%s.fine.conductance", name);
    OK(qw_cell(w, path, cond, qi_point(100 * 1000), NULL));
    snprintf(path, sizeof path, "%s.fine.tolerance", name);
    OK(qw_cell(w, path, ratio, qi_point(50000000), NULL));
    snprintf(path, sizeof path, "%s.spread", name);
    OK(qw_cell(w, path, temp, qi_point(0), NULL));
    snprintf(path, sizeof path, "%s.visible", name);
    OK(qw_set_cell(w, path, qw_boolean(w), FALSE_ONLY, NULL));
    snprintf(target, sizeof target, "%s.coarse", name);
    OK(qw_port(w, p, "west", "coarse", target));
    snprintf(target, sizeof target, "%s.fine.c0", name);
    OK(qw_port(w, p, "west", "fine", target));
    OK(qw_model_attach(w, p, "fine", m->diff));
    OK(qw_transition(w, p, "refine", "coarse", "fine", m->refine));
    OK(qw_transition(w, p, "abstract", "fine", "coarse", m->abstract));
    return p;
}

/* rule: the rooms get the conditions of the content: refine while visible,
 * abstract while not visible. */
static World make(uint64_t seed, bool rule)
{
    World m = { qw_new(seed, -6), 0, 0, 0, qk_diffusion_new(4), qk_refine_new(4), qk_abstract_new(4) };
    QwKind energy, temp, cap, cond, ratio;
    QwPhen a, wall;
    const int64_t c = 10000 * 1000;
    OK(qw_kind(m.w, "energy", "J", -3, true, &energy));
    OK(qw_kind(m.w, "temperature", "K", -6, false, &temp));
    OK(qw_kind(m.w, "capacity", "J/K", -3, false, &cap));
    OK(qw_kind(m.w, "conductance", "W/K", -3, false, &cond));
    OK(qw_kind(m.w, "ratio", "", -9, false, &ratio));
    OK(qw_phenomenon(m.w, "roomA", &a));
    lump(m.w, "roomA", energy, cap, temp, c, c / 100 * 30315);
    m.b = detail_room(&m, "roomB", "#DetailRoom", energy, cap, temp, cond, ratio);
    m.c = detail_room(&m, "roomC", "#DetailRoom", energy, cap, temp, cond, ratio);
    m.d = detail_room(&m, "roomD", "#BigRoom", energy, cap, temp, cond, ratio);
    OK(qw_phenomenon(m.w, "wall", &wall));
    OK(qw_cell(m.w, "wall.conductance", cond, qi_point(10 * 1000), NULL));
    OK(qw_cell(m.w, "wall.tolerance", ratio, qi_point(10000000), NULL));
    OK(qw_connect(m.w, wall, "hot", "roomA"));
    OK(qw_connect(m.w, wall, "cold", "roomB.west"));
    OK(qw_attach(m.w, wall, qk_conduction()));
    if (rule) {
        const QwPhen rooms[3] = { m.b, m.c, m.d };
        for (int i = 0; i < 3; i++) {
            QwCondition seen = qw_when_true("visible"), unseen = qw_when_false("visible");
            OK(qw_transition_when(m.w, rooms[i], "refine", &seen));
            OK(qw_transition_when(m.w, rooms[i], "abstract", &unseen));
        }
    }
    return m;
}

static void drop(World *m)
{
    qw_free(m->w);
    qk_free(m->diff);
    qk_free(m->refine);
    qk_free(m->abstract);
}

static bool fine(const World *m, QwPhen p) { return !strcmp(qw_active_model(m->w, p), "fine"); }

/* A condition of the content starts a switch at a boundary, and two rooms
 * with different inputs make different decisions with one rule. */
void test_condition_source(void)
{
    World m = make(1, true);
    QwCondition got;
    QwLevel level;
    OK(qw_condition(m.w, m.b, "refine", &got, &level));
    CHECK(level == QW_LEVEL_SOURCE && !strcmp(got.path, "visible") && got.mask == TRUE_ONLY);
    CHECK(!strcmp(qw_type(m.w, m.b), "#DetailRoom") && !strcmp(qw_type(m.w, m.d), "#BigRoom") && !strcmp(qw_type(m.w, 0), ""));
    CHECK(!strcmp(qw_type(m.w, 99), ""));

    /* nobody looks: each room stays coarse */
    OK(qw_run(m.w, 100 * SEC));
    CHECK(!fine(&m, m.b) && !fine(&m, m.c) && !fine(&m, m.d));
    CHECK_I64((int64_t) qw_switches(m.w), 0);

    /* somebody looks at roomB. The switch is at the start of the next run, at the same time */
    OK(qw_input_set(m.w, "roomB.visible", TRUE_ONLY));
    CHECK(!fine(&m, m.b));
    uint64_t b0 = qw_boundaries(m.w);
    OK(qw_run(m.w, 100 * SEC));
    CHECK(fine(&m, m.b) && !fine(&m, m.c) && !fine(&m, m.d));       /* one rule, and another input */
    CHECK_I64((int64_t) qw_switches(m.w), 1);
    CHECK_I64(qw_now(m.w), 100 * SEC);
    CHECK_I64((int64_t) (qw_boundaries(m.w) - b0), 0);
    OK(qw_run(m.w, 200 * SEC));
    CHECK(fine(&m, m.b));
    CHECK_I64((int64_t) qw_switches(m.w), 1);                       /* the condition stays true, and nothing more happens */

    /* an input that is not known changes nothing */
    OK(qw_input_set(m.w, "roomB.visible", EITHER));
    OK(qw_input_set(m.w, "roomC.visible", EITHER));
    OK(qw_run(m.w, 250 * SEC));
    CHECK(fine(&m, m.b) && !fine(&m, m.c));

    /* nobody looks at roomB: it becomes coarse again, with its energy */
    int64_t energy = 0;
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof path, "roomB.fine.c%d.energy", i);
        energy += qw_get(m.w, path).lo;
    }
    OK(qw_input_set(m.w, "roomB.visible", FALSE_ONLY));
    OK(qw_run(m.w, 250 * SEC));
    CHECK(!fine(&m, m.b));
    CHECK_I64(qw_get(m.w, "roomB.coarse.energy").lo, energy);
    CHECK_I64((int64_t) qw_switches(m.w), 2);

    /* a condition becomes true in the middle of a run: the switch is at the first boundary after it. The
     * room becomes fine when the coarse temperature is 0.3 K above its value now */
    const int64_t limit = qw_get(m.w, "roomB.coarse.temperature").hi + 300000;
    QwCondition warm = qw_when_in("coarse.temperature", (QInterval) { limit, Q_INF });
    OK(qw_transition_when(m.w, m.b, "refine", &warm));
    OK(qw_condition(m.w, m.b, "refine", &got, &level));
    CHECK(level == QW_LEVEL_SOURCE && !strcmp(got.path, "coarse.temperature") && got.range.lo == limit && !got.mask);
    QwCondition never = qw_never();
    OK(qw_transition_when(m.w, m.b, "abstract", &never));
    OK(qw_run(m.w, 251 * SEC));
    CHECK(!fine(&m, m.b));
    OK(qw_run(m.w, 600 * SEC));
    CHECK(fine(&m, m.b));
    CHECK_I64((int64_t) qw_switches(m.w), 3);
    drop(&m);

    /* the same run with stops finds the switch at the same time: a world with no stop between */
    World a = make(1, true), b = make(1, true);
    World *both[2] = { &a, &b };
    for (int i = 0; i < 2; i++) {
        QwCondition w2 = qw_when_in("coarse.temperature", (QInterval) { 294200000, Q_INF });
        OK(qw_transition_when(both[i]->w, both[i]->b, "refine", &w2));
        OK(qw_transition_when(both[i]->w, both[i]->b, "abstract", &never));
    }
    OK(qw_run(a.w, 600 * SEC));
    int64_t at = 0;
    for (int t = 5; t <= 600 && !at; t += 5) {
        OK(qw_run(b.w, t * SEC));
        if (fine(&b, b.b)) at = t;
    }
    CHECK(at > 100 && at < 600);
    /* before that time the temperature was below the limit, in a world that does not switch */
    World plain = make(1, false);
    OK(qw_run(plain.w, (at - 5) * SEC));
    CHECK(qw_get(plain.w, "roomB.coarse.temperature").lo < 294200000);
    OK(qw_run(plain.w, at * SEC));
    CHECK(qw_get(plain.w, "roomB.coarse.temperature").lo >= 294200000);
    CHECK(fine(&a, a.b));
    drop(&a);
    drop(&b);
    drop(&plain);
}

/* The application replaces a condition for an instance and for an exact
 * type. The instance has precedence over the type, and the type over the
 * content. A removal shows the level below. */
void test_condition_override(void)
{
    World m = make(1, true);
    QwCondition got, never = qw_never(), seen = qw_when_true("visible"), cold = qw_when_in("coarse.temperature", (QInterval) { Q_NINF, 400000000 });
    QwLevel level;
    for (int i = 0; i < 3; i++) OK(qw_input_set(m.w, i == 0 ? "roomB.visible" : i == 1 ? "roomC.visible" : "roomD.visible", TRUE_ONLY));

    /* the instance: roomB never becomes fine by itself */
    OK(qw_override_condition(m.w, m.b, "refine", &never));
    OK(qw_condition(m.w, m.b, "refine", &got, &level));
    CHECK(level == QW_LEVEL_INSTANCE && got.path == NULL);
    OK(qw_condition(m.w, m.c, "refine", &got, &level));
    CHECK(level == QW_LEVEL_SOURCE);
    OK(qw_run(m.w, 10 * SEC));
    CHECK(!fine(&m, m.b) && fine(&m, m.c) && fine(&m, m.d));

    /* the application can still switch roomB */
    OK(qw_switch_to(m.w, m.b, "fine"));
    OK(qw_run(m.w, 20 * SEC));
    CHECK(fine(&m, m.b));
    OK(qw_switch_to(m.w, m.b, "coarse"));

    /* the exact type: each #DetailRoom becomes coarse when it is fine. roomD is a #BigRoom, and it keeps the rule
     * of the content. roomB keeps the condition of the instance for refine */
    QwCondition always = qw_when_atoms("visible", EITHER);
    OK(qw_override_type_condition(m.w, "#DetailRoom", "abstract", &always));
    OK(qw_condition(m.w, m.c, "abstract", &got, &level));
    CHECK(level == QW_LEVEL_TYPE && got.mask == EITHER);
    OK(qw_condition(m.w, m.d, "abstract", &got, &level));
    CHECK(level == QW_LEVEL_SOURCE && got.mask == FALSE_ONLY);
    OK(qw_override_type_condition(m.w, "#DetailRoom", "refine", &never));
    OK(qw_condition(m.w, m.b, "refine", &got, &level));
    CHECK(level == QW_LEVEL_INSTANCE);
    OK(qw_condition(m.w, m.c, "refine", &got, &level));
    CHECK(level == QW_LEVEL_TYPE && got.path == NULL);
    OK(qw_run(m.w, 30 * SEC));
    CHECK(!fine(&m, m.b) && !fine(&m, m.c) && fine(&m, m.d));       /* a subtype has its own conditions */

    /* the instance has precedence over the type */
    OK(qw_override_condition(m.w, m.c, "refine", &seen));
    OK(qw_override_condition(m.w, m.c, "abstract", &never));
    OK(qw_run(m.w, 40 * SEC));
    CHECK(fine(&m, m.c) && !fine(&m, m.b));

    /* the removal of the instance shows the type again, and the removal of the type shows the content */
    OK(qw_clear_condition(m.w, m.c, "refine"));
    OK(qw_clear_condition(m.w, m.c, "abstract"));
    OK(qw_condition(m.w, m.c, "refine", &got, &level));
    CHECK(level == QW_LEVEL_TYPE);
    OK(qw_run(m.w, 50 * SEC));
    CHECK(!fine(&m, m.c));                                          /* the type: abstract always */
    OK(qw_clear_type_condition(m.w, "#DetailRoom", "abstract"));
    OK(qw_clear_type_condition(m.w, "#DetailRoom", "refine"));
    OK(qw_condition(m.w, m.c, "refine", &got, &level));
    CHECK(level == QW_LEVEL_SOURCE && got.mask == TRUE_ONLY);
    OK(qw_run(m.w, 60 * SEC));
    CHECK(fine(&m, m.c) && !fine(&m, m.b));                         /* the content again; roomB has its instance */
    OK(qw_clear_condition(m.w, m.b, "refine"));
    OK(qw_run(m.w, 70 * SEC));
    CHECK(fine(&m, m.b));

    /* a condition of the type with a range, for each room of the type */
    OK(qw_override_type_condition(m.w, "#DetailRoom", "abstract", &cold));
    OK(qw_run(m.w, 80 * SEC));
    /* the path is a cell of the coarse model, which has no value while the fine model is active: unknown */
    CHECK(fine(&m, m.b) && fine(&m, m.c));

    /* the errors change nothing */
    uint64_t h = qw_hash(m.w);
    QwCondition bad_path = qw_when_true("nothing"), bad_set = qw_when_in("visible", (QInterval) { 0, 1 });
    QwCondition bad_range = qw_when_true("spread"), bad_atoms = qw_when_atoms("visible", 4);
    CHECK(qw_override_condition(m.w, m.b, "jump", &seen) == QW_ENAME);
    HAS(qw_error(m.w), "roomB: no transition 'jump'");
    CHECK(qw_override_condition(m.w, 99, "refine", &seen) == QW_ENAME);
    CHECK(qw_override_condition(m.w, m.b, "refine", &bad_path) == QW_ENAME);
    HAS(qw_error(m.w), "roomB: the condition reads 'roomB.nothing', which is not a cell");
    CHECK(qw_override_condition(m.w, m.b, "refine", &bad_set) == QW_ETYPE);
    HAS(qw_error(m.w), "roomB: the condition on the set cell 'roomB.visible' needs atoms of that cell");
    CHECK(qw_override_condition(m.w, m.b, "refine", &bad_atoms) == QW_ETYPE);
    CHECK(qw_override_condition(m.w, m.b, "refine", &bad_range) == QW_ETYPE);
    HAS(qw_error(m.w), "roomB: the condition on 'roomB.spread' needs a range, and no atoms");
    CHECK(qw_transition_when(m.w, m.b, "refine", &bad_path) == QW_ENAME);
    CHECK(qw_transition_when(m.w, m.b, "jump", &seen) == QW_ENAME);
    CHECK(qw_clear_condition(m.w, m.b, "refine") == QW_ENAME);
    HAS(qw_error(m.w), "roomB: the transition 'refine' has no condition of the instance");
    CHECK(qw_clear_condition(m.w, m.b, "jump") == QW_ENAME);
    CHECK(qw_override_type_condition(m.w, "#Nothing", "refine", &seen) == QW_ENAME);
    HAS(qw_error(m.w), "no phenomenon of the type '#Nothing' has a transition 'refine'");
    CHECK(qw_override_type_condition(m.w, "#DetailRoom", "jump", &seen) == QW_ENAME);
    CHECK(qw_override_type_condition(m.w, "#DetailRoom", "refine", &bad_path) == QW_ENAME);
    CHECK(qw_clear_type_condition(m.w, "#BigRoom", "refine") == QW_ENAME);
    HAS(qw_error(m.w), "the type '#BigRoom' has no condition for the transition 'refine'");
    CHECK(qw_condition(m.w, m.b, "jump", &got, &level) == QW_ENAME && qw_condition(m.w, 99, "refine", &got, &level) == QW_ENAME);
    CHECK(qw_set_type(m.w, 99, "#X") == QW_ENAME);
    CHECK(qw_hash(m.w) == h);
    drop(&m);
}

/* Two transitions from the active model that are true together stop the
 * run. Two conditions that are true in turn cost one switch at each
 * boundary, and the run goes on. */
void test_condition_conflict(void)
{
    World m = make(1, false);
    QwCondition seen = qw_when_true("visible"), always = qw_when_atoms("visible", EITHER);
    /* a third model is not there, so make a second transition from coarse with another name: not possible for
     * one pair. Use the two transitions of roomB in turn */
    OK(qw_transition_when(m.w, m.b, "refine", &always));
    OK(qw_transition_when(m.w, m.b, "abstract", &always));
    OK(qw_run(m.w, 0));
    CHECK(fine(&m, m.b));
    CHECK_I64((int64_t) qw_switches(m.w), 1);                       /* one switch at the start, and not two */
    uint64_t b0 = qw_boundaries(m.w);
    OK(qw_run(m.w, 30 * SEC));
    /* one switch at each boundary, and one more at the start of the run */
    CHECK_I64((int64_t) qw_switches(m.w), 1 + 1 + (int64_t) (qw_boundaries(m.w) - b0));
    CHECK(qw_boundaries(m.w) - b0 > 3);
    drop(&m);

    /* only the second transition of a room has a condition */
    m = make(1, false);
    QwCondition unseen = qw_when_false("visible");
    OK(qw_override_condition(m.w, m.b, "abstract", &unseen));
    OK(qw_switch_to(m.w, m.b, "fine"));
    OK(qw_run(m.w, 5 * SEC));
    CHECK(!fine(&m, m.b));                                          /* nobody looks: coarse again */
    CHECK_I64((int64_t) qw_switches(m.w), 2);
    OK(qw_input_set(m.w, "roomB.visible", TRUE_ONLY));
    OK(qw_run(m.w, 10 * SEC));
    CHECK(!fine(&m, m.b));                                          /* refine has no condition */
    (void) seen;
    drop(&m);
}

/* The conditions of the application are state: the hash and a snapshot have
 * them. */
void test_condition_state(void)
{
    World a = make(1, true), b = make(1, true), c = make(1, true);
    QwCondition never = qw_never(), warm = qw_when_in("west.temperature", (QInterval) { 294200000, Q_INF });
    CHECK(qw_hash(a.w) == qw_hash(b.w));
    OK(qw_override_condition(a.w, a.b, "refine", &never));
    CHECK(qw_hash(a.w) != qw_hash(b.w));
    OK(qw_override_condition(b.w, b.b, "refine", &never));
    CHECK(qw_hash(a.w) == qw_hash(b.w));
    OK(qw_override_type_condition(a.w, "#BigRoom", "refine", &warm));
    CHECK(qw_hash(a.w) != qw_hash(b.w));
    OK(qw_override_type_condition(b.w, "#BigRoom", "refine", &never));
    CHECK(qw_hash(a.w) != qw_hash(b.w));                            /* another condition */
    OK(qw_override_type_condition(b.w, "#BigRoom", "refine", &warm));
    CHECK(qw_hash(a.w) == qw_hash(b.w));
    /* a removal names the type and the transition: the other condition of the type stays */
    QwCondition got0;
    QwLevel level0;
    OK(qw_override_type_condition(a.w, "#BigRoom", "abstract", &never));
    OK(qw_clear_type_condition(a.w, "#BigRoom", "abstract"));
    OK(qw_condition(a.w, a.d, "refine", &got0, &level0));
    CHECK(level0 == QW_LEVEL_TYPE && !strcmp(got0.path, "west.temperature"));
    OK(qw_condition(a.w, a.d, "abstract", &got0, &level0));
    CHECK(level0 == QW_LEVEL_SOURCE);
    CHECK(qw_hash(a.w) == qw_hash(b.w));
    OK(qw_clear_condition(a.w, a.b, "refine"));
    OK(qw_clear_condition(b.w, b.b, "refine"));
    OK(qw_clear_type_condition(a.w, "#BigRoom", "refine"));
    OK(qw_clear_type_condition(b.w, "#BigRoom", "refine"));
    CHECK(qw_hash(a.w) == qw_hash(c.w));

    /* a snapshot has the conditions, and a restore replaces those of the world */
    OK(qw_override_condition(a.w, a.c, "refine", &never));
    OK(qw_override_type_condition(a.w, "#DetailRoom", "refine", &warm));
    OK(qw_override_condition(c.w, c.d, "abstract", &never));        /* the restore removes this one */
    OK(qw_run(a.w, 100 * SEC));
    void *bytes = NULL;
    size_t len = 0;
    OK(qw_snapshot(a.w, &bytes, &len));
    OK(qw_restore(c.w, bytes, len));
    CHECK(qw_hash(a.w) == qw_hash(c.w));
    QwCondition got;
    QwLevel level;
    OK(qw_condition(c.w, c.c, "refine", &got, &level));
    CHECK(level == QW_LEVEL_INSTANCE && got.path == NULL);
    OK(qw_condition(c.w, c.b, "refine", &got, &level));
    CHECK(level == QW_LEVEL_TYPE && !strcmp(got.path, "west.temperature") && got.range.lo == 294200000);
    OK(qw_condition(c.w, c.d, "abstract", &got, &level));
    CHECK(level == QW_LEVEL_SOURCE);
    /* the two worlds go on in the same way: roomB becomes fine when its port is warm */
    OK(qw_run(a.w, 600 * SEC));
    OK(qw_run(c.w, 600 * SEC));
    CHECK(qw_hash(a.w) == qw_hash(c.w));
    CHECK(fine(&a, a.b) && fine(&c, c.b) && !fine(&c, c.c));
    CHECK_I64((int64_t) qw_switches(c.w), (int64_t) qw_switches(a.w));
    /* a snapshot that is cut in the conditions is refused, and the world keeps its state */
    uint64_t h = qw_hash(c.w);
    CHECK(qw_restore(c.w, bytes, len - 8) == QW_ESNAPSHOT);
    CHECK(qw_restore(c.w, bytes, len - 40) == QW_ESNAPSHOT);
    CHECK(qw_hash(c.w) == h);
    /* a snapshot with a condition for a phenomenon or a transition that the world does not have */
    {
        World s1 = make(1, true), s2 = make(1, true);
        void *sb = NULL;
        size_t sl = 0;
        OK(qw_override_condition(s1.w, s1.c, "refine", &never));
        OK(qw_snapshot(s1.w, &sb, &sl));
        /* the end of the bytes: the count 1, the phenomenon, the transition, the condition "never" in 5 words,
         * and the count 0 of the types */
        uint8_t *raw = sb;
        CHECK(raw[sl - 72] == 1 && raw[sl - 64] == (uint8_t) s1.c && raw[sl - 56] == 0);
        uint64_t h2 = qw_hash(s2.w);
        raw[sl - 64] = 99;                                          /* no such phenomenon */
        CHECK(qw_restore(s2.w, sb, sl) == QW_ESNAPSHOT);
        raw[sl - 64] = (uint8_t) s1.c;
        raw[sl - 56] = 7;                                           /* no such transition */
        CHECK(qw_restore(s2.w, sb, sl) == QW_ESNAPSHOT);
        CHECK(qw_hash(s2.w) == h2);
        raw[sl - 56] = 1;                                           /* the other transition of the room */
        OK(qw_restore(s2.w, sb, sl));
        QwCondition g2;
        QwLevel l2;
        OK(qw_condition(s2.w, s2.c, "abstract", &g2, &l2));
        CHECK(l2 == QW_LEVEL_INSTANCE);
        OK(qw_condition(s2.w, s2.c, "refine", &g2, &l2));
        CHECK(l2 == QW_LEVEL_SOURCE);
        free(sb);
        drop(&s1);
        drop(&s2);
    }
    /* a world with no rule has another structure */
    World plain = make(1, false);
    CHECK(qw_restore(plain.w, bytes, len) == QW_ESNAPSHOT);
    free(bytes);
    drop(&a);
    drop(&b);
    drop(&c);
    drop(&plain);
}

/* ADR 0021, acceptance test 5, the replay: the world records each call of
 * the application. The same calls in a new world give the same state hash
 * after each call, and each switch at the same boundary. */
void test_condition_replay(void)
{
    World a = make(7, true), b = make(7, true);
    QwCondition never = qw_never(), warm = qw_when_in("coarse.temperature", (QInterval) { 294200000, Q_INF });
    uint64_t hashes[32], bounds[32], switches[32];
    size_t n = 0;
    qw_record(a.w, true);
#define STEP(call)                                              \
    do {                                                        \
        OK(call);                                               \
        hashes[n] = qw_hash(a.w);                               \
        bounds[n] = qw_boundaries(a.w);                         \
        switches[n++] = qw_switches(a.w);                       \
    } while (0)
    STEP(qw_run(a.w, 50 * SEC));
    STEP(qw_input_set(a.w, "roomC.visible", TRUE_ONLY));
    STEP(qw_run(a.w, 120 * SEC));
    STEP(qw_override_type_condition(a.w, "#DetailRoom", "refine", &warm));
    STEP(qw_override_condition(a.w, a.c, "abstract", &never));
    STEP(qw_override_condition(a.w, a.d, "abstract", &never));
    STEP(qw_run(a.w, 400 * SEC));
    STEP(qw_switch_to(a.w, a.d, "fine"));
    STEP(qw_input(a.w, "roomD.fine.conductance", qi_point(50 * 1000)));
    STEP(qw_run(a.w, 450 * SEC));
    STEP(qw_clear_condition(a.w, a.c, "abstract"));
    STEP(qw_clear_type_condition(a.w, "#DetailRoom", "refine"));
    STEP(qw_switch(a.w, a.d, "coarse", a.abstract));
    STEP(qw_run(a.w, 600 * SEC));
#undef STEP
    /* a call that fails is not in the record, and a switch that the world made is not a call */
    CHECK(qw_input_set(a.w, "roomC.nothing", TRUE_ONLY) == QW_ENAME);
    CHECK(qw_switch_to(a.w, a.d, "coarse") == QW_EDUP);
    size_t ncalls = 0;
    const QwCall *calls = qw_calls(a.w, &ncalls);
    CHECK_I64((int64_t) ncalls, (int64_t) n);
    CHECK(qw_switches(a.w) > 3);                                    /* the rooms also made switches by their conditions */
    CHECK(calls[0].kind == QW_CALL_RUN && calls[0].time == 50 * SEC && calls[0].target == NULL);
    CHECK(calls[1].kind == QW_CALL_INPUT_SET && !strcmp(calls[1].target, "roomC.visible") && calls[1].mask == TRUE_ONLY &&
          calls[1].time == 50 * SEC);
    CHECK(calls[3].kind == QW_CALL_TYPE_CONDITION && !strcmp(calls[3].target, "#DetailRoom") && !strcmp(calls[3].name, "refine") &&
          !calls[3].clear && !strcmp(calls[3].cond.path, "coarse.temperature") && calls[3].cond.range.lo == 294200000);
    CHECK(calls[4].kind == QW_CALL_CONDITION && !strcmp(calls[4].target, "roomC") && calls[4].cond.path == NULL && !calls[4].clear);
    CHECK(calls[7].kind == QW_CALL_SWITCH && !strcmp(calls[7].target, "roomD") && !strcmp(calls[7].name, "fine"));
    CHECK(calls[8].kind == QW_CALL_INPUT && calls[8].value.lo == 50000 && calls[8].time == 400 * SEC);
    CHECK(calls[10].kind == QW_CALL_CONDITION && calls[10].clear && calls[11].kind == QW_CALL_TYPE_CONDITION && calls[11].clear);
    CHECK(calls[12].kind == QW_CALL_SWITCH && !strcmp(calls[12].name, "coarse"));

    /* the replay, one call at a time: the same hash, the same boundaries and the same switches after each call */
    qw_record(b.w, true);
    for (size_t i = 0; i < ncalls; i++) {
        OK(qw_replay(b.w, &calls[i], 1));
        CHECK(qw_hash(b.w) == hashes[i]);
        CHECK_I64((int64_t) qw_boundaries(b.w), (int64_t) bounds[i]);
        CHECK_I64((int64_t) qw_switches(b.w), (int64_t) switches[i]);
    }
    /* the record of the new world is the record of the first world */
    size_t nb = 0;
    const QwCall *again = qw_calls(b.w, &nb);
    CHECK_I64((int64_t) nb, (int64_t) ncalls);
    for (size_t i = 0; i < nb && i < ncalls; i++) CHECK(again[i].kind == calls[i].kind && again[i].time == calls[i].time);

    /* the whole record in one call, in a third world */
    World c = make(7, true);
    OK(qw_replay(c.w, calls, ncalls));
    CHECK(qw_hash(c.w) == qw_hash(a.w));
    /* a record does not fit a world at another time, or a world with other names */
    CHECK(qw_replay(c.w, &calls[1], 1) == QW_ETIME);
    HAS(qw_error(c.w), "the record has a call at the time 50000000, and the world is at the time 600000000");
    QwCall odd = calls[7];
    odd.time = qw_now(c.w);
    odd.target = "roomZ";
    CHECK(qw_replay(c.w, &odd, 1) == QW_ENAME);
    HAS(qw_error(c.w), "the record names the phenomenon 'roomZ'");
    drop(&c);

    /* with the record off, the world keeps the entries and adds none */
    qw_record(a.w, false);
    OK(qw_run(a.w, 610 * SEC));
    (void) qw_calls(a.w, &nb);
    CHECK_I64((int64_t) nb, (int64_t) ncalls);
    drop(&a);
    drop(&b);

    /* the start and the end of an instance are in the record */
    QWorld *w = qw_new(1, -6);
    QwPhen inst;
    OK(qw_instance(w, "spark(1)", &inst));
    qw_record(w, true);
    OK(qw_start(w, inst));
    OK(qw_run(w, 5 * SEC));
    OK(qw_end(w, inst));
    calls = qw_calls(w, &ncalls);
    CHECK_I64((int64_t) ncalls, 3);
    CHECK(calls[0].kind == QW_CALL_START && !strcmp(calls[0].target, "spark(1)") && calls[2].kind == QW_CALL_END && calls[2].time == 5 * SEC);
    QWorld *w2 = qw_new(1, -6);
    OK(qw_instance(w2, "spark(1)", &inst));
    OK(qw_replay(w2, calls, ncalls));
    CHECK(qw_hash(w2) == qw_hash(w) && !qw_alive(w2, inst) && qw_starts(w2) == 1 && qw_ends(w2) == 1);
    qw_free(w);
    qw_free(w2);
}
