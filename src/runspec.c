#include "runspec.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t rs_type_size(rs_type t)
{
    switch (t) {
    case RS_CHAR: case RS_UCHAR:   return 1;
    case RS_SHORT: case RS_USHORT: return 2;
    case RS_INT: case RS_UINT: case RS_FLOAT: return 4;
    default: return 8;
    }
}

static const char *const rs_type_names[] = {
    "float", "double", "char", "uchar", "short", "ushort", "int", "uint", "long", "ulong"
};

const char *rs_type_name(rs_type t) { return rs_type_names[t]; }

static int parse_type(const char *s, rs_type *out)
{
    for (unsigned i = 0; i < sizeof rs_type_names / sizeof *rs_type_names; i++)
        if (strcmp(s, rs_type_names[i]) == 0) { *out = (rs_type)i; return 0; }
    return -1;
}

/* Split a line into whitespace-separated tokens. The one directive that needs
 * embedded spaces (`build`) is handled separately by taking the rest of the line. */
static unsigned tokenize(char *line, char *tok[], unsigned max)
{
    unsigned n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
    }
    return n;
}

static void trim(char *s)
{
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) s[--len] = 0;
}

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); fclose(f); return -1; } while (0)

int rs_parse(const char *path, rs_spec *sp, char *err, size_t errsz)
{
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errsz, "cannot open spec '%s'", path); return -1; }

    memset(sp, 0, sizeof *sp);
    snprintf(sp->spec_path, sizeof sp->spec_path, "%s", path);
    sp->work_dim = 1;
    sp->global[0] = sp->global[1] = sp->global[2] = 1;
    sp->local[0]  = sp->local[1]  = sp->local[2]  = 1;

    char raw[1024];
    unsigned lineno = 0;
    while (fgets(raw, sizeof raw, f)) {
        lineno++;
        char *hash = strchr(raw, '#');
        if (hash) *hash = 0;
        trim(raw);

        char work[1024];
        snprintf(work, sizeof work, "%s", raw);
        char *tok[16];
        unsigned nt = tokenize(work, tok, 16);
        if (nt == 0) continue;

        if (strcmp(tok[0], "kernel") == 0 && nt >= 2) {
            snprintf(sp->kernel_path, sizeof sp->kernel_path, "%s", tok[1]);
        } else if (strcmp(tok[0], "name") == 0 && nt >= 2) {
            snprintf(sp->kernel_name, sizeof sp->kernel_name, "%s", tok[1]);
        } else if (strcmp(tok[0], "build") == 0) {
            /* everything after the keyword, verbatim */
            const char *p = strstr(raw, "build") + 5;
            while (*p && isspace((unsigned char)*p)) p++;
            snprintf(sp->build_options, sizeof sp->build_options, "%s", p);
        } else if (strcmp(tok[0], "global") == 0) {
            for (unsigned i = 1; i < nt && i <= 3; i++) sp->global[i - 1] = strtoul(tok[i], 0, 0);
            if (nt - 1 > sp->work_dim) sp->work_dim = nt - 1;
        } else if (strcmp(tok[0], "local") == 0) {
            for (unsigned i = 1; i < nt && i <= 3; i++) sp->local[i - 1] = strtoul(tok[i], 0, 0);
            if (nt - 1 > sp->work_dim) sp->work_dim = nt - 1;
        } else if (strcmp(tok[0], "arg") == 0) {
            if (sp->nargs >= 32) FAIL("line %u: too many args", lineno);
            if (nt < 4) FAIL("line %u: expected 'arg <name> <global|local|scalar> ...'", lineno);
            rs_arg *a = &sp->args[sp->nargs];
            memset(a, 0, sizeof *a);
            snprintf(a->name, sizeof a->name, "%s", tok[1]);
            if (strcmp(tok[2], "global") == 0) {
                if (nt < 6)
                    FAIL("line %u: expected 'arg %s global <type> <count> <in|out|inout> [init]'",
                         lineno, a->name);
                a->kind = RS_GLOBAL;
                if (parse_type(tok[3], &a->type)) FAIL("line %u: unknown type '%s'", lineno, tok[3]);
                a->count = strtoul(tok[4], 0, 0);
                a->dir = strcmp(tok[5], "out") == 0   ? RS_OUT
                       : strcmp(tok[5], "inout") == 0 ? RS_INOUT
                                                      : RS_IN;
                snprintf(a->init, sizeof a->init, "%s", nt >= 7 ? tok[6] : "zero");
            } else if (strcmp(tok[2], "local") == 0) {
                if (nt < 5) FAIL("line %u: expected 'arg %s local <type> <count>'", lineno, a->name);
                a->kind = RS_LOCAL;
                if (parse_type(tok[3], &a->type)) FAIL("line %u: unknown type '%s'", lineno, tok[3]);
                a->count = strtoul(tok[4], 0, 0);
            } else if (strcmp(tok[2], "scalar") == 0) {
                if (nt < 5) FAIL("line %u: expected 'arg %s scalar <type> <value>'", lineno, a->name);
                a->kind = RS_SCALAR;
                if (parse_type(tok[3], &a->type)) FAIL("line %u: unknown type '%s'", lineno, tok[3]);
                a->scalar = strtod(tok[4], 0);
            } else {
                FAIL("line %u: arg kind must be global|local|scalar, got '%s'", lineno, tok[2]);
            }
            sp->nargs++;
        } else if (strcmp(tok[0], "print") == 0 && nt >= 4) {
            if (sp->nprints >= 16) FAIL("line %u: too many print directives", lineno);
            rs_print *p = &sp->prints[sp->nprints++];
            snprintf(p->arg, sizeof p->arg, "%s", tok[1]);
            p->start = strtoul(tok[2], 0, 0);
            p->count = strtoul(tok[3], 0, 0);
        } else if (strcmp(tok[0], "expect") == 0 && nt >= 4) {
            if (sp->nexpects >= 128) FAIL("line %u: too many expect directives", lineno);
            rs_expect *e = &sp->expects[sp->nexpects++];
            snprintf(e->arg, sizeof e->arg, "%s", tok[1]);
            e->index = strtoul(tok[2], 0, 0);
            e->value = strtod(tok[3], 0);
            e->tol   = nt >= 5 ? strtod(tok[4], 0) : 1e-4;
        } else {
            FAIL("line %u: unknown directive '%s'", lineno, tok[0]);
        }
    }
    fclose(f);

    if (!sp->kernel_path[0]) { snprintf(err, errsz, "spec is missing 'kernel'"); return -1; }
    if (!sp->kernel_name[0]) { snprintf(err, errsz, "spec is missing 'name'"); return -1; }
    return 0;
}
