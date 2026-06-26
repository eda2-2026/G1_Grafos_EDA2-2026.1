# SeedForge Makefile
# CPU path needs only gcc; GPU path needs nvcc + the CUDA toolkit.

CUBIOMES = cubiomes
INC      = -I$(CUBIOMES)
CFLAGS   = -O3 -fwrapv -Wall
OMP      = -fopenmp

# GPU arches: sm_120 = RTX 5060 Ti (Blackwell, requires CUDA 12.5+ for native),
# sm_86 = RTX 3060 (Ampere). compute_89,code=compute_89 embeds PTX for
# forward-compat JIT on any arch >= 8.9 (covers sm_120 with newer driver).
GENCODE  = -gencode arch=compute_86,code=sm_86 \
           -gencode arch=compute_89,code=compute_89
NVCC     = nvcc
NVFLAGS  = -O3 -DSF_DEVICE -Xcompiler -fwrapv $(GENCODE)

.PHONY: all cpu gpu lib clean

all: cpu

lib:
	$(MAKE) -C $(CUBIOMES) libcubiomes

cpu: lib
	gcc $(CFLAGS) $(OMP) src/run_cpu.c src/config.c \
	    $(CUBIOMES)/libcubiomes.a $(INC) -lm -lpthread -o seedforge_cpu

# Compile cubiomes sources with nvcc so generation math gets device codegen,
# then link with the kernel. -rdc=true allows __device__ calls across TUs.
# -x cu forces CUDA compilation for .c files so CBHD functions get device code.
gpu:
	$(NVCC) $(NVFLAGS) -rdc=true -x cu $(INC) \
	    src/run_gpu.cu src/config.c \
	    $(CUBIOMES)/noise.c $(CUBIOMES)/biomes.c $(CUBIOMES)/layers.c \
	    $(CUBIOMES)/biomenoise.c $(CUBIOMES)/generator.c \
	    $(CUBIOMES)/finders.c $(CUBIOMES)/util.c $(CUBIOMES)/quadbase.c \
	    -lpthread -o seedforge_gpu

clean:
	rm -f seedforge_cpu seedforge_gpu
	$(MAKE) -C $(CUBIOMES) clean
