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

/* --- an interaction with no rate states a transfer of the kernel (ADR 0021 section 6) --------------- */

/* A kernel that moves 1 J for each second out of the world, and 2 J for each
 * second from `energy` to `store`, which no interaction of the test states. */
struct QkInstance { const QkHost *host; };

static const QkSlot vent_slots[2] = {
    { "energy", QK_STOCK, "J", -3, { Q_NINF, Q_INF } },
    { "store", QK_STOCK, "J", -3, { Q_NINF, Q_INF } },
};
static const QkTransfer vent_transfers[2] = { { "loss", 0, QK_NONE, false }, { "keep", 0, 1, true } };

static QkResult vent_create(const QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out)
{
    (void) self;
    (void) call;
    QkInstance *i = malloc(sizeof *i);
    if (!i) return QK_ERROR;
    i->host = host;
    *out = i;
    return QK_OK;
}

static void vent_destroy(QkInstance *i) { free(i); }

static QkResult vent_predict(QkInstance *i, QkCall *call, QkPrediction *out)
{
    out->horizon = i->host->now(call) + SEC;
    out->event_lo = out->event_hi = QK_NO_TIME;
    return QK_OK;
}

static QkResult vent_advance(QkInstance *i, QkCall *call, int64_t to)
{
    int64_t ms = (to - i->host->now(call)) / 1000;
    i->host->transfer(call, 0, ms);
    i->host->transfer(call, 1, 2 * ms);
    return QK_OK;
}

static const QkKernel vent = {
    .abi_version = QK_ABI_VERSION,
    .size = sizeof(QkKernel),
    .qr_version = QR_VERSION,
    .name = "test.vent",
    .flags = QK_EVOLVE | QK_STATELESS,
    .slots = vent_slots,
    .nslots = 2,
    .transfers = vent_transfers,
    .ntransfers = 2,
    .create = vent_create,
    .destroy = vent_destroy,
    .predict = vent_predict,
    .advance = vent_advance,
};

#define SAID(body) \
    "#Said: #ConductiveWall & { " body " }\n" \
    "roomA: #LumpedRoom & { temperature: 303.15K, capacity: 10000J/K }\n" \
    "roomB: #LumpedRoom & { temperature: 293.15K, capacity: 10000J/K }\n" \
    "wall: #Said & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 1pct }\n"

#define BOX(body, inst) "#Box: #Phenomenon & { energy: %energy, store: %energy, evaluator: \"test.vent\", " body " }\n" \
                        "box: #Box & { energy: 100J, store: 0J " inst " }\n"

void test_detail_stated_transfers(void)
{
    const QkKernel *given[1] = { &vent };
    QiOptions opt = { given, 1 };

    /* 1. the statement fits the transfer of the kernel. The IR names the interaction, and the world is the
     * same as without the statement */
    QIr *ir = compile(SAID("heat: #Interaction & { source: .hot.energy, sink: .cold.energy }"));
    QIr *plain = compile(SAID("conductance: %conductance"));
    if (ir && plain) {
        char *text = qi_text(ir), *ptext = qi_text(plain);
        HAS(text, "    transfer heat: roomA.energy -> roomB.energy, conserved, stated by wall.heat\n");
        HAS(ptext, "    transfer heat: roomA.energy -> roomB.energy, conserved\n");
        /* the source, the sink and the tolerance of the statement are not cells */
        CHECK(!strstr(text, "wall.heat.") && !strstr(text, "native.rate"));
        free(text);
        free(ptext);
        CHECK(ir->nkernels == 1 && ir->kernels[0].ntransfers == 1);
        CHECK(!strcmp(ir->kernels[0].transfers[0].stated, "wall.heat") && plain->kernels[0].transfers[0].stated == NULL);
        CHECK_I64(ir->ncells, plain->ncells);
        QWorld *a = build(ir, 1), *b = build(plain, 1);
        if (a && b) {
            OK(qw_run(a, 300 * SEC));
            OK(qw_run(b, 300 * SEC));
            CHECK(qw_hash(a) == qw_hash(b));
        }
        qw_free(a);
        qw_free(b);
    }
    qi_free(ir);
    qi_free(plain);

    /* 2. ADR 0021, acceptance test 6: a kernel with a transfer that source does not state. The IR has
     * each transfer of the contract, with its stocks and its flag */
    char *diag = NULL;
    ir = compile_with(BOX("out: #Interaction & { source: .energy, conserved: false }", ""), &opt, &diag);
    CHECK(ir != NULL);
    if (!ir) fprintf(stderr, "%s", diag);
    free(diag);
    if (ir) {
        const QiKernelUse *k = &ir->kernels[0];
        CHECK_I64(k->ntransfers, 2);
        const QiTransfer *loss = &k->transfers[0], *keep = &k->transfers[1];
        CHECK(!strcmp(loss->name, "loss") && !strcmp(ir->cells[loss->from].path, "box.energy") && loss->to == QI_NONE &&
              !loss->conserved && loss->stated && !strcmp(loss->stated, "box.out") && loss->rate == QI_NONE);
        CHECK(!strcmp(keep->name, "keep") && !strcmp(ir->cells[keep->from].path, "box.energy") &&
              !strcmp(ir->cells[keep->to].path, "box.store") && keep->conserved && keep->stated == NULL);
        char *text = qi_text(ir);
        HAS(text, "    transfer loss: box.energy -> nothing, stated by box.out\n"
                  "    transfer keep: box.energy -> box.store, conserved\n");
        HAS(text, "  box.store: read by K0; transferred by K0\n");
        free(text);
        QWorld *w = NULL;
        char *error = NULL;
        OK(qi_instantiate(ir, 1, &opt, &w, &error));
        free(error);
        if (w) {
            OK(qw_run(w, 10 * SEC));
            CHECK_I64(stock(w, "box.energy"), 100000 - 30000);
            CHECK_I64(stock(w, "box.store"), 20000);
        }
        qw_free(w);
    }
    qi_free(ir);

    /* a transfer of the kernel of a model, and the other transfers of that kernel */
    ir = compile("#Flowing: #FineRoom & { first: #Interaction & { source: .c0.energy, sink: .c1.energy } }\n"
                 "#D3: #DetailRoom & { fine: #Flowing }\n" ROOM("#D3"));
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "    transfer flow0: r.fine.c0.energy -> r.fine.c1.energy, conserved, stated by r.fine.first\n"
                  "    transfer flow1: r.fine.c1.energy -> r.fine.c2.energy, conserved\n");
        free(text);
    }
    qi_free(ir);

    /* a transfer through the port of the active model */
    ir = compile("#Said: #ConductiveWall & { heat: #Interaction & { source: .hot.energy, sink: .cold.energy } }\n"
                 "roomA: #LumpedRoom & { temperature: 303.15K, capacity: 10000J/K }\n" ROOM("#DetailRoom")
                 "wall: #Said & { hot: roomA.west, cold: r.west, conductance: 10W/K, tolerance: 1pct }\n");
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "    transfer heat: roomA.energy -> r.coarse.energy, conserved, stated by wall.heat\n");
        free(text);
    }
    qi_free(ir);

    /* 3. ADR 0021, acceptance test 3: a statement that the kernel does not make is a compile error */
    static const struct { const char *src, *message; bool vent; } cases[] = {
        { SAID("heat: #Interaction & { source: .cold.energy, sink: .hot.energy }"),
          "error: wall.heat states the transfer roomB.energy -> roomA.energy, conserved, and the kernel native.conduction "
          "makes no transfer between these stocks. Its transfers: heat (roomA.energy -> roomB.energy, conserved)\n", false },
        { SAID("heat: #Interaction & { source: .hot.energy, conserved: false }"),
          "error: wall.heat states the transfer roomA.energy -> nothing, not conserved, and the kernel native.conduction "
          "makes no transfer between these stocks.", false },
        { SAID("heat: #Interaction & { source: .hot.energy, sink: .cold.energy, conserved: false }"),
          "error: wall.heat states the transfer roomA.energy -> roomB.energy, not conserved, and the transfer heat of the "
          "kernel native.conduction is conserved: write `conserved: true`\n", false },
        { SAID("heat: #Interaction & { source: .hot.energy, sink: .cold.energy },\n"
               "  more: #Interaction & { source: .hot.energy, sink: .cold.energy }"),
          "error: wall.heat and wall.more state the same transfer, heat of the kernel native.conduction: one interaction "
          "states one transfer\n", false },
        { SAID("heat: #Interaction & { conserved: true }"),
          "error: wall.heat has no rate, no source and no sink: an interaction with no rate states a transfer of the "
          "kernel native.conduction, as in `source: .hot.energy, sink: .cold.energy`\n", false },
        { SAID("heat: #Interaction & { source: .hot.temperature, sink: .cold.energy }"),
          "error: the source of wall.heat is roomA.temperature, which is not a stock", false },
        { BOX("out: #Interaction & { source: .energy }", ""),
          "error: box.out states the transfer box.energy -> nothing, conserved, and the transfer loss of the kernel "
          "test.vent is not conserved: write `conserved: false`\n", true },
        { BOX("out: #Interaction & { source: .store, sink: .energy }", ""),
          "error: box.out states the transfer box.store -> box.energy, conserved, and the kernel test.vent makes no "
          "transfer between these stocks. Its transfers: loss (box.energy -> nothing, not conserved), keep (box.energy -> "
          "box.store, conserved)\n", true },
        { "#Flowing: #FineRoom & { skip: #Interaction & { source: .c0.energy, sink: .c2.energy } }\n"
          "#D3: #DetailRoom & { fine: #Flowing }\n" ROOM("#D3"),
          "error: r.fine.skip states the transfer r.fine.c0.energy -> r.fine.c2.energy, conserved, and the kernel "
          "native.diffusion makes no transfer between these stocks. Its transfers: flow0 (", false },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        diag = NULL;
        ir = compile_with(cases[i].src, cases[i].vent ? &opt : NULL, &diag);
        CHECK(ir == NULL);
        HAS(diag, cases[i].message);
        /* one statement gives one error */
        const char *first = strstr(diag, "error: ");
        CHECK(first && !strstr(first + 1, "error: "));
        free(diag);
        qi_free(ir);
    }
    /* a kernel with an error of its own: the statement gives no second error */
    HAS(compile_error("#Odd: #Wall & { evaluator: \"native.nothing\", heat: #Interaction & { source: .hot.energy, sink: .cold.energy } }\n"
                      "roomA: #LumpedRoom & { temperature: 303.15K, capacity: 10000J/K }\n"
                      "roomB: #LumpedRoom & { temperature: 293.15K, capacity: 10000J/K }\n"
                      "wall: #Odd & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 1pct }\n"),
        "world.quad:4:1: error: wall: unknown evaluator \"native.nothing\"\n");
}

/* --- reactions (ADR 0021 section 7) ---------------------------------------------------------------------- */

#define MASS "%mass: { dimension: \"kg\", scale: -6, conserved: true }\n"
#define BURN                                                                                            \
    "#Burn: #Reaction & { fuel: #Interaction & { ratio: 1, conserved: false },\n"                       \
    "                     heat: #Interaction & { conserved: false } }\n"
/* A fire with the reaction `burn`: `flows` gives the rate and the flows. */
#define FIRE(flows)                                                                                     \
    MASS BURN                                                                                           \
    "#Fire: #Phenomenon & { room: #ThermalPort, wood: %mass, life: %time, burning: %mass / %time,\n"    \
    "  heating: %energy / %mass, speed: .burning == .wood / .life,\n"                                   \
    "  burn: " flows " }\n"                                                                             \
    "r: #LumpedRoom & { temperature: 280K, capacity: 100000J/K }\n"                                     \
    "fire: #Fire & { room: r.west, wood: 5kg, life: 40min, heating: 15MJ/kg }\n"
#define FLOWS "#Burn & { rate: .burning, fuel: { source: .wood }, heat: { sink: .room.energy, ratio: .heating } }"

/* ADR 0021, acceptance test 4: a reaction that burns wood moves the mass
 * and the energy in the stated ratio, at each time of a run. */
void test_reaction_fire(void)
{
    QIr *ir = compile(FIRE(FLOWS));
    if (!ir) return;
    char *text = qi_text(ir);
    HAS(text, "  cell fire.burn.fuel.ratio: dim[1]@-9 = 1\n"
              "  cell fire.burn.tolerance: %ratio = 0.01\n"
              "  cell fire.burning: dim[kg s^-1]@-12 = any\n"
              "  cell fire.heating: dim[m^2 s^-2]@-9 = 15000000\n");
    HAS(text, "  alias fire.burn.heat.ratio = fire.heating\n  alias fire.burn.rate = fire.burning\n");
    HAS(text, "  kernel K0, at a boundary: native.rate\n"
              "    in burning = fire.burning\n"
              "    in burn.tolerance = fire.burn.tolerance\n"
              "    in burn.fuel.ratio = fire.burn.fuel.ratio\n"
              "    stock wood = fire.wood\n"
              "    in heating = fire.heating\n"
              "    link room.energy = r.energy\n"
              "    transfer burn.fuel: fire.wood -> nothing, at the rate fire.burning times the ratio fire.burn.fuel.ratio, "
              "with the tolerance fire.burn.tolerance\n"
              "    transfer burn.heat: nothing -> r.energy, at the rate fire.burning times the ratio fire.heating, with "
              "the tolerance fire.burn.tolerance\n");
    /* the flows have no tolerance of their own, and the reaction has one */
    CHECK(!strstr(text, "fire.burn.fuel.tolerance") && !strstr(text, "fire.burn.heat.tolerance"));
    free(text);
    const QiKernelUse *k = &ir->kernels[0];
    CHECK(k->ntransfers == 2 && k->transfers[0].ratio != QI_NONE && k->transfers[1].ratio != QI_NONE);
    CHECK(!strcmp(ir->cells[k->transfers[1].ratio].path, "fire.heating") && k->transfers[0].rate == k->transfers[1].rate &&
          k->transfers[0].tolerance == k->transfers[1].tolerance && !strcmp(k->transfers[0].stated, "fire.burn.fuel"));
    QWorld *w = build(ir, 1), *again = build(ir, 1);
    if (w && again) {
        const int64_t wood0 = stock(w, "fire.wood"), heat0 = stock(w, "r.energy");
        CHECK_I64(wood0, 5000000);
        for (int t = 1; t <= 7201; t += 300) {
            OK(qw_run(w, t * SEC));
            OK(qw_run(again, t * SEC));
            int64_t burned = wood0 - stock(w, "fire.wood"), heat = stock(w, "r.energy") - heat0;
            /* 15 MJ/kg is 15 J for each mg of wood. The wood has whole mg, so the heat is at most that of one mg more */
            CHECK(burned > 0 && heat >= 15000 * burned && heat < 15000 * (burned + 1));
            CHECK(qw_hash(w) == qw_hash(again));
        }
        /* after three times the life, 5 % of the wood is there */
        CHECK(stock(w, "fire.wood") > wood0 / 25 && stock(w, "fire.wood") < wood0 / 18);
    }
    qw_free(w);
    qw_free(again);
    qi_free(ir);

    /* a reaction with whole ratios: 2 parts of a and 1 part of b make 3 parts of c, and nothing is lost */
    ir = compile(MASS
                 "#Mix: #Reaction & { a: #Interaction & { ratio: 2 }, b: #Interaction & { ratio: 1 } }\n"
                 "#Vat: #Phenomenon & { a: %mass, b: %mass, c: %mass, speed: %mass / %time,\n"
                 "  mix: #Mix & { rate: .speed, a: { source: .a, sink: .c }, b: { source: .b, sink: .c } } }\n"
                 "vat: #Vat & { a: 10kg, b: 10kg, c: 0kg, speed: 1g/s }\n");
    if (ir) {
        w = build(ir, 1);
        if (w) {
            for (int t = 1; t <= 4001; t += 500) {
                OK(qw_run(w, t * SEC));
                CHECK_I64(stock(w, "vat.a"), 10000000 - 2000 * (int64_t) t);
                CHECK_I64(stock(w, "vat.b"), 10000000 - 1000 * (int64_t) t);
                CHECK_I64(stock(w, "vat.c"), 3000 * (int64_t) t);
            }
        }
        qw_free(w);
    }
    qi_free(ir);

    /* a reaction of a model works only while the model is active */
    ir = compile(MASS BURN
                 "#Lit: #Model & { wood: %mass, ash: %mass, speed: %mass / %time,\n"
                 "  burn: #Burn & { rate: .speed, fuel: { source: .wood }, heat: { sink: .ash, ratio: 0.5 } } }\n"
                 "#Out: #Model & { wood: %mass }\n"
                 "#Hearth: #Phenomenon & { model: text, lit: #Lit, out: #Out }\n"
                 "h: #Hearth & { model: \"lit\", lit: { wood: 1kg, ash: 0kg, speed: 2g/s } }\n");
    if (ir) {
        text = qi_text(ir);
        HAS(text, "  kernel K0, at a boundary, in the model lit: native.rate\n");
        free(text);
        w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 100 * SEC));
            CHECK_I64(stock(w, "h.lit.wood"), 1000000 - 200000);
            CHECK_I64(stock(w, "h.lit.ash"), 100000);
        }
        qw_free(w);
    }
    qi_free(ir);
}

void test_reaction_errors(void)
{
    static const struct { const char *src, *message; } cases[] = {
        { FIRE("#Burn & { rate: .burning, fuel: { source: .wood, rate: .burning }, heat: { sink: .room.energy, ratio: .heating } }"),
          "error: fire.burn.fuel is a flow of the reaction fire.burn, and it has a rate: the reaction gives the rate. Give "
          "the flow a ratio, as in `ratio: 1`\n" },
        { FIRE("#Burn & { rate: .burning, fuel: { source: .wood }, heat: { sink: .room.energy } }"),
          "error: fire.burn.heat is a flow of the reaction fire.burn, and it has no ratio: give the amount of its stock for "
          "each unit of the reaction, as in `ratio: 1`\n" },
        { FIRE("#Burn & { fuel: { source: .wood }, heat: { sink: .room.energy, ratio: .heating } }"),
          "error: the reaction fire.burn has no rate: give the amount of the reaction for each second, as in `rate: "
          ".burning`\n" },
        { FIRE("#Reaction & { rate: .burning }"),
          "error: the reaction fire.burn has no flows: give it an interaction with a ratio for each stock that it changes, "
          "as in `fuel: #Interaction & { source: .wood, ratio: 1, conserved: false }`\n" },
        { FIRE("#Burn & { rate: \"fast\", fuel: { source: .wood }, heat: { sink: .room.energy, ratio: .heating } }"),
          "error: the rate of fire.burn must be a quantity: the amount for each second\n" },
        { FIRE("#Burn & { rate: .burning, fuel: { source: .wood }, heat: { sink: .room.energy, ratio: .life } }"),
          "error: the ratio of fire.burn.heat is in s, the rate of the reaction fire.burn is in kg s^-1, and the stock "
          "r.energy is in J: the ratio must be the quantity of the stock for each unit of the reaction, in m^2 s^-2\n" },
        { FIRE("#Burn & { rate: .burning, fuel: { source: .wood }, heat: { sink: .room.energy, ratio: .room.capacity } }"),
          "error: the ratio of fire.burn.heat must be a number or a quantity that is a field of fire, as in `ratio: 1` or "
          "`ratio: .heating`\n" },
        { MASS "#Lose: #Reaction & { fuel: #Interaction & { ratio: 1 } }\n"
          "#P: #Phenomenon & { m: %mass, p: %mass / %time, burn: #Lose & { rate: .p, fuel: { source: .m } } }\n"
          "p: #P & { m: 1kg, p: 1g/s }\n",
          "error: p.burn.fuel has a source and no sink: the quantity then comes from outside the world, or goes there." },
        { MASS "#P: #Phenomenon & { m: %mass, p: %mass / %time,\n"
          "  out: #Interaction & { source: .m, rate: .p, ratio: 2, conserved: false } }\np: #P & { m: 1kg, p: 1g/s }\n",
          "error: p.out has a ratio, and it is not in a reaction: a ratio is for a flow of a #Reaction. An interaction "
          "alone has a rate, as in `rate: .power`\n" },
        { MASS BURN "#P: #Phenomenon & { m: %mass, e: %mass, p: %mass / %time, evaluator: \"native.conduction\",\n"
          "  burn: #Burn & { rate: .p, fuel: { source: .m }, heat: { sink: .e, ratio: 1 } } }\np: #P & { m: 1kg, e: 0kg, p: 1g/s }\n",
          "error: p.burn has a rate, and p names the evaluator \"native.conduction\": a phenomenon moves its quantities "
          "with rates or with a kernel, and not with both\n" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) HAS(compile_error(cases[i].src), cases[i].message);
}

/* More cases of reactions and models that a mutation round found. */
void test_reaction_more(void)
{
    /* each flow reports its own error */
    const char *two = compile_error(FIRE("#Burn & { rate: .burning, fuel: { source: .wood, rate: .burning }, heat: { sink: .room.energy } }"));
    HAS(two, "error: fire.burn.fuel is a flow of the reaction fire.burn, and it has a rate");
    HAS(two, "error: fire.burn.heat is a flow of the reaction fire.burn, and it has no ratio");

    /* two flows with a number as the ratio, on stocks with different scales: the rate has the finer scale */
    QIr *ir = compile(MASS "%grain: { dimension: \"kg\", scale: -3, conserved: true }\n"
                      "#Sift: #Reaction & { fine: #Interaction & { ratio: 1, conserved: false },\n"
                      "                     coarse: #Interaction & { ratio: 2, conserved: false } }\n"
                      "#Mill: #Phenomenon & { flour: %mass, bran: %grain, speed: %mass / %time,\n"
                      "  sift: #Sift & { rate: .speed, fine: { sink: .flour }, coarse: { sink: .bran } } }\n"
                      "mill: #Mill & { flour: 0kg, bran: 0kg, speed: 1g/s }\n");
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "  cell mill.speed: dim[kg s^-1]@-12 = 0.001\n");
        free(text);
        QWorld *w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 1000 * SEC));
            CHECK_I64(stock(w, "mill.flour"), 1000000);                    /* 1 kg, in mg */
            CHECK_I64(stock(w, "mill.bran"), 2000);                        /* 2 kg, in g */
        }
        qw_free(w);
    }
    qi_free(ir);

    /* a stock of a model that is not active at the start: a range from zero is a value also */
    HAS(compile_error(ROOM("#DetailRoom") "r: { fine: { c1: { energy: >= 0J } } }\n"),
        "error: r.fine.c1.energy has a value, and its model fine is not active at the start");
    HAS(compile_error(ROOM("#DetailRoom") "r: { fine: { c1: { energy: <= 0J } } }\n"),
        "error: r.fine.c1.energy has a value, and its model fine is not active at the start");
    /* and zero is no error */
    ir = compile(ROOM("#DetailRoom") "r: { fine: { c1: { energy: 0J } } }\n");
    qi_free(ir);
}

/* --- the condition of a map (ADR 0022) -------------------------------------------------------------------- */

/* A detail room with an input of the application and two conditions. */
#define WATCHED(refine, abstract)                                                                       \
    "#Watched: #Phenomenon & { west: #ThermalPort, spread: %temperature, model: *\"coarse\" | text,\n"  \
    "  visible: *false | bool, door: *\"closed\" | \"open\" | \"closed\", coarse: #CoarseRoom, fine: #FineRoom,\n" \
    "  refine: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.refine\"" refine " },\n" \
    "  abstract: #Map & { from: \"fine\", to: \"coarse\", evaluator: \"native.room.abstract\"" abstract " } }\n"
#define SEEN WATCHED(", when: .visible", ", when: .visible == false")
#define WROOM(name, type)                                                                               \
    name ": " type " & { coarse: { temperature: 293.15K, capacity: 10000J/K },\n"                       \
    "  fine: { c0: { capacity: 2500J/K }, c1: { capacity: 2500J/K }, c2: { capacity: 2500J/K },\n"      \
    "          c3: { capacity: 2500J/K }, conductance: 100W/K, tolerance: 5pct }, spread: 0K }\n"
#define WARM                                                                                            \
    "roomA: #LumpedRoom & { temperature: 303.15K, capacity: 10000J/K }\n"                               \
    "wall: #ConductiveWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 1pct }\n"

static bool is_fine(QWorld *w, const char *room)
{
    QwPhen p;
    return qw_find_phenomenon(w, room, &p) == QW_OK && !strcmp(qw_active_model(w, p), "fine");
}

/* ADR 0021, acceptance test 5: a condition from source, and its change
 * through the C API. Two rooms with different inputs make different
 * decisions with one rule. */
void test_when_source(void)
{
    QIr *ir = compile(SEEN "#Big: #Watched\n" WROOM("roomB", "#Watched") WROOM("roomC", "#Watched") WROOM("roomD", "#Big") WARM);
    if (!ir) return;
    char *text = qi_text(ir);
    HAS(text, "  cell roomB.visible: bool = false\n");
    HAS(text, "  map abstract, at a switch: fine -> coarse, native.room.abstract, for 4 cells, when roomB.visible is false\n");
    HAS(text, "  map refine, at a switch: coarse -> fine, native.room.refine, for 4 cells, when roomB.visible is true\n");
    /* the condition is not a law */
    CHECK(!strstr(text, "law roomB.abstract.when") && !strstr(text, "law roomB.refine.when"));
    free(text);
    const QiMap *abs = &ir->maps[0], *ref = &ir->maps[1];
    CHECK(!strcmp(ref->when.path, "visible") && ref->when.mask == 2 && !strcmp(ir->cells[ref->when.cell].path, "roomB.visible") &&
          !strcmp(ref->when.text, ".visible"));
    CHECK(!strcmp(abs->when.path, "visible") && abs->when.mask == 1 && !strcmp(abs->when.text, ".visible == false"));

    QWorld *w = build(ir, 3), *replayed = build(ir, 3);
    if (w && replayed) {
        QwPhen b, c, d;
        OK(qw_find_phenomenon(w, "roomB", &b));
        OK(qw_find_phenomenon(w, "roomC", &c));
        OK(qw_find_phenomenon(w, "roomD", &d));
        qw_record(w, true);
        /* the world has the type of each phenomenon and the condition of the content */
        CHECK(!strcmp(qw_type(w, b), "#Watched") && !strcmp(qw_type(w, d), "#Big"));
        QwCondition got;
        QwLevel level;
        OK(qw_condition(w, b, "refine", &got, &level));
        CHECK(level == QW_LEVEL_SOURCE && !strcmp(got.path, "visible") && got.mask == 2);

        /* two rooms, one rule, two inputs */
        OK(qw_run(w, 50 * SEC));
        OK(qw_input_set(w, "roomB.visible", 2));
        OK(qw_run(w, 100 * SEC));
        CHECK(is_fine(w, "roomB") && !is_fine(w, "roomC") && !is_fine(w, "roomD"));

        /* the application replaces the condition of the content for the exact type: a room becomes fine when
         * its coarse temperature is above 280 K, which is true at once. roomD is a #Big, and it keeps the rule
         * of the content */
        QwCondition warm = qw_when_in("coarse.temperature", (QInterval) { 280000000, Q_INF }), never = qw_never();
        OK(qw_override_type_condition(w, "#Watched", "refine", &warm));
        OK(qw_override_type_condition(w, "#Watched", "abstract", &never));
        OK(qw_run(w, 150 * SEC));
        CHECK(is_fine(w, "roomB") && is_fine(w, "roomC") && !is_fine(w, "roomD"));
        OK(qw_condition(w, c, "refine", &got, &level));
        CHECK(level == QW_LEVEL_TYPE);
        OK(qw_condition(w, d, "refine", &got, &level));
        CHECK(level == QW_LEVEL_SOURCE);

        /* the instance has precedence: roomC becomes coarse while nobody looks, and roomB stays fine */
        QwCondition unseen = qw_when_false("visible");
        OK(qw_override_condition(w, c, "abstract", &unseen));
        OK(qw_override_condition(w, c, "refine", &never));
        OK(qw_run(w, 200 * SEC));
        CHECK(is_fine(w, "roomB") && !is_fine(w, "roomC"));
        OK(qw_condition(w, c, "abstract", &got, &level));
        CHECK(level == QW_LEVEL_INSTANCE);

        /* the removal of both: the rule of the content again. Nobody looks at roomC, and somebody looks at roomB */
        OK(qw_clear_condition(w, c, "abstract"));
        OK(qw_clear_condition(w, c, "refine"));
        OK(qw_condition(w, c, "abstract", &got, &level));
        CHECK(level == QW_LEVEL_TYPE && got.path == NULL);
        OK(qw_clear_type_condition(w, "#Watched", "refine"));
        OK(qw_clear_type_condition(w, "#Watched", "abstract"));
        OK(qw_condition(w, c, "abstract", &got, &level));
        CHECK(level == QW_LEVEL_SOURCE && got.mask == 1);
        OK(qw_input_set(w, "roomB.visible", 1));
        OK(qw_input_set(w, "roomD.visible", 2));
        OK(qw_run(w, 300 * SEC));
        CHECK(!is_fine(w, "roomB") && !is_fine(w, "roomC") && is_fine(w, "roomD"));
        CHECK_I64((int64_t) qw_switches(w), 5);

        /* the replay: the same calls in a new world from the same IR give the same state, and the same
         * number of boundaries and of switches */
        size_t n = 0;
        const QwCall *calls = qw_calls(w, &n);
        CHECK_I64((int64_t) n, 16);
        OK(qw_replay(replayed, calls, n));
        CHECK(qw_hash(replayed) == qw_hash(w));
        CHECK_I64((int64_t) qw_boundaries(replayed), (int64_t) qw_boundaries(w));
        CHECK_I64((int64_t) qw_switches(replayed), 5);
    }
    qw_free(w);
    qw_free(replayed);
    qi_free(ir);
}

/* The forms of a condition in source. */
void test_when_forms(void)
{
    static const struct { const char *refine, *abstract, *text; } forms[] = {
        /* a quantity: the room becomes fine above 294 K, and coarse again when its first cell is above 297 K */
        { ", when: .coarse.temperature > 294K", ", when: .fine.c0.temperature >= 297K",
          "  map abstract, at a switch: fine -> coarse, native.room.abstract, for 4 cells, when roomB.fine.c0.temperature is >= 297\n" },
        { ", when: .coarse.temperature > 294K", "",
          "  map refine, at a switch: coarse -> fine, native.room.refine, for 4 cells, when roomB.coarse.temperature is >= 294.000001\n" },
        /* the path on the right, and a number between two steps of the field */
        { ", when: 294.0000005K <= .coarse.temperature", "",
          "native.room.refine, for 4 cells, when roomB.coarse.temperature is >= 294.000001\n" },
        { ", when: .coarse.temperature < 294.0000005K", "", "when roomB.coarse.temperature is <= 294\n" },
        { ", when: .coarse.temperature <= 294K", "", "when roomB.coarse.temperature is <= 294\n" },
        { ", when: .coarse.temperature == 294K", "", "when roomB.coarse.temperature is 294\n" },
        /* through the port of the active model */
        { ", when: .west.temperature > 294K", ", when: .west.temperature > 297K",
          "  map abstract, at a switch: fine -> coarse, native.room.abstract, for 4 cells, when roomB.west.temperature is >= 297.000001\n" },
        /* an atom of an enumeration, with == and with != */
        { ", when: .door == \"open\"", ", when: .door != \"open\"",
          "  map abstract, at a switch: fine -> coarse, native.room.abstract, for 4 cells, when roomB.door is closed\n" },
        { ", when: .visible != false", "", "when roomB.visible is true\n" },
        /* a truth value of the map itself */
        { ", when: true", "", "native.room.refine, for 4 cells, when roomB.refine.when is true\n" },
    };
    for (size_t i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        char src[4096];
        snprintf(src, sizeof src,
                 "#Watched: #Phenomenon & { west: #ThermalPort, spread: %%temperature, model: *\"coarse\" | text,\n"
                 "  visible: *false | bool, door: *\"closed\" | \"open\" | \"closed\", coarse: #CoarseRoom, fine: #FineRoom,\n"
                 "  refine: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.refine\"%s },\n"
                 "  abstract: #Map & { from: \"fine\", to: \"coarse\", evaluator: \"native.room.abstract\"%s } }\n%s%s",
                 forms[i].refine, forms[i].abstract, WROOM("roomB", "#Watched"), WARM);
        QIr *ir = compile(src);
        if (!ir) continue;
        char *text = qi_text(ir);
        HAS(text, forms[i].text);
        free(text);
        qi_free(ir);
    }

    /* the room becomes fine when it is warm, and coarse again when its first cell is warmer. Then the whole
     * room is above the first limit, so it becomes fine again at the next boundary: the test stops there */
    static const char *const pairs[2][2] = {
        { ", when: .coarse.temperature > 294K", ", when: .fine.c0.temperature >= 297K" },
        { ", when: .west.temperature > 294K", ", when: .west.temperature >= 297K" },     /* through the port */
    };
    for (int v = 0; v < 2; v++) {
        char src[4096];
        snprintf(src, sizeof src,
                 "#Watched: #Phenomenon & { west: #ThermalPort, spread: %%temperature, model: *\"coarse\" | text,\n"
                 "  visible: *false | bool, coarse: #CoarseRoom, fine: #FineRoom,\n"
                 "  refine: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.refine\"%s },\n"
                 "  abstract: #Map & { from: \"fine\", to: \"coarse\", evaluator: \"native.room.abstract\"%s } }\n%s%s",
                 pairs[v][0], pairs[v][1], WROOM("roomB", "#Watched"), WARM);
        QIr *ir = compile(src), *none = compile(WATCHED("", "") WROOM("roomB", "#Watched") WARM);
        QWorld *w = ir ? build(ir, 1) : NULL, *plain = none ? build(none, 1) : NULL;
        if (w && plain) {
            int became = 0, back = 0;
            for (int t = 5; t <= 1500 && !back; t += 5) {
                OK(qw_run(w, t * SEC));
                if (!became) OK(qw_run(plain, t * SEC));
                if (!became && is_fine(w, "roomB")) {
                    became = t;
                    /* the world with no condition is above the limit at that time, and it was not before */
                    CHECK(qw_get(plain, "roomB.coarse.temperature").lo > 294000000);
                    CHECK(qw_get(plain, "roomB.coarse.temperature").lo < 294200000);
                }
                if (became && qw_switches(w) >= 2) {
                    back = t;
                    /* an odd number of switches: fine. The room can be fine again at this stop */
                    CHECK(is_fine(w, "roomB") == (qw_switches(w) % 2 == 1));
                }
            }
            CHECK(became > 5 && back > became);
            CHECK_I64((int64_t) qw_switches(plain), 0);
            /* a map with no condition has no condition in the world */
            QwPhen b;
            QwCondition got;
            QwLevel level;
            OK(qw_find_phenomenon(plain, "roomB", &b));
            OK(qw_condition(plain, b, "refine", &got, &level));
            CHECK(level == QW_LEVEL_NONE && got.path == NULL);
            OK(qw_find_phenomenon(w, "roomB", &b));
            OK(qw_condition(w, b, "abstract", &got, &level));
            CHECK(level == QW_LEVEL_SOURCE && !strcmp(got.path, v ? "west.temperature" : "fine.c0.temperature") &&
                  got.range.lo == 297000000 && got.range.hi == Q_INF);
        }
        qw_free(w);
        qw_free(plain);
        qi_free(ir);
        qi_free(none);
    }

    /* two conditions that are true together stop the run, with the names of the two transitions */
    QIr *ir = compile("#One: #Model & { c0: #Lump }\n"
                      "#Three: #Phenomenon & { model: \"a\", go: bool, spread: %temperature, a: #CoarseRoom, b: #One, c: #One,\n"
                      "  ab: #Map & { from: \"a\", to: \"b\", evaluator: \"native.room.refine\", when: .go },\n"
                      "  ac: #Map & { from: \"a\", to: \"c\", evaluator: \"native.room.refine\", when: .go == true } }\n"
                      "t: #Three & { go: false, spread: 0K, a: { temperature: 300K, capacity: 1J/K },\n"
                      "  b: { c0: { capacity: 1J/K } }, c: { c0: { capacity: 1J/K } } }\n");
    if (ir) {
        QWorld *w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 10 * SEC));
            OK(qw_input_set(w, "t.go", 2));
            uint64_t h = qw_hash(w);
            CHECK(qw_run(w, 20 * SEC) == QW_ECONDITION);
            HAS(qw_error(w), "t: the conditions of the transitions 'ab' and 'ac' are true at one time, and a phenomenon "
                             "makes one switch: the conditions must exclude each other");
            CHECK(qw_hash(w) == h && qw_switches(w) == 0);
            /* with one of them removed, the run goes on */
            QwPhen t;
            QwCondition never = qw_never();
            OK(qw_find_phenomenon(w, "t", &t));
            CHECK(t == 0);
            OK(qw_override_type_condition(w, "#Three", "ac", &never));     /* for the first phenomenon of the world */
            QwCondition got;
            QwLevel level;
            OK(qw_condition(w, t, "ac", &got, &level));
            CHECK(level == QW_LEVEL_TYPE);
            OK(qw_run(w, 20 * SEC));
            CHECK(!strcmp(qw_active_model(w, t), "b"));
        }
        qw_free(w);
    }
    qi_free(ir);
}

/* The errors of a condition in source. */
void test_when_errors(void)
{
    static const struct { const char *refine, *message; } cases[] = {
        { ", when: .coarse.temperature", "error: the condition of the map roomB.refine is not a truth value. A condition is a path "
          "to a field with the type bool, as in `when: .visible`, a comparison, as in `when: .temperature > 300K`, or "
          "conditions with !, && and ||\n" },
        { ", when: .door", "error: the condition of the map roomB.refine is not a truth value." },
        { ", when: false", "error: the condition of the map roomB.refine is always false: remove it, and the map has no "
          "condition\n" },
        { ", when: .coarse.temperature == 300.0000005K", "error: the condition of the map roomB.refine is .coarse.temperature == "
          "300.0000005K: the number is between two steps of the field. Use <= or >=\n" },
        { ", when: .coarse.temperature > 5J", "error: the condition of the map roomB.refine is .coarse.temperature > 5J: the "
          "number is in J, and roomB.coarse.temperature is in K\n" },
        { ", when: .coarse.temperature == true", "error: the condition of the map roomB.refine is .coarse.temperature == true, "
          "and roomB.coarse.temperature is a quantity: compare it with a quantity\n" },
        { ", when: .visible == 5", "error: the condition of the map roomB.refine is .visible == 5, and roomB.visible is a set of "
          "atoms: compare it with an atom, with == or !=\n" },
        { ", when: .door == \"ajar\"", "error: the condition of the map roomB.refine is .door == \"ajar\", and ajar is not an "
          "atom of roomB.door. Its atoms: closed, open\n" },
        /* a condition with an operator (ADR 0022 sections 1 to 3) */
        { ", when: .visible && .coarse.temperature", "error: the condition .visible && .coarse.temperature reads roomB.coarse.temperature as a truth "
          "value, and it is a quantity. A condition has a path to a field with the type bool, a comparison of "
          "a path with an atom (== or !=), a comparison of two quantities, and the operators !, && and ||\n" },
        { ", when: .door || .visible", "error: the condition .door || .visible reads roomB.door as a truth value, and it does not have the "
          "type bool. A condition has" },
        { ", when: .visible && .door == \"ajar\"", "error: the condition .visible && (.door == \"ajar\") has the comparison .door == \"ajar\", and ajar "
          "is not an atom of roomB.door. Its atoms: closed, open\n" },
        { ", when: .visible || .coarse.temperature > 5J", "error: the condition .visible || (.coarse.temperature > 5J) compares .coarse.temperature, in K, "
          "with 5J, in J\n" },
        { ", when: .visible || .coarse.temperature + 1J > 300K", "error: the condition .visible || ((.coarse.temperature + 1J) > 300K) compares .coarse.temperature, "
          "in K, with 1J, in J\n" },
        { ", when: !.visible || .coarse.temperature == 300.0000005K", "error: the condition !.visible || (.coarse.temperature == 300.0000005K) compares "
          ".coarse.temperature with 300.0000005K: the number is between two steps of the field, so the two are "
          "never equal. Use <= or >=\n" },
        { ", when: .visible || .coarse.temperature != 300.0000005K", "error: the condition .visible || (.coarse.temperature != 300.0000005K) compares .coarse.temperature "
          "with 300.0000005K: the number is between two steps of the field, so the two are never equal. Use < "
          "or >\n" },
        { ", when: .visible == 5 || .visible", "error: the condition (.visible == 5) || .visible has a comparison with roomB.visible, which is a "
          "set of atoms. A set of atoms stands in a comparison with an atom, with == or !=\n" },
        { ", when: .visible || .coarse.temperature == true", "error: the condition .visible || (.coarse.temperature == true) has the comparison "
          ".coarse.temperature == true, and roomB.coarse.temperature is a quantity: compare it with a "
          "quantity\n" },
        { ", when: .visible || .coarse.temperature > true", "error: the atom true can stand in an expression only in a comparison with == or !=\n" },
        { ", when: .visible && .door > .door", "error: the condition .visible && (.door > .door) has a comparison with roomB.door, which is a set "
          "of atoms." },
        { ", when: .visible == .visible", "error: the condition .visible == .visible has a comparison with roomB.visible, which is a set of "
          "atoms. A set of atoms stands in a comparison with an atom, with == or !=\n" },
        { ", when: .coarse.temperature > .coarse.capacity", "error: the condition .coarse.temperature > .coarse.capacity compares .coarse.temperature, in K, "
          "with .coarse.capacity, in J/K\n" },
        { ", when: .coarse.temperature > .coarse.temperature * .coarse.capacity", "error: the condition .coarse.temperature > (.coarse.temperature * .coarse.capacity) compares "
          ".coarse.temperature, in K, with .coarse.temperature * .coarse.capacity, in J\n" },
        { ", when: !.coarse.temperature", "error: the condition !.coarse.temperature reads roomB.coarse.temperature as a truth value, and it "
          "is a quantity." },
        { ", when: .coarse.temperature < .spread < .coarse.temperature", "error: expected a field label" },
        { ", when: .visible && .nothing", "error: unknown field 'nothing'." },
        { ", when: .coarse.temperature != 5J", "error: the condition .coarse.temperature != 5J compares .coarse.temperature, in K, with 5J, in J\n" },
        { ", when: .west.capacity > 5J/K, extra: 1", "unknown field 'extra'" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char src[4096];
        snprintf(src, sizeof src,
                 "#Watched: #Phenomenon & { west: #ThermalPort, spread: %%temperature, model: *\"coarse\" | text,\n"
                 "  visible: *false | bool, door: *\"closed\" | \"open\" | \"closed\", coarse: #CoarseRoom, fine: #FineRoom,\n"
                 "  refine: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.refine\"%s } }\n%s%s",
                 cases[i].refine, WROOM("roomB", "#Watched"), WARM);
        HAS(compile_error(src), cases[i].message);
    }
    /* a field with one text value is not state, so a condition cannot read it */
    HAS(compile_error("#Solo: #Phenomenon & { west: #ThermalPort, spread: %temperature, model: \"coarse\", mode: \"on\",\n"
                      "  coarse: #CoarseRoom, fine: #FineRoom,\n"
                      "  refine: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.refine\", when: .mode != \"on\" } }\n"
                      WROOM("roomB", "#Solo")),
        "error: the condition of the map roomB.refine reads roomB.mode, which is not a cell of roomB\n");
    /* an instance can give the condition of a map of its type */
    QIr *ir = compile(WATCHED("", "") WROOM("roomB", "#Watched") "roomB: { refine: { when: .visible }, visible: true }\n" WARM);
    if (ir) {
        CHECK(ir->maps[1].when.path && !strcmp(ir->maps[1].when.path, "visible") && ir->maps[0].when.path == NULL);
        QWorld *w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 0));
            CHECK(is_fine(w, "roomB"));
        }
        qw_free(w);
    }
    qi_free(ir);
    /* an atom in a law that is not the condition of a map */
    HAS(compile_error("#P: #Phenomenon & { on: bool, rule: .on == true }\np: #P & { on: true }\n"),
        "error: the law .on == true uses p.on, which is a set of atoms: arithmetic needs quantities");
    HAS(compile_error("#P: #Phenomenon & { on: bool, x: %energy, rule: .x == true + 1J }\np: #P & { on: true }\n"),
        "error: the atom true can stand in an expression only in a comparison with == or !=");
}

/* --- a condition with operators (ADR 0022 sections 1 to 4) ------------------------------------------------ */

enum { K_FALSE, K_TRUE, K_UNKNOWN };
static const uint64_t K_MASK[3] = { 1, 2, 3 };
static int k_not(int a) { return a == K_UNKNOWN ? K_UNKNOWN : !a; }
static int k_and(int a, int b) { return a == K_FALSE || b == K_FALSE ? K_FALSE : a == K_TRUE && b == K_TRUE ? K_TRUE : K_UNKNOWN; }
static int k_or(int a, int b) { return k_not(k_and(k_not(a), k_not(b))); }

/* ADR 0022, acceptance test 2: each operator gives the result of section 2
 * for each pair of true, false and unknown. */
void test_truth_tables(void)
{
    QIr *ir = compile("#Gate: #Phenomenon & { a: bool, b: bool, both: bool, either: bool, nota: bool, mixed: bool, same: bool,\n"
                      "  laws: [ .both == (.a && .b), .either == (.a || .b), .nota == !.a,\n"
                      "          .mixed == (!(.a && .b) || .a == false && .b != true), .same == .a ] }\n"
                      "g: #Gate\n");
    QWorld *w = ir ? build(ir, 1) : NULL;
    for (int a = 0; w && a < 3; a++)
        for (int b = 0; b < 3; b++) {
            OK(qw_input_set(w, "g.a", K_MASK[a]));
            OK(qw_input_set(w, "g.b", K_MASK[b]));
            CHECK(qw_get_set(w, "g.both") == K_MASK[k_and(a, b)]);
            CHECK(qw_get_set(w, "g.either") == K_MASK[k_or(a, b)]);
            CHECK(qw_get_set(w, "g.nota") == K_MASK[k_not(a)]);
            /* && binds more than ||, and ! binds most */
            CHECK(qw_get_set(w, "g.mixed") == K_MASK[k_or(k_not(k_and(a, b)), k_and(k_not(a), k_not(b)))]);
            CHECK(qw_get_set(w, "g.same") == K_MASK[a]);
            /* the inputs keep their values */
            CHECK(qw_get_set(w, "g.a") == K_MASK[a] && qw_get_set(w, "g.b") == K_MASK[b]);
        }
    qw_free(w);
    qi_free(ir);
}

/* A detail room with a limit, a door, and a truth value that a law gives. */
#define GUARDED(refine, abstract)                                                                       \
    "#Guarded: #Phenomenon & { west: #ThermalPort, spread: %temperature, model: *\"coarse\" | text,\n"  \
    "  visible: *false | bool, limit: %temperature, door: *\"closed\" | \"open\" | \"closed\", busy: bool,\n" \
    "  coarse: #CoarseRoom, fine: #FineRoom,\n"                                                         \
    "  laws: [ .busy == (.visible || .door == \"open\") ],\n"                                           \
    "  refine: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.refine\"" refine " },\n" \
    "  abstract: #Map & { from: \"fine\", to: \"coarse\", evaluator: \"native.room.abstract\"" abstract " } }\n"

/* ADR 0022, acceptance test 3: a condition with || and a comparison of two
 * fields. The room makes its switch at the first boundary where one part is
 * true. While one part is unknown and the other is false, it makes none. */
void test_truth_when(void)
{
    /* the limit is 293 K: the condition is "above 294 K", as in the world with a condition on one cell */
    QIr *ir = compile(GUARDED(", when: .visible || .west.temperature > .limit + 1K", "") WROOM("roomB", "#Guarded & { limit: 293K }")
                      WARM);
    QIr *plain = compile(WATCHED(", when: .coarse.temperature > 294K", "") WROOM("roomB", "#Watched") WARM);
    QWorld *w = ir ? build(ir, 1) : NULL, *ref = plain ? build(plain, 1) : NULL;
    if (w && ref) {
        char *text = qi_text(ir);
        HAS(text, "  derived roomB.refine.when: bool = false | true\n");
        HAS(text, "  map refine, at a switch: coarse -> fine, native.room.refine, for 4 cells, when roomB.refine.when is true\n");
        /* the port of the active model gives a copy, and the comparison is a range of a difference */
        HAS(text, "  propagator P8, in an epoch, in the model roomB.coarse: roomB.refine.when.1 = roomB.coarse.temperature\n");
        HAS(text, "  propagator P11, in an epoch: roomB.refine.when.4 = roomB.refine.when.1 - roomB.refine.when.3\n");
        HAS(text, "  propagator P12, in an epoch: roomB.refine.when.5 <=> roomB.refine.when.4 is >= 0.000001\n");
        HAS(text, "  propagator P13, in an epoch: roomB.refine.when <=> roomB.visible || roomB.refine.when.5\n"
                  "    law roomB.refine.when: .visible || (.west.temperature > (.limit + 1K))\n");
        HAS(text, "  propagator P6, in an epoch: roomB.laws.first.1 <=> roomB.door is open\n");
        HAS(text, "  roomB.refine.when: narrowed by P13\n");
        free(text);
        QwPhen b;
        QwCondition got;
        QwLevel level;
        OK(qw_find_phenomenon(w, "roomB", &b));
        OK(qw_condition(w, b, "refine", &got, &level));
        CHECK(level == QW_LEVEL_SOURCE && !strcmp(got.path, "refine.when") && got.mask == 2);
        /* the cell of a condition is derived: it takes no input */
        CHECK(qw_input_set(w, "roomB.refine.when", 2) == QW_EOWNER);
        HAS(qw_error(w), "'roomB.refine.when' is derived: a law gives its value");
        int became = 0;
        for (int t = 5; t <= 1500; t += 5) {
            OK(qw_run(w, t * SEC));
            OK(qw_run(ref, t * SEC));
            CHECK(is_fine(w, "roomB") == is_fine(ref, "roomB"));
            CHECK(qw_get(w, "roomA.temperature").lo == qw_get(ref, "roomA.temperature").lo);
            if (!became && is_fine(w, "roomB")) became = t;
            /* the cell has the value of the condition at each stop */
            CHECK(qw_get_set(w, "roomB.refine.when") == (became ? 2u : 1u));
        }
        CHECK(became > 5 && became < 1500);
        CHECK_I64((int64_t) qw_switches(w), 1);
    }
    qw_free(w);
    qw_free(ref);
    qi_free(ir);
    qi_free(plain);

    /* the limit has no value: the comparison is unknown, and the room makes no switch */
    ir = compile(GUARDED(", when: .visible || .west.temperature > .limit + 1K", "") WROOM("roomB", "#Guarded") WARM);
    for (int v = 0; ir && v < 2; v++) {
        w = build(ir, 1);
        if (!w) continue;
        for (int t = 100; t <= 1500; t += 100) {
            OK(qw_run(w, t * SEC));
            CHECK(qw_get_set(w, "roomB.refine.when") == 3);
        }
        CHECK(!is_fine(w, "roomB") && qw_switches(w) == 0);
        /* v 0: true || unknown is true. v 1: the limit gets a value, and the comparison is true */
        if (v == 0) OK(qw_input_set(w, "roomB.visible", 2));
        else OK(qw_input(w, "roomB.limit", qi_point(293000000)));
        CHECK(qw_get_set(w, "roomB.refine.when") == 2);
        OK(qw_run(w, 1500 * SEC));
        CHECK(is_fine(w, "roomB") && qw_switches(w) == 1);
        qw_free(w);
    }
    qi_free(ir);

    /* ! and &&: the room is coarse again when nobody looks at it and it is below a limit */
    ir = compile(GUARDED(", when: .visible", ", when: !.visible && .west.temperature < .limit + 100K")
                 WROOM("roomB", "#Guarded & { limit: 293K }") WARM);
    w = ir ? build(ir, 1) : NULL;
    if (w) {
        OK(qw_run(w, 10 * SEC));
        CHECK(!is_fine(w, "roomB") && qw_get_set(w, "roomB.abstract.when") == 2);   /* true, and `fine` is not active */
        OK(qw_input_set(w, "roomB.visible", 2));
        CHECK(qw_get_set(w, "roomB.abstract.when") == 1);
        OK(qw_run(w, 20 * SEC));
        CHECK(is_fine(w, "roomB"));
        OK(qw_input_set(w, "roomB.visible", 1));
        OK(qw_run(w, 30 * SEC));
        CHECK(!is_fine(w, "roomB"));
        CHECK_I64((int64_t) qw_switches(w), 2);
    }
    qw_free(w);
    qi_free(ir);
}

/* ADR 0022, acceptance tests 4 and 5: the application replaces a condition
 * with one that names a truth value from a law, and a replay gives the same
 * state after each call. */
void test_truth_override(void)
{
    QIr *ir = compile(GUARDED(", when: .visible && .west.temperature > .limit", ", when: !.busy && !.visible")
                      WROOM("roomB", "#Guarded & { limit: 900K }") WARM);
    QWorld *w = ir ? build(ir, 1) : NULL, *again = ir ? build(ir, 1) : NULL;
    if (w && again) {
        uint64_t hashes[16];
        size_t nh = 0;
#define STEP(call)                       \
    do {                                 \
        OK(call);                        \
        hashes[nh++] = qw_hash(w);       \
    } while (0)
        QwPhen b;
        OK(qw_find_phenomenon(w, "roomB", &b));
        qw_record(w, true);
        /* the condition of the content is false: the room is not above 900 K */
        STEP(qw_input_set(w, "roomB.visible", 2));
        STEP(qw_run(w, 10 * SEC));
        CHECK(!is_fine(w, "roomB"));
        STEP(qw_input_set(w, "roomB.visible", 1));
        /* the new condition: the truth value `busy`, which the law gives from the door */
        QwCondition busy = qw_when_true("busy"), got;
        QwLevel level;
        STEP(qw_override_condition(w, b, "refine", &busy));
        OK(qw_condition(w, b, "refine", &got, &level));
        CHECK(level == QW_LEVEL_INSTANCE && !strcmp(got.path, "busy") && got.mask == 2);
        STEP(qw_run(w, 20 * SEC));
        CHECK(!is_fine(w, "roomB") && qw_get_set(w, "roomB.busy") == 1);
        STEP(qw_input_set(w, "roomB.door", 2));                          /* open */
        CHECK(qw_get_set(w, "roomB.busy") == 2);
        STEP(qw_run(w, 30 * SEC));
        CHECK(is_fine(w, "roomB"));
        /* the door closes: the condition of the content for `abstract` is true */
        STEP(qw_input_set(w, "roomB.door", 1));
        STEP(qw_run(w, 40 * SEC));
        CHECK(!is_fine(w, "roomB"));
        /* a condition can name the cell of another transition. A run to the current time reads the conditions */
        QwCondition other = qw_when_true("abstract.when");
        STEP(qw_override_condition(w, b, "refine", &other));
        STEP(qw_run(w, 40 * SEC));
        CHECK(is_fine(w, "roomB"));
        CHECK_I64((int64_t) qw_switches(w), 3);
#undef STEP

        size_t n = 0;
        const QwCall *calls = qw_calls(w, &n);
        CHECK_I64((int64_t) n, (int64_t) nh);
        for (size_t k = 0; k < n && k < nh; k++) {
            OK(qw_replay(again, calls + k, 1));
            CHECK(qw_hash(again) == hashes[k]);
        }
        CHECK_I64((int64_t) qw_switches(again), 3);
        CHECK_I64((int64_t) qw_boundaries(again), (int64_t) qw_boundaries(w));
    }
    qw_free(w);
    qw_free(again);
    qi_free(ir);
}

/* The forms of a comparison in a condition. */
void test_truth_compare(void)
{
    QIr *ir = compile("#Box: #Phenomenon & { t: %temperature, u: %temperature, a: bool,\n"
                      "  more: bool, always: bool, same: bool, never: bool, twice: bool, differs: bool, turned: bool,\n"
                      "  laws: [ .more == (.t > .u + 1K), .always == (.t > .t - 1K), .same == (.t + .u >= .u + .t),\n"
                      "          .never == (.t - .u < .t - .u), .twice == (.t + .u > 2 * .u), .differs == (.t != 300K),\n"
                      "          .turned == (300K < .t) ] }\n"
                      "box: #Box\n");
    QWorld *w = ir ? build(ir, 1) : NULL;
    if (w) {
        OK(qw_run(w, 0));
        /* with no values: the same text on the two sides goes away, and each other comparison is unknown */
        CHECK(qw_get_set(w, "box.always") == 2 && qw_get_set(w, "box.same") == 2 && qw_get_set(w, "box.never") == 1);
        CHECK(qw_get_set(w, "box.more") == 3 && qw_get_set(w, "box.twice") == 3 && qw_get_set(w, "box.differs") == 3);
        static const struct { int64_t t, u; uint64_t more, twice, differs, turned; } cases[] = {
            { 300000000, 298000000, 2, 2, 1, 1 },
            { 300000000, 299000000, 1, 2, 1, 1 },     /* t is u + 1 K: not more */
            { 300000001, 299000000, 2, 2, 2, 2 },     /* one step above */
            { 299999999, 299999999, 1, 1, 2, 1 },     /* t + u is 2 * u */
            { 290000000, 299000000, 1, 1, 2, 1 },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            OK(qw_input(w, "box.t", qi_point(cases[i].t)));
            OK(qw_input(w, "box.u", qi_point(cases[i].u)));
            CHECK(qw_get_set(w, "box.more") == cases[i].more);
            CHECK(qw_get_set(w, "box.twice") == cases[i].twice);
            CHECK(qw_get_set(w, "box.differs") == cases[i].differs);
            CHECK(qw_get_set(w, "box.turned") == cases[i].turned);
            CHECK(qw_get_set(w, "box.always") == 2 && qw_get_set(w, "box.same") == 2 && qw_get_set(w, "box.never") == 1);
        }
        /* a range of t that has the limit: unknown */
        OK(qw_input(w, "box.t", (QInterval) { 299000000, 301000000 }));
        CHECK(qw_get_set(w, "box.differs") == 3 && qw_get_set(w, "box.turned") == 3);
        OK(qw_input(w, "box.t", (QInterval) { 300000001, 301000000 }));
        CHECK(qw_get_set(w, "box.differs") == 2 && qw_get_set(w, "box.turned") == 2);
    }
    qw_free(w);
    if (ir) {
        char *text = qi_text(ir);
        /* a difference and its range */
        HAS(text, "  propagator P1, in an epoch: box.laws.first.3 = box.t - box.laws.first.2\n"
                  "    law box.laws.first: .more == (.t > (.u + 1K))\n"
                  "  propagator P2, in an epoch: box.more <=> box.laws.first.3 is >= 0.000001\n");
        /* the same text on the two sides goes away: a number is left, or nothing */
        HAS(text, "  inner box.laws.rest.first.1: dim[K]@0 = 1\n");
        HAS(text, "  propagator P3, in an epoch: box.always <=> box.laws.rest.first.1 is >= 1\n");
        HAS(text, "  inner box.laws.rest.rest.first.1: bool = false\n");
        HAS(text, "  propagator P4, in an epoch: box.same <=> !box.laws.rest.rest.first.1\n");
        HAS(text, "  inner box.laws.rest.rest.rest.first.1: bool = true\n");
        /* one cell and one number: a range of the cell, with no cell for the number */
        HAS(text, "  propagator P10, in an epoch: box.laws.rest.rest.rest.rest.rest.first.1 <=> box.t is 300\n");
        HAS(text, "  propagator P11, in an epoch: box.differs <=> !box.laws.rest.rest.rest.rest.rest.first.1\n");
        HAS(text, "  propagator P12, in an epoch: box.turned <=> box.t is >= 300.000001\n");
        CHECK(!strstr(text, "rest.rest.rest.rest.rest.rest.first.1"));
        free(text);
    }
    qi_free(ir);

    /* a law is a relation: a result that the instance gives makes the fields of the condition smaller */
    ir = compile("#Box: #Phenomenon & { t: %temperature, a: bool, r: bool, laws: [ .r == (.a || .t > 300K) ] }\n"
                 "box: #Box & { r: false }\n");
    w = ir ? build(ir, 1) : NULL;
    if (w) {
        OK(qw_run(w, 0));
        CHECK(qw_get_set(w, "box.a") == 1);
        CHECK(qw_get(w, "box.t").hi == 300000000 && qw_get(w, "box.t").lo == Q_NINF);
    }
    qw_free(w);
    qi_free(ir);

    /* the values of the start contradict the law */
    HAS(compile_error("#Box: #Phenomenon & { a: bool, b: bool, r: bool, laws: [ .r == (.a || .b) ] }\n"
                      "box: #Box & { a: true, r: false }\n"),
        "error: the values of box at the start contradict the law box.laws.first: .r == (.a || .b) (box.r = false, box.a = "
        "true, box.b = false or true)\n");
    HAS(compile_error("#Box: #Phenomenon & { t: %temperature, r: bool, laws: [ .r == !(.t > 300K) ] }\n"
                      "box: #Box & { t: 301K, r: true }\n"),
        "error: the values of box at the start contradict the law box.laws.first");
    /* the forms of a law with a truth value */
    HAS(compile_error("#Box: #Phenomenon & { t: %temperature, a: bool, b: bool, laws: [ .t == (.a && .b) ] }\nbox: #Box\n"),
        "error: the law .t == (.a && .b) gives the value of a condition to box.t, which does not have the type bool\n");
    HAS(compile_error("#Box: #Phenomenon & { door: \"open\" | \"closed\", a: bool, laws: [ .door == !.a ] }\nbox: #Box\n"),
        "error: the law .door == !.a gives the value of a condition to box.door, which does not have the type bool\n");
    HAS(compile_error("#Box: #Phenomenon & { a: bool, b: bool, laws: [ .a && .b ] }\nbox: #Box\n"),
        "error: the law .a && .b is a condition with a logic operator. A law can give a condition to a field with the type "
        "bool, as in `.busy == (.a && .b)`\n");
    HAS(compile_error("#Box: #Phenomenon & { a: bool, laws: [ !.a ] }\nbox: #Box\n"),
        "error: the law !.a is a condition with a logic operator.");
}

/* A condition reads through a connection and through the port of the active
 * model, and a snapshot keeps a world with such conditions. */
void test_truth_paths(void)
{
    /* through a connection: the field of another phenomenon */
    QIr *ir = compile("#Linked: #Phenomenon & { other: #ThermalPort, west: #ThermalPort, spread: %temperature, model: \"coarse\",\n"
                      "  coarse: #CoarseRoom, fine: #FineRoom,\n"
                      "  refine: #Map & { from: \"coarse\", to: \"fine\", evaluator: \"native.room.refine\", when: .other.temperature < 300K } }\n"
                      "roomA: #LumpedRoom & { temperature: 303.15K, capacity: 10000J/K }\n"
                      WROOM("roomB", "#Linked & { other: roomA.west }")
                      "wall: #ConductiveWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 1pct }\n");
    QWorld *w = ir ? build(ir, 1) : NULL;
    if (w) {
        char *text = qi_text(ir);
        HAS(text, "when roomB.refine.when is true\n");
        HAS(text, "  propagator P6, in an epoch: roomB.refine.when <=> roomA.temperature is <= 299.999999\n");
        free(text);
        int became = 0;
        for (int t = 5; t <= 1500 && !became; t += 5) {
            OK(qw_run(w, t * SEC));
            if (is_fine(w, "roomB")) became = t;
            /* the switch is at the first stop where the other room is below 300 K */
            CHECK((qw_get(w, "roomA.temperature").hi < 300000000) == (became != 0));
        }
        CHECK(became > 5);
    }
    qw_free(w);
    qi_free(ir);

    /* a set of atoms through the port of the active model: the truth value follows a switch */
    ir = compile("#Lit: { lit: bool }\n"
                 "#FlagPort: #ThermalPort & #Lit\n"
                 "#P: #Model & #Lump & #Lit & { west: #FlagPort & { temperature: .temperature, capacity: .capacity, "
                 "energy: .energy, lit: .lit } }\n"
                 "#Q: #Model & #Lit & { c0: #Lump, west: #FlagPort & { temperature: .c0.temperature, capacity: .c0.capacity, "
                 "energy: .c0.energy, lit: .lit } }\n"
                 "#R: #Phenomenon & { west: #FlagPort, spread: %temperature, model: \"p\", p: #P, q: #Q, seen: bool,\n"
                 "  laws: [ .seen == (.west.lit && .west.temperature > 280K) ],\n"
                 "  pq: #Map & { from: \"p\", to: \"q\", evaluator: \"native.room.refine\" } }\n"
                 "r: #R & { spread: 0K, p: { temperature: 300K, capacity: 1J/K, lit: true }, q: { c0: { capacity: 1J/K }, "
                 "lit: false } }\n");
    w = ir ? build(ir, 1) : NULL;
    if (w) {
        OK(qw_run(w, 5 * SEC));
        CHECK(qw_get_set(w, "r.seen") == 2);
        OK(qi_switch(ir, w, "r", "q"));
        OK(qw_run(w, 10 * SEC));
        CHECK(qw_get_set(w, "r.seen") == 1);
        CHECK(qw_get(w, "r.q.c0.temperature").lo == 300000000);
    }
    qw_free(w);
    qi_free(ir);

    /* a snapshot: the cell of a condition is not state, and the world goes on in the same way */
    ir = compile(GUARDED(", when: .visible || .west.temperature > .limit + 1K", ", when: !.visible && .door == \"open\"")
                 WROOM("roomB", "#Guarded & { limit: 293K }") WARM);
    w = ir ? build(ir, 1) : NULL;
    QWorld *copy = ir ? build(ir, 1) : NULL;
    if (w && copy) {
        void *bytes = NULL;
        size_t len = 0;
        OK(qw_run(w, 100 * SEC));
        OK(qw_snapshot(w, &bytes, &len));
        OK(qw_restore(copy, bytes, len));
        free(bytes);
        CHECK(qw_hash(copy) == qw_hash(w));
        CHECK(qw_get_set(copy, "roomB.refine.when") == qw_get_set(w, "roomB.refine.when"));
        OK(qw_run(w, 1000 * SEC));
        OK(qw_run(copy, 1000 * SEC));
        CHECK(is_fine(w, "roomB") && is_fine(copy, "roomB"));
        CHECK(qw_hash(copy) == qw_hash(w));
        OK(qw_input_set(w, "roomB.door", 2));
        OK(qw_input_set(copy, "roomB.door", 2));
        OK(qw_run(w, 1100 * SEC));
        OK(qw_run(copy, 1100 * SEC));
        /* the room is above the limit: fine and coarse in turn, one switch at each boundary */
        CHECK(qw_switches(w) > 2 && qw_switches(copy) == qw_switches(w));
        CHECK(qw_hash(copy) == qw_hash(w));
    }
    qw_free(w);
    qw_free(copy);
    qi_free(ir);
}
