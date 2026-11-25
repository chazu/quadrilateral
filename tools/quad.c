#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quad/ir.h"
#include "quad/world.h"

/* quad: a command-line tool for Quadrilateral source.
 *
 *   quad check FILE...             reports the errors of a content
 *   quad ir FILE...                prints the IR (docs/adr/0016)
 *   quad run FILE... [options]     builds a world and prints its values
 *
 * Options of run:
 *   --until TIME    runs the world to this time: 1000s, 20min, 2h, 1d
 *   --every TIME    prints a row at each step of this length
 *   --show PATHS    the cells to print, with commas: roomA.temperature,wall.power
 *   --seed N        the seed of the world (the default is 1)
 *   --switch P=M@T  switches the phenomenon P to its model M at the time T, with
 *                   the map that the content gives: roomB=fine@200s
 *
 * Without --until, run prints each cell after propagation, with no time
 * step. A value is in the coherent unit of its dimension. */

static void usage(void)
{
    fputs("usage: quad check FILE...\n"
          "       quad ir FILE...\n"
          "       quad run FILE... [--until TIME] [--every TIME] [--show PATH,PATH] [--seed N]\n"
          "                        [--switch PHENOMENON=MODEL@TIME]...\n"
          "A time is a number and a unit: 1000s, 20min, 2h, 1d.\n",
          stderr);
}

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

/* A time in ticks of the world, or -1. The tick is 10^scale seconds. */
static int64_t parse_time(const char *text, int scale)
{
    char *end;
    double v = strtod(text, &end);
    if (end == text || v < 0) return -1;
    double unit = !strcmp(end, "s") || !*end ? 1 : !strcmp(end, "ms") ? 1e-3 : !strcmp(end, "min") ? 60
                  : !strcmp(end, "h") ? 3600 : !strcmp(end, "d") ? 86400 : -1;
    if (unit < 0) return -1;
    double ticks = v * unit;
    for (int i = scale; i < 0; i++) ticks *= 10;
    for (int i = scale; i > 0; i--) ticks /= 10;
    return ticks > 9e18 ? -1 : (int64_t) (ticks + 0.5);
}

static const QiCell *find_cell(const QIr *ir, const char *path)
{
    for (uint32_t i = 0; i < ir->ncells; i++)
        if (!strcmp(ir->cells[i].path, path)) return &ir->cells[i];
    return NULL;
}

/* The value of a cell as text: 303.15, 2.5 .. 3, or open | closed. */
static void format_value(const QIr *ir, QWorld *w, const QiCell *c, char *out, size_t len)
{
    if (c->domain == QI_SET) {
        const QiEnum *en = &ir->enums[c->kind];
        uint64_t mask = qw_get_set(w, c->path);
        size_t n = 0;
        out[0] = 0;
        for (uint32_t a = 0; a < en->natoms && n < len; a++)
            if (mask >> a & 1) n += (size_t) snprintf(out + n, len - n, "%s%s", n ? " | " : "", en->atoms[a]);
        return;
    }
    qi_format_interval(qw_get(w, c->path), ir->kinds[c->kind].scale, out, len);
}

static const char *cell_unit(const QIr *ir, const QiCell *c)
{
    return c->domain == QI_SET ? "" : qi_unit(ir->kinds[c->kind].dimension);
}

/* Reports a failed run. For a contradiction, it names the law that could not
 * hold and shows the values of that law. The network adds the propagators of
 * the IR first, in their order, so a number of a propagator is an index. */
static void report(const QIr *ir, QWorld *w, int64_t ticks)
{
    QnCell where;
    uint32_t prop;
    char at[64] = "", text[256];
    if (ticks > 0) {
        q_format_decimal(ticks, ir->tick_scale, text, sizeof text);
        snprintf(at, sizeof at, " at %s s", text);
    }
    if (!qn_contradicted(qw_net(w), &where) || !qn_contradicted_by(qw_net(w), &prop) || prop >= ir->nprops) {
        fprintf(stderr, "quad:%s %s\n", at, qw_error(w));
        return;
    }
    const QiProp *p = &ir->props[prop];
    fprintf(stderr, "quad:%s a law cannot hold with these values: %s: %s\n", at, p->name, p->law);
    if (p->file) fprintf(stderr, "  the law is at %s:%d\n", p->file, p->line);
    for (int i = 0; i < 3; i++) {
        if (p->cell[i] == QI_NONE) continue;
        const QiCell *c = &ir->cells[p->cell[i]];
        format_value(ir, w, c, text, sizeof text);
        fprintf(stderr, "  %s%s = %s %s\n", c->path, c->inner ? " (an inner value of the law)" : "", text, cell_unit(ir, c));
    }
    /* A transfer changed a stock of the law, and each other field of the law
     * keeps one value at every time. Then the type has no @derived for the
     * field that the laws compute (ADR 0019). */
    bool stock = false;
    char fixed[512] = "";
    for (int i = 0; i < 3; i++) {
        if (p->cell[i] == QI_NONE) continue;
        const QiCell *c = &ir->cells[p->cell[i]];
        if (c->stock) stock = true;
        else if (!c->derived && c->domain == QI_INTERVAL && c->base.lo == c->base.hi)
            snprintf(fixed + strlen(fixed), sizeof fixed - strlen(fixed), "%s%s", *fixed ? ", " : "", c->path);
    }
    if (stock && ticks > 0 && *fixed)
        fprintf(stderr, "  hint: a transfer changed the stock, and these fields keep their values at every time: %s.\n"
                        "  If the laws compute one of them from the stock, mark it with @derived in its type.\n", fixed);
}

/* A switch of a model that --switch asks for. */
typedef struct { char phen[128], model[128]; const char *when; int64_t at; bool done; } Switch;

/* Makes each switch whose time is not after `t`, in the order of the times.
 * False with a report if the run or a switch fails. */
static bool switches_to(const QIr *ir, QWorld *w, Switch *sw, size_t nsw, int64_t t)
{
    for (;;) {
        Switch *next = NULL;
        for (size_t i = 0; i < nsw; i++)
            if (!sw[i].done && sw[i].at <= t && (!next || sw[i].at < next->at)) next = &sw[i];
        if (!next) return true;
        next->done = true;
        if (qw_run(w, next->at) != QW_OK) {
            report(ir, w, next->at);
            return false;
        }
        if (qi_switch(ir, w, next->phen, next->model) != QW_OK) {
            fprintf(stderr, "quad: --switch %s=%s@%s: %s\n", next->phen, next->model, next->when, qw_error(w));
            return false;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage();
        return 2;
    }
    const char *cmd = argv[1];
    bool check = !strcmp(cmd, "check"), show_ir = !strcmp(cmd, "ir"), run = !strcmp(cmd, "run");
    if (!check && !show_ir && !run) {
        usage();
        return 2;
    }
    const char *names[64], *until_text = NULL, *every_text = NULL, *show = NULL;
    char *sources[64];
    size_t n = 0, nsw = 0;
    uint64_t seed = 1;
    Switch sw[16];
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        bool opt = !strncmp(a, "--", 2);
        if (opt && (!run || i + 1 >= argc)) {
            usage();
            return 2;
        }
        if (!strcmp(a, "--until")) until_text = argv[++i];
        else if (!strcmp(a, "--every")) every_text = argv[++i];
        else if (!strcmp(a, "--show")) show = argv[++i];
        else if (!strcmp(a, "--seed")) seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--switch")) {
            const char *v = argv[++i], *eq = strchr(v, '='), *at = eq ? strchr(eq, '@') : NULL;
            if (!at || eq == v || at == eq + 1 || !at[1] || nsw == 16 || eq - v > 127 || at - eq > 127) {
                fprintf(stderr, "quad: --switch needs PHENOMENON=MODEL@TIME, for example roomB=fine@200s, 16 times at most\n");
                return 2;
            }
            memset(&sw[nsw], 0, sizeof sw[nsw]);
            memcpy(sw[nsw].phen, v, (size_t) (eq - v));
            memcpy(sw[nsw].model, eq + 1, (size_t) (at - eq - 1));
            sw[nsw++].when = at + 1;
        }
        else if (opt) {
            fprintf(stderr, "quad: unknown option %s\n", a);
            return 2;
        } else if (n < 64) {
            names[n] = a;
            sources[n] = read_file(a);
            if (!sources[n]) {
                fprintf(stderr, "quad: cannot read %s\n", a);
                return 2;
            }
            n++;
        }
    }
    if (!n) {
        usage();
        return 2;
    }

    char *diag = NULL;
    QIr *ir = qi_compile(names, (const char *const *) sources, n, NULL, &diag);
    fputs(diag, stderr);
    free(diag);
    for (size_t i = 0; i < n; i++) free(sources[i]);
    if (!ir) return 1;
    if (!ir->nphens)
        fprintf(stderr, "quad: note: the content has no phenomenon. An instance is in the world if its type is below "
                        "#Phenomenon, as in `#Room: #Phenomenon & { ... }`\n");
    if (check) {
        printf("ok: %u phenomena, %u cells, %u laws and kernels\n", ir->nphens, ir->ncells, ir->nprops + ir->nkernels);
        qi_free(ir);
        return 0;
    }
    if (show_ir) {
        char *text = qi_text(ir);
        fputs(text, stdout);
        free(text);
        qi_free(ir);
        return 0;
    }

    int64_t until = until_text ? parse_time(until_text, ir->tick_scale) : 0;
    int64_t every = every_text ? parse_time(every_text, ir->tick_scale) : until;
    if (until < 0 || every < 0 || (until_text && every == 0)) {
        fprintf(stderr, "quad: a time is a number and a unit, for example 1000s or 20min\n");
        return 2;
    }

    for (size_t i = 0; i < nsw; i++) {
        sw[i].at = parse_time(sw[i].when, ir->tick_scale);
        if (sw[i].at < 0 || !until_text || sw[i].at > until) {
            fprintf(stderr, "quad: --switch %s=%s@%s: the time must be a time of the run, so --until must give a later time\n",
                    sw[i].phen, sw[i].model, sw[i].when);
            return 2;
        }
    }

    /* the cells to print: the ones that --show names, or every field */
    const QiCell **cols = calloc(ir->ncells + 1, sizeof *cols);
    size_t ncols = 0;
    if (show) {
        char *list = strdup(show);
        for (char *p = strtok(list, ","); p; p = strtok(NULL, ",")) {
            const QiCell *c = find_cell(ir, p);
            if (!c) {
                fprintf(stderr, "quad: no cell %s. `quad ir` lists the cells\n", p);
                return 2;
            }
            cols[ncols++] = c;
        }
        free(list);
    } else {
        for (uint32_t i = 0; i < ir->ncells; i++)
            if (!ir->cells[i].inner) cols[ncols++] = &ir->cells[i];
    }

    QWorld *w = NULL;
    char *error = NULL;
    if (qi_instantiate(ir, seed, NULL, &w, &error) != QW_OK) {
        fprintf(stderr, "quad: %s\n", error ? error : "cannot build the world");
        free(error);
        return 1;
    }
    int status = 0;
    char text[512];
    if (!until_text) {
        /* the values after propagation, one cell on each line */
        if (qw_run(w, 0) != QW_OK) {
            report(ir, w, 0);
            status = 1;
        }
        size_t width = 0;
        for (size_t i = 0; i < ncols; i++)
            if (strlen(cols[i]->path) > width) width = strlen(cols[i]->path);
        for (size_t i = 0; i < ncols && !status; i++) {
            format_value(ir, w, cols[i], text, sizeof text);
            printf("%-*s  %s %s\n", (int) width, cols[i]->path, text, cell_unit(ir, cols[i]));
        }
    } else {
        /* a table: one row for each step */
        printf("%-10s", "time [s]");
        for (size_t i = 0; i < ncols; i++) {
            const char *u = cell_unit(ir, cols[i]);
            snprintf(text, sizeof text, "%s%s%s%s", cols[i]->path, *u ? " [" : "", u, *u ? "]" : "");
            printf("  %*s", 14, text);
        }
        putchar('\n');
        for (int64_t t = 0; !status; t = t + every > until ? until : t + every) {
            if (!switches_to(ir, w, sw, nsw, t)) {
                status = 1;
                break;
            }
            if (qw_run(w, t) != QW_OK) {
                report(ir, w, t);
                status = 1;
                break;
            }
            q_format_decimal(t, ir->tick_scale, text, sizeof text);
            printf("%-10s", text);
            for (size_t i = 0; i < ncols; i++) {
                const char *u = cell_unit(ir, cols[i]);
                size_t head = strlen(cols[i]->path) + (*u ? strlen(u) + 3 : 0);
                format_value(ir, w, cols[i], text, sizeof text);
                printf("  %*s", (int) (head > 14 ? head : 14), text);
            }
            putchar('\n');
            if (t >= until) break;
        }
    }
    free(cols);
    qw_free(w);
    qi_free(ir);
    return status;
}
