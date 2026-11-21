#include <stdio.h>
#include <string.h>

#include "test.h"

int t_checks, t_failures;

#define TEST(name) void name(void);
#include "tests.def"
#undef TEST

static const struct { const char *name; void (*fn)(void); } tests[] = {
#define TEST(name) { #name, name },
#include "tests.def"
#undef TEST
};

/* Runs every test, or only the tests whose names contain argv[1]. */
int main(int argc, char **argv)
{
    int ran = 0;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        if (argc > 1 && !strstr(tests[i].name, argv[1])) continue;
        int before = t_failures;
        tests[i].fn();
        ran++;
        if (t_failures != before) fprintf(stderr, "FAIL %s\n", tests[i].name);
    }
    printf("%d tests, %d checks, %d failures\n", ran, t_checks, t_failures);
    return t_failures ? 1 : 0;
}
