#include <stdlib.h>
#include <time.h>
#include "kmp.h"
#include "kmp_debug.h"

extern int __kmp_determine_teamsize() {
  static int value;
  static int initialized = 0;
  KA_TRACE(10, ("__kmp_determine_teamsize: called\n"));
  if (!initialized) {
    srand(time(NULL));
    initialized = 1;
  }

  // zufällige Teamgröße zwischen 1 und 8
  value = (rand() % 16) + 1;
  KA_TRACE(10, ("__kmp_determine_teamsize: returning %d\n", value));
  return value;
}