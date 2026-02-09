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
#include <poll.h>
#include <fcntl.h>
#include <errno.h>

namespace {

struct ThreadState {
  int fd = -1;
  int cached_threads = 0;
  uint64_t cache_until_ns = 0;
  uint32_t seq = 1;
};

thread_local ThreadState ts;

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

bool set_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool wait_fd(int fd, short events, int timeout_ms) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = events;
  for (;;) {
    int r = poll(&pfd, 1, timeout_ms);
    if (r > 0) return true;
    if (r == 0) return false;
    if (errno == EINTR) continue;
    return false;
  }
}



bool connect_once() {
  if (ts.fd != -1) return true;

  ts.fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ts.fd < 0) return false;
  if (!set_cloexec(ts.fd)) {
    close(ts.fd);
    ts.fd = -1;
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const char* path = "/tmp/omp-rm.sock";
  std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(ts.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(ts.fd);
    ts.fd = -1;
    return false;
  }
  if (!set_nonblocking(ts.fd)) {
    close(ts.fd);
    ts.fd = -1;
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
  if (ts.cached_threads > 0 && t < ts.cache_until_ns) {
    return clamp(ts.cached_threads, 1, max_threads);
  }

  if (!connect_once()) {
    return fallback;
  }

  // Request: pid(u32), max(u16), hint(u16), flags(u32), ts.seq(u32)
  uint8_t req[16];
  uint32_t pid = static_cast<uint32_t>(getpid());
  uint16_t maxu = static_cast<uint16_t>(clamp(max_threads, 1, 65535));
  uint16_t hint = static_cast<uint16_t>(clamp(fallback, 1, 65535));
  uint32_t flags = 0;
  uint32_t s = ts.seq++;

  std::memcpy(req + 0,  &pid, 4);
  std::memcpy(req + 4,  &maxu, 2);
  std::memcpy(req + 6,  &hint, 2);
  std::memcpy(req + 8,  &flags, 4);
  std::memcpy(req + 12, &s, 4);

  const int timeout_ms = 2;

  size_t wrote = 0;
  while (wrote < sizeof(req)) {
    const ssize_t w = write(ts.fd, req + wrote, sizeof(req) - wrote);
    if (w < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!wait_fd(ts.fd, POLLOUT, timeout_ms)) {
          return fallback;
        }
        continue;
      }
      close(ts.fd); ts.fd = -1;
      return fallback;
    }
    if (w == 0) {
      close(ts.fd); ts.fd = -1;
      return fallback;
    }
    wrote += static_cast<size_t>(w);
  }

  uint8_t rep[8];
  size_t read_bytes = 0;
  while (read_bytes < sizeof(rep)) {
    const ssize_t r = read(ts.fd, rep + read_bytes, sizeof(rep) - read_bytes);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!wait_fd(ts.fd, POLLIN, timeout_ms)) {
          return fallback;
        }
        continue;
      }
      close(ts.fd); ts.fd = -1;
      return fallback;
    }
    if (r == 0) {
      close(ts.fd); ts.fd = -1;
      return fallback;
    }
    read_bytes += static_cast<size_t>(r);
  }

  uint16_t granted, ttl_ms;
  uint32_t epoch;
  std::memcpy(&granted, rep + 0, 2);
  std::memcpy(&ttl_ms, rep + 2, 2);
  std::memcpy(&epoch,  rep + 4, 4);
  (void)epoch;

  int g = granted > 0 ? granted : 1;
  ts.cached_threads = g;
  if (ttl_ms == 0) ttl_ms = 5;
  ts.cache_until_ns = t + uint64_t(ttl_ms) * 1'000'000ull;

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
