/*
 * 3-point box blur over a 1-D signal, staged through a local-memory tile.
 *
 * This is the flagship demo kernel and it contains a real, deliberate bug.
 * See README.md ("The bug") for the diagnosis walkthrough.
 */
__kernel void blur3(__global const float *in,
                    __global float *out,
                    __local float *tile,
                    const int n)
{
    int gid = get_global_id(0);
    int lid = get_local_id(0);
    int lsz = get_local_size(0);

    /* Every work-item stages its own element into the interior of the tile. */
    tile[lid + 1] = in[gid];

    /* The two edge work-items additionally stage the halo elements, clamping
     * at the ends of the global signal so we never read outside `in`. */
    if (lid == 0) {
        int left_src = (gid == 0) ? 0 : gid - 1;
        tile[0] = in[left_src];
    }
    if (lid == lsz - 1) {
        int right_src = (gid == n) ? n - 1 : gid + 1;   /* BUG: guard should be (gid == n - 1) */
        tile[lsz + 1] = in[right_src];
    }

    barrier(CLK_LOCAL_MEM_FENCE);

    float l = tile[lid];
    float c = tile[lid + 1];
    float r = tile[lid + 2];
    out[gid] = (l + c + r) / 3.0f;
}
