/*
 * ocl-runner -- the debuggee.
 *
 * A plain, ordinary OpenCL host program: it reads a .ocl-run spec, builds the kernel
 * through pocl, allocates and fills the buffers, launches the NDRange, reads the
 * results back and reports them. It contains no debugger support code whatsoever --
 * oclgdb halts it with ptrace and reads its memory from the outside, exactly as it
 * would for any other unmodified OpenCL host program.
 *
 * Standalone use:   ./ocl-runner demo/blur3.ocl-run
 */
#define CL_TARGET_OPENCL_VERSION 300

#include "runspec.h"

#include <CL/cl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void die(const char *fmt, ...)
{
    va_list ap;
    __builtin_va_start(ap, fmt);
    fprintf(stderr, "ocl-runner: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    __builtin_va_end(ap);
    exit(1);
}

static void ck(cl_int e, const char *what)
{
    if (e != CL_SUCCESS) die("%s failed: OpenCL error %d", what, e);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open '%s'", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) die("short read on '%s'", path);
    buf[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* ------------------------------------------------------------------ buffers */

static void fill(void *mem, const rs_arg *a)
{
    size_t esz = rs_type_size(a->type);
    memset(mem, 0, esz * a->count);
    if (strcmp(a->init, "zero") == 0) return;

    double step = 1.0, base = 0.0;
    int is_iota = 0, is_const = 0;
    if (strncmp(a->init, "iota", 4) == 0) {
        is_iota = 1;
        if (a->init[4] == ':') step = strtod(a->init + 5, 0);
    } else if (strncmp(a->init, "const:", 6) == 0) {
        is_const = 1;
        base = strtod(a->init + 6, 0);
    } else {
        die("unknown buffer initialiser '%s'", a->init);
    }

    for (size_t i = 0; i < a->count; i++) {
        double v = is_const ? base : (is_iota ? (double)i * step : 0.0);
        switch (a->type) {
        case RS_FLOAT:  ((float *)mem)[i]    = (float)v;    break;
        case RS_DOUBLE: ((double *)mem)[i]   = v;           break;
        case RS_CHAR:   ((signed char *)mem)[i] = (signed char)v; break;
        case RS_UCHAR:  ((unsigned char *)mem)[i] = (unsigned char)v; break;
        case RS_SHORT:  ((short *)mem)[i]    = (short)v;    break;
        case RS_USHORT: ((unsigned short *)mem)[i] = (unsigned short)v; break;
        case RS_INT:    ((int *)mem)[i]      = (int)v;      break;
        case RS_UINT:   ((unsigned *)mem)[i] = (unsigned)v; break;
        case RS_LONG:   ((long long *)mem)[i]  = (long long)v; break;
        case RS_ULONG:  ((unsigned long long *)mem)[i] = (unsigned long long)v; break;
        }
    }
}

static double elem(const void *mem, rs_type t, size_t i)
{
    switch (t) {
    case RS_FLOAT:  return ((const float *)mem)[i];
    case RS_DOUBLE: return ((const double *)mem)[i];
    case RS_CHAR:   return ((const signed char *)mem)[i];
    case RS_UCHAR:  return ((const unsigned char *)mem)[i];
    case RS_SHORT:  return ((const short *)mem)[i];
    case RS_USHORT: return ((const unsigned short *)mem)[i];
    case RS_INT:    return ((const int *)mem)[i];
    case RS_UINT:   return ((const unsigned *)mem)[i];
    case RS_LONG:   return (double)((const long long *)mem)[i];
    default:        return (double)((const unsigned long long *)mem)[i];
    }
}

/* --------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: ocl-runner <spec.ocl-run>\n");
        return 2;
    }

    rs_spec sp;
    char err[512];
    if (rs_parse(argv[1], &sp, err, sizeof err) != 0) die("%s", err);

    cl_platform_id plat;
    cl_uint nplat = 0;
    ck(clGetPlatformIDs(1, &plat, &nplat), "clGetPlatformIDs");
    if (nplat == 0) die("no OpenCL platform found");

    cl_device_id dev;
    ck(clGetDeviceIDs(plat, CL_DEVICE_TYPE_CPU, 1, &dev, NULL), "clGetDeviceIDs");

    char devname[256] = {0};
    clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof devname, devname, NULL);

    cl_int e;
    cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &e);
    ck(e, "clCreateContext");
    cl_command_queue q = clCreateCommandQueueWithProperties(ctx, dev, NULL, &e);
    ck(e, "clCreateCommandQueue");

    size_t srclen;
    char *src = slurp(sp.kernel_path, &srclen);
    cl_program prog = clCreateProgramWithSource(ctx, 1, (const char **)&src, &srclen, &e);
    ck(e, "clCreateProgramWithSource");

    e = clBuildProgram(prog, 1, &dev, sp.build_options, NULL, NULL);
    if (e != CL_SUCCESS) {
        size_t lsz = 0;
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &lsz);
        char *log = malloc(lsz + 1);
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, lsz, log, NULL);
        log[lsz] = 0;
        fprintf(stderr, "ocl-runner: kernel build failed (%d):\n%s\n", e, log);
        return 1;
    }

    cl_kernel k = clCreateKernel(prog, sp.kernel_name, &e);
    ck(e, "clCreateKernel");

    /* Host mirrors so we can initialise inputs and read outputs back. */
    void   *host[32] = {0};
    cl_mem  dbuf[32] = {0};

    for (unsigned i = 0; i < sp.nargs; i++) {
        rs_arg *a = &sp.args[i];
        size_t esz = rs_type_size(a->type);
        switch (a->kind) {
        case RS_GLOBAL: {
            size_t bytes = esz * a->count;
            host[i] = malloc(bytes);
            fill(host[i], a);
            cl_mem_flags flags = a->dir == RS_OUT ? CL_MEM_WRITE_ONLY
                               : a->dir == RS_IN  ? CL_MEM_READ_ONLY
                                                  : CL_MEM_READ_WRITE;
            dbuf[i] = clCreateBuffer(ctx, flags | CL_MEM_COPY_HOST_PTR, bytes, host[i], &e);
            ck(e, "clCreateBuffer");
            ck(clSetKernelArg(k, i, sizeof(cl_mem), &dbuf[i]), "clSetKernelArg(global)");
            break;
        }
        case RS_LOCAL:
            ck(clSetKernelArg(k, i, esz * a->count, NULL), "clSetKernelArg(local)");
            break;
        case RS_SCALAR: {
            unsigned char raw[8] = {0};
            double v = a->scalar;
            switch (a->type) {
            case RS_FLOAT:  { float  t = (float)v;  memcpy(raw, &t, 4); break; }
            case RS_DOUBLE: { double t = v;         memcpy(raw, &t, 8); break; }
            case RS_CHAR: case RS_UCHAR:   { char t = (char)v; memcpy(raw, &t, 1); break; }
            case RS_SHORT: case RS_USHORT: { short t = (short)v; memcpy(raw, &t, 2); break; }
            case RS_INT: case RS_UINT:     { int t = (int)v; memcpy(raw, &t, 4); break; }
            default: { long long t = (long long)v; memcpy(raw, &t, 8); break; }
            }
            ck(clSetKernelArg(k, i, esz, raw), "clSetKernelArg(scalar)");
            break;
        }
        }
    }

    printf("ocl-runner: device '%s'\n", devname);
    printf("ocl-runner: launching %s<<<global=(%zu,%zu,%zu) local=(%zu,%zu,%zu)>>>\n",
           sp.kernel_name, sp.global[0], sp.global[1], sp.global[2],
           sp.local[0], sp.local[1], sp.local[2]);
    fflush(stdout);

    e = clEnqueueNDRangeKernel(q, k, sp.work_dim, NULL, sp.global, sp.local, 0, NULL, NULL);
    ck(e, "clEnqueueNDRangeKernel");
    ck(clFinish(q), "clFinish");

    for (unsigned i = 0; i < sp.nargs; i++) {
        rs_arg *a = &sp.args[i];
        if (a->kind == RS_GLOBAL && (a->dir & RS_OUT))
            ck(clEnqueueReadBuffer(q, dbuf[i], CL_TRUE, 0,
                                   rs_type_size(a->type) * a->count, host[i], 0, NULL, NULL),
               "clEnqueueReadBuffer");
    }
    ck(clFinish(q), "clFinish");

    for (unsigned p = 0; p < sp.nprints; p++) {
        rs_print *pr = &sp.prints[p];
        int found = -1;
        for (unsigned i = 0; i < sp.nargs; i++)
            if (strcmp(sp.args[i].name, pr->arg) == 0) found = (int)i;
        if (found < 0 || !host[found]) { printf("  (no such buffer '%s')\n", pr->arg); continue; }
        printf("  %s[%zu..%zu] =", pr->arg, pr->start, pr->start + pr->count - 1);
        for (size_t j = 0; j < pr->count && pr->start + j < sp.args[found].count; j++)
            printf(" %g", elem(host[found], sp.args[found].type, pr->start + j));
        printf("\n");
    }

    unsigned bad = 0;
    for (unsigned x = 0; x < sp.nexpects; x++) {
        rs_expect *ex = &sp.expects[x];
        int found = -1;
        for (unsigned i = 0; i < sp.nargs; i++)
            if (strcmp(sp.args[i].name, ex->arg) == 0) found = (int)i;
        if (found < 0 || !host[found]) continue;
        double got = elem(host[found], sp.args[found].type, ex->index);
        if (fabs(got - ex->value) > ex->tol) {
            printf("  MISMATCH %s[%zu] = %g, expected %g\n", ex->arg, ex->index, got, ex->value);
            bad++;
        }
    }
    if (sp.nexpects) {
        if (bad) printf("ocl-runner: VERIFICATION FAILED -- %u of %u checked elements wrong\n",
                        bad, sp.nexpects);
        else     printf("ocl-runner: verification passed (%u elements)\n", sp.nexpects);
    }

    fflush(stdout);
    return bad ? 3 : 0;
}
