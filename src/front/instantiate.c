#include <stdarg.h>
#include <stdio.h>

#include "front/front.h"
#include "quad/kernels.h"

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

/* The kernel native.rate for the transfers of a kernel use (ADR 0020): one
 * slot for each bind, with the quantity type of its cell. NULL without memory. */
static QkKernel *rate_kernel(const QIr *ir, const QiKernelUse *ku)
{
    QkSlot *slots = calloc(ku->nbinds ? ku->nbinds : 1, sizeof *slots);
    QkRateTransfer *moves = calloc(ku->ntransfers, sizeof *moves);
    QkKernel *k = NULL;
    if (slots && moves) {
        for (uint32_t b = 0; b < ku->nbinds; b++) {
            const QiKind *kind = &ir->kinds[ir->cells[ku->binds[b].cell].kind];
            slots[b] = (QkSlot) { ku->binds[b].slot, ku->binds[b].role, kind->dimension, kind->scale, { Q_NINF, Q_INF } };
        }
        for (uint32_t t = 0; t < ku->ntransfers; t++) {
            const QiTransfer *tr = &ku->transfers[t];
            uint32_t ends[5] = { tr->rate, tr->tolerance, tr->from, tr->to, tr->ratio };
            uint32_t at[5] = { QK_NONE, QK_NONE, QK_NONE, QK_NONE, QK_NONE };
            for (int e = 0; e < 5; e++)
                for (uint32_t b = 0; b < ku->nbinds && ends[e] != QI_NONE; b++)
                    if (ku->binds[b].cell == ends[e]) at[e] = b;
            moves[t] = (QkRateTransfer) { tr->name, at[0], at[1], at[2], at[3], at[4] };
            /* the kernel reads the tolerance as a part of one, in units of 10^-9 */
            if (at[1] != QK_NONE) {
                slots[at[1]].scale = -9;
                slots[at[1]].valid = (QInterval) { 1, 1000000000 };
            }
        }
        k = qk_rate_new(slots, ku->nbinds, moves, ku->ntransfers);
    }
    free(slots);
    free(moves);
    return k;
}

/* Adds the propagator of the IR to a network. set: cell[0] is a set cell. */
QnStatus qi_add_prop(QNet *net, const QiProp *p, bool set, QnCell z, QnCell x, QnCell y, QStatus *q)
{
    switch (p->op) {
    case QI_EQUAL:      return set ? qn_set_equal(net, z, x) : qn_equal(net, z, x, q);
    case QI_SUM:        return qn_sum(net, z, x, y, q);
    case QI_DIFFERENCE: return qn_difference(net, z, x, y, q);
    case QI_PRODUCT:    return qn_product(net, z, x, y, q);
    case QI_QUOTIENT:   return qn_quotient(net, z, x, y, q);
    case QI_LESS_EQUAL: return qn_less_equal(net, z, x, false, q);
    case QI_LESS:       return qn_less_equal(net, z, x, true, q);
    case QI_IN_RANGE:   return qn_reify_in(net, z, x, p->range);
    case QI_IN_SET:     return qn_reify_set(net, z, x, p->mask);
    case QI_AND:        return qn_and(net, z, x, y);
    case QI_OR:         return qn_or(net, z, x, y);
    case QI_NOT:        return qn_not(net, z, x);
    }
    return QN_EVALUE;
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
        if (k->anonymous) {
            s = qw_anonymous_kind(w, k->dimension, k->scale, &kinds[i]);
            continue;
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
    for (uint32_t i = 0; i < ir->nphens && !s; i++) {
        const QiPhen *ph = &ir->phens[i];
        s = qw_phenomenon(w, ph->name, &phens[i]);
        if (!s) s = qw_set_type(w, phens[i], ph->type);
        /* the first model that the world gets is active: the model of the start, and then the others (ADR 0021) */
        if (!s && ph->nmodels) s = qw_model(w, phens[i], ph->models[ph->start]);
        for (uint32_t m = 0; m < ph->nmodels && !s; m++)
            if (m != ph->start) s = qw_model(w, phens[i], ph->models[m]);
    }
    for (uint32_t i = 0; i < ir->ncells && !s; i++) {
        const QiCell *c = &ir->cells[i];
        s = c->domain == QI_SET && c->derived ? qw_derived_set_cell(w, c->path, kinds[ir->nkinds + c->kind], c->mask, &cells[i])
            : c->domain == QI_SET ? qw_set_cell(w, c->path, kinds[ir->nkinds + c->kind], c->mask, &cells[i])
            : c->derived        ? qw_derived_cell(w, c->path, kinds[c->kind], c->base, &cells[i])
                                : qw_cell(w, c->path, kinds[c->kind], c->base, &cells[i]);
    }
    for (uint32_t i = 0; i < ir->naliases && !s; i++)
        s = qw_alias(w, ir->aliases[i].path, ir->cells[ir->aliases[i].cell].path);
    for (uint32_t i = 0; i < ir->nprops && !s; i++) {
        const QiProp *p = &ir->props[i];
        QnCell z = cells[p->cell[0]], x = cells[p->cell[1]], y = p->cell[2] == QI_NONE ? 0 : cells[p->cell[2]];
        QNet *net = qw_net(w);
        QStatus q = Q_OK;
        /* a propagator of a model runs only while the model is active */
        if (p->model != QI_NONE && (s = qw_model_begin(w, phens[p->mphen], ir->phens[p->mphen].models[p->model]))) break;
        QnStatus n = qi_add_prop(net, p, ir->cells[p->cell[0]].domain == QI_SET, z, x, y, &q);
        if (p->model != QI_NONE) qw_model_end(w);
        if (n != QN_OK) {
            Buf b = { 0 };
            qf_buf_printf(&b, "the law %s: %s: %s%s%s", p->name, p->law, qn_status_str(n),
                          n == QN_ETYPE ? ": " : "", n == QN_ETYPE ? q_status_str(q) : "");
            why = b.s;
            s = n == QN_ENOMEM ? QW_ENOMEM : QW_ETYPE;
        }
    }
    for (uint32_t i = 0; i < ir->nports && !s; i++) {
        const QiPort *pt = &ir->ports[i];
        if (pt->state == QI_PORT_CONNECTED) s = qw_connect(w, phens[pt->phen], pt->name, pt->target);
        for (uint32_t m = 0; pt->state == QI_PORT_MODEL && m < ir->phens[pt->phen].nmodels && !s; m++)
            s = qw_port(w, phens[pt->phen], pt->name, ir->phens[pt->phen].models[m], pt->targets[m]);
    }
    for (uint32_t i = 0; i < ir->nkernels && !s; i++) {
        const QiKernelUse *ku = &ir->kernels[i];
        const char *model = ku->model == QI_NONE ? NULL : ir->phens[ku->phen].models[ku->model];
        const QkKernel *k;
        if (ku->ntransfers && ku->transfers[0].rate != QI_NONE) {
            /* interactions with a rate: the kernel comes from the IR, and the world frees it */
            QkKernel *made = rate_kernel(ir, ku);
            s = made ? qw_adopt_kernel(w, made, qk_rate_free) : QW_ENOMEM;
            if (s && made) qk_rate_free(made);
            k = made;
        } else if (ku->size) {
            /* a kernel of a room with n cells: the same rule */
            QkKernel *made = qi_room_kernel(ku->kernel, ku->size);
            s = made ? qw_adopt_kernel(w, made, qk_free) : QW_ENOMEM;
            if (s && made) qk_free(made);
            k = made;
        } else if (!(k = qi_find_kernel(opt, ku->kernel))) {
            Buf b = { 0 };
            qf_buf_printf(&b, "%s: unknown evaluator \"%s\"", ir->phens[ku->phen].name, ku->kernel);
            why = b.s;
            s = QW_ENAME;
            break;
        }
        if (!s) s = model ? qw_model_attach(w, phens[ku->phen], model, k) : qw_attach(w, phens[ku->phen], k);
    }
    for (uint32_t i = 0; i < ir->nmaps && !s; i++) {
        const QiMap *mp = &ir->maps[i];
        const QiPhen *ph = &ir->phens[mp->phen];
        const QkKernel *k = mp->size ? NULL : qi_find_kernel(opt, mp->kernel);
        if (mp->size) {
            QkKernel *made = qi_room_kernel(mp->kernel, mp->size);
            s = made ? qw_adopt_kernel(w, made, qk_free) : QW_ENOMEM;
            if (s && made) qk_free(made);
            if (s) break;
            k = made;
        }
        if (!k) {
            Buf b = { 0 };
            qf_buf_printf(&b, "the map %s.%s: unknown evaluator \"%s\"", ph->name, mp->name, mp->kernel);
            why = b.s;
            s = QW_ENAME;
            break;
        }
        s = qw_transition(w, phens[mp->phen], mp->name, ph->models[mp->from], ph->models[mp->to], k);
        if (!s && mp->when.path) {
            /* the condition of the content (ADR 0022) */
            QwCondition c = { mp->when.path, mp->when.range, mp->when.mask };
            s = qw_transition_when(w, phens[mp->phen], mp->name, &c);
        }
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

QwStatus qi_switch(const QIr *ir, QWorld *w, const char *phen, const char *model)
{
    (void) ir;
    QwPhen p;
    QwStatus s = qw_find_phenomenon(w, phen, &p);
    return s ? s : qw_switch_to(w, p, model);
}
