#include <omp.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

int main() {
  printf("PID: %d\n", getpid());

  #pragma omp parallel num_threads(16)
  {
    printf("[REGION 1] pthread id: %lu, omp tid: %d/%d\n",
           pthread_self(),
           omp_get_thread_num(),
           omp_get_num_threads());
    sleep(1);
  }

  #pragma omp parallel num_threads(4)
  {
    printf("[REGION 2] pthread id: %lu, omp tid: %d/%d\n",
           pthread_self(),
           omp_get_thread_num(),
           omp_get_num_threads());
    sleep(1);
  }

  #pragma omp parallel num_threads(4)
  {
    printf("[REGION 3] pthread id: %lu, omp tid: %d/%d\n",
           pthread_self(),
           omp_get_thread_num(),
           omp_get_num_threads());
    sleep(1);
  }

  printf("done\n");
}
