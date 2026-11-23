#include <stdarg.h>
#include <stdio.h>

#include "front/front.h"

/* Builds a world from an IR, through the C API of quad/world.h (ADR 0011
 * section 3, step 4). Each call is made in the order of the IR, so two
 * equal IRs give two equal worlds. */

static QwStatus stop(QWorld *w, char **error, QwStatus s, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static QwStatus stop(QWorld *w, char **error, QwStatus s, const char *fmt, ...)
{
    if (error) {
        Buf b = { 0 };
        char msg[600];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        qf_buf_puts(&b, msg);
        *error = b.s;
    }
    qw_free(w);
    return s;
}

/* The world takes a name of letters, digits and '_'. A name of the IR, such
 * as %energy, shop.%coin or "closed" | "open", gets those characters. Then a
 * number makes it different from the names before it. */
static char *world_name(const char *name, char **taken, uint32_t ntaken)
{
    Buf b = { 0 };
    for (const char *p = name; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_';
        if (ok) qf_buf_putc(&b, *p);
        else if (b.len && b.s[b.len - 1] != '_') qf_buf_putc(&b, '_');
    }
    while (b.len && b.s[b.len - 1] == '_') b.s[--b.len] = 0;
    if (!b.len) qf_buf_puts(&b, "kind");
    size_t stem = b.len;
    for (unsigned k = 2;; k++) {
        bool free_ = true;
        for (uint32_t i = 0; i < ntaken; i++) free_ &= strcmp(taken[i], b.s) != 0;
        if (free_) return b.s;
        b.len = stem;
        b.s[stem] = 0;
        qf_buf_printf(&b, "_%u", k);
    }
}

QwStatus qi_instantiate(const QIr *ir, uint64_t seed, const QiOptions *opt, QWorld **out, char **error)
{
    *out = NULL;
    if (error) *error = NULL;
    QWorld *w = qw_new(seed, ir->tick_scale);
    if (!w) return stop(NULL, error, QW_ENOMEM, "no memory for a world");
    QwStatus s = QW_OK;
    uint32_t nnames = ir->nkinds + ir->nenums;
    char   **names = qf_xcalloc(nnames + 1, sizeof *names);
    QwKind  *kinds = qf_xcalloc(nnames + 1, sizeof *kinds);          /* the kinds, then the enumerations */
    QwPhen  *phens = qf_xcalloc(ir->nphens + 1, sizeof *phens);
    QnCell  *cells = qf_xcalloc(ir->ncells + 1, sizeof *cells);
    uint32_t taken = 0;
    char    *why = NULL;                                              /* a message that is not the world's */

    for (uint32_t i = 0; i < ir->nbases && !s; i++) s = qw_base_dim(w, ir->bases[i]);
    for (uint32_t i = 0; i < ir->nkinds && !s; i++) {
        const QiKind *k = &ir->kinds[i];
        if (k->rounding != Q_ROUND_HALF_EVEN) {
            Buf b = { 0 };
            qf_buf_printf(&b, "kind %s: the world rounds half to even, and it has no other rounding yet", k->name);
            why = b.s;
            s = QW_ETYPE;
            break;
        }
        names[taken] = world_name(k->name, names, taken);
        s = qw_kind(w, names[taken], k->dimension, k->scale, k->conserved, &kinds[i]);
        taken++;
    }
    for (uint32_t i = 0; i < ir->nenums && !s; i++) {
        const QiEnum *en = &ir->enums[i];
        if (!strcmp(en->name, "bool")) {                              /* the world's built-in kind: false, true */
            kinds[ir->nkinds + i] = qw_boolean(w);
            continue;
        }
        names[taken] = world_name(en->name, names, taken);
        s = qw_enum_kind(w, names[taken], (const char *const *) en->atoms, en->natoms, &kinds[ir->nkinds + i]);
        taken++;
    }
    for (uint32_t i = 0; i < ir->nphens && !s; i++) s = qw_phenomenon(w, ir->phens[i].name, &phens[i]);
    for (uint32_t i = 0; i < ir->ncells && !s; i++) {
        const QiCell *c = &ir->cells[i];
        s = c->domain == QI_SET ? qw_set_cell(w, c->path, kinds[ir->nkinds + c->kind], c->mask, &cells[i])
                                : qw_cell(w, c->path, kinds[c->kind], c->base, &cells[i]);
    }
    for (uint32_t i = 0; i < ir->naliases && !s; i++)
        s = qw_alias(w, ir->aliases[i].path, ir->cells[ir->aliases[i].cell].path);
    for (uint32_t i = 0; i < ir->nprops && !s; i++) {
        const QiProp *p = &ir->props[i];
        QnCell z = cells[p->cell[0]], x = cells[p->cell[1]], y = p->cell[2] == QI_NONE ? 0 : cells[p->cell[2]];
        QNet *net = qw_net(w);
        QStatus q = Q_OK;
        QnStatus n = p->op == QI_EQUAL        ? qn_equal(net, z, x, &q)
                     : p->op == QI_SUM        ? qn_sum(net, z, x, y, &q)
                     : p->op == QI_DIFFERENCE ? qn_difference(net, z, x, y, &q)
                     : p->op == QI_PRODUCT    ? qn_product(net, z, x, y, &q)
                                              : qn_quotient(net, z, x, y, &q);
        if (n != QN_OK) {
            Buf b = { 0 };
            qf_buf_printf(&b, "the law %s: %s: %s%s%s", p->name, p->law, qn_status_str(n),
                          n == QN_ETYPE ? ": " : "", n == QN_ETYPE ? q_status_str(q) : "");
            why = b.s;
            s = n == QN_ENOMEM ? QW_ENOMEM : QW_ETYPE;
        }
    }
    for (uint32_t i = 0; i < ir->nports && !s; i++)
        if (ir->ports[i].state == QI_PORT_CONNECTED)
            s = qw_connect(w, phens[ir->ports[i].phen], ir->ports[i].name, ir->ports[i].target);
    for (uint32_t i = 0; i < ir->nkernels && !s; i++) {
        const QkKernel *k = qi_find_kernel(opt, ir->kernels[i].kernel);
        if (!k) {
            Buf b = { 0 };
            qf_buf_printf(&b, "%s: unknown evaluator \"%s\"", ir->phens[ir->kernels[i].phen].name, ir->kernels[i].kernel);
            why = b.s;
            s = QW_ENAME;
            break;
        }
        s = qw_attach(w, phens[ir->kernels[i].phen], k);
    }

    for (uint32_t i = 0; i < taken; i++) free(names[i]);
    free(names);
    free(kinds);
    free(phens);
    free(cells);
    if (s) {
        QwStatus r = stop(w, error, s, "%s", why ? why : qw_error(w));
        free(why);
        return r;
    }
    *out = w;
    return QW_OK;
}
