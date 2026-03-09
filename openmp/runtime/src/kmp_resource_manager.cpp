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
  bool pending_reply = false; // a request is in flight, reply not yet consumed
  uint8_t rep_buf[8] = {};    // partial reply accumulator
  size_t rep_read = 0;        // bytes read into rep_buf so far
};

thread_local ThreadState ts;

uint64_t now_ns() {
  timespec tp{};
  clock_gettime(CLOCK_MONOTONIC, &tp);
  return uint64_t(tp.tv_sec) * 1'000'000'000ull + tp.tv_nsec;
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

bool connect_once() {
  if (ts.fd != -1) return true;

  ts.fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ts.fd < 0) return false;
  if (!set_cloexec(ts.fd)) {
    close(ts.fd); ts.fd = -1;
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const char* path = "/tmp/omp-rm.sock";
  std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(ts.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(ts.fd); ts.fd = -1;
    return false;
  }
  if (!set_nonblocking(ts.fd)) {
    close(ts.fd); ts.fd = -1;
    return false;
  }
  return true;
}

int clamp(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Non-blocking: consume bytes from the socket into rep_buf.
// Returns true if a complete reply was received and the cache was updated.
bool try_drain_reply() {
  while (ts.rep_read < sizeof(ts.rep_buf)) {
    ssize_t r = read(ts.fd, ts.rep_buf + ts.rep_read,
                     sizeof(ts.rep_buf) - ts.rep_read);
    if (r > 0) {
      ts.rep_read += static_cast<size_t>(r);
    } else if (r == 0) {
      // peer closed
      close(ts.fd); ts.fd = -1;
      ts.pending_reply = false;
      ts.rep_read = 0;
      return false;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return false; // not ready yet, come back next call
      close(ts.fd); ts.fd = -1;
      ts.pending_reply = false;
      ts.rep_read = 0;
      return false;
    }
  }

  // Full 8-byte reply received — update cache
  uint16_t granted, ttl_ms;
  uint32_t epoch;
  std::memcpy(&granted, ts.rep_buf + 0, 2);
  std::memcpy(&ttl_ms,  ts.rep_buf + 2, 2);
  std::memcpy(&epoch,   ts.rep_buf + 4, 4);
  (void)epoch;

  ts.cached_threads = granted > 0 ? static_cast<int>(granted) : 1;
  if (ttl_ms == 0) ttl_ms = 5;
  ts.cache_until_ns = now_ns() + uint64_t(ttl_ms) * 1'000'000ull;
  ts.pending_reply = false;
  ts.rep_read = 0;
  return true;
}

// Non-blocking: write the 16-byte request to the socket in one shot.
// Returns true if the full request was sent.
bool try_send_request(int fallback, int max_threads) {
  uint8_t req[16];
  uint32_t pid   = static_cast<uint32_t>(getpid());
  uint16_t maxu  = static_cast<uint16_t>(clamp(max_threads, 1, 65535));
  uint16_t hint  = static_cast<uint16_t>(clamp(fallback,    1, 65535));
  uint32_t flags = 0;
  uint32_t s     = ts.seq++;
  std::memcpy(req + 0,  &pid,   4);
  std::memcpy(req + 4,  &maxu,  2);
  std::memcpy(req + 6,  &hint,  2);
  std::memcpy(req + 8,  &flags, 4);
  std::memcpy(req + 12, &s,     4);

  ssize_t w = write(ts.fd, req, sizeof(req));
  if (w == static_cast<ssize_t>(sizeof(req)))
    return true;
  if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return false; // send buffer full, skip this cycle
  // partial write or hard error — reset connection
  close(ts.fd); ts.fd = -1;
  ts.pending_reply = false;
  ts.rep_read = 0;
  return false;
}

} // anonymous namespace

int rm_get_granted_threads(int fallback, int max_threads) {
  fallback    = clamp(fallback,    1, max_threads);
  max_threads = clamp(max_threads, 1, max_threads);

  if (!connect_once())
    return fallback;

  // Step 1: non-blocking drain of any in-flight reply
  if (ts.pending_reply)
    try_drain_reply();

  // Step 2: if cache is still fresh, return immediately
  if (ts.cached_threads > 0 && now_ns() < ts.cache_until_ns)
    return clamp(ts.cached_threads, 1, max_threads);

  // Step 3: cache stale — fire a new request if none already in flight
  if (!ts.pending_reply) {
    if (try_send_request(fallback, max_threads))
      ts.pending_reply = true;
  }

  // Step 4: return immediately — stale cache or fallback
  int result = ts.cached_threads > 0 ? ts.cached_threads : fallback;
  return clamp(result, 1, max_threads);
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
  int dflt = __kmp_dflt_team_nth > 0 ? __kmp_dflt_team_nth : __kmp_avail_proc;
  int max  = __kmp_max_nth > 0 ? __kmp_max_nth : dflt;
  int value = rm_get_granted_threads(dflt, max);
  KA_TRACE(10, ("__kmp_determine_teamsize: returning %d\n", value));

  return value;
}
