#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "front/front.h"
#include "quad/ir.h"
#include "quad/kernels.h"
#include "quad/world.h"
#include "test.h"

/* Stage 3 of the front end: the elaborator, the IR, and the world that the
 * IR builds (ADR 0011 section 3, ADR 0016). */

#define OK(x) CHECK((x) == QW_OK)

#define STR_EQ(got, want)                                                              \
    do {                                                                               \
        const char *g_ = (got), *w_ = (want);                                          \
        t_checks++;                                                                    \
        if (strcmp(g_, w_)) {                                                          \
            t_failures++;                                                              \
            fprintf(stderr, "%s:%d:\n  got    %s\n  wanted %s\n", __FILE__, __LINE__, g_, w_); \
        }                                                                              \
    } while (0)

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

/* The types of the thermal world: tests/data/elab/thermal.quad. */
static const char *thermal(void)
{
    static char *text;
    if (!text) text = read_file("tests/data/elab/thermal.quad");
    CHECK(text != NULL);
    return text ? text : "";
}

/* Compiles the thermal types with `rest`. NULL with the diagnostics on stderr. */
static QIr *compile(const char *rest, const QiOptions *opt)
{
    const char *names[2] = { "thermal.quad", "world.quad" }, *src[2] = { thermal(), rest };
    char *diag = NULL;
    QIr *ir = qi_compile(names, src, 2, opt, &diag);
    CHECK(ir != NULL);
    if (!ir) fprintf(stderr, "%s", diag);
    free(diag);
    return ir;
}

/* The diagnostics of a content with the thermal types, or "compiled". */
static const char *compile_error(const char *rest)
{
    static char out[4096];
    const char *names[2] = { "thermal.quad", "world.quad" }, *src[2] = { thermal(), rest };
    char *diag = NULL;
    QIr *ir = qi_compile(names, src, 2, NULL, &diag);
    snprintf(out, sizeof out, "%s", ir ? "compiled" : diag);
    CHECK((ir != NULL) == (diag[0] == 0));
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

/* --- the two-room world ------------------------------------------------------------------------- */

/* The world of tests/test_world.c, built through the C API: two lumped rooms
 * at ta and tb (K) with capacities ca and cb (J/K), and a wall of g (W/K). */
static QWorld *c_rooms(uint64_t seed, double ta, double tb, int64_t ca, int64_t cb, int64_t g, int64_t tol_e9)
{
    QWorld *w = qw_new(seed, -6);
    QwKind energy, temp, cap, cond, ratio;
    QwPhen a, b, wall;
    OK(qw_kind(w, "energy", "J", -3, true, &energy));
    OK(qw_kind(w, "temperature", "K", -6, false, &temp));
    OK(qw_kind(w, "capacity", "J/K", -3, false, &cap));
    OK(qw_kind(w, "conductance", "W/K", -3, false, &cond));
    OK(qw_kind(w, "ratio", "", -9, false, &ratio));
    OK(qw_phenomenon(w, "roomA", &a));
    OK(qw_phenomenon(w, "roomB", &b));
    OK(qw_phenomenon(w, "wall", &wall));
    const char *names[2] = { "roomA", "roomB" };
    double temps[2] = { ta, tb };
    int64_t caps[2] = { ca, cb };
    for (int i = 0; i < 2; i++) {
        char path[64];
        QnCell e, c, t;
        int64_t cq = caps[i] * 1000;
        int64_t eq = (int64_t) llround((double) cq * temps[i]);
        snprintf(path, sizeof path, "%s.energy", names[i]);
        OK(qw_cell(w, path, energy, qi_point(eq), &e));
        snprintf(path, sizeof path, "%s.capacity", names[i]);
        OK(qw_cell(w, path, cap, qi_point(cq), &c));
        snprintf(path, sizeof path, "%s.temperature", names[i]);
        OK(qw_cell(w, path, temp, qi_top(), &t));
        CHECK(qn_product(qw_net(w), e, c, t, NULL) == QN_OK);
    }
    OK(qw_cell(w, "wall.conductance", cond, qi_point(g * 1000), NULL));
    OK(qw_cell(w, "wall.tolerance", ratio, qi_point(tol_e9), NULL));
    OK(qw_connect(w, wall, "hot", "roomA"));
    OK(qw_connect(w, wall, "cold", "roomB"));
    OK(qw_attach(w, wall, qk_conduction()));
    return w;
}

static const char *const CELLS[8] = { "roomA.energy",      "roomA.capacity",   "roomA.temperature", "roomB.energy",
                                      "roomB.capacity",    "roomB.temperature", "wall.conductance",  "wall.tolerance" };

/* The world from source and the world from C have the same state at each
 * time in `times` (in seconds). */
static void same_runs(QWorld *src, QWorld *c, const int *times, int n)
{
    CHECK(qw_hash(src) == qw_hash(c));
    for (int i = 0; i < n; i++) {
        OK(qw_run(src, (int64_t) times[i] * 1000000));
        OK(qw_run(c, (int64_t) times[i] * 1000000));
        CHECK(qw_hash(src) == qw_hash(c));
        CHECK(qw_hash(src) != 0);
        CHECK_I64((int64_t) qw_boundaries(src), (int64_t) qw_boundaries(c));
        CHECK_I64((int64_t) qw_advances(src), (int64_t) qw_advances(c));
        for (int k = 0; k < 8; k++) {
            QInterval x = qw_get(src, CELLS[k]), y = qw_get(c, CELLS[k]);
            CHECK(x.lo == y.lo && x.hi == y.hi);
        }
    }
}

/* ADR 0011, acceptance test 1: the two-room world in source gives the same
 * state hash at 1000 s as the world that the C tests build. */
void test_elab_two_rooms(void)
{
    char *rooms = read_file("tests/data/elab/tworooms.quad");
    CHECK(rooms != NULL);
    if (!rooms) return;
    QIr *ir = compile(rooms, NULL);
    free(rooms);
    if (!ir) return;
    QWorld *w = build(ir, 1);
    QWorld *c = c_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
    if (w) {
        const int times[3] = { 0, 500, 1000 };
        int64_t total = qw_get(w, "roomA.energy").lo + qw_get(w, "roomB.energy").lo;
        same_runs(w, c, times, 3);
        CHECK_I64(qw_now(w), 1000000000);
        CHECK_I64((int64_t) qw_boundaries(w), 200);
        CHECK_I64(qw_get(w, "roomA.energy").lo + qw_get(w, "roomB.energy").lo, total);   /* conservation is exact */
        double diff = (double) (qw_get(w, "roomA.temperature").lo - qw_get(w, "roomB.temperature").lo) * 1e-6;
        CHECK(diff > 1.3 && diff < 1.4);                 /* after two time constants: about 10 e^-2 K */
        /* a port is a view: its paths name the room's cells */
        QnCell direct, through;
        OK(qw_find(w, "roomA.energy", &direct));
        OK(qw_find(w, "roomA.west.energy", &through));
        CHECK(direct == through);
        /* the wall's port reaches the room */
        QInterval hot = qw_get(w, "roomA.west.temperature"), own = qw_get(w, "roomA.temperature");
        CHECK(hot.lo == own.lo && hot.hi == own.hi && hot.lo != Q_NINF);
    }
    qw_free(w);
    qw_free(c);
    qi_free(ir);

    /* unequal rooms, and another seed */
    ir = compile("roomA: #LumpedRoom & { energy: 1550000J, capacity: 5000J/K }\n"
                 "roomB: #LumpedRoom & { energy: 5600000J, capacity: 20000J/K }\n"
                 "wall: #ConductiveWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 1pct }\n",
                 NULL);
    if (!ir) return;
    w = build(ir, 7);
    c = c_rooms(7, 310.0, 280.0, 5000, 20000, 10, 10000000);
    if (w) {
        const int times[2] = { 700, 2000 };
        same_runs(w, c, times, 2);
    }
    qw_free(w);
    qw_free(c);
    /* one IR builds any number of equal worlds */
    QWorld *w1 = build(ir, 3), *w2 = build(ir, 3);
    if (w1 && w2) {
        OK(qw_run(w1, 250000000));
        OK(qw_run(w2, 250000000));
        CHECK(qw_hash(w1) == qw_hash(w2));
    }
    qw_free(w1);
    qw_free(w2);
    qi_free(ir);
}

/* The IR of the two-room world, as text: kinds, cells with owners, ports,
 * propagators, the kernel with the cell of each slot, and the dependencies. */
void test_elab_ir_text(void)
{
    char *rooms = read_file("tests/data/elab/tworooms.quad");
    CHECK(rooms != NULL);
    if (!rooms) return;
    QIr *ir = compile(rooms, NULL);
    free(rooms);
    if (!ir) return;
    char *text = qi_text(ir);
    STR_EQ(text,
           "quadrilateral ir 1\n"
           "tick 10^-6 s\n"
           "base deg\n"
           "kind %capacity: m^2 kg s^-2 K^-1, scale -3\n"
           "kind %conductance: m^2 kg s^-3 K^-1, scale -3\n"
           "kind %energy: m^2 kg s^-2, scale -3, conserved\n"
           "kind %ratio: 1, scale -9\n"
           "kind %temperature: K, scale -6\n"
           "phenomenon roomA: #LumpedRoom\n"
           "  cell roomA.capacity: %capacity = 10000\n"
           "  stock roomA.energy: %energy = 3031500\n"
           "  cell roomA.temperature: %temperature = any\n"
           "  alias roomA.west.capacity = roomA.capacity\n"
           "  alias roomA.west.energy = roomA.energy\n"
           "  alias roomA.west.temperature = roomA.temperature\n"
           "  port west: #ThermalPort, a view\n"
           "  propagator P0, in an epoch: roomA.energy = roomA.capacity * roomA.temperature\n"
           "    law roomA.storage: .energy == (.capacity * .temperature)\n"
           "phenomenon roomB: #LumpedRoom\n"
           "  cell roomB.capacity: %capacity = 10000\n"
           "  stock roomB.energy: %energy = 2931500\n"
           "  cell roomB.temperature: %temperature = any\n"
           "  alias roomB.west.capacity = roomB.capacity\n"
           "  alias roomB.west.energy = roomB.energy\n"
           "  alias roomB.west.temperature = roomB.temperature\n"
           "  port west: #ThermalPort, a view\n"
           "  propagator P1, in an epoch: roomB.energy = roomB.capacity * roomB.temperature\n"
           "    law roomB.storage: .energy == (.capacity * .temperature)\n"
           "phenomenon wall: #ConductiveWall\n"
           "  cell wall.conductance: %conductance = 10\n"
           "  cell wall.tolerance: %ratio = 0.01\n"
           "  port cold: #ThermalPort, connected to roomB.west\n"
           "  port hot: #ThermalPort, connected to roomA.west\n"
           "  kernel K0, at a boundary: native.conduction\n"
           "    in hot.temperature = roomA.temperature\n"
           "    in cold.temperature = roomB.temperature\n"
           "    in hot.capacity = roomA.capacity\n"
           "    in cold.capacity = roomB.capacity\n"
           "    link hot.energy = roomA.energy\n"
           "    link cold.energy = roomB.energy\n"
           "    in conductance = wall.conductance\n"
           "    in tolerance = wall.tolerance\n"
           "    transfer heat: roomA.energy -> roomB.energy, conserved\n"
           "dependencies\n"
           "  roomA.capacity: narrowed by P0; read by K0\n"
           "  roomA.energy: narrowed by P0; transferred by K0\n"
           "  roomA.temperature: narrowed by P0; read by K0\n"
           "  roomB.capacity: narrowed by P1; read by K0\n"
           "  roomB.energy: narrowed by P1; transferred by K0\n"
           "  roomB.temperature: narrowed by P1; read by K0\n"
           "  wall.conductance: read by K0\n"
           "  wall.tolerance: read by K0\n");
    free(text);

    /* the same facts as data */
    CHECK(ir->tick_scale == -6 && ir->nphens == 3 && ir->ncells == 8 && ir->naliases == 6 && ir->nports == 4);
    CHECK(ir->nprops == 2 && ir->nkernels == 1 && ir->nkinds == 5 && ir->nenums == 0);
    if (ir->ncells == 8 && ir->nprops == 2 && ir->nkernels == 1) {
        const QiCell *energy = &ir->cells[1];
        STR_EQ(energy->path, "roomA.energy");
        CHECK(energy->stock && energy->phen == 0 && energy->domain == QI_INTERVAL);
        CHECK(energy->base.lo == 3031500000 && energy->base.hi == 3031500000);            /* millijoules */
        STR_EQ(ir->kinds[energy->kind].name, "%energy");
        CHECK(ir->kinds[energy->kind].conserved && ir->kinds[energy->kind].scale == -3);
        const QiCell *temp = &ir->cells[2];
        CHECK(!temp->stock && temp->base.lo == Q_NINF && temp->base.hi == Q_INF);
        const QiProp *p = &ir->props[0];
        CHECK(p->op == QI_PRODUCT && p->phen == 0 && p->cell[0] == 1 && p->cell[1] == 0 && p->cell[2] == 2);
        STR_EQ(p->file, "thermal.quad");
        STR_EQ(p->name, "roomA.storage");
        CHECK(p->line == 20);
        const QiKernelUse *k = &ir->kernels[0];
        CHECK(k->phen == 2 && k->nbinds == 8 && k->ntransfers == 1);
        CHECK(k->transfers[0].from == 1 && k->transfers[0].to == 4 && k->transfers[0].conserved);
        CHECK(k->binds[4].role == QK_LINK && k->binds[4].cell == 1);
        /* the dependency graph: the wall's kernel reads the room's temperature, and it does not write it */
        int reads = 0, writes = 0, transfers = 0, narrows = 0;
        for (uint32_t i = 0; i < ir->ndeps; i++) {
            const QiDep *d = &ir->deps[i];
            if (d->cell == 2 && d->kernel && d->use == QI_READS) reads++;
            if (d->use == QI_WRITES) writes++;
            if (d->use == QI_TRANSFERS) transfers++;
            if (d->use == QI_NARROWS) narrows++;
        }
        CHECK(reads == 1 && writes == 0 && transfers == 2 && narrows == 6);
    }
    qi_free(ir);
}

/* --- order -------------------------------------------------------------------------------------- */

/* The declarations of a source in another order, as new source text. */
static char *shuffled(const char *name, const char *src, uint64_t *rng)
{
    Front *f = qf_new();
    DeclVec d = { 0 };
    bool ok = qf_parse(f, name, src, &d);
    CHECK(ok);
    Buf b = { 0 };
    qf_buf_puts(&b, "");
    if (ok && d.n) {
        for (size_t i = d.n - 1; i > 0; i--) {
            size_t j = (size_t) t_range(rng, 0, (int64_t) i);
            Decl tmp = d.v[i]; d.v[i] = d.v[j]; d.v[j] = tmp;
        }
        for (size_t i = 0; i < d.n; i++) qf_print_decls(&b, &d.v[i], 1);
    }
    VEC_FREE(d);
    qf_free(f);
    return b.s;
}

static const char *const HOUSE =
    "#Material: \"brick\" | \"wood\" | \"stone\"\n"
    "#Room: #LumpedRoom & { door: \"open\" | \"closed\", of: #Material, lit: bool, celsius: %temperature,\n"
    "    warm: .celsius == .temperature - 273.15K, loss: %energy / %time, leak: .loss == 2W/K * (.temperature - 280K) }\n"
    "kitchen: #Room & { energy: 3031500J, capacity: 10000J/K, door: \"open\", of: \"brick\" }\n"
    "kitchen: { lit: true }\n"
    "hall: #Room & { energy: 2931500J, capacity: 10000J/K, of: \"wood\" | \"stone\" }\n"
    "cellar: #Room & { energy: 5600000J, capacity: 20000J/K, lit: false }\n"
    "#InnerWall: #ConductiveWall & { tolerance: 0.01 }\n"
    "w1: #InnerWall & { hot: kitchen.west, cold: hall.west, conductance: 10W/K }\n"
    "w2: #InnerWall & { hot: hall.west, cold: cellar.west, conductance: 4W/K }\n";

/* The IR does not depend on the order of the files or of the declarations
 * (ADR 0016 section 6), and neither does the world that it builds. */
void test_prop_elab_order(void)
{
    QIr *base = compile(HOUSE, NULL);
    if (!base) return;
    char *want = qi_text(base);
    QWorld *w0 = build(base, 5);
    uint64_t hash = 0;
    if (w0) {
        OK(qw_run(w0, 300000000));
        hash = qw_hash(w0);
        CHECK(qw_boundaries(w0) > 10);
    }
    qw_free(w0);
    qi_free(base);

    uint64_t rng = 2026;
    for (int round = 0; round < 25; round++) {
        char *a = shuffled("thermal.quad", thermal(), &rng), *b = shuffled("world.quad", HOUSE, &rng);
        bool swap = t_range(&rng, 0, 1);
        const char *names[2] = { swap ? "world.quad" : "thermal.quad", swap ? "thermal.quad" : "world.quad" };
        const char *src[2] = { swap ? b : a, swap ? a : b };
        char *diag = NULL;
        QIr *ir = qi_compile(names, src, 2, NULL, &diag);
        CHECK(ir != NULL);
        if (ir) {
            char *got = qi_text(ir);
            CHECK(!strcmp(got, want));
            if (strcmp(got, want) && t_failures < 3) {
                const char *g = got, *w = want;
                while (*g && *g == *w) { g++; w++; }
                fprintf(stderr, "  round %d: the texts differ at\n  got    %.200s\n  wanted %.200s\n", round, g, w);
            }
            free(got);
            if (round < 5) {
                QWorld *w = build(ir, 5);
                if (w) {
                    OK(qw_run(w, 300000000));
                    CHECK(qw_hash(w) == hash);
                }
                qw_free(w);
            }
        } else
            fprintf(stderr, "%s", diag);
        free(diag);
        qi_free(ir);
        free(a);
        free(b);
    }
    free(want);
}

/* --- sets ----------------------------------------------------------------------------------------- */

/* A field of atoms is a set cell (ADR 0012). Its enumeration is every atom
 * that the field can have by its declaration, in the order of the names. */
void test_elab_sets(void)
{
    QIr *ir = compile(HOUSE, NULL);
    if (!ir) return;
    char *text = qi_text(ir);
    HAS(text, "enumeration \"closed\" | \"open\": closed open\n");
    HAS(text, "enumeration #Material: brick stone wood\n");
    HAS(text, "enumeration bool: false true\n");
    HAS(text, "  cell kitchen.door: \"closed\" | \"open\" = open\n");
    HAS(text, "  cell hall.door: \"closed\" | \"open\" = closed | open\n");
    HAS(text, "  cell kitchen.of: #Material = brick\n");
    HAS(text, "  cell hall.of: #Material = stone | wood\n");
    HAS(text, "  cell cellar.of: #Material = brick | stone | wood\n");
    HAS(text, "  cell kitchen.lit: bool = true\n");
    HAS(text, "  cell hall.lit: bool = false | true\n");
    /* a type that sets a field gives the value to its instances */
    HAS(text, "  cell w1.tolerance: %ratio = 0.01\n");
    HAS(text, "phenomenon w2: #InnerWall\n");
    /* one enumeration for each type of field, however many cells use it */
    int lines = 0;
    for (const char *p = text; (p = strstr(p, "\nenumeration ")) != NULL; p++) lines++;
    CHECK(lines == 3 && ir->nenums == 3);
    free(text);
    QWorld *w = build(ir, 1);
    if (w) {
        OK(qw_run(w, 0));
        CHECK(qw_get_set(w, "kitchen.door") == 2);       /* closed is atom 0, open is atom 1 */
        CHECK(qw_get_set(w, "hall.door") == 3);
        CHECK(qw_get_set(w, "kitchen.of") == 1);         /* brick, stone, wood */
        CHECK(qw_get_set(w, "hall.of") == 6);
        CHECK(qw_get_set(w, "cellar.of") == 7);
        CHECK(qw_get_set(w, "kitchen.lit") == 2);        /* the world's boolean: false, true */
        CHECK(qw_get_set(w, "cellar.lit") == 1);
        CHECK(qw_get_set(w, "hall.lit") == 3);
        QnCell lit;
        OK(qw_find(w, "kitchen.lit", &lit));
        CHECK(qn_universe(qw_net(w), lit) == 3);
        OK(qw_input_set(w, "hall.door", 1));
        CHECK(qw_get_set(w, "hall.door") == 1);
    }
    qw_free(w);
    qi_free(ir);

    STR_EQ(compile_error("#Odd: #Phenomenon & { tag: \"two words\" | \"one\" }\nx: #Odd\n"),
           "world.quad:2:1: error: x.tag: the atom \"two words\" needs a name of letters, digits and '_' to be in a set cell\n");
}

/* --- laws ----------------------------------------------------------------------------------------- */

static const char *const PLATE =
    "%length: { scale: -3 }\n"
    "%area: { dimension: \"m^2\", scale: -6 }\n"
    "#Plate: #Phenomenon & {\n"
    "    width: %length, height: %length, area: %area, half: %length, extra: %length, same: %length,\n"
    "    aspect: %ratio, twice: %length\n"
    "    twice: .width + .width\n"
    "    by_area: .area == .width * .height, by_half: .half == .width + .height\n"
    "    by_extra: .width - .height == .extra, by_same: .same == .width, by_aspect: .aspect == .width / .height\n"
    "}\n";

/* Each form of a law gives one propagator, and the world narrows in every direction. */
void test_elab_laws(void)
{
    char src[2048];
    snprintf(src, sizeof src, "%splate: #Plate & { width: 2m, height: 0.5m }\nback: #Plate & { area: 6m^2, height: 2m }\n", PLATE);
    QIr *ir = compile(src, NULL);
    if (!ir) return;
    char *text = qi_text(ir);
    HAS(text, ": plate.area = plate.width * plate.height\n    law plate.by_area: .area == (.width * .height)\n");
    HAS(text, ": plate.half = plate.width + plate.height\n");
    HAS(text, ": plate.extra = plate.width - plate.height\n    law plate.by_extra: (.width - .height) == .extra\n");
    HAS(text, ": plate.same = plate.width\n    law plate.by_same: .same == .width\n");
    HAS(text, ": plate.aspect = plate.width / plate.height\n");
    HAS(text, ": plate.twice = plate.width + plate.width\n    law plate.twice: .width + .width\n");
    HAS(text, "  plate.width: narrowed by P");
    free(text);
    CHECK(ir->nprops == 12 && ir->nkernels == 0 && ir->nports == 0);
    QWorld *w = build(ir, 1);
    if (w) {
        OK(qw_run(w, 0));
        CHECK_I64(qw_get(w, "plate.area").lo, 1000000);       /* 1 m^2 in mm^2 */
        CHECK_I64(qw_get(w, "plate.area").hi, 1000000);
        CHECK_I64(qw_get(w, "plate.half").lo, 2500);
        CHECK_I64(qw_get(w, "plate.extra").lo, 1500);
        CHECK_I64(qw_get(w, "plate.same").hi, 2000);
        CHECK_I64(qw_get(w, "plate.aspect").lo, 4000000000);  /* 4, in units of 10^-9 */
        CHECK_I64(qw_get(w, "plate.twice").lo, 4000);
        /* backward: the area and the height give the width */
        CHECK_I64(qw_get(w, "back.width").lo, 3000);
        CHECK_I64(qw_get(w, "back.width").hi, 3000);
        CHECK_I64(qw_get(w, "back.twice").hi, 6000);
    }
    qw_free(w);
    qi_free(ir);

    /* a contradiction between a law and the values is found when the world settles */
    snprintf(src, sizeof src, "%sbad: #Plate & { width: 2m, height: 1m, area: 3m^2 }\n", PLATE);
    ir = compile(src, NULL);
    if (ir && (w = build(ir, 1))) {
        CHECK(qw_run(w, 0) == QW_ECONTRADICTION);
        HAS(qw_error(w), "bad.");
        qw_free(w);
    }
    qi_free(ir);
}

/* A law can have inner expressions, numbers and inequalities (ADR 0018). An
 * inner value and a number are derived cells. */
void test_elab_expressions(void)
{
    /* the wall law of the stage 2 fixture: a nested expression, and a power with a dimension and no kind */
    char *physics = read_file("tests/data/resolve/physics.quad");
    CHECK(physics != NULL);
    if (physics) {
        const char *names[2] = { "physics.quad", "world.quad" };
        const char *src[2] = { physics, "roomA: #LumpedRoom & { energy: 303150J, capacity: 1000J/K }\n"
                                        "roomB: #LumpedRoom & { energy: 293150J, capacity: 1000J/K }\n"
                                        "wall: #Wall & { conductance: 20W/K, hot: roomA.west, cold: roomB.west }\n" };
        char *diag = NULL;
        QIr *ir = qi_compile(names, src, 2, NULL, &diag);
        CHECK(ir != NULL);
        if (!ir) fprintf(stderr, "%s", diag);
        free(diag);
        if (ir) {
            char *text = qi_text(ir);
            HAS(text, "kind dim[m^2 kg s^-3]@-3: m^2 kg s^-3, scale -3, anonymous\n");
            HAS(text, "phenomenon wall: #Wall\n"
                      "  cell wall.conductance: %conductance = 20\n"
                      "  derived wall.laws.first.1: %temperature = any\n"
                      "  cell wall.power: dim[m^2 kg s^-3]@-3 = any\n"
                      "  port cold: #ThermalPort, connected to roomB.west\n"
                      "  port hot: #ThermalPort, connected to roomA.west\n"
                      "  propagator P2, in an epoch: wall.laws.first.1 = roomA.temperature - roomB.temperature\n"
                      "    law wall.laws.first: .power == (.conductance * (.hot.temperature - .cold.temperature))\n"
                      "  propagator P3, in an epoch: wall.power = wall.conductance * wall.laws.first.1\n"
                      "    law wall.laws.first: .power == (.conductance * (.hot.temperature - .cold.temperature))\n");
            HAS(text, "  wall.laws.first.1: narrowed by P2, P3\n");
            free(text);
            QWorld *w = build(ir, 1);
            if (w) {
                OK(qw_run(w, 0));
                CHECK_I64(qw_get(w, "wall.laws.first.1").lo, 10000000);     /* 10 K */
                CHECK_I64(qw_get(w, "wall.power").lo, 200000);              /* 20 W/K × 10 K = 200 W, in mW */
                CHECK_I64(qw_get(w, "wall.power").hi, 200000);
            }
            qw_free(w);
        }
        qi_free(ir);
        free(physics);
    }

    /* numbers, inequalities, and expressions on both sides */
    const char *forms = "#T: #Phenomenon & {\n"
                        "    kelvin: %temperature, celsius: %temperature, a: %temperature, b: %temperature,\n"
                        "    twice: %temperature, half: %temperature, low: %temperature, high: %temperature\n"
                        "    c: .celsius == .kelvin - 273.15K\n"
                        "    d: .a + 1K <= .b\n"
                        "    e: .twice == 2 * (.a + .b)\n"
                        "    f: .a > 2K\n"
                        "    g: .half * 2 == .kelvin - 100K\n"
                        "    h: .low < .high, i: .high >= .kelvin\n"
                        "}\n";
    char src[2048];
    snprintf(src, sizeof src, "%st: #T & { kelvin: 300K, b: <=5K, low: >=310K }\n", forms);
    QIr *ir = compile(src, NULL);
    if (ir) {
        char *text = qi_text(ir);
        /* a number has the scale that makes it exact: 273.15 K is 27315 × 10^-2, and 100 K is 1 × 10^2 */
        HAS(text, "kind dim[1]@0: 1, scale 0, anonymous\n");
        HAS(text, "kind dim[K]@-2: K, scale -2, anonymous\n");
        HAS(text, "kind dim[K]@0: K, scale 0, anonymous\n");
        HAS(text, "kind dim[K]@2: K, scale 2, anonymous\n");
        HAS(text, "  derived t.g.3: dim[K]@2 = 100\n");
        HAS(text, "  derived t.c.1: dim[K]@-2 = 273.15\n");
        HAS(text, "  propagator P0, in an epoch: t.celsius = t.kelvin - t.c.1\n    law t.c: .celsius == (.kelvin - 273.15K)\n");
        HAS(text, "  propagator P1, in an epoch: t.d.2 = t.a + t.d.1\n    law t.d: (.a + 1K) <= .b\n"
                  "  propagator P2, in an epoch: t.d.2 <= t.b\n    law t.d: (.a + 1K) <= .b\n");
        HAS(text, "  propagator P5, in an epoch: t.f.1 < t.a\n    law t.f: .a > 2K\n");
        /* expressions on both sides: a derived cell for each side, and an equality */
        HAS(text, "  propagator P6, in an epoch: t.g.2 = t.g.4\n");
        HAS(text, "  propagator P7, in an epoch: t.g.4 = t.kelvin - t.g.3\n");
        HAS(text, "  propagator P8, in an epoch: t.g.2 = t.half * t.g.1\n");
        HAS(text, "  propagator P9, in an epoch: t.low < t.high\n    law t.h: .low < .high\n"
                  "  propagator P10, in an epoch: t.kelvin <= t.high\n    law t.i: .high >= .kelvin\n");
        free(text);
        int nderived = 0;
        for (uint32_t i = 0; i < ir->ncells; i++) nderived += ir->cells[i].derived;
        CHECK(nderived == 10 && ir->nprops == 11);
        QWorld *w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 0));
            CHECK_I64(qw_get(w, "t.celsius").lo, 26850000);             /* 26.85, in the kind's quanta of 10^-6 K */
            CHECK_I64(qw_get(w, "t.celsius").hi, 26850000);
            CHECK_I64(qw_get(w, "t.a").lo, 2000001);                    /* more than 2 K: one quantum more */
            CHECK_I64(qw_get(w, "t.a").hi, 4000000);                    /* a + 1 K <= b, and b <= 5 K */
            CHECK_I64(qw_get(w, "t.b").lo, 3000001);
            CHECK_I64(qw_get(w, "t.twice").lo, 2 * (2000001 + 3000001));
            CHECK_I64(qw_get(w, "t.twice").hi, 2 * (4000000 + 5000000));
            CHECK_I64(qw_get(w, "t.half").lo, 100000000);               /* (300 - 100) / 2 = 100 K */
            CHECK_I64(qw_get(w, "t.half").hi, 100000000);
            CHECK_I64(qw_get(w, "t.high").lo, 310000001);               /* above low, which is at least 310 K */
            CHECK(qw_get(w, "t.low").hi == Q_INF);
            /* a derived cell is not state: it takes no input */
            CHECK(qw_input(w, "t.c.1", qi_point(5)) == QW_EOWNER);
            OK(qw_input(w, "t.kelvin", qi_point(400000000)));
            CHECK_I64(qw_get(w, "t.celsius").lo, 126850000);
        }
        qw_free(w);
    }
    qi_free(ir);

    /* a law that the values contradict */
    snprintf(src, sizeof src, "%st: #T & { kelvin: 300K, a: 7K, b: 5K }\n", forms);
    ir = compile(src, NULL);
    QWorld *w = ir ? build(ir, 1) : NULL;
    if (w) CHECK(qw_run(w, 0) == QW_ECONTRADICTION);
    qw_free(w);
    qi_free(ir);

    /* the value of a field: an expression with an inner expression */
    ir = compile("#V: #Phenomenon & { a: %temperature, b: %temperature, mean: %temperature, mean: (.a + .b) / 2 }\n"
                 "v: #V & { a: 280K, b: 300K }\n",
                 NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "  propagator P0, in an epoch: v.mean.1 = v.a + v.b\n    law v.mean: (.a + .b) / 2\n"
                  "  propagator P1, in an epoch: v.mean = v.mean.1 / v.mean.2\n");
        free(text);
        w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 0));
            CHECK_I64(qw_get(w, "v.mean").lo, 290000000);
            CHECK_I64(qw_get(w, "v.mean").hi, 290000000);
        }
        qw_free(w);
    }
    qi_free(ir);

    /* one law that two declarations state is one law, with one set of derived cells */
    ir = compile("#D: #Phenomenon & { a: %temperature, b: %temperature, up: .b == (.a + 1K) * 2 }\n"
                 "d: #D & { a: 2K }\nd: #D & { up: .b == (.a + 1K) * 2 }\n",
                 NULL);
    if (ir) CHECK(ir->nprops == 2 && ir->ncells == 5);
    qi_free(ir);

    /* a field with a dimension and no kind is a cell, with the default scale of the dimension (ADR 0001) */
    ir = compile("#R: #Phenomenon & { rate: %energy / %time, gain: %temperature / %time, flux: %energy / %time }\n"
                 "r: #R & { rate: 5W, gain: >=0.5K/s, flux: 1.5kW }\n",
                 NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "kind dim[s^-1 K]@-3: s^-1 K, scale -3, anonymous\n");
        HAS(text, "  cell r.flux: dim[m^2 kg s^-3]@-3 = 1500\n  cell r.gain: dim[s^-1 K]@-3 = 0.5 .. inf\n"
                  "  cell r.rate: dim[m^2 kg s^-3]@-3 = 5\n");
        free(text);
        CHECK(ir->nkinds == 2);                          /* the two powers have one anonymous kind */
        w = build(ir, 1);
        if (w) {
            CHECK_I64(qw_get(w, "r.rate").lo, 5000);
            OK(qw_input(w, "r.rate", qi_point(7000)));  /* such a cell is state, and it takes an input */
        }
        qw_free(w);
    }
    qi_free(ir);
    STR_EQ(compile_error("#R: #Phenomenon & { rate: %energy / %time }\nr: #R & { rate: 0.00001W }\n"),
           "world.quad:2:1: error: r.rate has no kind, so it has the scale -3 of its dimension, and its value does not "
           "fit: the value is not a whole number of quanta of the kind. Give the field a kind with another scale\n");
    STR_EQ(compile_error("#R: #Phenomenon & { rate: %energi / %time }\nr: #R\n"),
           "world.quad:1:27: error: unknown kind %energi. Declare it: %energi: { dimension: \"J\" }\n"
           "world.quad:2:4: error: cannot expand #R\n");
}

/* A law is a field with a name, so a subtype adds a law by unification, and
 * the IR has the name of each law (ADR 0016 section 4). */
void test_elab_named_laws(void)
{
    QIr *ir = compile("#Heated: #LumpedRoom & { gain: %temperature, twice: %temperature, heating: .twice == .gain + .gain }\n"
                      "#Lit: #Heated & { glow: %temperature, lighting: .glow == .twice }\n"
                      "h: #Lit & { energy: 1J, capacity: 1J/K, gain: 2K }\n",
                      NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "  propagator P0, in an epoch: h.twice = h.gain + h.gain\n    law h.heating: .twice == (.gain + .gain)\n"
                  "  propagator P1, in an epoch: h.glow = h.twice\n    law h.lighting: .glow == .twice\n"
                  "  propagator P2, in an epoch: h.energy = h.capacity * h.temperature\n"
                  "    law h.storage: .energy == (.capacity * .temperature)\n");
        free(text);
        CHECK(ir->nprops == 3 && ir->ncells == 6);          /* a law is not a cell */
        QWorld *w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 0));
            CHECK_I64(qw_get(w, "h.glow").lo, 4000000);
            CHECK_I64(qw_get(w, "h.temperature").hi, 1000000);
        }
        qw_free(w);
    }
    qi_free(ir);

    /* two declarations of one law must agree */
    HAS(compile_error("#Odd: #LumpedRoom & { storage: .energy == .capacity + .temperature }\no: #Odd & { energy: 1J, capacity: 1J/K }\n"),
        "conflict at storage");

    /* The labels of the prelude are in its package. A type of the content
     * can have a field `evaluator` or `laws` of its own, and a list of laws
     * still works where a type declares one. */
    ir = compile("#Recipe: { evaluator: text, laws: text }\n"
                 "soup: #Recipe & { evaluator: \"cook\", laws: \"none\" }\n"
                 "#Listed: #Phenomenon & { a: %temperature, b: %temperature, rules: [...constraint] }\n"
                 "l: #Listed & { a: 3K, rules: [ .b == .a + .a ] }\n"
                 "roomA: #LumpedRoom & { energy: 3031500J, capacity: 10000J/K }\n"
                 "roomB: #LumpedRoom & { energy: 2931500J, capacity: 10000J/K }\n"
                 "wall: #ConductiveWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 0.01 }\n",
                 NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "  kernel K0, at a boundary: native.conduction\n");
        HAS(text, "  propagator P0, in an epoch: l.b = l.a + l.a\n    law l.rules.first: .b == (.a + .a)\n");
        free(text);
        CHECK(ir->nphens == 4 && ir->nkernels == 1);
    }
    qi_free(ir);

    /* the prelude by its full name, and a type of the content with the name of a prelude type */
    const char *names[3] = { "thermal.quad", "attic.quad", "world.quad" };
    const char *src[3] = { thermal(),
                           "package attic\n#Phenomenon: { dusty: bool }\n#Box: quad.#Phenomenon & { load: %mass }\n"
                           "#Idea: #Phenomenon & { weight: %mass }\n",
                           "box: attic.#Box & { load: 2kg }\nidea: attic.#Idea & { weight: 1kg }\n" };
    char *diag = NULL;
    ir = qi_compile(names, src, 3, NULL, &diag);
    CHECK(ir != NULL);
    if (!ir) fprintf(stderr, "%s", diag);
    free(diag);
    if (ir) {
        /* attic.#Phenomenon is a type of the content: only the box is a phenomenon */
        CHECK(ir->nphens == 1 && ir->ncells == 1);
        if (ir->nphens == 1) STR_EQ(ir->phens[0].name, "box");
    }
    qi_free(ir);

    /* a content cannot declare in the package of the prelude */
    const char *bad[2] = { thermal(), "package quad\n#Extra: { x: text }\n#More: { y: text }\n" };
    ir = qi_compile(names, bad, 2, NULL, &diag);
    CHECK(ir == NULL);
    STR_EQ(diag, "attic.quad:2:1: error: the package quad is the prelude: a content cannot declare in it\n");
    free(diag);
    qi_free(ir);
}

/* Unrelated types can declare the same field, port or law (ADR 0017). */
void test_elab_shared_fields(void)
{
    /* two wall types, each with its own ports, and two types with a law of one name */
    QIr *ir = compile("roomA: #LumpedRoom & { energy: 3031500J, capacity: 10000J/K }\n"
                      "roomB: #LumpedRoom & { energy: 2931500J, capacity: 10000J/K }\n"
                      "#Pane: #Phenomenon & { hot: #ThermalPort, cold: #ThermalPort, shine: %ratio }\n"
                      "#Tank: #Phenomenon & #Thermal & { storage: .energy == .capacity * .temperature }\n"
                      "pane: #Pane & { hot: roomA.west, cold: roomB.west, shine: 0.5 }\n"
                      "tank: #Tank & { energy: 5J, capacity: 1J/K }\n"
                      "wall: #ConductiveWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 0.01 }\n",
                      NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "phenomenon pane: #Pane\n"
                  "  cell pane.shine: %ratio = 0.5\n"
                  "  port cold: #ThermalPort, connected to roomB.west\n"
                  "  port hot: #ThermalPort, connected to roomA.west\n");
        HAS(text, "    law tank.storage: .energy == (.capacity * .temperature)\n");
        HAS(text, "    law roomA.storage: .energy == (.capacity * .temperature)\n");
        HAS(text, "    link hot.energy = roomA.energy\n");
        free(text);
        /* the world is the two-room world, with two more phenomena that do not change it */
        QWorld *w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 1000000000));
            CHECK_I64((int64_t) qw_boundaries(w), 200);
            CHECK_I64(qw_get(w, "tank.temperature").lo, 5000000);
        }
        qw_free(w);
    }
    qi_free(ir);

    /* Two types give one field two enumerations. Each cell has the
     * enumeration of its own type. */
    ir = compile("#Gate: #Phenomenon & { state: \"open\" | \"closed\" }\n"
                 "#Lamp: #Phenomenon & { state: \"on\" | \"off\" | \"dim\" }\n"
                 "#Night: #Lamp & { state: \"off\" | \"dim\" }\n"
                 "gate: #Gate & { state: \"open\" }\n"
                 "lamp: #Lamp\n"
                 "late: #Night\n",
                 NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "enumeration \"closed\" | \"open\": closed open\n");
        HAS(text, "enumeration \"dim\" | \"off\" | \"on\": dim off on\n");
        HAS(text, "  cell gate.state: \"closed\" | \"open\" = open\n");
        HAS(text, "  cell lamp.state: \"dim\" | \"off\" | \"on\" = dim | off | on\n");
        HAS(text, "  cell late.state: \"dim\" | \"off\" | \"on\" = dim | off\n");      /* the enumeration of the supertype */
        free(text);
        CHECK(ir->nenums == 2);
        QWorld *w = build(ir, 1);
        if (w) {
            CHECK(qw_get_set(w, "gate.state") == 2);
            CHECK(qw_get_set(w, "late.state") == 3);
        }
        qw_free(w);
    }
    qi_free(ir);
}

/* --- data, and types that connect ------------------------------------------------------------------ */

void test_elab_data_and_types(void)
{
    /* an instance that is not a phenomenon is data. A phenomenon that names
     * it gets a copy, and the numbers of the copy are its own cells. */
    QIr *ir = compile("%density: { dimension: \"kg/m^3\" }\n"
                      "#Stuff: { density: %density, name: text }\n"
                      "oak: #Stuff & { density: 700kg/m^3, name: \"oak\" }\n"
                      "#Block: #Phenomenon & { made: #Stuff, spare: #Stuff }\n"
                      "block: #Block & { made: oak }\n",
                      NULL);
    if (ir) {
        char *text = qi_text(ir);
        STR_EQ(text, "quadrilateral ir 1\ntick 10^-6 s\nbase deg\n"
                     "kind %density: m^-3 kg, scale -3\n"
                     "phenomenon block: #Block\n"
                     "  cell block.made.density: %density = 700\n"
                     "  cell block.spare.density: %density = any\n"
                     "dependencies\n");
        free(text);
        QWorld *w = build(ir, 1);
        if (w) CHECK_I64(qw_get(w, "block.made.density").lo, 700000);
        qw_free(w);
    }
    qi_free(ir);

    /* a copy of data and a connection in one phenomenon */
    ir = compile("%density: { dimension: \"kg/m^3\" }\n"
                 "#Stuff: { density: %density }\n"
                 "oak: #Stuff & { density: 700kg/m^3 }\n"
                 "pine: #Stuff & { density: 500kg/m^3 }\n"
                 "roomA: #LumpedRoom & { energy: 3031500J, capacity: 10000J/K }\n"
                 "#Shelf: #Phenomenon & { board: #Stuff, edge: #Stuff, near: #ThermalPort }\n"
                 "shelf: #Shelf & { board: oak, near: roomA.west, edge: pine }\n",
                 NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "phenomenon shelf: #Shelf\n"
                  "  cell shelf.board.density: %density = 700\n"
                  "  cell shelf.edge.density: %density = 500\n"
                  "  port near: #ThermalPort, connected to roomA.west\n");
        free(text);
    }
    qi_free(ir);

    /* One value at two fields is one cell. The cell has the shortest path, and
     * of two paths of one length, the first in the order of the text. */
    ir = compile("#In: { deep: %temperature, far: %temperature }\n"
                 "#Twin: #Phenomenon & { zed: %temperature, alpha: .zed, inner: #In & { deep: .top, far: .alpha }, top: %temperature }\n"
                 "twin: #Twin & { zed: 300K }\n",
                 NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "phenomenon twin: #Twin\n"
                  "  cell twin.alpha: %temperature = 300\n"
                  "  cell twin.top: %temperature = any\n"
                  "  alias twin.inner.deep = twin.top\n"
                  "  alias twin.inner.far = twin.alpha\n"
                  "  alias twin.zed = twin.alpha\n"
                  "dependencies\n");
        free(text);
        QWorld *w = build(ir, 1);
        if (w) CHECK_I64(qw_get(w, "twin.zed").lo, 300000000);
        qw_free(w);
    }
    qi_free(ir);

    /* A label that two packages have: the path of a cell has the label,
     * without the package. */
    {
        const char *names[3] = { "thermal.quad", "attic.quad", "world.quad" };
        const char *src[3] = { thermal(), "package attic\n#Trunk: #Phenomenon & { size: %length }\n",
                               "#Crate: #Phenomenon & { size: %length }\n"
                               "trunk: attic.#Trunk & { size: 2m }\ncrate: #Crate & { size: 1m }\n" };
        char *diag = NULL;
        ir = qi_compile(names, src, 3, NULL, &diag);
        CHECK(ir != NULL);
        if (!ir) fprintf(stderr, "%s", diag);
        free(diag);
        if (ir) {
            char *text = qi_text(ir);
            HAS(text, "  cell crate.size: %length = 1\n");
            HAS(text, "  cell trunk.size: %length = 2\n");
            free(text);
            QWorld *w = build(ir, 1);
            if (w) CHECK_I64(qw_get(w, "trunk.size").lo, 20000);
            qw_free(w);
        }
        qi_free(ir);
    }

    /* a type can state a connection, and its instances have it */
    ir = compile("roomA: #LumpedRoom & { energy: 3031500J, capacity: 10000J/K }\n"
                 "roomB: #LumpedRoom & { energy: 2931500J, capacity: 10000J/K }\n"
                 "#WallOfA: #ConductiveWall & { hot: roomA.west, conductance: 10W/K, tolerance: 0.01 }\n"
                 "wall: #WallOfA & { cold: roomB.west }\n",
                 NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "  port hot: #ThermalPort, connected to roomA.west\n");
        HAS(text, "    link hot.energy = roomA.energy\n");
        free(text);
        QWorld *w = build(ir, 1), *c = c_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
        if (w) {
            const int times[1] = { 1000 };
            same_runs(w, c, times, 1);
        }
        qw_free(w);
        qw_free(c);
    }
    qi_free(ir);

    /* a content with no phenomenon gives an empty world */
    ir = compile("#Thing: { size: %length }\nt: #Thing & { size: 2m }\n", NULL);
    if (ir) {
        CHECK(ir->nphens == 0 && ir->ncells == 0 && ir->nkinds == 0);
        QWorld *w = build(ir, 1);
        if (w) OK(qw_run(w, 1000000));
        qw_free(w);
    }
    qi_free(ir);
}

/* --- ports ------------------------------------------------------------------------------------------ */

void test_elab_ports(void)
{
    /* the ports of a phenomenon are in the order of their names, not of their declarations */
    QIr *ir = compile("roomA: #LumpedRoom & { energy: 3031500J, capacity: 10000J/K }\n"
                      "roomB: #LumpedRoom & { energy: 2931500J, capacity: 10000J/K }\n"
                      "roomC: #LumpedRoom & { energy: 2931500J, capacity: 10000J/K }\n"
                      "#Junction: #Phenomenon & { zed: #ThermalPort, alpha: #ThermalPort, mid: #ThermalPort,\n"
                      "    spare: #ThermalPort, own: %temperature, rule: .own == .mid.temperature }\n"
                      "j: #Junction & { zed: roomA.west, mid: roomB.west, alpha: roomC.west }\n",
                      NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "phenomenon j: #Junction\n"
                  "  cell j.own: %temperature = any\n"
                  "  port alpha: #ThermalPort, connected to roomC.west\n"
                  "  port mid: #ThermalPort, connected to roomB.west\n"
                  "  port spare: #ThermalPort, open\n"
                  "  port zed: #ThermalPort, connected to roomA.west\n"
                  "  propagator P0, in an epoch: j.own = roomB.temperature\n");
        /* a law through a port narrows with the other phenomenon's cell, and the graph shows it */
        HAS(text, "  roomB.temperature: narrowed by P0, P2\n");
        free(text);
        CHECK(ir->nports == 7);
        QWorld *w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 0));
            CHECK_I64(qw_get(w, "j.own").lo, 293150000);
            CHECK_I64(qw_get(w, "j.own").hi, 293150000);
        }
        qw_free(w);
    }
    qi_free(ir);

    /* a port inside a port is a part of the outer port, and one field with no cell makes the port open */
    ir = compile("#Duplex: #Port & { inlet: #ThermalPort, outlet: #ThermalPort }\n"
                 "#Tank: #Phenomenon & #Thermal & {\n"
                 "    both: #Duplex & { inlet: { temperature: .temperature, capacity: .capacity, energy: .energy } }\n"
                 "}\n"
                 "tank: #Tank & { energy: 1J, capacity: 1J/K }\n",
                 NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "phenomenon tank: #Tank\n"
                  "  cell tank.capacity: %capacity = 1\n"
                  "  stock tank.energy: %energy = 1\n"
                  "  cell tank.temperature: %temperature = any\n"
                  "  alias tank.both.inlet.capacity = tank.capacity\n"
                  "  alias tank.both.inlet.energy = tank.energy\n"
                  "  alias tank.both.inlet.temperature = tank.temperature\n"
                  "  port both: #Duplex, open\n"
                  "dependencies\n");
        free(text);
        CHECK(ir->nports == 1 && ir->ncells == 3);
    }
    qi_free(ir);

    /* A law that a port type states belongs to the phenomenon that owns the
     * port. The phenomenon that connects to the port has a copy, and the copy
     * gives no second propagator. */
    ir = compile("#Gauge: #Port & { depth: %length, twice: %length, twice: .depth + .depth }\n"
                 "#Pool: #Phenomenon & { level: %length, double: %length, gauge: #Gauge & { depth: .level, twice: .double } }\n"
                 "#Reader: #Phenomenon & { at: #Gauge, seen: %length, rule: .seen == .at.twice }\n"
                 "pool_9: #Pool & { level: 2m }\n"
                 "reader: #Reader & { at: pool_9.gauge }\n",
                 NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "  propagator P0, in an epoch: pool_9.double = pool_9.level + pool_9.level\n    law pool_9.gauge.twice: .depth + .depth\n");
        HAS(text, "  propagator P1, in an epoch: reader.seen = pool_9.double\n");
        HAS(text, "  port at: #Gauge, connected to pool_9.gauge\n");
        free(text);
        CHECK(ir->nprops == 2 && ir->ncells == 3);
        QWorld *w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 0));
            CHECK_I64(qw_get(w, "reader.seen").lo, 40000);       /* 4 m, in units of 0.1 mm */
            CHECK_I64(qw_get(w, "pool_9.gauge.twice").hi, 40000);
        }
        qw_free(w);
    }
    qi_free(ir);
}

/* --- the IR as data ---------------------------------------------------------------------------------- */

void test_elab_ir_data(void)
{
    /* a cell that a law names two times has one edge in the dependency graph */
    QIr *ir = compile("#Pair: #Phenomenon & { a: %temperature, b: %temperature, rule: .b == .a + .a }\n"
                      "p: #Pair & { a: 2K }\n",
                      NULL);
    if (ir) {
        CHECK(ir->nprops == 1 && ir->ndeps == 2);
        if (ir->ndeps == 2) {
            CHECK(ir->deps[0].cell == 0 && ir->deps[1].cell == 1);
            CHECK(!ir->deps[0].kernel && ir->deps[0].node == 0 && ir->deps[0].use == QI_NARROWS);
        }
        QWorld *w = build(ir, 1);
        if (w) {
            OK(qw_run(w, 0));
            CHECK_I64(qw_get(w, "p.b").lo, 4000000);
        }
        qw_free(w);
        /* The IR is data, and another producer can write it. The world checks
         * what it builds: a product in place of the sum does not fit the kinds. */
        ir->props[0].op = QI_PRODUCT;
        char *error = NULL;
        CHECK(qi_instantiate(ir, 1, NULL, &w, &error) == QW_ETYPE);
        CHECK(w == NULL && error != NULL);
        if (error) HAS(error, "the law p.rule: .b == (.a + .a): ");
        free(error);
        ir->props[0].op = QI_SUM;
        /* a cell with a path that names no phenomenon */
        const char *path = ir->cells[0].path;
        ir->cells[0].path = "nowhere.a";
        error = NULL;
        CHECK(qi_instantiate(ir, 1, NULL, &w, &error) == QW_ENAME);
        CHECK(w == NULL && error != NULL);
        if (error) HAS(error, "nowhere.a");
        free(error);
        ir->cells[0].path = path;
    }
    qi_free(ir);

    /* the same, when the cell is the result and an operand */
    ir = compile("#Scaled: #Phenomenon & { a: %temperature, r: %ratio, rule: .a == .a * .r }\ns: #Scaled\n", NULL);
    if (ir) CHECK(ir->nprops == 1 && ir->ndeps == 2 && ir->deps[0].cell == 0 && ir->deps[1].cell == 1);
    qi_free(ir);

    /* a kind with another rounding compiles, and the world does not take it yet */
    ir = compile("%dose: { dimension: \"kg\", rounding: \"floor\" }\n#Pill: #Phenomenon & { dose: %dose }\np: #Pill\n", NULL);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "kind %dose: kg, scale -3, floor\n");
        free(text);
        QWorld *w = NULL;
        char *error = NULL;
        CHECK(qi_instantiate(ir, 1, NULL, &w, &error) == QW_ETYPE);
        if (error) STR_EQ(error, "kind %dose: the world rounds half to even, and it has no other rounding yet");
        free(error);
    }
    qi_free(ir);

    /* kinds of packages, and a declared base dimension, get names that the world takes */
    const char *names[2] = { "a.quad", "b.quad" };
    const char *src[2] = { "package shop\n%coin: { base: \"coin\", scale: -2 }\n%price: { dimension: \"coin/kg\" }\n",
                           "%shop_coin: { dimension: \"kg\" }\n"
                           "#Till: #Phenomenon & { cash: shop.%coin, each: shop.%price, weight: %shop_coin }\n"
                           "till: #Till & { cash: 12.5coin, weight: 2kg }\n" };
    char *diag = NULL;
    ir = qi_compile(names, src, 2, NULL, &diag);
    CHECK(ir != NULL);
    if (!ir) fprintf(stderr, "%s", diag);
    free(diag);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "base coin\nbase deg\n");
        HAS(text, "kind %shop_coin: kg, scale -3\nkind shop.%coin: coin, scale -2\nkind shop.%price: kg^-1 coin, scale -3\n");
        free(text);
        QWorld *w = build(ir, 1);
        if (w) {
            CHECK_I64(qw_get(w, "till.cash").lo, 1250);
            CHECK_I64(qw_get(w, "till.weight").lo, 2000);
        }
        qw_free(w);
    }
    qi_free(ir);
}

/* --- kernels ---------------------------------------------------------------------------------------- */

void test_elab_kernels(void)
{
    /* a kernel that the caller gives */
    QkKernel mine = *qk_conduction();
    mine.name = "test.conduction";
    const QkKernel *list[1] = { &mine };
    QiOptions opt = { list, 1 };
    const char *src = "roomA: #LumpedRoom & { energy: 3031500J, capacity: 10000J/K }\n"
                      "roomB: #LumpedRoom & { energy: 2931500J, capacity: 10000J/K }\n"
                      "#MyWall: #Wall & { evaluator: \"test.conduction\" }\n"
                      "wall: #MyWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 0.01 }\n";
    QIr *ir = compile(src, &opt);
    if (ir) {
        CHECK(ir->nkernels == 1);
        if (ir->nkernels == 1) STR_EQ(ir->kernels[0].kernel, "test.conduction");
        QWorld *w = NULL;
        char *error = NULL;
        OK(qi_instantiate(ir, 1, &opt, &w, &error));
        CHECK(error == NULL);
        QWorld *c = c_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
        if (w) {
            const int times[1] = { 1000 };
            same_runs(w, c, times, 1);
        }
        qw_free(w);
        qw_free(c);
        /* the world needs the same kernels as the compile */
        CHECK(qi_instantiate(ir, 1, NULL, &w, &error) == QW_ENAME);
        CHECK(w == NULL && error != NULL);
        if (error) STR_EQ(error, "wall: unknown evaluator \"test.conduction\"");
        free(error);
    }
    qi_free(ir);
    HAS(compile_error(src), "world.quad:4:1: error: wall: unknown evaluator \"test.conduction\"\n");

    /* a kernel that writes: the dependency graph has an edge for each write */
    static const QkSlot wslots[3] = { { "level", QK_STATE, "K", -6, { Q_NINF, Q_INF } },
                                      { "shown", QK_OUT, "K", -6, { Q_NINF, Q_INF } },
                                      { "rate", QK_IN, "K", -6, { Q_NINF, Q_INF } } };
    QkKernel writer = *qk_conduction();
    writer.name = "test.writer";
    writer.slots = wslots;
    writer.nslots = 3;
    writer.ntransfers = 0;
    const QkKernel *wlist[1] = { &writer };
    QiOptions wopt = { wlist, 1 };
    ir = compile("#Tank: #Phenomenon & { level: %temperature, shown: %temperature, rate: %temperature,\n"
                 "    evaluator: \"test.writer\" }\n"
                 "tank: #Tank & { level: 1K, shown: 1K, rate: 1K }\n",
                 &wopt);
    if (ir) {
        char *text = qi_text(ir);
        HAS(text, "    state level = tank.level\n    out shown = tank.shown\n    in rate = tank.rate\n");
        HAS(text, "dependencies\n"
                  "  tank.level: read by K0; written by K0\n"
                  "  tank.rate: read by K0\n"
                  "  tank.shown: read by K0; written by K0\n");
        free(text);
        int writes = 0;
        for (uint32_t i = 0; i < ir->ndeps; i++) writes += ir->deps[i].use == QI_WRITES && ir->deps[i].kernel;
        CHECK(writes == 2);
    }
    qi_free(ir);

    /* a phenomenon with no evaluator has no kernel */
    ir = compile("roomA: #LumpedRoom & { energy: 3031500J, capacity: 10000J/K }\n", NULL);
    if (ir) CHECK(ir->nkernels == 0 && ir->nprops == 1);
    qi_free(ir);

    /* the world checks a contract that the cells do not fit: here a temperature in joules */
    const char *odd = "%energy: { dimension: \"J\", scale: -3, conserved: true }\n"
                      "%capacity: { dimension: \"J/K\" }\n%conductance: { dimension: \"W/K\" }\n"
                      "%warmth: { dimension: \"J\", scale: -6 }\n"
                      "#Odd: { temperature: %warmth, capacity: %capacity, energy: %energy }\n"
                      "#OddPort: #Port & #Odd\n"
                      "#OddRoom: #Phenomenon & #Odd & {\n"
                      "    west: #OddPort & { temperature: .temperature, capacity: .capacity, energy: .energy } }\n"
                      "#OddWall: #Phenomenon & { hot: #OddPort, cold: #OddPort, conductance: %conductance,\n"
                      "    tolerance: %ratio, evaluator: \"native.conduction\" }\n"
                      "a: #OddRoom & { energy: 1J, capacity: 1J/K, temperature: 1J }\n"
                      "b: #OddRoom & { energy: 1J, capacity: 1J/K, temperature: 1J }\n"
                      "wall: #OddWall & { hot: a.west, cold: b.west, conductance: 1W/K, tolerance: 0.01 }\n";
    const char *name = "odd.quad";
    char *diag = NULL;
    ir = qi_compile(&name, &odd, 1, NULL, &diag);
    CHECK(ir != NULL);
    if (!ir) fprintf(stderr, "%s", diag);
    free(diag);
    if (ir) {
        QWorld *w = NULL;
        char *error = NULL;
        CHECK(qi_instantiate(ir, 1, NULL, &w, &error) == QW_ETYPE);
        CHECK(w == NULL && error != NULL);
        if (error) HAS(error, "hot.temperature");
        free(error);
    }
    qi_free(ir);
}

/* --- errors ----------------------------------------------------------------------------------------- */

#define ROOMS                                                           \
    "roomA: #LumpedRoom & { energy: 3031500J, capacity: 10000J/K }\n"  \
    "roomB: #LumpedRoom & { energy: 2931500J, capacity: 10000J/K }\n"

void test_elab_errors(void)
{
    /* a number needs a kind or a dimension */
    STR_EQ(compile_error("#P: #Phenomenon & { size: number }\np: #P\n"),
           "world.quad:2:1: error: p.size is a number with no kind and no dimension: a cell needs one, as in "
           "`size: %energy`\n");
    /* a stock needs an exact value */
    STR_EQ(compile_error("r: #LumpedRoom & { capacity: 10000J/K }\n"),
           "world.quad:1:1: error: the stock r.energy needs an exact value: only transfers change a stock, so its first "
           "value is a point\n");
    STR_EQ(compile_error("r: #LumpedRoom & { capacity: 10000J/K, energy: >=0J & <=5J }\n"),
           "world.quad:1:1: error: the stock r.energy needs an exact value: only transfers change a stock, so its first "
           "value is a point\n");

    /* connections */
    STR_EQ(compile_error(ROOMS "wall: #ConductiveWall & { hot: roomA.west, conductance: 10W/K, tolerance: 0.01 }\n"),
           "world.quad:3:1: error: wall: the kernel native.conduction needs wall.cold.capacity, and the port wall.cold "
           "is not connected\n"
           "world.quad:3:1: error: wall: the kernel native.conduction needs wall.cold.energy, and the port wall.cold "
           "is not connected\n"
           "world.quad:3:1: error: wall: the kernel native.conduction needs wall.cold.temperature, and the port wall.cold "
           "is not connected\n");
    STR_EQ(compile_error(ROOMS "#Probe: #Phenomenon & { reading: %temperature }\np: #Probe & { reading: roomA.temperature }\n"),
           "world.quad:4:1: error: p.reading refers to roomA.temperature, which is not a port: a phenomenon reaches "
           "another phenomenon only through a port\n");
    STR_EQ(compile_error(ROOMS "#Twin: #Phenomenon & { other: #LumpedRoom }\nt: #Twin & { other: roomA }\n"),
           "world.quad:4:1: error: t.other refers to roomA, which is not a port: a phenomenon reaches another phenomenon "
           "only through a port\n");
    STR_EQ(compile_error(ROOMS "wall: #ConductiveWall & { hot: roomA.west & { temperature: >=280K }, cold: roomB.west,\n"
                               "    conductance: 10W/K, tolerance: 0.01 }\n"),
           "world.quad:3:1: error: wall.hot connects to roomA.west and adds a constraint to it: a connection cannot "
           "change a port in this stage\n");
    STR_EQ(compile_error(ROOMS "#Sides: { inlet: #ThermalPort }\n#Deep: #Phenomenon & { sides: #Sides }\n"
                               "d: #Deep & { sides: { inlet: roomA.west } }\n"),
           "world.quad:5:1: error: d.sides.inlet connects to roomA.west: a connection must be a field of the phenomenon\n");
    STR_EQ(compile_error(ROOMS "wall: #ConductiveWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 0.01 }\n"
                               "#Tap: #Phenomenon & { on: #ThermalPort }\ntap: #Tap & { on: wall.hot }\n"),
           "world.quad:5:1: error: tap.on connects to wall.hot, which is itself a connection to roomA.west: connect to "
           "that port\n");
    STR_EQ(compile_error("#Bare: #Phenomenon & { out: #ThermalPort }\nbare: #Bare\n"
                         "#Tap: #Phenomenon & { on: #ThermalPort }\ntap: #Tap & { on: bare.out }\n"),
           "world.quad:4:1: error: tap.on connects to bare.out, which has a field with no cell: each field of that port "
           "must be a field of its phenomenon\n");
    /* two connections at one port: the two copies unify only if the two ports have equal values */
    STR_EQ(compile_error(ROOMS "roomC: #LumpedRoom & { energy: 3031500J, capacity: 10000J/K }\n"
                               "wall: #ConductiveWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 0.01 }\n"
                               "wall: { hot: roomC.west }\n"),
           "world.quad:4:1: error: wall.hot has two connections: a port connects to one port\n");
    HAS(compile_error(ROOMS "wall: #ConductiveWall & { hot: roomA.west, cold: roomB.west, conductance: 10W/K, tolerance: 0.01 }\n"
                            "wall: { hot: roomB.west }\n"),
        "wall: conflict at hot.energy");

    /* a phenomenon inside a phenomenon */
    STR_EQ(compile_error("#House: #Phenomenon & { room: #LumpedRoom }\nh: #House & { room: { energy: 1J, capacity: 1J/K } }\n"),
           "world.quad:2:1: error: h.room is a phenomenon inside a phenomenon, which is not in this stage\n");

    /* laws */
#define FORM                                                                                               \
    " has a form that the elaborator cannot lower. A law is an equation or an inequality (< <= > >=) of "  \
    "references, numbers, and + - * /\n"
    STR_EQ(compile_error("#W: #Phenomenon & { a: %temperature, b: %temperature, rule: .a != .b }\nw: #W\n"),
           "world.quad:1:64: error: the law .a != .b" FORM);
    STR_EQ(compile_error("#W: #Phenomenon & { a: %temperature, b: %temperature, rule: .a == .b + (.a != .b) }\nw: #W\n"),
           "world.quad:1:64: error: the law .a == (.b + (.a != .b))" FORM);
    /* an inner expression has the same checks as a whole law */
    STR_EQ(compile_error("#W: #Phenomenon & { e: %energy, c: %capacity, t: %temperature, rule: .e == .c * (.t + .c) }\n"
                         "w: #W & { e: 1J }\n"),
           "world.quad:1:73: error: the law .e == (.c * (.t + .c)) does not fit the kinds of its quantities: dimension "
           "mismatch\n");
    STR_EQ(compile_error("#W: #Phenomenon & { t: %temperature, rule: .t == 2 * .t + 5m }\nw: #W\n"),
           "world.quad:1:47: error: the law .t == ((2 * .t) + 5m) does not fit the kinds of its quantities: dimension "
           "mismatch\n");
    STR_EQ(compile_error("#W: #Phenomenon & { t: %temperature, rule: .t <= 5furlong }\nw: #W\n"),
           "world.quad:1:50: error: unknown unit 'furlong'\nworld.quad:2:4: error: cannot expand #W\n");
    STR_EQ(compile_error("#W: #Phenomenon & { e: %energy, c: %capacity, t: %temperature, rule: .e == .c + .t }\n"
                         "w: #W & { e: 1J }\n"),
           "world.quad:1:73: error: the law .e == (.c + .t) does not fit the kinds of its quantities: dimension "
           "mismatch\n");
    STR_EQ(compile_error("#W: #Phenomenon & { t: %temperature, door: \"open\" | \"closed\", rule: .t == .door }\nw: #W\n"),
           "world.quad:1:72: error: the law .t == .door uses w.door, which is a set of atoms: arithmetic needs quantities\n");
    STR_EQ(compile_error("#W: #Phenomenon & { t: %temperature, note: text, rule: .t == .note }\nw: #W\n"),
           "world.quad:1:59: error: the law .t == .note reads w.note, which is not a cell\n");
    STR_EQ(compile_error("#W: #Phenomenon & { t: %temperature, in: #ThermalPort, rule: .t == .in.temperature }\nw: #W\n"),
           "world.quad:1:65: error: the law .t == .in.temperature reads w.in.temperature, and the port w.in is not "
           "connected\n");

    /* names */
    STR_EQ(compile_error("#W: #Phenomenon & { t: %temperature }\n") , "compiled");
    {
        /* a phenomenon in a package has its plain name, if one package has that name */
        const char *names[3] = { "thermal.quad", "attic.quad", "cellar.quad" };
        const char *src[3] = { thermal(), "package attic\nloft: #LumpedRoom & { energy: 1J, capacity: 1J/K }\n",
                               "package cellar\nvault: #LumpedRoom & { energy: 1J, capacity: 1J/K }\n" };
        char *diag = NULL;
        QIr *ir = qi_compile(names, src, 3, NULL, &diag);
        CHECK(ir != NULL && ir->nphens == 2);
        if (ir && ir->nphens == 2) STR_EQ(ir->phens[0].name, "loft");
        free(diag);
        qi_free(ir);
        src[2] = "package cellar\nloft: #LumpedRoom & { energy: 1J, capacity: 1J/K }\n";
        ir = qi_compile(names, src, 3, NULL, &diag);
        CHECK(ir == NULL);
        STR_EQ(diag, "attic.quad:2:1: error: the phenomenon attic.loft needs a name of letters, digits and '_', with no "
                     "package\n"
                     "cellar.quad:2:1: error: the phenomenon cellar.loft needs a name of letters, digits and '_', with no "
                     "package\n");
        free(diag);
        qi_free(ir);
    }

    {
        /* one phenomenon with one label from two packages: the two cells would have one path */
        const char *names[2] = { "attic.quad", "world.quad" };
        const char *src[2] = { "package attic\n%length: { dimension: \"m\" }\n#Trunk: #Phenomenon & { size: %length }\n",
                               "#Crate: #Phenomenon & { size: attic.%length }\n#Both: attic.#Trunk & #Crate\n"
                               "b: #Both & { size: 1m, attic.size: 2m }\n" };
        char *diag = NULL;
        QIr *ir = qi_compile(names, src, 2, NULL, &diag);
        CHECK(ir == NULL);
        STR_EQ(diag, "world.quad:3:1: error: b.size is the path of two values: two fields of the phenomenon have the same "
                     "label\n");
        free(diag);
        qi_free(ir);
    }

    /* an error of an earlier stage stops the compile, and the diagnostics are sorted by location */
    HAS(compile_error("r: #LumpedRoom & { energy: 1.00001J, capacity: 1J/K }\n"), "world.quad:1:");
    STR_EQ(compile_error("b: #LumpedRoom & { capacity: 1J/K }\n#P: #Phenomenon & { size: number }\na: #P\n"),
           "world.quad:1:1: error: the stock b.energy needs an exact value: only transfers change a stock, so its first "
           "value is a point\n"
           "world.quad:3:1: error: a.size is a number with no kind and no dimension: a cell needs one, as in "
           "`size: %energy`\n");
}
