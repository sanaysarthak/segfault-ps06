/* Parser for .ocl-run specs -- the description of one OpenCL launch.
 *
 * The same parser is compiled into both `ocl-runner` (which executes the launch)
 * and `oclgdb` (which needs to know the NDRange, the build options and the argument
 * layout before it ever starts the inferior). Keeping it in C keeps it shareable.
 */
#ifndef OCLGDB_RUNSPEC_H
#define OCLGDB_RUNSPEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { RS_GLOBAL, RS_LOCAL, RS_SCALAR } rs_arg_kind;

typedef enum {
    RS_FLOAT, RS_DOUBLE, RS_CHAR, RS_UCHAR, RS_SHORT, RS_USHORT,
    RS_INT, RS_UINT, RS_LONG, RS_ULONG
} rs_type;

typedef enum { RS_IN = 1, RS_OUT = 2, RS_INOUT = 3 } rs_dir;

typedef struct {
    char        name[64];
    rs_arg_kind kind;
    rs_type     type;
    size_t      count;        /* elements, for RS_GLOBAL / RS_LOCAL */
    rs_dir      dir;          /* for RS_GLOBAL */
    char        init[64];     /* zero | iota | iota:<step> | const:<v> | ramp:<a>:<b> */
    double      scalar;       /* for RS_SCALAR */
} rs_arg;

typedef struct { char arg[64]; size_t start, count; } rs_print;
typedef struct { char arg[64]; size_t index; double value, tol; } rs_expect;

typedef struct {
    char     spec_path[512];
    char     kernel_path[512];
    char     kernel_name[128];
    char     build_options[512];
    unsigned work_dim;
    size_t   global[3];
    size_t   local[3];

    rs_arg   args[32];
    unsigned nargs;

    rs_print  prints[16];
    unsigned  nprints;
    rs_expect expects[128];
    unsigned  nexpects;
} rs_spec;

/* Returns 0 on success; on failure returns -1 and fills `err`. */
int  rs_parse(const char *path, rs_spec *out, char *err, size_t errsz);
size_t rs_type_size(rs_type t);
const char *rs_type_name(rs_type t);

#ifdef __cplusplus
}
#endif
#endif
