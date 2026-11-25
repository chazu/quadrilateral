#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quad/ir.h"
#include "quad/kernels.h"
#include "quad/world.h"
#include "test.h"

/* Stage 4 of the front end, first part: the models of a phenomenon, the
 * ports of the active model, and the maps, in source (ADR 0021 sections 1 to
 * 5 and 8). The types are in tests/data/elab/thermal.quad and detail.quad. */

#define OK(x) CHECK((x) == QW_OK)
#define SEC 1000000LL

#define HAS(text, part)                                                                \
    do {                                                                               \
        const char *t_ = (text), *p_ = (part);                                         \
        t_checks++;                                                                    \
        if (!strstr(t_, p_)) {                                                         \
            t_failures++;                                                              \
            fprintf(stderr, "%s:%d:\n  got    %s\n  wanted a text with %s\n", __FILE__, __LINE__, t_, p_); \
        }                                                                              \
    } while (0)

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *s = malloc((size_t) n + 1);
    size_t got = s ? fread(s, 1, (size_t) n, f) : 0;
    fclose(f);
    if (!s) return NULL;
    s[got] = 0;
    return s;
}

static const char *types(int which)
{
    static char *text[2];
    static const char *const files[2] = { "tests/data/elab/thermal.quad", "tests/data/elab/detail.quad" };
    if (!text[which]) text[which] = read_file(files[which]);
    CHECK(text[which] != NULL);
    return text[which] ? text[which] : "";
}

/* Compiles the thermal types and the detail room with `rest`. *diag gets the
 * diagnostics if it is not NULL. */
static QIr *compile_with(const char *rest, const QiOptions *opt, char **diag)
{
    const char *names[3] = { "thermal.quad", "detail.quad", "world.quad" }, *src[3] = { types(0), types(1), rest };
    return qi_compile(names, src, 3, opt, diag);
}

static QIr *compile(const char *rest)
{
    char *diag = NULL;
    QIr *ir = compile_with(rest, NULL, &diag);
    CHECK(ir != NULL);
    if (!ir) fprintf(stderr, "%s", diag);
    free(diag);
    return ir;
}

/* The diagnostics of a content, or "compiled". */
static const char *compile_error(const char *rest)
{
    static char out[4096];
    char *diag = NULL;
    QIr *ir = compile_with(rest, NULL, &diag);
    snprintf(out, sizeof out, "%s", ir ? "compiled" : diag);
    free(diag);
    qi_free(ir);
    return out;
}

static QWorld *build(const QIr *ir, uint64_t seed)
{
    QWorld *w = NULL;
    char *error = NULL;
    QwStatus s = qi_instantiate(ir, seed, NULL, &w, &error);
    CHECK(s == QW_OK && w != NULL);
    if (error) fprintf(stderr, "  %s\n", error);
    free(error);
    return w;
}

/* One detail room of the type `type`, with the coarse model at the start. */
#define ROOM(type)                                                                                      \
    "r: " type " & { coarse: { temperature: 293.15K, capacity: 10000J/K },\n"                          \
    "  fine: { c0: { capacity: 2500J/K }, c1: { capacity: 2500J/K }, c2: { capacity: 2500J/K },\n"       \
    "          c3: { capacity: 2500J/K }, conductance: 100W/K }, spread: 0K }\n"

/* --- the world of phase 5 --------------------------------------------------------------------------- */

#define MIXED(spread, model)                                                                            \
    "roomA: #LumpedRoom & { temperature: 303.15K, capacity: 10000J/K }\n"                                \
    "roomB: #DetailRoom & {\n"                                                                           \
    "    coarse: { " model " capacity: 10000J/K }\n"                                                     \
    "    fine:   { c0: { capacity: 2500J/K }, c1: { capacity: 2500J/K }, c2: { capacity: 2500J/K },\n"   \
    "              c3: { capacity: 2500J/K }, conductance: 100W/K, tolerance: 5pct }\n"                  \
    "    spread: " spread "\n"                                                                           \
    "}\n"                                                                                                \
    "wall: #ConductiveWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 1pct }\n"

/* The world of tests/test_models.c through the C API, with the names of the
 * source: the models coarse and fine. A temperature is a derived cell, as in
 * a world from source (ADR 0019). */
typedef struct {
    QWorld   *w;
    QwPhen    b;
    QkKernel *diff, *refine, *abstract;
} CWorld;

static void c_lump(QWorld *w, const char *prefix, QwKind energy, QwKind cap, QwKind temp, int64_t cap_mJK, int64_t energy_mJ)
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

static CWorld c_mixed(uint64_t seed, int64_t spread_uK)
{
    CWorld m = { qw_new(seed, -6), 0, qk_diffusion_new(4), qk_refine_new(4), qk_abstract_new(4) };
    QwKind energy, temp, cap, cond, ratio;
    QwPhen a, wall;
    const int64_t c = 10000 * 1000;
    OK(qw_kind(m.w, "energy", "J", -3, true, &energy));
    OK(qw_kind(m.w, "temperature", "K", -6, false, &temp));
    OK(qw_kind(m.w, "capacity", "J/K", -3, false, &cap));
    OK(qw_kind(m.w, "conductance", "W/K", -3, false, &cond));
    OK(qw_kind(m.w, "ratio", "", -9, false, &ratio));
    OK(qw_phenomenon(m.w, "roomA", &a));
    OK(qw_phenomenon(m.w, "roomB", &m.b));
    OK(qw_phenomenon(m.w, "wall", &wall));
    c_lump(m.w, "roomA", energy, cap, temp, c, c / 100 * 30315);
    OK(qw_model(m.w, m.b, "coarse"));
    OK(qw_model(m.w, m.b, "fine"));
    OK(qw_model_begin(m.w, m.b, "coarse"));
    c_lump(m.w, "roomB.coarse", energy, cap, temp, c, c / 100 * 29315);
    qw_model_end(m.w);
    OK(qw_model_begin(m.w, m.b, "fine"));
    for (int i = 0; i < 4; i++) {
        char prefix[64];
        snprintf(prefix, sizeof prefix, "roomB.fine.c%d", i);
        c_lump(m.w, prefix, energy, cap, temp, c / 4, 0);
    }
    qw_model_end(m.w);
    OK(qw_cell(m.w, "roomB.fine.conductance", cond, qi_point(100 * 1000), NULL));
    OK(qw_cell(m.w, "roomB.fine.tolerance", ratio, qi_point(50000000), NULL));
    OK(qw_cell(m.w, "roomB.spread", temp, qi_point(spread_uK), NULL));
    OK(qw_port(m.w, m.b, "west", "coarse", "roomB.coarse"));
    OK(qw_port(m.w, m.b, "west", "fine", "roomB.fine.c0"));
    OK(qw_model_attach(m.w, m.b, "fine", m.diff));
    OK(qw_cell(m.w, "wall.conductance", cond, qi_point(10 * 1000), NULL));
    OK(qw_cell(m.w, "wall.tolerance", ratio, qi_point(10000000), NULL));
    OK(qw_connect(m.w, wall, "hot", "roomA"));
    OK(qw_connect(m.w, wall, "cold", "roomB.west"));
    OK(qw_attach(m.w, wall, qk_conduction()));
    return m;
}

static void c_free(CWorld *m)
{
    qw_free(m->w);
    qk_free(m->diff);
    qk_free(m->refine);
    qk_free(m->abstract);
}

static const char *const STATE[] = {
    "roomA.energy",          "roomA.temperature",          "roomB.coarse.energy",   "roomB.coarse.temperature",
    "roomB.fine.c0.energy",  "roomB.fine.c0.temperature",  "roomB.fine.c1.energy",  "roomB.fine.c2.energy",
    "roomB.fine.c3.energy",  "roomB.fine.c3.temperature",  "roomB.west.temperature", "roomB.west.energy",
};

static void same_state(QWorld *src, QWorld *c)
{
    CHECK(qw_hash(src) == qw_hash(c));
    CHECK_I64((int64_t) qw_boundaries(src), (int64_t) qw_boundaries(c));
    CHECK_I64((int64_t) qw_advances(src), (int64_t) qw_advances(c));
    CHECK_I64((int64_t) qw_switches(src), (int64_t) qw_switches(c));
    for (size_t k = 0; k < sizeof STATE / sizeof STATE[0]; k++) {
        QInterval x = qw_get(src, STATE[k]), y = qw_get(c, STATE[k]);
        CHECK(x.lo == y.lo && x.hi == y.hi);
    }
}

static int64_t stock(QWorld *w, const char *path) { return qw_get(w, path).lo; }

/* The energy of the four cells of the fine model of the room `room`. */
static int64_t fine_of(QWorld *w, const char *room)
{
    int64_t sum = 0;
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof path, "%s.fine.c%d.energy", room, i);
        sum += stock(w, path);
    }
    return sum;
}

static int64_t fine_sum(QWorld *w) { return fine_of(w, "roomB"); }

/* ADR 0021, acceptance test 1: the world of phase 5 from source has the same
 * state hash as the world from C, at each time and at each switch. */
void test_detail_phase5(void)
{
    static const struct { const char *text; int64_t uK; } spreads[2] = { { "0.05K", 50000 }, { "0K", 0 } };
    for (int v = 0; v < 2; v++)
        for (uint64_t seed = 7; seed < 9; seed++) {
            QIr *ir = v == 0 ? compile(MIXED("0.05K", "temperature: 293.15K,")) : compile(MIXED("0K", "temperature: 293.15K,"));
            if (!ir) return;
            QWorld *src = build(ir, seed);
            CWorld c = c_mixed(seed, spreads[v].uK);
            if (src) {
                QwPhen b;
                OK(qw_find_phenomenon(src, "roomB", &b));
                CHECK(!strcmp(qw_active_model(src, b), "coarse"));
                const int64_t total = stock(src, "roomA.energy") + stock(src, "roomB.coarse.energy");
                same_state(src, c.w);
                const int before[] = { 100, 200 }, between[] = { 201, 300, 600 }, after[] = { 800, 1000 };
                for (int i = 0; i < 2; i++) {
                    OK(qw_run(src, before[i] * SEC));
                    OK(qw_run(c.w, before[i] * SEC));
                    same_state(src, c.w);
                }
                int64_t coarse = stock(src, "roomB.coarse.energy");
                OK(qi_switch(ir, src, "roomB", "fine"));
                OK(qw_switch(c.w, c.b, "fine", c.refine));
                same_state(src, c.w);
                CHECK(!strcmp(qw_active_model(src, b), "fine"));
                CHECK_I64(stock(src, "roomB.coarse.energy"), 0);
                CHECK_I64(fine_sum(src), coarse);                     /* the switch conserves the energy exactly */
                /* the port of the room now leads to the first cell */
                QInterval west = qw_get(src, "roomB.west.temperature"), c0 = qw_get(src, "roomB.fine.c0.temperature");
                CHECK(west.lo == c0.lo && west.hi == c0.hi && west.lo != Q_NINF);
                for (int i = 0; i < 3; i++) {
                    OK(qw_run(src, between[i] * SEC));
                    OK(qw_run(c.w, between[i] * SEC));
                    same_state(src, c.w);
                }
                int64_t fine = fine_sum(src);
                OK(qi_switch(ir, src, "roomB", "coarse"));
                OK(qw_switch(c.w, c.b, "coarse", c.abstract));
                same_state(src, c.w);
                CHECK_I64(fine_sum(src), 0);
                CHECK_I64(stock(src, "roomB.coarse.energy"), fine);
                for (int i = 0; i < 2; i++) {
                    OK(qw_run(src, after[i] * SEC));
                    OK(qw_run(c.w, after[i] * SEC));
                    same_state(src, c.w);
                }
                CHECK_I64(stock(src, "roomA.energy") + stock(src, "roomB.coarse.energy"), total);
                CHECK_I64((int64_t) qw_switches(src), 2);
                CHECK(qw_boundaries(src) > 100);
            }
            qw_free(src);
            c_free(&c);
            qi_free(ir);
        }
}

/* The same seed gives the same detail, and another seed gives other detail
 * with the same total (design section 14.4). */
void test_detail_seed(void)
{
    QIr *ir = compile(MIXED("0.05K", "temperature: 293.15K,"));
    if (!ir) return;
    QWorld *w[3] = { build(ir, 7), build(ir, 7), build(ir, 8) };
    for (int i = 0; i < 3; i++) {
        OK(qw_run(w[i], 100 * SEC));
        OK(qi_switch(ir, w[i], "roomB", "fine"));
    }
    CHECK(qw_hash(w[0]) == qw_hash(w[1]));
    CHECK_I64(stock(w[0], "roomB.fine.c1.energy"), stock(w[1], "roomB.fine.c1.energy"));
    CHECK(stock(w[0], "roomB.fine.c1.energy") != stock(w[2], "roomB.fine.c1.energy"));
    CHECK_I64(fine_sum(w[0]), fine_sum(w[2]));
    for (int i = 0; i < 3; i++) qw_free(w[i]);
    qi_free(ir);
}

/* ADR 0021, acceptance test 2: a fine room with a temperature gradient
 * becomes coarse and then fine again, with zero spread. The total energy
 * does not change, and the four cells then have one temperature, to the
 * precision of their stocks. */
void test_detail_uniform_refinement(void)
{
    QIr *ir = compile("roomB: #DetailRoom & {\n"
                      "    model: \"fine\"\n"
                      "    coarse: { capacity: 10001J/K }\n"
                      "    fine: { c0: { temperature: 300.0001K, capacity: 2500J/K }, c1: { temperature: 295K, capacity: 2500J/K },\n"
                      "            c2: { temperature: 290K, capacity: 2500J/K }, c3: { temperature: 285K, capacity: 2501J/K },\n"
                      "            conductance: 100W/K }\n"
                      "    spread: 0K\n"
                      "}\n");
    if (!ir) return;
    QWorld *w = build(ir, 3);
    if (w) {
        QwPhen b;
        OK(qw_find_phenomenon(w, "roomB", &b));
        CHECK(!strcmp(qw_active_model(w, b), "fine"));
        CHECK_I64(stock(w, "roomB.coarse.energy"), 0);
        CHECK_I64(stock(w, "roomB.fine.c0.energy"), 750000250);          /* 2500 J/K × 300.0001 K, in mJ */
        const int64_t total = fine_sum(w);
        CHECK_I64(total, 750000250LL + 737500000 + 725000000 + 712785000);
        OK(qw_run(w, 0));                                                /* the laws give the temperatures */
        /* the gradient: 15 K from the first cell to the last */
        CHECK(qw_get(w, "roomB.fine.c0.temperature").lo - qw_get(w, "roomB.fine.c3.temperature").hi > 14 * SEC);
        OK(qi_switch(ir, w, "roomB", "coarse"));
        CHECK_I64(stock(w, "roomB.coarse.energy"), total);
        CHECK_I64(fine_sum(w), 0);
        OK(qi_switch(ir, w, "roomB", "fine"));
        CHECK_I64(fine_sum(w), total);
        CHECK_I64(stock(w, "roomB.coarse.energy"), 0);
        /* each cell has the mean temperature: its energy is its capacity times that temperature, to one
         * quantum, and the last cell takes the remainder of the rounding */
        int64_t lo = INT64_MAX, hi = INT64_MIN;
        for (int i = 0; i < 4; i++) {
            char path[64];
            snprintf(path, sizeof path, "roomB.fine.c%d.temperature", i);
            QInterval t = qw_get(w, path);
            CHECK(t.lo != Q_NINF && t.hi != Q_INF && t.hi - t.lo <= 1);
            if (t.lo < lo) lo = t.lo;
            if (t.hi > hi) hi = t.hi;
        }
        CHECK(hi - lo <= 2);                                             /* one temperature, in µK */
        CHECK(lo > 292 * SEC && hi < 293 * SEC);
        /* a second cycle changes nothing */
        int64_t c0 = stock(w, "roomB.fine.c0.energy");
        OK(qi_switch(ir, w, "roomB", "coarse"));
        OK(qi_switch(ir, w, "roomB", "fine"));
        CHECK_I64(stock(w, "roomB.fine.c0.energy"), c0);
    }
    qw_free(w);
    qi_free(ir);
}

/* The IR of a phenomenon with models, and its text form (ADR 0021 section 8). */
void test_detail_ir(void)
{
    QIr *ir = compile(MIXED("0K", "temperature: 293.15K,"));
    if (!ir) return;
    char *text = qi_text(ir);
    HAS(text, "phenomenon roomB: #DetailRoom\n"
              "  model coarse, active at the start\n"
              "  model fine\n"
              "  cell roomB.coarse.capacity: %capacity = 10000\n"
              "  stock roomB.coarse.energy: %energy = 2931500\n");
    HAS(text, "  stock roomB.fine.c0.energy: %energy = 0\n");
    HAS(text, "  port west: #ThermalPort, the port of the active model: coarse -> roomB.coarse.west, fine -> roomB.fine.west\n");
    HAS(text, ", in the model roomB.fine: roomB.fine.c0.energy = roomB.fine.c0.capacity * roomB.fine.c0.temperature\n");
    HAS(text, "  kernel K0, at a boundary, in the model fine: native.diffusion, for 4 cells\n");
    HAS(text, "  map abstract, at a switch: fine -> coarse, native.room.abstract, for 4 cells\n"
              "    stock from.c0.energy = roomB.fine.c0.energy\n");
    HAS(text, "  map refine, at a switch: coarse -> fine, native.room.refine, for 4 cells\n"
              "    stock from.energy = roomB.coarse.energy\n");
    HAS(text, "    in spread = roomB.spread\n");
    HAS(text, "    link cold.energy = roomB.coarse.energy, by the model of roomB: coarse -> roomB.coarse.energy, "
              "fine -> roomB.fine.c0.energy\n");
    HAS(text, "  roomB.fine.c0.energy: narrowed by P2; read by K0; transferred by K0, K1\n");
    free(text);

    CHECK_I64(ir->nphens, 3);
    const QiPhen *b = &ir->phens[1];
    CHECK(!strcmp(b->name, "roomB") && b->nmodels == 2 && b->start == 0 && !strcmp(b->models[0], "coarse") &&
          !strcmp(b->models[1], "fine"));
    CHECK(ir->phens[0].nmodels == 0 && ir->phens[0].start == QI_NONE && ir->phens[0].models == NULL);
    for (uint32_t i = 0; i < ir->ncells; i++) {
        const QiCell *c = &ir->cells[i];
        uint32_t want = !strncmp(c->path, "roomB.coarse.", 13) ? 0 : !strncmp(c->path, "roomB.fine.", 11) ? 1 : QI_NONE;
        CHECK(c->model == want);
    }
    CHECK_I64(ir->nmaps, 2);
    CHECK(!strcmp(ir->maps[0].name, "abstract") && ir->maps[0].from == 1 && ir->maps[0].to == 0 && ir->maps[0].size == 4 &&
          ir->maps[0].phen == 1 && ir->maps[0].nbinds == 5);
    CHECK(!strcmp(ir->maps[1].name, "refine") && ir->maps[1].from == 0 && ir->maps[1].to == 1 && ir->maps[1].nbinds == 10);
    uint32_t model_props = 0;
    for (uint32_t i = 0; i < ir->nprops; i++)
        if (ir->props[i].model != QI_NONE) {
            CHECK(ir->props[i].mphen == 1 && ir->props[i].phen == 1);
            model_props++;
        } else CHECK(ir->props[i].mphen == QI_NONE);
    CHECK_I64(model_props, 5);
    CHECK_I64(ir->nkernels, 2);
    CHECK(ir->kernels[0].model == 1 && ir->kernels[0].size == 4 && ir->kernels[1].model == QI_NONE && ir->kernels[1].size == 0);
    const QiBind *cold = &ir->kernels[1].binds[1];
    CHECK(!strcmp(cold->slot, "cold.temperature") && cold->mphen == 1 && cold->mcells != NULL);
    if (cold->mcells) {
        CHECK(!strcmp(ir->cells[cold->mcells[0]].path, "roomB.coarse.temperature"));
        CHECK(!strcmp(ir->cells[cold->mcells[1]].path, "roomB.fine.c0.temperature"));
        CHECK(cold->cell == cold->mcells[0]);
    }
    CHECK(ir->kernels[1].binds[0].mphen == QI_NONE && ir->kernels[1].binds[0].mcells == NULL);
    qi_free(ir);

    /* the model of the start is the model that the phenomenon names */
    ir = compile("roomB: #DetailRoom & { model: \"fine\", coarse: { capacity: 4J/K },\n"
                 "  fine: { c0: { temperature: 300K, capacity: 1J/K }, c1: { temperature: 300K, capacity: 1J/K },\n"
                 "          c2: { temperature: 300K, capacity: 1J/K }, c3: { temperature: 300K, capacity: 1J/K },\n"
                 "          conductance: 1W/K }, spread: 0K }\n");
    if (ir) {
        CHECK(ir->phens[0].start == 1);
        text = qi_text(ir);
        HAS(text, "  model coarse\n  model fine, active at the start\n");
        HAS(text, "  stock roomB.coarse.energy: %energy = 0\n");
        HAS(text, "  stock roomB.fine.c0.energy: %energy = 300\n");
        free(text);
    }
    qi_free(ir);
}

/* A detail room with the maps `maps`, for a test that needs other maps. */
#define D2(maps)                                                                                        \
    "#D2: #Phenomenon & { west: #ThermalPort, spread: %temperature, model: *\"coarse\" | text,\n"       \
    "  coarse: #CoarseRoom, fine: #FineRoom, " maps " }\n"

/* A law of another phenomenon that reads through the port of the active
 * model follows a switch (ADR 0021 section 4). */
void test_detail_laws(void)
{
    QIr *ir = compile("#PowerWall: #ConductiveWall & {\n"
                      "    power: %energy / %time\n"
                      "    flow:  .power == .conductance * (.hot.temperature - .cold.temperature)\n"
                      "}\n"
                      "roomA: #LumpedRoom & { temperature: 303.15K, capacity: 10000J/K }\n"
                      ROOM("#DetailRoom")
                      "wall: #PowerWall & { hot: roomA.west, cold: r.west, conductance: 10W/K, tolerance: 1pct }\n");
    if (!ir) return;
    char *text = qi_text(ir);
    /* the law reads a copy of the temperature, and each model has a propagator that gives the copy its value */
    HAS(text, ", in the model r.coarse: wall.flow.1 = r.coarse.temperature\n    law wall.flow: ");
    HAS(text, ", in the model r.fine: wall.flow.1 = r.fine.c0.temperature\n    law wall.flow: ");
    HAS(text, "  inner wall.flow.1: dim[K]@-6 = any\n");
    free(text);
    int scale = 0;
    for (uint32_t i = 0; i < ir->ncells; i++)
        if (!strcmp(ir->cells[i].path, "wall.power")) scale = ir->kinds[ir->cells[i].kind].scale;
    double unit = 1;
    for (int i = scale; i < 0; i++) unit /= 10;
    QWorld *w = build(ir, 1);
    if (w) {
        const char *cold[2] = { "r.coarse.temperature", "r.fine.c0.temperature" };
        for (int step = 0; step < 4; step++) {
            /* 0: coarse at the start; 1: coarse after a run; 2: fine after the switch; 3: fine after a run */
            if (step == 1 || step == 3) OK(qw_run(w, (step == 1 ? 200 : 260) * SEC));
            if (step == 2) OK(qi_switch(ir, w, "r", "fine"));
            OK(qw_run(w, qw_now(w)));
            int fine = step >= 2;
            QInterval hot = qw_get(w, "roomA.temperature"), cl = qw_get(w, cold[fine]), p = qw_get(w, "wall.power");
            QInterval through = qw_get(w, "r.west.temperature");
            CHECK(through.lo == cl.lo && through.hi == cl.hi);
            CHECK(p.lo != Q_NINF && p.hi != Q_INF);
            double want = 10.0 * ((double) (hot.lo + hot.hi) / 2 - (double) (cl.lo + cl.hi) / 2) / 1e6;
            double got = (double) (p.lo + p.hi) / 2 * unit;
            CHECK(got > want - 0.001 && got < want + 0.001);
            if (step == 0) CHECK(got > 99.999 && got < 100.001);
            /* after the switch the first cell warms faster than the whole room did, so less power goes through */
            if (step == 3) CHECK(got < 60 && got > 1);
        }
        /* the temperature of the model that is not active has no value, and the law does not read it */
        QInterval off = qw_get(w, "r.coarse.temperature");
        CHECK(off.lo == Q_NINF && off.hi == Q_INF);
    }
    qw_free(w);
    qi_free(ir);
}

#define HEATER                                                                                          \
    "#Heater: #Phenomenon & { room: #ThermalPort, power: %energy / %time,\n"                            \
    "  heat: #Interaction & { sink: .room.energy, rate: .power, conserved: false } }\n"

/* Interactions with a rate (ADR 0020) and models: an interaction of another
 * phenomenon moves a stock behind the port of the active model, and an
 * interaction of a model works only while the model is active. */
void test_detail_interactions(void)
{
    QIr *ir = compile(HEATER ROOM("#DetailRoom") "heater: #Heater & { room: r.west, power: 1000W }\n");
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "    link room.energy = r.coarse.energy, by the model of r: coarse -> r.coarse.energy, fine -> r.fine.c0.energy\n");
        HAS(text, "  r.fine.c0.energy: narrowed by P1; transferred by K0; read by K1; transferred by K1\n");
        free(text);
        QWorld *w = build(ir, 1);
        if (w) {
            const int64_t e0 = stock(w, "r.coarse.energy");
            OK(qw_run(w, 10 * SEC));
            CHECK_I64(stock(w, "r.coarse.energy"), e0 + 10000000);          /* 1000 W for 10 s, in mJ */
            OK(qi_switch(ir, w, "r", "fine"));
            CHECK_I64(fine_of(w, "r"), e0 + 10000000);
            int64_t far = stock(w, "r.fine.c3.energy");
            OK(qw_run(w, 11 * SEC));
            CHECK_I64(fine_of(w, "r"), e0 + 11000000);                           /* the heat now goes into the first cell */
            CHECK_I64(stock(w, "r.coarse.energy"), 0);
            CHECK(stock(w, "r.fine.c0.energy") > far && stock(w, "r.fine.c0.energy") > stock(w, "r.fine.c1.energy"));
            OK(qi_switch(ir, w, "r", "coarse"));
            OK(qw_run(w, 20 * SEC));
            CHECK_I64(stock(w, "r.coarse.energy"), e0 + 20000000);
        }
        qw_free(w);
    }
    qi_free(ir);

    /* a model with a leak, and a model with none */
    static const char *const leaky =
        "#Leaky: #CoarseRoom & { loss: %energy / %time, leak: #Interaction & { source: .energy, rate: .loss, conserved: false } }\n"
        "#Two: #Phenomenon & { model: text, tight: #CoarseRoom, leaky: #Leaky }\n";
    for (int start = 0; start < 2; start++) {
        char src[1024];
        snprintf(src, sizeof src, "%sr: #Two & { model: \"%s\", tight: { %s capacity: 10J/K }, leaky: { %s capacity: 10J/K, loss: 2W } }\n",
                 leaky, start ? "leaky" : "tight", start ? "" : "temperature: 300K,", start ? "temperature: 300K," : "");
        ir = compile(src);
        if (!ir) continue;
        char *text = qi_text(ir);
        HAS(text, "  kernel K0, at a boundary, in the model leaky: native.rate\n"
                  "    in leaky.loss = r.leaky.loss\n"
                  "    in leaky.leak.tolerance = r.leaky.leak.tolerance\n"
                  "    stock leaky.energy = r.leaky.energy\n"
                  "    transfer leaky.leak: r.leaky.energy -> nothing, at the rate r.leaky.loss, with the tolerance r.leaky.leak.tolerance\n");
        free(text);
        CHECK(ir->nkernels == 1 && ir->kernels[0].model == 0);
        QWorld *w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 5 * SEC));
            /* the leak of a model that is not active does not run: its stock stays at zero */
            CHECK_I64(stock(w, "r.leaky.energy"), start ? 3000000 - 10000 : 0);
            CHECK_I64(stock(w, "r.tight.energy"), start ? 0 : 3000000);
        }
        qw_free(w);
        qi_free(ir);
    }
}

/* qi_switch and the transitions of the world (ADR 0021 section 5). */
void test_detail_switch(void)
{
    /* a room with one map: it can become fine, and it cannot return */
    QIr *ir = compile(D2("refine: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.refine\" }") ROOM("#D2"));
    if (ir) {
        QWorld *w = build(ir, 1);
        if (w) {
            CHECK(qi_switch(ir, w, "r", "coarse") == QW_EDUP);
            HAS(qw_error(w), "r: model 'coarse' is already active");
            CHECK(qi_switch(ir, w, "r", "medium") == QW_ENAME);
            HAS(qw_error(w), "r: no model 'medium'");
            CHECK(qi_switch(ir, w, "nobody", "fine") == QW_ENAME);
            uint64_t h = qw_hash(w);
            OK(qi_switch(ir, w, "r", "fine"));
            CHECK(qw_hash(w) != h);
            h = qw_hash(w);
            CHECK(qi_switch(ir, w, "r", "coarse") == QW_ENAME);
            HAS(qw_error(w), "r: no transition from the model 'fine' to the model 'coarse'");
            CHECK(qw_hash(w) == h);
        }
        qw_free(w);
    }
    qi_free(ir);

    /* the transitions of a world through the C API */
    CWorld c = c_mixed(1, 0);
    CHECK(qw_switch_to(c.w, c.b, "fine") == QW_ENAME);                     /* no transition yet */
    CHECK(qw_transition(c.w, c.b, "refine", "coarse", "medium", c.refine) == QW_ENAME);
    HAS(qw_error(c.w), "roomB: no model 'medium'");
    CHECK(qw_transition(c.w, c.b, "refine", "nothing", "fine", c.refine) == QW_ENAME);
    HAS(qw_error(c.w), "roomB: no model 'nothing'");
    CHECK(qw_transition(c.w, c.b, "refine", "fine", "fine", c.refine) == QW_EDUP);
    CHECK(qw_transition(c.w, c.b, "re fine", "coarse", "fine", c.refine) == QW_ENAME);
    CHECK(qw_transition(c.w, c.b, "refine", "coarse", "fine", qk_conduction()) == QW_EKERNEL);
    CHECK(qw_transition(c.w, 99, "refine", "coarse", "fine", c.refine) == QW_ENAME);
    OK(qw_transition(c.w, c.b, "refine", "coarse", "fine", c.refine));
    CHECK(qw_transition(c.w, c.b, "refine", "fine", "coarse", c.abstract) == QW_EDUP);   /* the name */
    CHECK(qw_transition(c.w, c.b, "again", "coarse", "fine", c.refine) == QW_EDUP);      /* the pair */
    HAS(qw_error(c.w), "roomB: the transition 'refine' already goes from 'coarse' to 'fine'");
    OK(qw_transition(c.w, c.b, "abstract", "fine", "coarse", c.abstract));
    CHECK(qw_switch_to(c.w, 99, "fine") == QW_ENAME);
    OK(qw_switch_to(c.w, c.b, "fine"));
    CHECK(!strcmp(qw_active_model(c.w, c.b), "fine"));
    OK(qw_switch_to(c.w, c.b, "coarse"));
    CHECK_I64((int64_t) qw_switches(c.w), 2);
    QwPhen p;
    OK(qw_find_phenomenon(c.w, "wall", &p));
    CHECK(p == 2 && qw_find_phenomenon(c.w, "wal", &p) == QW_ENAME);
    c_free(&c);

    /* the hash does not depend on the order of the models: a world that starts fine, from source and from C */
    static const char *const fine_start =
        "roomB: #DetailRoom & { model: \"fine\", coarse: { capacity: 10000J/K },\n"
        "  fine: { c0: { temperature: 293.15K, capacity: 2500J/K }, c1: { temperature: 293.15K, capacity: 2500J/K },\n"
        "          c2: { temperature: 293.15K, capacity: 2500J/K }, c3: { temperature: 293.15K, capacity: 2500J/K },\n"
        "          conductance: 100W/K, tolerance: 5pct }, spread: 0K }\n"
        "roomA: #LumpedRoom & { temperature: 303.15K, capacity: 10000J/K }\n"
        "wall: #ConductiveWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 1pct }\n";
    ir = compile(fine_start);
    if (ir) {
        QWorld *w = build(ir, 1);
        c = c_mixed(1, 0);
        OK(qw_switch(c.w, c.b, "fine", c.refine));                         /* zero spread: the same four cells */
        if (w) {
            /* the C world made a switch, so its epoch is one more: compare the state after a restart is not
             * possible, so compare the cells and then the two runs */
            for (size_t k = 0; k < sizeof STATE / sizeof STATE[0]; k++) {
                OK(qw_run(w, 0));
                QInterval x = qw_get(w, STATE[k]), y = qw_get(c.w, STATE[k]);
                CHECK(x.lo == y.lo && x.hi == y.hi);
            }
            OK(qw_run(w, 300 * SEC));
            OK(qw_run(c.w, 300 * SEC));
            for (size_t k = 0; k < sizeof STATE / sizeof STATE[0]; k++) {
                QInterval x = qw_get(w, STATE[k]), y = qw_get(c.w, STATE[k]);
                CHECK(x.lo == y.lo && x.hi == y.hi);
            }
            CHECK_I64((int64_t) qw_boundaries(w), (int64_t) qw_boundaries(c.w));
            QIr *ir2 = compile(fine_start);
            QWorld *w2 = ir2 ? build(ir2, 1) : NULL;
            if (w2) {
                OK(qw_run(w2, 300 * SEC));
                CHECK(qw_hash(w2) == qw_hash(w));
            }
            qw_free(w2);
            qi_free(ir2);
        }
        qw_free(w);
        c_free(&c);
    }
    qi_free(ir);
}

/* The errors of models, ports and maps in source. */
void test_detail_errors(void)
{
    static const struct { const char *src, *message; } cases[] = {
        /* the model at the start (section 2) */
        { "#Two: #Phenomenon & { a: #CoarseRoom, b: #CoarseRoom }\n"
          "r: #Two & { a: { temperature: 300K, capacity: 1J/K }, b: { capacity: 1J/K } }\n",
          "world.quad:2:1: error: r has models and does not name the model that is active at the start. Write `model: \"a\"`. "
          "Its models: a, b\n" },
        { "r: #DetailRoom & { model: \"medium\", coarse: { capacity: 1J/K }, fine: { conductance: 1W/K }, spread: 0K }\n",
          "error: r names the model \"medium\" for the start, and it has no model with that name. Its models: coarse, fine\n" },
        { "r: #LumpedRoom & { model: \"x\", temperature: 300K, capacity: 1J/K }\n",
          "error: r names the model \"x\", and it has no models: a model is a field with a type below #Model\n" },
        /* a model that is not active has no state (section 3) */
        { ROOM("#DetailRoom") "r: { fine: { c0: { temperature: 300K } } }\n",
          "error: r.fine.c0.temperature has a value, and its model fine is not active at the start: a model that is not "
          "active has no state. The map gives the model its state at a switch. Remove the value, or start with `model: "
          "\"fine\"`\n" },
        { ROOM("#DetailRoom") "r: { fine: { c1: { energy: 5J } } }\n",
          "error: r.fine.c1.energy has a value, and its model fine is not active at the start" },
        /* a model is a field of the phenomenon, and the phenomenon has no kernel of its own (section 1) */
        { ROOM("#DetailRoom") "r: { evaluator: \"native.conduction\" }\n",
          "error: r has models and names the evaluator \"native.conduction\": a phenomenon with models has no kernel of "
          "its own. Give the evaluator to a model\n" },
        { "#Box: { inner: #CoarseRoom }\n#Deep: #Phenomenon & { box: #Box }\n"
          "r: #Deep & { box: { inner: { temperature: 300K, capacity: 1J/K } } }\n",
          "error: r.box.inner is a model inside a structure of r: a model is a field of the phenomenon itself\n" },
        { "#Nest: #Model & { inner: #CoarseRoom }\n#P: #Phenomenon & { model: \"a\", a: #Nest }\n"
          "r: #P & { a: { inner: { temperature: 300K, capacity: 1J/K } } }\n",
          "error: r.a.inner is a model inside a structure of r: a model is a field of the phenomenon itself\n" },
        { "#P: #DetailRoom & { loss: %energy / %time, leak: #Interaction & { source: .coarse.energy, rate: .loss, conserved: false } }\n"
          ROOM("#P & { loss: 1W }"),
          "error: r.leak has a rate, and r has models: a phenomenon with models has no kernel of its own. Put the "
          "interaction in a model\n" },
        { "#L: #CoarseRoom & { other: %energy, loss: %energy / %time,\n"
          "  leak: #Interaction & { source: .other, rate: .loss, conserved: false } }\n"
          "#P: #Phenomenon & { model: \"z\", z: #L & { other: .b.energy }, b: #CoarseRoom }\n"
          "r: #P & { z: { temperature: 300K, capacity: 1J/K, loss: 1W }, b: { capacity: 1J/K } }\n",
          "error: r.z.leak moves the stock r.b.energy of the model b, and the interaction is in the model z: an "
          "interaction of a model moves the stocks of that model\n" },
        /* the kernel of a model */
        { "#E: #Model & { c0: #Lump, evaluator: \"native.room.abstract\" }\n#P: #Phenomenon & { model: \"e\", e: #E }\n"
          "r: #P & { e: { c0: { temperature: 300K, capacity: 1J/K } } }\n",
          "error: r.e: the evaluator \"native.room.abstract\" is a map kernel, which moves the state at a switch. Name it "
          "in a map, as in `refine: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.abstract\" }`\n" },
        { "#E: #Model & { evaluator: \"native.diffusion\" }\n#P: #Phenomenon & { model: \"e\", e: #E }\nr: #P\n",
          "error: r.e: the kernel native.diffusion is for a room with the cells c0, c1, and so on, and r.e has no field "
          "c0 with an energy\n" },
        { "#E: #Model & { evaluator: \"native.nothing\" }\n#P: #Phenomenon & { model: \"e\", e: #E }\nr: #P\n",
          "error: r.e: unknown evaluator \"native.nothing\"\n" },
        { "#E: #Model & { evaluator: \"native.conduction\" }\n#P: #Phenomenon & { model: \"e\", e: #E }\nr: #P\n",
          "error: r.e: the kernel native.conduction needs r.e.hot.temperature, which is not a cell\n" },
        /* the port of the active model (section 4) */
        { "#NoPort: #Model & #Lump\n#P: #Phenomenon & { west: #ThermalPort, model: \"a\", a: #CoarseRoom, b: #NoPort }\n"
          "r: #P & { a: { temperature: 300K, capacity: 1J/K }, b: { capacity: 1J/K } }\n",
          "error: r.west is a port with no view, so each model must give it. These models have no port west: b\n" },
        { "#Other: #Port & { capacity: %capacity }\n#Odd: #Model & #Lump & { west: #Other & { capacity: .capacity } }\n"
          "#P: #Phenomenon & { west: #ThermalPort, model: \"a\", a: #CoarseRoom, b: #Odd }\n"
          "r: #P & { a: { temperature: 300K, capacity: 1J/K }, b: { capacity: 1J/K } }\n",
          "error: r.west is a #ThermalPort, and the model b gives a #Other: the port of a model has the type of the port "
          "of the phenomenon, or a type below it\n" },
        { "#Open: #Model & #Lump & { west: #ThermalPort & { capacity: .capacity } }\n"
          "#P: #Phenomenon & { west: #ThermalPort, model: \"a\", a: #CoarseRoom, b: #Open }\n"
          "r: #P & { a: { temperature: 300K, capacity: 1J/K }, b: { capacity: 1J/K } }\n",
          "error: r.b.west is the port west of the model b, and it has a field with no cell: a model gives the port as a "
          "view of its own state, as in `temperature: .temperature`\n" },
        /* maps (section 5) */
        { D2("extra: #Map & { from: \"coarse\", to: \"medium\", evaluator: \"native.room.refine\" }") ROOM("#D2"),
          "error: the map r.extra names the model \"medium\", and r has no model with that name. Its models: coarse, fine\n" },
        { D2("extra: #Map & { from: \"nothing\", to: \"fine\", evaluator: \"native.room.refine\" }") ROOM("#D2"),
          "error: the map r.extra names the model \"nothing\", and r has no model" },
        { D2("extra: #Map & { from: \"fine\", to: \"fine\", evaluator: \"native.room.refine\" }") ROOM("#D2"),
          "error: the map r.extra goes from the model fine to the same model\n" },
        { D2("one: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.refine\" },\n"
             "two: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.refine\" }") ROOM("#D2"),
          "error: the maps r.one and r.two both go from coarse to fine: a phenomenon has one map for each pair of models\n" },
        { D2("extra: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.nothing\" }") ROOM("#D2"),
          "error: the map r.extra: unknown evaluator \"native.nothing\"\n" },
        { D2("extra: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.conduction\" }") ROOM("#D2"),
          "error: the map r.extra: the evaluator \"native.conduction\" is not a map kernel\n" },
        { D2("extra: #Map & { from: \"coarse\" }") ROOM("#D2"),
          "error: the map r.extra needs two models and a map kernel, as in `extra: #Map & { from: \"coarse\", to: \"fine\", "
          "evaluator: \"native.room.refine\" }`\n" },
        { "#M: #LumpedRoom & { extra: #Map & { from: \"a\", to: \"b\", evaluator: \"native.room.refine\" } }\n"
          "r: #M & { temperature: 300K, capacity: 1J/K }\n",
          "error: r.extra is a map, and r has no models: a map moves the state from one model to another\n" },
        { "#Box: { deep: #Map }\n#M: #DetailRoom & { box: #Box }\n" ROOM("#M"),
          "error: r.box.deep is a map inside a structure of r: a map is a field of the phenomenon itself\n" },
        /* the contract of a map kernel: a map in the wrong direction finds no cells */
        { D2("extra: #Map & { from: \"fine\", to: \"coarse\", evaluator: \"native.room.refine\" }") ROOM("#D2"),
          "error: the map r.extra: the kernel native.room.refine is for a room with the cells c0, c1, and so on, and the "
          "model coarse has no field c0 with an energy\n" },
        { D2("extra: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.abstract\" }") ROOM("#D2"),
          "error: the map r.extra: the kernel native.room.abstract is for a room with the cells c0, c1, and so on, and the "
          "model coarse has no field c0 with an energy\n" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) HAS(compile_error(cases[i].src), cases[i].message);

    /* a map kernel from the host that does not fit the models: the message names the cell that it needs */
    QkKernel *five = qk_refine_new(5);
    const QkKernel *given[1] = { five };
    QiOptions opt = { given, 1 };
    char *diag = NULL;
    QIr *ir = compile_with(D2("refine: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.refine\" }") ROOM("#D2"),
                           &opt, &diag);
    CHECK(ir == NULL);
    HAS(diag, "error: the map r.refine: the kernel native.room.refine needs r.fine.c4.energy, which is not a cell\n");
    free(diag);
    qi_free(ir);
    qk_free(five);

    /* a port with no view that no model gives stays open, and a world can have it */
    ir = compile("#P: #Phenomenon & { east: #ThermalPort, model: \"a\", a: #CoarseRoom }\n"
                 "r: #P & { a: { temperature: 300K, capacity: 1J/K } }\n");
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "  port east: #ThermalPort, open\n");
        free(text);
    }
    qi_free(ir);
}

/* The order of the declarations and of the files does not change the IR. */
void test_detail_order(void)
{
    static const char *const parts[4] = {
        "roomA: #LumpedRoom & { temperature: 303.15K, capacity: 10000J/K }\n",
        "roomB: #DetailRoom & { coarse: { temperature: 293.15K, capacity: 10000J/K }, spread: 0K }\n",
        ("roomB: { fine: { c0: { capacity: 2500J/K }, c1: { capacity: 2500J/K }, c2: { capacity: 2500J/K },\n"
         "                 c3: { capacity: 2500J/K }, conductance: 100W/K, tolerance: 5pct } }\n"),
        "wall: #ConductiveWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 1pct }\n",
    };
    char *first = NULL;
    for (int order = 0; order < 4; order++) {
        char src[2048] = "";
        for (int k = 0; k < 4; k++) strcat(src, parts[(k * (order % 2 ? 3 : 1) + order) % 4]);
        const char *names[3] = { "a.quad", "b.quad", "c.quad" };
        const char *fwd[3] = { types(0), types(1), src }, *back[3] = { src, types(1), types(0) };
        char *diag = NULL;
        QIr *ir = qi_compile(names, order < 2 ? fwd : back, 3, NULL, &diag);
        CHECK(ir != NULL);
        if (!ir) fprintf(stderr, "%s", diag);
        free(diag);
        if (!ir) continue;
        char *text = qi_text(ir);
        if (!first) first = text;
        else {
            CHECK(!strcmp(text, first));
            free(text);
        }
        qi_free(ir);
    }
    free(first);
}

/* A snapshot of a world from source in the middle of a run with the fine
 * model, and its restore in a new world (ADR 0004 rule 8). */
void test_detail_snapshot(void)
{
    QIr *ir = compile(MIXED("0.05K", "temperature: 293.15K,"));
    if (!ir) return;
    QWorld *a = build(ir, 5), *b = build(ir, 5);
    if (a && b) {
        OK(qw_run(a, 100 * SEC));
        OK(qi_switch(ir, a, "roomB", "fine"));
        OK(qw_run(a, 150 * SEC));
        void *bytes = NULL;
        size_t len = 0;
        OK(qw_snapshot(a, &bytes, &len));
        OK(qw_restore(b, bytes, len));
        free(bytes);
        CHECK(qw_hash(a) == qw_hash(b));
        OK(qw_run(a, 400 * SEC));
        OK(qw_run(b, 400 * SEC));
        CHECK(qw_hash(a) == qw_hash(b));
        OK(qi_switch(ir, a, "roomB", "coarse"));
        OK(qi_switch(ir, b, "roomB", "coarse"));
        OK(qw_run(a, 500 * SEC));
        OK(qw_run(b, 500 * SEC));
        CHECK(qw_hash(a) == qw_hash(b));
    }
    qw_free(a);
    qw_free(b);
    qi_free(ir);
}

/* Two rooms with models, and a wall between them: each port of the wall
 * follows the model of its own room. */
void test_detail_two_rooms(void)
{
    QIr *ir = compile("east: #DetailRoom & { coarse: { temperature: 303.15K, capacity: 10000J/K },\n"
                      "  fine: { c0: { capacity: 2500J/K }, c1: { capacity: 2500J/K }, c2: { capacity: 2500J/K },\n"
                      "          c3: { capacity: 2500J/K }, conductance: 100W/K }, spread: 0K }\n"
                      ROOM("#DetailRoom")
                      "wall: #ConductiveWall & { hot: east.west, cold: r.west, conductance: 10W/K, tolerance: 1pct }\n");
    if (!ir) return;
    char *text = qi_text(ir);
    HAS(text, "    in hot.temperature = east.coarse.temperature, by the model of east: coarse -> east.coarse.temperature, "
              "fine -> east.fine.c0.temperature\n"
              "    in cold.temperature = r.coarse.temperature, by the model of r: coarse -> r.coarse.temperature, "
              "fine -> r.fine.c0.temperature\n");
    free(text);
    CHECK(ir->phens[0].nmodels == 2 && ir->phens[1].nmodels == 2 && ir->phens[2].nmodels == 0);
    CHECK_I64(ir->nmaps, 4);
    CHECK(ir->maps[0].phen == 0 && ir->maps[2].phen == 1 && !strcmp(ir->maps[2].name, "abstract"));
    QWorld *w = build(ir, 2);
    if (w) {
        const int64_t total = stock(w, "east.coarse.energy") + stock(w, "r.coarse.energy");
        OK(qw_run(w, 50 * SEC));
        OK(qi_switch(ir, w, "east", "fine"));
        OK(qw_run(w, 100 * SEC));
        CHECK_I64(fine_of(w, "east") + stock(w, "r.coarse.energy"), total);
        int64_t before = stock(w, "r.coarse.energy");
        OK(qi_switch(ir, w, "r", "fine"));
        OK(qw_run(w, 150 * SEC));
        CHECK_I64(fine_of(w, "east") + fine_of(w, "r"), total);
        CHECK(fine_of(w, "r") > before);                                   /* the heat still goes from east to r */
        /* the heat leaves the first cell of east, and it comes to the first cell of r */
        CHECK(stock(w, "east.fine.c0.energy") < stock(w, "east.fine.c3.energy"));
        CHECK(stock(w, "r.fine.c0.energy") > stock(w, "r.fine.c3.energy"));
        OK(qi_switch(ir, w, "east", "coarse"));
        OK(qw_run(w, 200 * SEC));
        CHECK_I64(stock(w, "east.coarse.energy") + fine_of(w, "r"), total);
    }
    qw_free(w);
    qi_free(ir);

    /* a port of the phenomenon that the walk finds before the ports of the models */
    ir = compile("#M2: #Model & #Lump & { alpha: #ThermalPort & { temperature: .temperature, capacity: .capacity, energy: .energy } }\n"
                 "#P: #Phenomenon & { alpha: #ThermalPort, model: \"m\", m: #M2, n: #M2 }\n"
                 "p: #P & { m: { temperature: 300K, capacity: 1J/K }, n: { capacity: 1J/K } }\n");
    if (ir) {
        text = qi_text(ir);
        HAS(text, "  port alpha: #ThermalPort, the port of the active model: m -> p.m.alpha, n -> p.n.alpha\n");
        free(text);
    }
    qi_free(ir);
}

/* The host can give the kernels of the room. They then have precedence over
 * the kernels that the front end generates. */
void test_detail_host_kernels(void)
{
    QkKernel *made[3] = { qk_diffusion_new(4), qk_refine_new(4), qk_abstract_new(4) };
    const QkKernel *given[3] = { made[0], made[1], made[2] };
    QiOptions opt = { given, 3 };
    char *diag = NULL;
    QIr *host = compile_with(MIXED("0.05K", "temperature: 293.15K,"), &opt, &diag), *own = compile(MIXED("0.05K", "temperature: 293.15K,"));
    CHECK(host != NULL);
    free(diag);
    if (host && own) {
        CHECK(host->kernels[0].size == 0 && host->maps[0].size == 0 && host->maps[1].size == 0);
        CHECK(own->kernels[0].size == 4 && own->maps[0].size == 4 && own->maps[1].size == 4);
        QWorld *a = NULL, *b = build(own, 9);
        char *error = NULL;
        OK(qi_instantiate(host, 9, &opt, &a, &error));
        free(error);
        /* without the kernels of the host, the world has no kernel with that name for the model */
        QWorld *none = NULL;
        CHECK(qi_instantiate(host, 9, NULL, &none, &error) == QW_ENAME && none == NULL);
        HAS(error ? error : "", "unknown evaluator \"native.diffusion\"");
        free(error);
        if (a && b) {
            OK(qw_run(a, 100 * SEC));
            OK(qw_run(b, 100 * SEC));
            OK(qi_switch(host, a, "roomB", "fine"));
            OK(qi_switch(own, b, "roomB", "fine"));
            OK(qw_run(a, 300 * SEC));
            OK(qw_run(b, 300 * SEC));
            CHECK(qw_hash(a) == qw_hash(b));
            OK(qi_switch(host, a, "roomB", "coarse"));
            OK(qi_switch(own, b, "roomB", "coarse"));
            CHECK(qw_hash(a) == qw_hash(b));
        }
        qw_free(a);
        qw_free(b);
    }
    qi_free(host);
    qi_free(own);
    for (int i = 0; i < 3; i++) qk_free(made[i]);
}
