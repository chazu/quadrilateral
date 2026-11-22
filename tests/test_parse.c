#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "front/front.h"
#include "test.h"

/* The parser of the front end (ADR 0011 section 1, ADR 0014). */

/* Parses one expression and gives its text form, or the diagnostic. The
 * result is valid until the next call. */
static const char *expr_text(const char *src)
{
    static Buf out;
    out.len = 0;
    Front *f = qf_new();
    X *x = qf_parse_expr(f, "t.quad", src);
    if (x) qf_print(&out, x);
    else qf_buf_puts(&out, f->diags.n ? f->diags.v[0].msg : "no diagnostic");
    qf_free(f);
    return out.s ? out.s : "";
}

#define TEXT(src, want)                                                                \
    do {                                                                               \
        const char *got_ = expr_text(src);                                             \
        t_checks++;                                                                    \
        if (strcmp(got_, want)) {                                                      \
            t_failures++;                                                              \
            fprintf(stderr, "%s:%d: %s\n  gives  %s\n  wanted %s\n", __FILE__, __LINE__, src, got_, want); \
        }                                                                              \
    } while (0)

/* The first diagnostic of a file with an error. */
static const char *file_error(const char *src)
{
    static Buf out;
    out.len = 0;
    Front *f = qf_new();
    DeclVec d = { 0 };
    bool ok = qf_parse(f, "t.quad", src, &d);
    CHECK(!ok);
    CHECK(d.n == 0);   /* a file with an error adds no declarations */
    CHECK(f->nerrors == 1 && f->diags.n == 1);
    qf_buf_puts(&out, f->diags.n ? f->diags.v[0].msg : "no diagnostic");
    VEC_FREE(d);
    qf_free(f);
    return out.s;
}

#define FILE_ERROR(src, want)                                                          \
    do {                                                                               \
        const char *got_ = file_error(src);                                            \
        t_checks++;                                                                    \
        if (strcmp(got_, want)) {                                                      \
            t_failures++;                                                              \
            fprintf(stderr, "%s:%d: wanted the error\n  %s\n  got\n  %s\n", __FILE__, __LINE__, want, got_); \
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

static const Decl *find_decl(const DeclVec *d, const char *label)
{
    for (size_t i = 0; i < d->n; i++)
        if (!strcmp(d->v[i].label, label)) return &d->v[i];
    return NULL;
}

static const XField *find_field(const X *st, const char *label)
{
    if (!st || st->k != X_STRUCT) return NULL;
    for (int i = 0; i < st->u.st.n; i++)
        if (st->u.st.fields[i].label && !strcmp(st->u.st.fields[i].label, label)) return &st->u.st.fields[i];
    return NULL;
}

/* --- the workshop example: stage 1 is done when it parses ------------------------------- */

void test_parse_workshop(void)
{
    static const struct { const char *file; size_t decls; } files[] = {
        { "blueprints.tri", 10 }, { "data.tri", 5 }, { "levers.tri", 7 }, { "operations.tri", 3 },
        { "rooms.tri", 9 },       { "text.tri", 3 }, { "things.tri", 8 },
    };
    Front *f = qf_new();
    DeclVec all = { 0 };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char path[128];
        snprintf(path, sizeof path, "tests/data/workshop/%s", files[i].file);
        char *src = read_file(path);
        CHECK(src != NULL);
        if (!src) continue;
        size_t before = all.n;
        bool ok = qf_parse(f, path, src, &all);
        if (!ok && f->diags.n) fprintf(stderr, "%s\n", f->diags.v[0].msg);
        CHECK(ok);
        CHECK(all.n - before == files[i].decls);

        /* the text form is source that parses to the same tree */
        Buf text = { 0 }, again = { 0 };
        qf_print_decls(&text, all.v + before, all.n - before);
        Front *g = qf_new();
        DeclVec d = { 0 };
        CHECK(text.s && qf_parse(g, "printed", text.s, &d));
        CHECK(d.n == files[i].decls);
        qf_print_decls(&again, d.v, d.n);
        CHECK(text.s && again.s && !strcmp(text.s, again.s));
        VEC_FREE(d);
        qf_free(g);
        qf_buf_free(&text);
        qf_buf_free(&again);
        free(src);
    }
    CHECK(f->nerrors == 0);
    CHECK(all.n == 45);

    /* a type, with its docstring and its attributes */
    const Decl *physical = find_decl(&all, "#Physical");
    CHECK(physical && physical->kind == L_DEF && physical->value->k == X_STRUCT);
    if (physical) {
        CHECK(physical->doc && !strcmp(physical->doc, "Anything with mass and a material."));
        CHECK(!strcmp(physical->loc.file, "tests/data/workshop/things.tri"));
        CHECK(physical->loc.line == 10 && physical->loc.col == 1);
        const X *st = physical->value;
        CHECK(st->u.st.nattrs == 1 && !strcmp(st->u.st.attrs[0].name, "component") &&
              !strcmp(st->u.st.attrs[0].args, "Physical"));
        const XField *mass = find_field(st, "mass");
        CHECK(mass && mass->value->k == X_NAME && !strcmp(mass->value->u.name, "mass"));
        CHECK(mass && mass->nattrs == 1 && !strcmp(mass->attrs[0].name, "field") &&
              !strcmp(mass->attrs[0].args, "content, saved"));
    }
    /* an atom's declaration, and an instance with quantity literals */
    const Decl *steel = find_decl(&all, "\"steel\"");
    CHECK(steel && steel->kind == L_ATOM && steel->value->k == X_OR);
    const Decl *wrench = find_decl(&all, "wrench_19");
    CHECK(wrench && wrench->kind == L_INST && wrench->value->k == X_AND);
    if (wrench) {
        const XField *mass = find_field(wrench->value->u.bin.b, "mass");
        CHECK(mass && mass->value->k == X_NUM);
        if (mass) {
            CHECK_I64(mass->value->u.num.mant, 85);   /* 0.85kg */
            CHECK_I64(mass->value->u.num.frac, 2);
            CHECK(!strcmp(mass->value->u.num.unit, "kg"));
        }
    }
    VEC_FREE(all);
    qf_free(f);
}

/* --- numbers and units (ADR 0014) --------------------------------------------------------- */

static XNum num_of(const char *src)
{
    Front *f = qf_new();
    X *x = qf_parse_expr(f, "t.quad", src);
    XNum n = { 0, -1, NULL };
    static char unit[64];
    CHECK(x && x->k == X_NUM);
    if (x && x->k == X_NUM) {
        n = x->u.num;
        if (n.unit) {
            snprintf(unit, sizeof unit, "%s", n.unit);
            n.unit = unit;
        }
    }
    qf_free(f);
    return n;
}

void test_parse_numbers(void)
{
    XNum n = num_of("250mm");
    CHECK(n.mant == 250 && n.frac == 0 && !strcmp(n.unit, "mm"));
    n = num_of("0.35kg");
    CHECK(n.mant == 35 && n.frac == 2 && !strcmp(n.unit, "kg"));
    n = num_of("-1.50");
    CHECK(n.mant == -150 && n.frac == 2 && n.unit == NULL);
    n = num_of("42");
    CHECK(n.mant == 42 && n.frac == 0 && n.unit == NULL);
    n = num_of("293.15K");
    CHECK(n.mant == 29315 && n.frac == 2 && !strcmp(n.unit, "K"));
    n = num_of("9223372036854775807");
    CHECK(n.mant == INT64_MAX);

    /* an exponent moves the decimal point, exactly; frac is negative for a large number */
    n = num_of("1e5");
    CHECK(n.mant == 1 && n.frac == -5 && n.unit == NULL);
    n = num_of("1e-5");
    CHECK(n.mant == 1 && n.frac == 5 && n.unit == NULL);
    n = num_of("5.67e-8W/m^2/K^4");
    CHECK(n.mant == 567 && n.frac == 10 && !strcmp(n.unit, "W/m^2/K^4"));
    n = num_of("1.989E30kg");
    CHECK(n.mant == 1989 && n.frac == -27 && !strcmp(n.unit, "kg"));
    n = num_of("-1.5e+2m");
    CHECK(n.mant == -15 && n.frac == -1 && !strcmp(n.unit, "m"));
    n = num_of("2.50e2");
    CHECK(n.mant == 250 && n.frac == 0 && n.unit == NULL);
    n = num_of("1e9999");
    CHECK(n.mant == 1 && n.frac == -9999);
    /* an e with no digits after it is a unit */
    n = num_of("3eV");
    CHECK(n.mant == 3 && n.frac == 0 && !strcmp(n.unit, "eV"));
    n = num_of("2e");
    CHECK(n.mant == 2 && n.frac == 0 && !strcmp(n.unit, "e"));
    n = num_of("1e5e");
    CHECK(n.mant == 1 && n.frac == -5 && !strcmp(n.unit, "e"));
    TEXT("1e - 5", "1e - 5");
    TEXT("5.67e-8W/m^2/K^4", "0.0000000567W/m^2/K^4");
    TEXT("1.989E30kg", "1989e27kg");
    TEXT("-2e3 * 1e-3", "-2e3 * 0.001");
    TEXT(">=1e-3m & <1e3m", ">=0.001m & <1e3m");
    TEXT("0e2", "0e2");
    TEXT("-1.5e+2m", "-15e1m");
    TEXT("1.e5", "t.quad:1:1: error: malformed number: 1.e5");
    TEXT("1e10000", "t.quad:1:1: error: number out of range: 1e10000");
    TEXT("1e99999999999999999999", "t.quad:1:1: error: number out of range: 1e99999999999999999999");

    /* compound units: the unit is attached, with *, / and ^ */
    n = num_of("1000J/K");
    CHECK(n.mant == 1000 && !strcmp(n.unit, "J/K"));
    n = num_of("5kg*m^2/s^2");
    CHECK(n.mant == 5 && !strcmp(n.unit, "kg*m^2/s^2"));
    n = num_of("3m^-1");
    CHECK(!strcmp(n.unit, "m^-1"));
    n = num_of("1m3");
    CHECK(!strcmp(n.unit, "m3"));

    /* an operator continues a unit only when a letter follows it at once */
    TEXT("10mm*2", "10mm * 2");
    TEXT("10mm/2", "10mm / 2");
    TEXT("10m / x", "10m / x");
    TEXT("10m * x", "10m * x");
    TEXT("10m/x", "10m/x");   /* one number with the unit m/x: a later stage rejects the unit */
    TEXT("2 - 1", "2 - 1");
    TEXT("a -1", "a - 1");
    TEXT("[-1, 2 * -3]", "[-1, 2 * -3]");
    TEXT("0.050", "0.050");
    TEXT("-0.5m", "-0.5m");

    TEXT(">=10mm & <=24mm", ">=10mm & <=24mm");
    TEXT("{ open: 20W/K, closed: 10W/K }", "{ open: 20W/K, closed: 10W/K }");

    TEXT("1.2.3", "t.quad:1:1: error: malformed number: 1.2.3");
    TEXT("1.", "t.quad:1:1: error: malformed number: 1.");
    TEXT("5m^", "t.quad:1:1: error: expected digits after '^' in a unit");
    TEXT("5m^-", "t.quad:1:1: error: expected digits after '^' in a unit");
    TEXT("9223372036854775808", "t.quad:1:1: error: number out of range: 9223372036854775808");
    TEXT(">=x.", "t.quad:1:4: error: unexpected character '.'");

    /* a reference starts with '.', so it never joins a unit */
    TEXT("10m/.duration", "10m / .duration");
    TEXT("10m*.w", "10m * .w");

    /* an operator joined a name to the unit: no valid source has these forms */
    TEXT("10m/sqrt(4)", "t.quad:1:1: error: '(' directly after the unit 'm/sqrt'. To multiply or divide by a name, "
                        "put spaces around the operator");
    TEXT("2 + 10m/v.size", "t.quad:1:5: error: '.' directly after the unit 'm/v'. To multiply or divide by a name, "
                           "put spaces around the operator");
    TEXT("10kg*xs[0]", "t.quad:1:1: error: '[' directly after the unit 'kg*xs'. To multiply or divide by a name, "
                       "put spaces around the operator");
    TEXT("10m / sqrt(4)", "10m / sqrt(4)");
    TEXT("10m(", "t.quad:1:4: error: unexpected input after expression");   /* a simple unit: no hint */

    /* the SI form, with a space: the error shows the attached form */
    TEXT("10 m", "t.quad:1:4: error: unexpected input after expression. "
                 "If 'm' is a unit, write it directly after the number: 10m");
    FILE_ERROR("a: { x: 20 W/K, y: 2 }\n", "t.quad:1:13: error: expected ':' after field label. "
                                           "If 'W/K' is a unit, write it directly after the number: 20W/K");
    FILE_ERROR("a: 1e3 kg\nb: 2\n", "t.quad:2:1: error: expected ':' after declaration label. "
                                    "If 'kg' is a unit, write it directly after the number: 1e3kg");
    FILE_ERROR("a: [10 m, 2]\n", "t.quad:1:8: error: expected ',' or ']'. "
                                 "If 'm' is a unit, write it directly after the number: 10m");
    FILE_ERROR("a: 5 kg*m^2\n", "t.quad:1:10: error: unexpected character '^'. "
                                "A '^' is valid only in a unit, and a unit follows its number with no space: 5kg*m^2");
    /* no hint: the number has a unit, the name is on the next line, the name
     * starts a clause of a comprehension, or the name cannot be a unit */
    FILE_ERROR("a: { x: 10mm m }\n", "t.quad:1:16: error: expected ':' after field label");
    FILE_ERROR("a: { x: 1m2 m }\n", "t.quad:1:15: error: expected ':' after field label");
    FILE_ERROR("a: {\n    b: 1\n    c 2\n}\n", "t.quad:3:7: error: expected ':' after field label");
    FILE_ERROR("a: [for x in xs if x > 3 for y of ys { x }]\n", "t.quad:1:32: error: expected 'in' after for's variables");
    FILE_ERROR("a: [for x in xs if x > 3 for y.z in ys { x }]\n", "t.quad:1:30: error: expected a variable's name");
    FILE_ERROR("a: { x: 10 _m }\n", "t.quad:1:15: error: expected ':' after field label");
    TEXT("[for x in xs if x > 3 for y in ys { x }]", "[for x in xs if (x > 3) for y in ys { x }]");
}

/* --- expressions and their text form -------------------------------------------------------- */

void test_parse_expressions(void)
{
    /* unification, disjunction, defaults, bounds */
    TEXT("*0 | >=0 & <=1000", "*0 | (>=0 & <=1000)");
    TEXT("#Physical & #Sized & { wear: 1 }", "#Physical & #Sized & { wear: 1 }");
    TEXT("a | b & c | d", "a | (b & c) | d");
    TEXT("(a | b) & c", "(a | b) & c");
    TEXT("\"steel\" | \"brass\"", "\"steel\" | \"brass\"");
    TEXT("true | false", "true | false");
    TEXT("_", "_");
    TEXT("$t & int", "$t & int");
    TEXT(">=lo & <(a + 1)", ">=(lo) & <(a + 1)");
    TEXT("pkg.#Type & { a: 1 }", "pkg.#Type & { a: 1 }");
    TEXT("x.y[0].z", "x.y[0].z");

    /* structs and lists */
    TEXT("{}", "{}");
    TEXT("[]", "[]");
    TEXT("{a: 1 b: 2}", "{ a: 1, b: 2 }");
    TEXT("{ a: b: c: 1 }", "{ a: { b: { c: 1 } } }");
    TEXT("[1, 2, ...]", "[1, 2, ...]");
    TEXT("[...length]", "[...length]");
    TEXT("[...]", "[...]");

    /* arithmetic, comparisons and logic. An equation has the same tree as a
     * condition; the elaborator decides what it means (ADR 0014). */
    TEXT("a - b - c", "(a - b) - c");
    TEXT("a - (b - c)", "a - (b - c)");
    TEXT("a + b * c", "a + (b * c)");
    TEXT("2 * *3", "2 * *3");
    TEXT(".energy == .capacity * .temperature", ".energy == (.capacity * .temperature)");
    TEXT("!done && ready || x == 1", "(!done && ready) || (x == 1)");
    TEXT("a != b", "a != b");
    TEXT("a <= b && b >= c", "(a <= b) && (b >= c)");

    /* strings, regular expressions, interpolation */
    TEXT("#\"raw \\d \"quoted\"\"#", "\"raw \\\\d \\\"quoted\\\"\"");
    TEXT("\"\\u00e9\\n\"", "\"\xc3\xa9\\n\"");
    TEXT("text & =~#\"^[A-Z]+-\\d+$\"#", "text & =~\"^[A-Z]+-\\\\d+$\"");
    TEXT("text & !~\"x\"", "text & !~\"x\"");
    TEXT("!(=~\"r\")", "!(=~\"r\")");   /* not !=~"r", which reads as != */
    TEXT("\"Nails-\\(.count)\"", "\"Nails-\\(.count)\"");
    TEXT("\"\\(a + 1) of \\(len(xs))\"", "\"\\(a + 1) of \\(len(xs))\"");

    /* functions: the parser does not know their names */
    TEXT("math.Clamp($w + 10, 0, 1000)", "math.Clamp($w + 10, 0, 1000)");
    TEXT("nosuch.Fn(1, \"a\")", "nosuch.Fn(1, \"a\")");
    TEXT("ref(12)", "ref(12)");

    /* comprehensions and computed labels */
    TEXT("[for r in ratios if r > 1 { r * 12 }]", "[for r in ratios if (r > 1) { r * 12 }]");
    TEXT("[for k, v in src { \"\\(k)\": v }]", "[for k, v in src { (\"\\(k)\"): v }]");
    TEXT("{ for m in #Material { (m): int & >=0 } }", "{ for m in #Material { (m): int & >=0 } }");
    TEXT("{ if a > 1 { b: 2 } }", "{ if (a > 1) { b: 2 } }");
    TEXT("[for x in xs { x.size }]", "[for x in xs { x.size }]");
    TEXT("{ (k): 1, \"\\(k)_x\": 2 }", "{ (k): 1, (\"\\(k)_x\"): 2 }");

    /* errors in an expression: file, line, column */
    TEXT("a ? b", "t.quad:1:3: error: unexpected character '?'");
    TEXT("\"abc", "t.quad:1:1: error: unterminated string");
    TEXT("\"a\\qb\"", "t.quad:1:3: error: unknown escape in a string; the escapes are \\a \\b \\f \\n \\r \\t \\v \\\\ \\/ \\\" \\u \\U");
    TEXT("{ a 1 }", "t.quad:1:5: error: expected ':' after field label");
    TEXT("{ a: 1", "t.quad:1:1: error: unterminated struct");
    TEXT("[1 2]", "t.quad:1:4: error: expected ',' or ']'");
    TEXT("a = 1", "t.quad:1:3: error: unexpected input after expression");
    TEXT("a &", "t.quad:1:4: error: expected a value");
    TEXT("-x", "t.quad:1:1: error: unexpected character '-'");
    TEXT("=~ 5", "t.quad:1:4: error: expected a string after =~ or !~");
    TEXT("[for x of xs { x }]", "t.quad:1:8: error: expected 'in' after for's variables");
    TEXT("\"\\()\"", "t.quad:1:4: error: an empty interpolation: \\()");
}

/* --- attributes: any name, arguments as text ------------------------------------------------- */

void test_parse_attributes(void)
{
    TEXT("{ @component(Tool), wear: 1 @field(saved) @x }", "{ @component(Tool), wear: 1 @field(saved) @x }");
    TEXT("{ a: 1 @anything(b, c: (d, e)) }", "{ a: 1 @anything(b, c: (d, e)) }");
    /* @embed is an ordinary attribute: the parser reads no file */
    TEXT("{ notes: text @embed(file=no_such_file.txt, type=text) }",
         "{ notes: text @embed(file=no_such_file.txt, type=text) }");
    TEXT("{ a: 1 @b( }", "t.quad:1:8: error: unterminated attribute");
    TEXT("{ a: 1 @ }", "t.quad:1:8: error: expected an attribute name after '@'");

    Front *f = qf_new();
    DeclVec d = { 0 };
    CHECK(qf_parse(f, "t.quad", "ops: { a: 1 } @family(operations) @quickcheck(target.state)\n", &d));
    CHECK(d.n == 1 && d.v[0].nattrs == 2);
    if (d.n == 1 && d.v[0].nattrs == 2) {
        CHECK(!strcmp(d.v[0].attrs[0].name, "family") && !strcmp(d.v[0].attrs[0].args, "operations"));
        CHECK(!strcmp(d.v[0].attrs[1].name, "quickcheck") && !strcmp(d.v[0].attrs[1].args, "target.state"));
    }
    VEC_FREE(d);
    qf_free(f);
}

/* --- declarations, packages, docstrings ------------------------------------------------------- */

void test_parse_declarations(void)
{
    const char *src = "package tools\n"
                      "\n"
                      "// A comment that is not a docstring.\n"
                      "\n"
                      "// A wrench.\n"
                      "//\n"
                      "// It turns nuts.\n"
                      "#Wrench: #Tool & { size: length }\n"
                      "\"steel\": \"mild\" | \"forged\"\n"
                      "wrench_19: #Wrench & {\n"
                      "    size: 19mm\n"
                      "}\n"
                      "smithy.#Anvil: { mass: >=50kg }\n";
    Front *f = qf_new();
    DeclVec d = { 0 };
    CHECK(qf_parse(f, "tools.quad", src, &d));
    CHECK(d.n == 4);
    if (d.n == 4) {
        CHECK(d.v[0].kind == L_DEF && !strcmp(d.v[0].label, "#Wrench"));
        CHECK(d.v[0].doc && !strcmp(d.v[0].doc, "A wrench.\n\nIt turns nuts."));
        CHECK(d.v[0].loc.line == 8 && d.v[0].loc.col == 1 && !strcmp(d.v[0].loc.file, "tools.quad"));
        CHECK(d.v[1].kind == L_ATOM && !strcmp(d.v[1].label, "\"steel\"") && !d.v[1].doc);
        CHECK(d.v[2].kind == L_INST && !strcmp(d.v[2].label, "wrench_19"));
        CHECK(d.v[3].kind == L_DEF && !strcmp(d.v[3].label, "smithy.#Anvil"));
        for (size_t i = 0; i < 4; i++) CHECK(d.v[i].pkg && !strcmp(d.v[i].pkg, "tools"));
        /* a struct knows the package of its file */
        CHECK(d.v[3].value->k == X_STRUCT && !strcmp(d.v[3].value->u.st.pkg, "tools"));
        /* the location of a value inside the file */
        const XField *size = find_field(d.v[2].value->u.bin.b, "size");
        CHECK(size && size->loc.line == 11 && size->loc.col == 5);
        CHECK(size && size->value->loc.line == 11 && size->value->loc.col == 11);
    }
    Buf text = { 0 };
    qf_print_decls(&text, d.v, d.n);
    CHECK(text.s && !strcmp(text.s, "package tools\n"
                                    "// A wrench.\n"
                                    "//\n"
                                    "// It turns nuts.\n"
                                    "#Wrench: #Tool & { size: length }\n"
                                    "\"steel\": \"mild\" | \"forged\"\n"
                                    "wrench_19: #Wrench & { size: 19mm }\n"
                                    "smithy.#Anvil: { mass: >=50kg }\n"));
    qf_buf_free(&text);

    /* a second file adds to the same vector; a file with an error adds nothing */
    CHECK(qf_parse(f, "more.quad", "hammer: #Tool\n", &d));
    CHECK(d.n == 5 && d.v[4].pkg == NULL);
    CHECK(!qf_parse(f, "bad.quad", "ok: 1\nbroken: [\n", &d));
    CHECK(d.n == 5);
    CHECK(f->nerrors == 1 && !strncmp(f->diags.v[0].msg, "bad.quad:", 9));
    VEC_FREE(d);
    qf_free(f);

    FILE_ERROR("a = 1\n", "t.quad:1:3: error: expected ':' after declaration label ('=' only binds a let)");
    FILE_ERROR("a: 1\n  b 2\n", "t.quad:2:5: error: expected ':' after declaration label");
    FILE_ERROR("a: {\n    b: 1\n    c 2\n}\n", "t.quad:3:7: error: expected ':' after field label");
    FILE_ERROR("package a.b\n", "t.quad:1:9: error: a package name is one name");
    FILE_ERROR("tools.wrench: 1\n",
               "t.quad:1:1: error: a declaration's name is its own: declare it in its package's files, unqualified");
    FILE_ERROR("5: 1\n", "t.quad:1:1: error: expected a declaration");
    FILE_ERROR("\"a\\(b)\": 1\n", "t.quad:1:1: error: a declaration's label can't be interpolated");
    FILE_ERROR("a: \"line one\n", "t.quad:1:4: error: unterminated string");
    /* an error inside an interpolation has its own place in the file */
    FILE_ERROR("a: 1\nb: \"x \\(1 ? 2)\"\n", "t.quad:2:11: error: unexpected character '?'");
}

/* --- lets and references ------------------------------------------------------------------------ */

void test_parse_lets_and_refs(void)
{
    const char *src = "let socket = 30mm\n"
                      "#Ratchet: #Lever & { reach: .arm + socket, _spare: 2, extra: _spare + 1 }\n"
                      "set: { sizes: [socket, later], let later = socket * 2 }\n";
    Front *f = qf_new();
    DeclVec d = { 0 };
    CHECK(qf_parse(f, "t.quad", src, &d));
    CHECK(d.n == 2);
    if (d.n == 2) {
        /* a reference: the declaration gains { arm: .arm }, marked as made
         * for references, and the text form leaves it out */
        const X *v = d.v[0].value;
        CHECK(v->k == X_AND && v->u.bin.b->k == X_STRUCT && v->u.bin.b->u.st.refs);
        const XField *arm = find_field(v->u.bin.b, "arm");
        CHECK(arm && arm->value->k == X_TAG && !strcmp(arm->value->u.name, ".arm"));
        Buf text = { 0 };
        qf_print_decls(&text, d.v, d.n);
        /* each use of a let is the expression that the let binds; a hidden
         * field is a let; a let can come after its use */
        CHECK(text.s && !strcmp(text.s, "#Ratchet: #Lever & { reach: .arm + 30mm, extra: 2 + 1 }\n"
                                        "set: { sizes: [30mm, 30mm * 2] }\n"));
        qf_buf_free(&text);
    }
    /* the names that lets bind, for a later check against declarations */
    CHECK(f->lets.n == 3);
    if (f->lets.n == 3) {
        CHECK(!strcmp(f->lets.v[0].name, "socket") && !f->lets.v[0].var);
        CHECK(!strcmp(f->lets.v[1].name, "_spare"));
        CHECK(!strcmp(f->lets.v[2].name, "later"));
    }
    VEC_FREE(d);
    qf_free(f);

    /* a reference with a list index */
    f = qf_new();
    DeclVec e = { 0 };
    CHECK(qf_parse(f, "t.quad", "kit: { steps: [1, 2], second: .steps[1] }\n", &e));
    if (e.n == 1) {
        const X *v = e.v[0].value;
        CHECK(v->k == X_AND && v->u.bin.b->u.st.refs);
        const XField *steps = find_field(v->u.bin.b, "steps");
        const XField *rest = steps ? find_field(steps->value, "rest") : NULL;
        const XField *first = rest ? find_field(rest->value, "first") : NULL;
        CHECK(first && first->value->k == X_TAG && !strcmp(first->value->u.name, ".steps[1]"));
    }
    /* a comprehension's variable is in the list of bound names */
    CHECK(qf_parse(f, "u.quad", "xs: [for k, v in src { v }]\n", &e));
    CHECK(f->lets.n == 2 && f->lets.v[0].var && f->lets.v[1].var);
    VEC_FREE(e);
    qf_free(f);

    FILE_ERROR("let a = b\nlet b = a\nx: a\n", "t.quad:1:5: error: let a refers to itself");
    FILE_ERROR("let a = 1\nlet a = 2\n", "t.quad:2:5: error: let a: the name is already bound, by the let at 1:5");
    FILE_ERROR("let true = 1\n", "t.quad:1:5: error: 'true' can't be a let's name");
    FILE_ERROR("x: [for a in xs { [for a in ys { a }] }]\n",
               "t.quad:1:24: error: a: the name is already bound, by the for at 1:9");
    FILE_ERROR("x: { a: { b: .a } }\n",
               "t.quad:1:14: error: .a refers to a field that contains it (.a.b), which would make the structure cyclic");
    FILE_ERROR("x: { .a: 1 }\n", "t.quad:1:6: error: a field's label is a plain name; a reference (.a.b) is a value");
}

/* --- the sketch of ADR 0011 section 2 ------------------------------------------------------------ */

void test_parse_adr_sketch(void)
{
    const char *src =
        "energy:      #Kind & { dimension: \"J\",   scale: -3, conserved: true }\n"
        "temperature: #Kind & { dimension: \"K\",   scale: -6 }\n"
        "capacity:    #Kind & { dimension: \"J/K\", scale: -3 }\n"
        "\n"
        "#ThermalPort: #Port & {\n"
        "    temperature: temperature   // an observation: intensive, read only\n"
        "    capacity:    capacity\n"
        "    energy:      energy        // a stock: reached only through transfers\n"
        "}\n"
        "\n"
        "// A room as one lump.\n"
        "#LumpedRoom: #Phenomenon & {\n"
        "    energy:      energy\n"
        "    capacity:    capacity & 1000J/K\n"
        "    temperature: temperature\n"
        "    laws: [ .energy == .capacity * .temperature ]\n"
        "    ports: west: #ThermalPort & { temperature: .temperature, capacity: .capacity, energy: .energy }\n"
        "}\n"
        "\n"
        "#ConductiveWall: #Phenomenon & {\n"
        "    ports: { hot: #ThermalPort, cold: #ThermalPort }\n"
        "    conductance: thermal_conductance & { open: 20W/K, closed: 10W/K }\n"
        "    interaction: #EnergyTransfer & { source: .ports.hot.energy, sink: .ports.cold.energy }\n"
        "    evaluator: \"native.conduction\"\n"
        "}\n";
    Front *f = qf_new();
    DeclVec d = { 0 };
    bool ok = qf_parse(f, "wall.quad", src, &d);
    if (!ok && f->diags.n) fprintf(stderr, "%s\n", f->diags.v[0].msg);
    CHECK(ok);
    CHECK(d.n == 6);
    const Decl *room = find_decl(&d, "#LumpedRoom");
    CHECK(room && room->doc && !strcmp(room->doc, "A room as one lump."));
    if (room) {
        /* the law is a list with one equation over references */
        const X *v = room->value;
        while (v->k == X_AND && v->u.bin.b->k == X_STRUCT && v->u.bin.b->u.st.refs) v = v->u.bin.a;
        const XField *laws = find_field(v->u.bin.b, "laws");
        CHECK(laws && laws->value->k == X_LIST && laws->value->u.list.n == 1);
        if (laws && laws->value->u.list.n == 1) {
            const X *eq = laws->value->u.list.items[0];
            CHECK(eq->k == X_CMP && eq->u.ar.op == 'e');
            CHECK(eq->u.ar.a->k == X_TAG && !strcmp(eq->u.ar.a->u.name, ".energy"));
            CHECK(eq->u.ar.b->k == X_ARITH && eq->u.ar.b->u.ar.op == '*');
        }
        CHECK(room->comp);   /* the equation is work for a later stage */
    }
    Buf text = { 0 };
    qf_print_decls(&text, d.v + 4, 1);
    CHECK(text.s && !strcmp(text.s,
        "// A room as one lump.\n"
        "#LumpedRoom: #Phenomenon & { energy: energy, capacity: capacity & 1000J/K, temperature: temperature, "
        "laws: [.energy == (.capacity * .temperature)], "
        "ports: { west: #ThermalPort & { temperature: .temperature, capacity: .capacity, energy: .energy } } }\n"));
    qf_buf_free(&text);
    VEC_FREE(d);
    qf_free(f);
}

/* --- a property: the text form of a random tree parses back to the same text ------------------- */

static void random_expr(uint64_t *rng, Buf *b, int depth)
{
    static const char *const leaves[] = { "a", "#T", "\"s\"", "12", "3.5kg", "20W/K", "5.67e-8W/m^2", "2e30kg", "$t", ".p.q", "_", "true",
                                          ">=1mm", "<x", "=~\"r\"", "pkg.#U", "b.c[1]" };
    static const char *const ops[] = { " & ", " | ", " + ", " - ", " * ", " / ", " == ", " < ", " && ", " || " };
    int pick = depth <= 0 ? 0 : (int) t_range(rng, 0, 9);
    switch (pick) {
    default:
        qf_buf_puts(b, leaves[t_range(rng, 0, (int64_t) (sizeof leaves / sizeof *leaves) - 1)]);
        break;
    case 4: case 5: case 6:
        qf_buf_putc(b, '(');
        random_expr(rng, b, depth - 1);
        qf_buf_puts(b, ops[t_range(rng, 0, (int64_t) (sizeof ops / sizeof *ops) - 1)]);
        random_expr(rng, b, depth - 1);
        qf_buf_putc(b, ')');
        break;
    case 7: {
        int n = (int) t_range(rng, 0, 3);
        qf_buf_puts(b, "{ ");
        for (int i = 0; i < n; i++) {
            qf_buf_printf(b, "f%d: ", i);
            random_expr(rng, b, depth - 1);
            qf_buf_puts(b, t_range(rng, 0, 3) ? ", " : " @note(x), ");
        }
        qf_buf_puts(b, "}");
        break;
    }
    case 8: {
        int n = (int) t_range(rng, 0, 3);
        qf_buf_putc(b, '[');
        for (int i = 0; i < n; i++) {
            random_expr(rng, b, depth - 1);
            qf_buf_puts(b, ", ");
        }
        qf_buf_puts(b, t_range(rng, 0, 1) ? "...]" : "]");
        break;
    }
    case 9:
        qf_buf_puts(b, t_range(rng, 0, 1) ? "*(" : "!(");
        random_expr(rng, b, depth - 1);
        qf_buf_putc(b, ')');
        break;
    }
}

void test_prop_parse_print(void)
{
    uint64_t rng = 2026;
    for (int i = 0; i < 2000; i++) {
        Buf src = { 0 }, one = { 0 }, two = { 0 };
        random_expr(&rng, &src, 4);
        Front *f = qf_new();
        X *x = qf_parse_expr(f, "t.quad", src.s);
        CHECK(x != NULL);
        if (x) {
            qf_print(&one, x);
            X *y = qf_parse_expr(f, "t.quad", one.s);
            CHECK(y != NULL);
            if (y) qf_print(&two, y);
            CHECK(y && !strcmp(one.s, two.s));
            if (!y || strcmp(one.s, two.s))
                fprintf(stderr, "  source %s\n  first  %s\n  second %s\n", src.s, one.s, two.s ? two.s : "(error)");
        } else if (f->diags.n) {
            fprintf(stderr, "  source %s\n  %s\n", src.s, f->diags.v[0].msg);
        }
        qf_free(f);
        qf_buf_free(&src);
        qf_buf_free(&one);
        qf_buf_free(&two);
    }
}
