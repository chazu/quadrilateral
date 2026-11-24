#include <stdio.h>

#include "front/front.h"

/* The IR as text, and the entry point of the front end (ADR 0016). */

static void put_value(Buf *b, const QIr *ir, const QiCell *c)
{
    if (c->domain == QI_SET) {
        const QiEnum *en = &ir->enums[c->kind];
        bool first = true;
        for (uint32_t a = 0; a < en->natoms; a++)
            if (c->mask >> a & 1) {
                qf_buf_printf(b, "%s%s", first ? "" : " | ", en->atoms[a]);
                first = false;
            }
        return;
    }
    int scale = ir->kinds[c->kind].scale;
    char lo[64], hi[64];
    q_format_decimal(c->base.lo, scale, lo, sizeof lo);
    q_format_decimal(c->base.hi, scale, hi, sizeof hi);
    if (c->base.lo == Q_NINF && c->base.hi == Q_INF) qf_buf_puts(b, "any");
    else if (c->base.lo == c->base.hi) qf_buf_puts(b, lo);
    else if (c->base.hi == Q_INF) qf_buf_printf(b, ">= %s", lo);
    else if (c->base.lo == Q_NINF) qf_buf_printf(b, "<= %s", hi);
    else qf_buf_printf(b, "%s .. %s", lo, hi);
}

static const char *cell_path(const QIr *ir, uint32_t c) { return c == QI_NONE ? "nothing" : ir->cells[c].path; }

char *qi_text(const QIr *ir)
{
    static const char *const rounding[] = { "half-even", "floor", "ceiling", "zero" };
    static const char *const ops[] = { "", "+", "-", "*", "/" };
    static const char *const roles[] = { "in", "out", "state", "stock", "link" };
    static const char *const uses[] = { "narrowed by", "read by", "written by", "transferred by" };
    Buf b = { 0 };
    qf_buf_printf(&b, "quadrilateral ir 1\ntick 10^%d s\n", ir->tick_scale);
    for (uint32_t i = 0; i < ir->nbases; i++) qf_buf_printf(&b, "base %s\n", ir->bases[i]);
    for (uint32_t i = 0; i < ir->nkinds; i++) {
        const QiKind *k = &ir->kinds[i];
        qf_buf_printf(&b, "kind %s: %s, scale %d%s%s", k->name, k->dimension, k->scale, k->conserved ? ", conserved" : "",
                      k->anonymous ? ", anonymous" : "");
        if (k->rounding) qf_buf_printf(&b, ", %s", rounding[k->rounding & 3]);
        qf_buf_putc(&b, '\n');
    }
    for (uint32_t i = 0; i < ir->nenums; i++) {
        qf_buf_printf(&b, "enumeration %s:", ir->enums[i].name);
        for (uint32_t a = 0; a < ir->enums[i].natoms; a++) qf_buf_printf(&b, " %s", ir->enums[i].atoms[a]);
        qf_buf_putc(&b, '\n');
    }
    for (uint32_t p = 0; p < ir->nphens; p++) {
        qf_buf_printf(&b, "phenomenon %s: %s\n", ir->phens[p].name, ir->phens[p].type);
        for (uint32_t i = 0; i < ir->ncells; i++) {
            const QiCell *c = &ir->cells[i];
            if (c->phen != p) continue;
            qf_buf_printf(&b, "  %s %s: %s = ", c->stock ? "stock" : c->inner ? "inner" : c->derived ? "derived" : "cell", c->path,
                          c->domain == QI_SET ? ir->enums[c->kind].name : ir->kinds[c->kind].name);
            put_value(&b, ir, c);
            qf_buf_putc(&b, '\n');
        }
        for (uint32_t i = 0; i < ir->naliases; i++)
            if (ir->cells[ir->aliases[i].cell].phen == p)
                qf_buf_printf(&b, "  alias %s = %s\n", ir->aliases[i].path, ir->cells[ir->aliases[i].cell].path);
        for (uint32_t i = 0; i < ir->nports; i++) {
            const QiPort *pt = &ir->ports[i];
            if (pt->phen != p) continue;
            qf_buf_printf(&b, "  port %s: %s, ", pt->name, pt->type);
            if (pt->state == QI_PORT_CONNECTED) qf_buf_printf(&b, "connected to %s\n", pt->target);
            else qf_buf_puts(&b, pt->state == QI_PORT_VIEW ? "a view\n" : "open\n");
        }
        for (uint32_t i = 0; i < ir->nprops; i++) {
            const QiProp *pr = &ir->props[i];
            if (pr->phen != p) continue;
            qf_buf_printf(&b, "  propagator P%u, in an epoch: %s %s %s", i, cell_path(ir, pr->cell[0]),
                          pr->op == QI_LESS_EQUAL ? "<=" : pr->op == QI_LESS ? "<" : "=", cell_path(ir, pr->cell[1]));
            if (pr->op >= QI_SUM && pr->op <= QI_QUOTIENT) qf_buf_printf(&b, " %s %s", ops[pr->op], cell_path(ir, pr->cell[2]));
            qf_buf_printf(&b, "\n    law %s: %s\n", pr->name, pr->law);
        }
        for (uint32_t i = 0; i < ir->nkernels; i++) {
            const QiKernelUse *k = &ir->kernels[i];
            if (k->phen != p) continue;
            qf_buf_printf(&b, "  kernel K%u, at a boundary: %s\n", i, k->kernel);
            for (uint32_t s = 0; s < k->nbinds; s++)
                qf_buf_printf(&b, "    %s %s = %s\n", roles[k->binds[s].role], k->binds[s].slot,
                              cell_path(ir, k->binds[s].cell));
            for (uint32_t t = 0; t < k->ntransfers; t++)
                qf_buf_printf(&b, "    transfer %s: %s -> %s%s\n", k->transfers[t].name, cell_path(ir, k->transfers[t].from),
                              cell_path(ir, k->transfers[t].to), k->transfers[t].conserved ? ", conserved" : "");
        }
    }
    qf_buf_puts(&b, "dependencies\n");
    for (uint32_t i = 0; i < ir->ndeps; i++) {
        const QiDep *d = &ir->deps[i];
        bool first = !i || ir->deps[i - 1].cell != d->cell;
        bool same = !first && ir->deps[i - 1].use == d->use;
        if (first) qf_buf_printf(&b, "%s  %s: ", i ? "\n" : "", ir->cells[d->cell].path);
        if (same) qf_buf_puts(&b, ", ");
        else qf_buf_printf(&b, "%s%s ", first ? "" : "; ", uses[d->use]);
        qf_buf_printf(&b, "%c%u", d->kernel ? 'K' : 'P', d->node);
    }
    if (ir->ndeps) qf_buf_putc(&b, '\n');
    return b.s;
}

void qi_format_interval(QInterval v, int scale, char *out, size_t len)
{
    char lo[64], hi[64];
    q_format_decimal(v.lo, scale, lo, sizeof lo);
    q_format_decimal(v.hi, scale, hi, sizeof hi);
    if (v.lo == Q_NINF && v.hi == Q_INF) snprintf(out, len, "unknown");
    else if (v.lo == v.hi) snprintf(out, len, "%s", lo);
    else if (v.lo != Q_NINF && v.hi != Q_INF && v.hi - v.lo == 1) snprintf(out, len, "~%s", lo);
    else if (v.hi == Q_INF) snprintf(out, len, ">= %s", lo);
    else if (v.lo == Q_NINF) snprintf(out, len, "<= %s", hi);
    else snprintf(out, len, "%s .. %s", lo, hi);
}

const char *qi_unit(const char *dimension)
{
    static const char *const known[][2] = {
        { "1", "" },
        { "m^2 kg s^-2", "J" },
        { "m^2 kg s^-3", "W" },
        { "m^2 kg s^-2 K^-1", "J/K" },
        { "m^2 kg s^-3 K^-1", "W/K" },
        { "m kg s^-2", "N" },
        { "m^-1 kg s^-2", "Pa" },
        { "s^-1", "Hz" },
        { "m^2 kg s^-3 A^-1", "V" },
        { "m^2 kg s^-3 A^-2", "ohm" },
        { "s A", "C" },
    };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcmp(dimension, known[i][0])) return known[i][1];
    return dimension;
}

void qi_free(QIr *ir)
{
    if (ir) qf_arena_free(ir->mem);
}

QIr *qi_compile(const char *const *names, const char *const *sources, size_t n, const QiOptions *opt, char **diagnostics)
{
    Front *f = qf_new();
    QIr *ir = qf_load(f, names, sources, n) ? qf_elaborate(f, opt) : NULL;
    if (diagnostics) {
        Buf b = { 0 };
        qf_buf_puts(&b, "");
        for (size_t i = 0; i < f->diags.n; i++) qf_buf_printf(&b, "%s\n", f->diags.v[i].msg);
        *diagnostics = b.s;
    }
    qf_free(f);
    return ir;
}
