#include <stdlib.h>
#include <time.h>
#include "kmp.h"
#include "kmp_debug.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>

// Global DRM CPU assignment — written by the master thread when a DRM reply
// arrives, read by every worker thread at the fork barrier so that the entire
// OpenMP team is confined to the DRM-assigned CPU slice.
static std::atomic<uint16_t> g_drm_base_cpu{0};
static std::atomic<uint16_t> g_drm_num_cpus{0};

namespace {

// Reply layout (12 bytes):
//   0-1:  granted  (u16)
//   2-3:  ttl_ms   (u16)
//   4-7:  epoch    (u32)
//   8-9:  base_cpu (u16)  — first CPU of assigned range; 0 if no pinning
//  10-11: num_cpus (u16)  — number of consecutive CPUs; 0 if no pinning
static constexpr size_t REPLY_SIZE = 12;

struct ThreadState {
  int fd = -1;
  int cached_threads = 0;
  uint16_t cached_base_cpu = 0;
  uint16_t cached_num_cpus = 0;
  uint64_t cache_until_ns = 0;
  uint32_t seq = 1;
  bool pending_reply = false; // a request is in flight, reply not yet consumed
  uint8_t rep_buf[REPLY_SIZE] = {};
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

// Restrict the calling thread's CPU affinity to [base_cpu, base_cpu + num_cpus).
// New OpenMP worker threads created afterwards inherit this affinity, so the
// entire team is confined to the DRM-assigned CPU slice without any additional
// per-thread calls.
static void apply_cpu_affinity(uint16_t base_cpu, uint16_t num_cpus) {
  if (num_cpus == 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  for (uint16_t i = 0; i < num_cpus; ++i) {
    CPU_SET(static_cast<int>(base_cpu) + i, &set);
  }
  // pid=0 → current thread; new threads inherit this mask at creation time.
  sched_setaffinity(0, sizeof(set), &set);
}

// Non-blocking: consume bytes from the socket into rep_buf.
// Returns true if a complete reply was received and the cache was updated.
bool try_drain_reply() {
  while (ts.rep_read < REPLY_SIZE) {
    ssize_t r = read(ts.fd, ts.rep_buf + ts.rep_read,
                     REPLY_SIZE - ts.rep_read);
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

  // Full reply received — update cache.
  uint16_t granted, ttl_ms, base_cpu, num_cpus;
  uint32_t epoch;
  std::memcpy(&granted,  ts.rep_buf + 0,  2);
  std::memcpy(&ttl_ms,   ts.rep_buf + 2,  2);
  std::memcpy(&epoch,    ts.rep_buf + 4,  4);
  std::memcpy(&base_cpu, ts.rep_buf + 8,  2);
  std::memcpy(&num_cpus, ts.rep_buf + 10, 2);
  (void)epoch;

  ts.cached_threads  = granted > 0 ? static_cast<int>(granted) : 1;
  ts.cached_base_cpu = base_cpu;
  ts.cached_num_cpus = num_cpus;
  if (ttl_ms == 0) ttl_ms = 5;
  ts.cache_until_ns = now_ns() + uint64_t(ttl_ms) * 1'000'000ull;
  ts.pending_reply = false;
  ts.rep_read = 0;

  // Publish the CPU assignment globally so worker threads can read it at the
  // fork barrier and pin themselves to the same slice as the master.
  g_drm_base_cpu.store(base_cpu, std::memory_order_release);
  g_drm_num_cpus.store(num_cpus, std::memory_order_release);

  // Apply CPU affinity on the master thread immediately.
  apply_cpu_affinity(base_cpu, num_cpus);

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



// Called by every worker thread at the fork barrier (kmp_barrier.cpp) so that
// the full OpenMP team is confined to the DRM-assigned CPU slice, not just the
// master.  Each worker caches the last applied assignment and only issues a
// sched_setaffinity syscall when the DRM assignment actually changes — this
// avoids syscall overhead on every parallel region entry.
extern "C" void __kmp_drm_apply_affinity() {
  uint16_t base = g_drm_base_cpu.load(std::memory_order_acquire);
  uint16_t num  = g_drm_num_cpus.load(std::memory_order_acquire);
  // No real assignment yet (DRM reply not arrived or pool exhausted → 0+0).
  // Skip without caching so the real assignment takes effect when it arrives.
  if (num == 0)
    return;
  // Thread-local cache: skip syscall if assignment unchanged.
  static thread_local uint16_t last_base = UINT16_MAX;
  static thread_local uint16_t last_num  = UINT16_MAX;
  if (base == last_base && num == last_num)
    return;
  last_base = base;
  last_num  = num;
  apply_cpu_affinity(base, num);
  fprintf(stderr, "[DRM-pin] pid=%d tid=%ld assigned=%d-%d running_on=%d\n",
          getpid(), syscall(SYS_gettid),
          (int)base, (int)(base + num - 1),
          sched_getcpu());
}

extern int __kmp_determine_teamsize() {
  KA_TRACE(10, ("__kmp_determine_teamsize: called\n"));
  int dflt = __kmp_dflt_team_nth > 0 ? __kmp_dflt_team_nth : __kmp_avail_proc;
  int max  = __kmp_max_nth > 0 ? __kmp_max_nth : dflt;
  int value = rm_get_granted_threads(dflt, max);
  KA_TRACE(10, ("__kmp_determine_teamsize: returning %d\n", value));

  return value;
}
