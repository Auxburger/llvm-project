#!/bin/bash
export OMP_PLACES=threads
export OMP_PROC_BIND=spread
export OMP_DISPLAY_AFFINITY=true
export KMP_AFFINITY=verbose
export KMP_DEBUG=1
export TCM_ENABLE=1
export TCM_VERSION=1
export KMP_A_DEBUG=10
#cmake -S llvm -B build -G Ninja  -DLLVM_ENABLE_PROJECTS="clang;openmp"
#ninja -C build -j 24


USE_INTEL_OPENMP=true

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

./omp &
./omp &