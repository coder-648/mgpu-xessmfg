// lpbench: what does a producer-side GPU codec cost on a 3080 Ti?
// Isolates the *cost* side of "replace downsample/upsample with lossless
// compression": read N bytes, optionally write N/r. All kernels are
// data-independent in cost (fixed-rate streaming), so timings transfer;
// only the ratio needs real dumps.
#include <cstdio>
#include <cuda_runtime.h>
#include <cstdint>

#define CK(x) do { cudaError_t e = (x); if (e) { printf("CUDA %s @ %d\n", cudaGetErrorString(e), __LINE__); return 1; } } while (0)
#define CKN(x) do { cudaError_t e_ = (x); if (e_) printf("CUDA %s @ %d\n", cudaGetErrorString(e_), __LINE__); } while (0)

__global__ void k_copy(const uint4* __restrict__ in, uint4* __restrict__ out, size_t n4) {
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t s = (size_t)gridDim.x * blockDim.x;
    for (; i < n4; i += s) out[i] = in[i];
}

// streaming transform: read full, write half (bandwidth floor of any 2:1 codec)
__global__ void k_pack2x(const uint4* __restrict__ in, uint2* __restrict__ out, size_t n4) {
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t s = (size_t)gridDim.x * blockDim.x;
    for (; i < n4; i += s) {
        uint4 v = in[i];
        uint32_t a = v.x ^ (v.x >> 13);
        uint32_t b = v.y ^ (v.y >> 13);
        uint32_t c = v.z ^ (v.z >> 13);
        uint32_t d = v.w ^ (v.w >> 13);
        out[i] = make_uint2((a & 0xffff0000u) | (b >> 16), (c & 0xffff0000u) | (d >> 16));
    }
}

__global__ void k_unpack2x(const uint2* __restrict__ in, uint4* __restrict__ out, size_t n4) {
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t s = (size_t)gridDim.x * blockDim.x;
    for (; i < n4; i += s) {
        uint2 v = in[i];
        out[i] = make_uint4(v.x, v.y, v.x << 16, v.y << 16);
    }
}

// the pass every entropy coder needs: per-tile bit-length scan (variable length)
__global__ void k_tile_maxbits(const uint32_t* __restrict__ in, uint32_t* __restrict__ out, int tile) {
    extern __shared__ uint32_t sm[];
    size_t t = (size_t)blockIdx.x;
    size_t base = t * (size_t)tile;
    uint32_t m = 0;
    for (int k = threadIdx.x; k < tile; k += blockDim.x) m |= in[base + k];
    sm[threadIdx.x] = m;
    __syncthreads();
    for (int st = blockDim.x / 2; st > 0; st >>= 1) {
        if (threadIdx.x < (unsigned)st) sm[threadIdx.x] |= sm[threadIdx.x + st];
        __syncthreads();
    }
    if (threadIdx.x == 0) out[t] = sm[0] ? (32u - __clz(sm[0])) : 0u;
}

// what the producer does today instead: 9-tap medoid/median downsample
// src 3840x2160 u32-pairs -> dst 2560x1440 (StreamPercent=150)
__device__ __forceinline__ uint32_t med9(const uint32_t* s, int sw, int x, int y) {
    uint32_t v[9];
    int i = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            int px = min(max(x + dx, 0), sw - 1);
            int py = min(max(y + dy, 0), 2159);
            v[i++] = s[(size_t)py * sw + px];
        }
    // 9-element median network is overkill; insertion sort, 20-30 ALU ops (as in the doc)
    for (int a = 1; a < 9; ++a) {
        uint32_t key = v[a]; int b = a - 1;
        while (b >= 0 && v[b] > key) { v[b + 1] = v[b]; --b; }
        v[b + 1] = key;
    }
    return v[4];
}
__global__ void k_downsample9(const uint32_t* __restrict__ in, uint32_t* __restrict__ out, int dw, int dh) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dw || y >= dh) return;
    int sx = (int)((x + 0.5f) * (3840.0f / dw));
    int sy = (int)((y + 0.5f) * (2160.0f / dh));
    out[(size_t)y * dw + x] = med9(in, 3840, sx, sy);
}

static float ms_of(cudaEvent_t a, cudaEvent_t b) { float m = 0; cudaEventElapsedTime(&m, a, b); return m; }

int main() { setvbuf(stdout, NULL, _IONBF, 0);
    cudaDeviceProp prop; CK(cudaGetDeviceProperties(&prop, 0));
    printf("device: %s  SMs=%d  sm_clock=%.0f MHz  bus_bw=%.0f GB/s  sm_%d%d\n",
           prop.name, prop.multiProcessorCount, prop.clockRate / 1000.0,
           2.0 * prop.memoryClockRate * (prop.memoryBusWidth / 8) / 1.0e6,
           prop.major, prop.minor);

    uint32_t *d_in, *d_out, *d_bits;

    const size_t nwords = 3840ull * 2160ull * 2ull;   // 4K RGBA16F / R32 = 66.4 MB
    CK(cudaMalloc(&d_in, nwords * 4));
    CK(cudaMalloc(&d_out, nwords * 4));
    CK(cudaMalloc(&d_bits, (nwords / 256 + 1) * 4));
    CK(cudaMemset(d_in, 0x3c, nwords * 4));

    cudaEvent_t e0, e1; CK(cudaEventCreate(&e0)); CK(cudaEventCreate(&e1));
    const int REP = 50, blocks = prop.multiProcessorCount * 32;
    const double bytes = (double)nwords * 4.0;

    auto bench = [&](const char* name, double moved, auto launch) -> void {
        launch(); CKN(cudaDeviceSynchronize());
        CKN(cudaEventRecord(e0));
        for (int i = 0; i < REP; ++i) launch();
        CKN(cudaEventRecord(e1)); CKN(cudaDeviceSynchronize());
        float ms = ms_of(e0, e1) / REP;
        printf("%-34s %8.3f ms   %7.1f GB/s effective\n", name, ms, moved / (ms * 1e6));
    };

    bench("copy 66MB -> 66MB (plain copy)", bytes * 2, [&] {
        k_copy<<<blocks, 256>>>((const uint4*)d_in, (uint4*)d_out, nwords / 4); });
    bench("pack 2:1  66MB -> 33MB", bytes * 1.5, [&] {
        k_pack2x<<<blocks, 256>>>((const uint4*)d_in, (uint2*)d_out, nwords / 4); });
    bench("unpack 2:1 33MB -> 66MB", bytes * 1.5, [&] {
        k_unpack2x<<<blocks, 256>>>((const uint2*)d_in, (uint4*)d_out, nwords / 4); });
    bench("tile bit-width scan (64k tiles)", bytes * 1.0, [&] {
        k_tile_maxbits<<<(unsigned)(nwords / 256), 256, 256 * 4>>>(d_in, d_bits, 256); });
    // today's producer work for one stream: 4K -> 150% (2560x1440) 9-tap median
    bench("downsample 4K->150% 9-tap median", bytes * 1.25, [&] {
        dim3 b(32, 8);
        dim3 g((2560 + 31) / 32, (1440 + 7) / 8);
        k_downsample9<<<g, b>>>(d_in, d_out, 2560, 1440); });
    return 0;
}
