#include <omp.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

void parallelRegion(int id){
    printf("[REGION %d] pthread id: %lu, omp tid: %d/%d\n",
           id,
           pthread_self(),
           omp_get_thread_num(),
           omp_get_num_threads());
    sleep(1);
}

int main() {
  printf("PID: %d\n", getpid());
  #pragma omp parallel
  {
    parallelRegion(1);
  }

  #pragma omp parallel
  {
    parallelRegion(2);

  }

  #pragma omp parallel
  {
    parallelRegion(3);
  }
  printf("done\n");
}
