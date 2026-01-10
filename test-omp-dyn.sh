#!/bin/bash
# export OMP_NUM_THREADS=4
export OMP_PLACES=cores
export OMP_PROC_BIND=spread
export OMP_DISPLAY_AFFINITY=1
export OMP_DYNAMIC=true

export KMP_A_DEBUG=20
export KMP_F_DEBUG=20
export KMP_DEBUG=1
export KMP_SETTINGS=1
export KMP_AFFINITY=verbose

#export TCM_ENABLE=1
#export TCM_VERSION=1
#cmake -S llvm -B build -G Ninja  -DLLVM_ENABLE_PROJECTS="clang;openmp"
ninja -C build -j 4


USE_INTEL_OPENMP=false

if [ "$USE_INTEL_OPENMP" = true ]; then
  echo "Using Intel oneAPI OpenMP runtime"
  export LD_LIBRARY_PATH=/opt/intel/oneapi/compiler/latest/lib:$LD_LIBRARY_PATH
  clang -fopenmp=libiomp5 \
  -I./build/projects/openmp/runtime/src \
  -L/opt/intel/oneapi/compiler/latest/lib \
  -Wl,-rpath,/opt/intel/oneapi/compiler/latest/lib \
  -liomp5 \
  omp_dyn.c -o omp
else
  echo "Using LLVM OpenMP runtime"
  ./build/bin/clang -fopenmp   -I./build/projects/openmp/runtime/src   omp_dyn.c -o omp   -Wl,-rpath,./build/lib
fi

./omp 2>&1 | tee omp_debug_dyn.log
