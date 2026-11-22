#include "front/front.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* The text form of a syntax tree: Quadrilateral source that parses to the
 * same tree. Tests and tools use it to inspect what the parser made.
 *
 * The printer puts a binary expression in parentheses when it is an operand,
 * so the text does not depend on operator precedence. The parser drops
 * parentheses, so they do not change the tree. */

static void operand(Buf *b, const X *x);

static bool binary(const X *x)
{
    return x->k == X_AND || x->k == X_OR || x->k == X_ARITH || x->k == X_CMP || x->k == X_LOGIC;
}

/* The conjunct { a: { b: .a.b } } that the parser made for a reference. */
static bool ref_conjunct(const X *x) { return x->k == X_STRUCT && x->u.st.refs; }

static void print_num(Buf *b, const XNum *n)
{
    uint64_t mag = n->mant < 0 ? (uint64_t) -n->mant : (uint64_t) n->mant;
    if (n->mant < 0) qf_buf_putc(b, '-');
    if (n->frac < 0) {
        qf_buf_printf(b, "%" PRIu64 "e%d", mag, -n->frac);   /* 1989e27 */
    } else {
        /* the digits, with zeros in front so that one digit is left of the point */
        qf_buf_printf(b, "%0*" PRIu64, n->frac + 1, mag);
        if (n->frac) {
            size_t point = b->len - (size_t) n->frac;
            qf_buf_putc(b, b->s[b->len - 1]);
            memmove(b->s + point + 1, b->s + point, (size_t) n->frac - 1);
            b->s[point] = '.';
        }
    }
    if (n->unit) qf_buf_puts(b, n->unit);
}

static void print_attrs(Buf *b, const Attr *attrs, int n)
{
    for (int i = 0; i < n; i++) {
        qf_buf_printf(b, " @%s", attrs[i].name);
        if (attrs[i].args[0]) qf_buf_printf(b, "(%s)", attrs[i].args);
    }
}

static void print_struct(Buf *b, const X *x)
{
    bool first = true;
    qf_buf_putc(b, '{');
    for (int i = 0; i < x->u.st.nattrs; i++, first = false) {
        qf_buf_puts(b, first ? "" : ",");
        print_attrs(b, &x->u.st.attrs[i], 1);
    }
    for (int i = 0; i < x->u.st.n; i++, first = false) {
        const XField *f = &x->u.st.fields[i];
        qf_buf_puts(b, first ? " " : ", ");
        if (field_comp(f)) {
            qf_print(b, f->value);
            continue;
        }
        if (f->dlabel) {
            qf_buf_putc(b, '(');
            qf_print(b, f->dlabel);
            qf_buf_puts(b, "): ");
        } else {
            qf_buf_printf(b, "%s: ", f->label);
        }
        qf_print(b, f->value);
        print_attrs(b, f->attrs, f->nattrs);
    }
    qf_buf_puts(b, first ? "}" : " }");
}

static void print_comp(Buf *b, const X *x)
{
    for (int i = 0; i < x->u.comp.n; i++) {
        const CompClause *c = &x->u.comp.clauses[i];
        if (c->kind == C_FOR) {
            qf_buf_puts(b, "for ");
            if (c->key) qf_buf_printf(b, "%s, ", c->key->name);
            qf_buf_printf(b, "%s in ", c->val->name);
        } else {
            qf_buf_puts(b, "if ");
        }
        operand(b, c->expr);
        qf_buf_putc(b, ' ');
    }
    /* in a list, the body is one item: a struct, or one value in braces */
    if (x->u.comp.body->k == X_STRUCT) {
        qf_print(b, x->u.comp.body);
    } else {
        qf_buf_puts(b, "{ ");
        qf_print(b, x->u.comp.body);
        qf_buf_puts(b, " }");
    }
}

static const char *op_text(const X *x)
{
    if (x->k == X_AND) return "&";
    if (x->k == X_OR) return "|";
    if (x->k == X_LOGIC) return x->u.ar.op == '&' ? "&&" : "||";
    switch (x->u.ar.op) {
    case 'e': return "==";
    case 'n': return "!=";
    case 'l': return "<=";
    case 'g': return ">=";
    case '<': return "<";
    case '>': return ">";
    case '+': return "+";
    case '-': return "-";
    case '*': return "*";
    default:  return "/";
    }
}

void qf_print(Buf *b, const X *x)
{
    switch (x->k) {
    case X_TOP: qf_buf_putc(b, '_'); break;
    case X_NAME: case X_ATOM: case X_TAG: qf_buf_puts(b, x->u.name); break;
    case X_LET: qf_buf_puts(b, "<let>"); break;   /* the parser replaces every one */
    case X_VAR:
        qf_buf_puts(b, x->u.var.var->name);
        if (x->u.var.path) qf_buf_puts(b, x->u.var.path);
        break;
    case X_NUM: print_num(b, &x->u.num); break;
    case X_BOUND:
        qf_buf_puts(b, x->u.bound.op == B_GE ? ">=" : x->u.bound.op == B_LE ? "<=" : x->u.bound.op == B_GT ? ">" : "<");
        if (x->u.bound.of) {
            qf_buf_putc(b, '(');
            qf_print(b, x->u.bound.of);
            qf_buf_putc(b, ')');
        } else {
            print_num(b, &x->u.bound.n);
        }
        break;
    case X_REGEX:
        qf_buf_puts(b, x->u.rx.neg ? "!~" : "=~");
        qf_buf_puts(b, x->u.rx.pat);
        break;
    case X_INTERP:
        qf_buf_putc(b, '"');
        for (int i = 0; i <= x->u.in.n; i++) {
            qf_string_escape_into(b, x->u.in.lits[i], strlen(x->u.in.lits[i]));
            if (i == x->u.in.n) break;
            qf_buf_puts(b, "\\(");
            qf_print(b, x->u.in.parts[i]);
            qf_buf_putc(b, ')');
        }
        qf_buf_putc(b, '"');
        break;
    case X_CALL:
        qf_buf_printf(b, "%s(", x->u.call.name);
        for (int i = 0; i < x->u.call.n; i++) {
            if (i) qf_buf_puts(b, ", ");
            qf_print(b, x->u.call.args[i]);
        }
        qf_buf_putc(b, ')');
        break;
    case X_STRUCT: print_struct(b, x); break;
    case X_LIST:
        qf_buf_putc(b, '[');
        for (int i = 0; i < x->u.list.n; i++) {
            if (i) qf_buf_puts(b, ", ");
            qf_print(b, x->u.list.items[i]);
        }
        if (x->u.list.open) {
            qf_buf_puts(b, x->u.list.n ? ", ..." : "...");
            if (x->u.list.elem) operand(b, x->u.list.elem);
        }
        qf_buf_putc(b, ']');
        break;
    case X_COMP: print_comp(b, x); break;
    case X_DEFAULT:
        qf_buf_putc(b, '*');
        operand(b, x->u.sub);
        break;
    case X_NOT:
        qf_buf_putc(b, '!');
        if (x->u.sub->k == X_REGEX) {   /* !=~"r" would read as != */
            qf_buf_putc(b, '(');
            qf_print(b, x->u.sub);
            qf_buf_putc(b, ')');
        } else {
            operand(b, x->u.sub);
        }
        break;
    case X_AND: case X_OR: {
        if (x->k == X_AND && ref_conjunct(x->u.bin.b)) {
            qf_print(b, x->u.bin.a);
            break;
        }
        /* a chain of one operator, a & b & c, needs no parentheses on its left */
        const X *a = x->u.bin.a;
        while (a->k == X_AND && ref_conjunct(a->u.bin.b)) a = a->u.bin.a;
        if (a->k == x->k) qf_print(b, a);
        else operand(b, a);
        qf_buf_printf(b, " %s ", op_text(x));
        operand(b, x->u.bin.b);
        break;
    }
    case X_ARITH: case X_CMP: case X_LOGIC:
        operand(b, x->u.ar.a);
        qf_buf_printf(b, " %s ", op_text(x));
        operand(b, x->u.ar.b);
        break;
    }
}

static void operand(Buf *b, const X *x)
{
    while (x->k == X_AND && ref_conjunct(x->u.bin.b)) x = x->u.bin.a;
    if (!binary(x)) {
        qf_print(b, x);
        return;
    }
    qf_buf_putc(b, '(');
    qf_print(b, x);
    qf_buf_putc(b, ')');
}

void qf_print_decls(Buf *b, const Decl *d, size_t n)
{
    if (n && d[0].pkg) qf_buf_printf(b, "package %s\n", d[0].pkg);
    for (size_t i = 0; i < n; i++) {
        for (const char *line = d[i].doc; line;) {
            const char *nl = strchr(line, '\n');
            size_t len = nl ? (size_t) (nl - line) : strlen(line);
            qf_buf_printf(b, len ? "// %.*s\n" : "//%.*s\n", (int) len, line);
            line = nl ? nl + 1 : NULL;
        }
        qf_buf_printf(b, "%s: ", d[i].label);
        qf_print(b, d[i].value);
        print_attrs(b, d[i].attrs, d[i].nattrs);
        qf_buf_putc(b, '\n');
    }
}
