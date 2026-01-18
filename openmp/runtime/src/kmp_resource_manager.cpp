#include <stdlib.h>
#include <time.h>
#include "kmp.h"
#include "kmp_debug.h"
#include <cstdint>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>

namespace {

int rm_fd = -1;
int cached_threads = 0;
uint64_t cache_until_ns = 0;
uint32_t seq = 1;

uint64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1'000'000'000ull + ts.tv_nsec;
}

bool set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool connect_once() {
  if (rm_fd != -1) return true;

  rm_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (rm_fd < 0) return false;

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const char* path = "/tmp/omp-rm.sock";
  std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(rm_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(rm_fd);
    rm_fd = -1;
    return false;
  }
  return true;
}


int clamp(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

} // anonymous namespace

int rm_get_granted_threads(int fallback, int max_threads) {
  fallback = clamp(fallback, 1, max_threads);
  max_threads = clamp(max_threads, 1, max_threads);

  const uint64_t t = now_ns();
  if (cached_threads > 0 && t < cache_until_ns) {
    return clamp(cached_threads, 1, max_threads);
  }

  if (!connect_once()) {
    return fallback;
  }

  // Request: pid(u32), max(u16), hint(u16), flags(u32), seq(u32)
  uint8_t req[16];
  uint32_t pid = static_cast<uint32_t>(getpid());
  uint16_t maxu = static_cast<uint16_t>(clamp(max_threads, 1, 65535));
  uint16_t hint = static_cast<uint16_t>(clamp(fallback, 1, 65535));
  uint32_t flags = 0;
  uint32_t s = seq++;

  std::memcpy(req + 0,  &pid, 4);
  std::memcpy(req + 4,  &maxu, 2);
  std::memcpy(req + 6,  &hint, 2);
  std::memcpy(req + 8,  &flags, 4);
  std::memcpy(req + 12, &s, 4);

  if (write(rm_fd, req, sizeof(req)) != sizeof(req)) {
    close(rm_fd); rm_fd = -1;
    return fallback;
  }

  uint8_t rep[8];
  const ssize_t r = read(rm_fd, rep, sizeof(rep));
  if (r != sizeof(rep)) {
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return fallback;
    }
    close(rm_fd); rm_fd = -1;
    return fallback;
  }

  uint16_t granted, ttl_ms;
  uint32_t epoch;
  std::memcpy(&granted, rep + 0, 2);
  std::memcpy(&ttl_ms, rep + 2, 2);
  std::memcpy(&epoch,  rep + 4, 4);
  (void)epoch;

  int g = granted > 0 ? granted : 1;
  cached_threads = g;
  if (ttl_ms == 0) ttl_ms = 5;
  cache_until_ns = t + uint64_t(ttl_ms) * 1'000'000ull;

  return clamp(g, 1, max_threads);
}



extern int __kmp_determine_teamsize() {
  /* static int value;
  static int initialized = 0;
  KA_TRACE(10, ("__kmp_determine_teamsize: called\n"));
  if (!initialized) {
    srand(time(NULL));
    initialized = 1;
  }

  // zufällige Teamgröße zwischen 1 und 8
  value = (rand() % 16) + 1;
  KA_TRACE(10, ("__kmp_determine_teamsize: returning %d\n", value));
  return value; */
  KA_TRACE(10, ("__kmp_determine_teamsize: called\n"));
  int value = rm_get_granted_threads(2, 16);
  KA_TRACE(10, ("__kmp_determine_teamsize: returning %d\n", value));

  return value;
}
