#include "front/front.h"

/* The quick check (Malouf, Carroll & Copestake; Kiefer et al.): each structure
 * gets a vector of the types at a few paths where unification most often
 * fails, and two vectors are compared by meet before unifying. The root type
 * is always the first element. A path a structure doesn't have counts as top,
 * so the check never rejects a unification that would succeed.
 *
 * Borrowed from Triangle's qc.c (bruggadung f425574), under ADR 0003. A path
 * is a list of features, not text (ADR 0015 section 8). The check reads the
 * content and writes nothing, so it needs no operation context. */

struct QfQuickCheck {
    const Front *f;
    VEC(U32Vec) paths;
};

QfQuickCheck *qf_qc_new(const Front *f, const uint32_t *const *paths, const size_t *lens, size_t n)
{
    QfQuickCheck *qc = qf_xcalloc(1, sizeof *qc);
    qc->f = f;
    for (size_t i = 0; i < n; i++) {
        if (!lens[i]) continue;                  /* the root is always there */
        U32Vec p = { 0 };
        for (size_t k = 0; k < lens[i]; k++) VEC_PUSH(p, paths[i][k]);
        VEC_PUSH(qc->paths, p);
    }
    return qc;
}

void qf_qc_free(QfQuickCheck *qc)
{
    if (!qc) return;
    for (size_t i = 0; i < qc->paths.n; i++) VEC_FREE(qc->paths.v[i]);
    VEC_FREE(qc->paths);
    free(qc);
}

size_t qf_qc_size(const QfQuickCheck *qc) { return qc->paths.n + 1; }

void qf_qc_vector(const QfQuickCheck *qc, const QfNode *node, uint32_t *out)
{
    out[0] = node->type;
    for (size_t i = 0; i < qc->paths.n; i++) {
        QfNode *n = node_at((QfNode *) node, qc->paths.v[i].v, qc->paths.v[i].n);
        out[i + 1] = n ? n->type : qc->f->top;
    }
}

bool qf_qc_compatible(const QfQuickCheck *qc, const uint32_t *a, const uint32_t *b)
{
    const Front *f = qc->f;
    size_t k = qc->paths.n + 1;
    for (size_t i = 0; i < k; i++) {
        if (a[i] == b[i] || meet_pure(f, a[i], b[i]) != QF_BOTTOM) continue;
        /* an enum's atom can meet text: unification takes the atom's name as text */
        if ((a[i] == f->ttext && f->types.v[b[i]].kind == TK_ATOM) ||
            (b[i] == f->ttext && f->types.v[a[i]].kind == TK_ATOM)) continue;
        return false;
    }
    return true;
}
