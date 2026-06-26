/* cuda_compat.h — CBHD macro for __host__ __device__ annotation.
 * Include before other cubiomes headers when building with nvcc.
 * Plain C builds: CBHD expands to nothing.
 */
#pragma once
#ifdef __CUDACC__
#  define CBHD __host__ __device__
#else
#  define CBHD
#endif
