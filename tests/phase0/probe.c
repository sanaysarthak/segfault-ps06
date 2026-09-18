/* Phase 0 research probe: compile an OpenCL kernel with -g under pocl and run it,
 * so we can inspect what pocl's kernel compiler left behind in its cache. */
#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(*len + 1);
    if (fread(b, 1, *len, f) != *len) { perror("read"); exit(1); }
    b[*len] = 0; fclose(f); return b;
}
#define CK(x) do { cl_int _e = (x); if (_e != CL_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _e); exit(1); } } while (0)

int main(int argc, char **argv) {
    const char *src_path = argc > 1 ? argv[1] : "kernels/vecadd.cl";
    const char *opts     = argc > 2 ? argv[2] : "-g";
    size_t n = argc > 3 ? (size_t)atoi(argv[3]) : 64;
    size_t lws_in = argc > 4 ? (size_t)atoi(argv[4]) : 16;

    cl_platform_id plat; CK(clGetPlatformIDs(1, &plat, NULL));
    cl_device_id dev;    CK(clGetDeviceIDs(plat, CL_DEVICE_TYPE_CPU, 1, &dev, NULL));
    cl_int err;
    cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err); CK(err);
    cl_command_queue q = clCreateCommandQueueWithProperties(ctx, dev, NULL, &err); CK(err);

    size_t slen; char *src = slurp(src_path, &slen);
    cl_program prog = clCreateProgramWithSource(ctx, 1, (const char **)&src, &slen, &err); CK(err);
    err = clBuildProgram(prog, 1, &dev, opts, NULL, NULL);
    if (err != CL_SUCCESS) {
        size_t lsz; clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &lsz);
        char *log = malloc(lsz + 1);
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, lsz, log, NULL);
        log[lsz] = 0;
        fprintf(stderr, "BUILD FAILED (%d):\n%s\n", err, log);
        exit(1);
    }
    printf("build OK with options: '%s'\n", opts);

    cl_kernel k = clCreateKernel(prog, "vecadd", &err); CK(err);
    float *A = malloc(n*4), *B = malloc(n*4), *C = malloc(n*4);
    for (size_t i = 0; i < n; i++) { A[i] = (float)i; B[i] = (float)(i*2); C[i] = -1.f; }
    cl_mem da = clCreateBuffer(ctx, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, n*4, A, &err); CK(err);
    cl_mem db = clCreateBuffer(ctx, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, n*4, B, &err); CK(err);
    cl_mem dc = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, n*4, NULL, &err); CK(err);
    int ni = (int)n;
    CK(clSetKernelArg(k, 0, sizeof(cl_mem), &da));
    CK(clSetKernelArg(k, 1, sizeof(cl_mem), &db));
    CK(clSetKernelArg(k, 2, sizeof(cl_mem), &dc));
    CK(clSetKernelArg(k, 3, sizeof(int), &ni));
    size_t gws = n, lws = lws_in;
    CK(clEnqueueNDRangeKernel(q, k, 1, NULL, &gws, &lws, 0, NULL, NULL));
    CK(clEnqueueReadBuffer(q, dc, CL_TRUE, 0, n*4, C, 0, NULL, NULL));
    CK(clFinish(q));
    printf("C[0]=%g C[1]=%g C[63]=%g (expect 0, 3, 189)\n", C[0], C[1], C[63]);
    return 0;
}
