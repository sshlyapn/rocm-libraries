// gfx1100 (RDNA3) compute shader for the PM4 inter-kernel gap microbench.
// ABI: COMPUTE_USER_DATA_0/1 (s0:s1) hold the 64-bit address of a single
// dword counter. The shader does data[0] = data[0] + 1 with L2-coherent
// (glc) flat access, so a chain of dependent dispatches must serialize:
// dispatch N+1 must observe dispatch N's increment. After K dispatches the
// counter must equal K, which validates that the per-dispatch fence really
// orders the chain.
.text
.p2align 8
.globl gap_kernel
gap_kernel:
        v_mov_b32 v0, s0
        v_mov_b32 v1, s1
        flat_load_b32 v2, v[0:1] glc slc
        s_waitcnt 0
        v_add_nc_u32 v2, v2, 1
        flat_store_b32 v[0:1], v2 glc slc
        s_waitcnt 0
        s_endpgm
