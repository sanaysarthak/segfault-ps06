__kernel void vecadd(__global const float *a,
                     __global const float *b,
                     __global float *c,
                     const int n)
{
    int gid = get_global_id(0);
    int lid = get_local_id(0);
    float av = a[gid];
    float bv = b[gid];
    float sum = av + bv;
    c[gid] = sum;
}
