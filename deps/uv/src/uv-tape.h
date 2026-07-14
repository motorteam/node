#ifndef UV_TAPE_H_
#define UV_TAPE_H_

/*
 * Deterministic record/replay of a Node program's observable behavior.
 *
 * The tape is a log of every crossing of the program->host boundary and nothing
 * else. Replay re-runs the program but serves each effect from the tape instead
 * of calling libc, so a recorded run reproduces exactly.
 *
 * This header lives inside libuv deliberately: libuv is C and Node is C++, and
 * the effect chokepoints are in libuv (uv__hrtime, uv__fs_work, uv__work_done).
 * Keeping the declarations here means libuv's .c files can include them without
 * any include-path surgery in uv.gyp; the definitions live in Node's src/tape.cc
 * and resolve at the final link.
 *
 * See NODEJS.md for the design, and RUBY.md for the wire format (which this
 * shares byte for byte with the Ruby and Python ports, and with Watt).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host build-time tools (node_js2c, mkcodecache) link libuv but not tape.cc.
 * Weak fallbacks let those tools resolve the tape ABI to no-ops; the strong
 * definitions in src/tape.cc win at the final node link. UV_TAPE_WEAK marks the
 * fallbacks, which live in a single .c file (uv-tape-weak.c).
 */
#if defined(__GNUC__) || defined(__clang__)
# define UV_TAPE_WEAK __attribute__((weak))
#else
# define UV_TAPE_WEAK
#endif

/*
 * The effect table -- Node's equivalent of Watt's kernel externals. The index is
 * the `func_index` written to the tape, so this is **append only**: reordering
 * invalidates every existing tape.
 *
 * 0-6 match the Ruby and Python ports, so a tape from any of the three has the
 * same shape at the head.
 */
enum uv_tape_effect {
  UV_TAPE_CLOCK_REALTIME  = 0,   /* Date.now() */
  UV_TAPE_CLOCK_MONOTONIC = 1,   /* uv_hrtime: the loop clock, performance.now,
                                  * process.hrtime, process.uptime */
  UV_TAPE_RANDOM_BYTES    = 2,   /* uv_random -> crypto.randomBytes */
  UV_TAPE_IO_READ         = 3,
  UV_TAPE_IO_WRITE        = 4,
  UV_TAPE_KERNEL_HALT     = 5,
  UV_TAPE_KERNEL_ABORT    = 6,
  /* One async completion delivered by the loop -- the schedule. Carries the
   * request seq, its fs_type, the result, and any payload (statbuf or bytes). */
  UV_TAPE_FS_DONE         = 7,
  UV_TAPE_EFFECT_MAX
};

/* Outcome tag for a recorded entry. Mirrors Watt's `Action`. */
enum uv_tape_action {
  UV_TAPE_ACTION_HALT   = 0,
  UV_TAPE_ACTION_ABORT  = 1,
  UV_TAPE_ACTION_RESUME = 2
};

int uv_tape_recording(void);
int uv_tape_replaying(void);

/* True if a tape is active at all. The chokepoints test this first, so an
 * untaped run pays one predictable branch and nothing else. */
#define UV_TAPE_ACTIVE() (uv_tape_recording() || uv_tape_replaying())

/* ---- Per-effect record/replay pairs --------------------------------------
 * Each chokepoint calls exactly one of these, so the patch at the call site
 * stays two or three lines and the marshalling lives in tape.cc.
 */

/*
 * The monotonic clock -- uv__hrtime.
 *
 * This is the whole timer story. libuv's timer heap is ordered by
 * (timeout, start_id) against `loop->time`, not wall time (timer.c:37-54, :78),
 * and loop->time comes from here. So pinning this clock makes setTimeout
 * ordering fall out for free: the timer subsystem needs no tape entries at all.
 */
uint64_t uv_tape_hrtime(uint64_t real);

/* The wall clock -- Date.now(), via NodePlatform::CurrentClockTimeMillis. */
double uv_tape_clock_millis(double real);

/* Entropy -- uv_random, and therefore crypto.randomBytes. */
void uv_tape_random(void* buf, size_t len, int ret);

/* ---- The async schedule --------------------------------------------------
 *
 * This is the part with no analogue in the Ruby or Python ports, because there
 * the effect boundary was a synchronous call that returned. In Node it is a
 * callback that fires -- and out of order, because libuv's thread pool completes
 * work as workers finish, not as the program issued it.
 *
 * Initiation order is deterministic (JS is single-threaded), so we stamp a
 * sequence number on each request at submit. The tape then records completions
 * in *delivery* order, each tagged with its seq. On replay the thread pool never
 * runs: uv_tape_submit registers the request as pending instead of posting it,
 * and the pump (driven from uv_run in place of uv__io_poll) delivers the next
 * recorded completion to the matching pending request. See NODEJS.md.
 */
struct uv__work;

/* Effect kinds for the thread-pool schedule. Append only. */
enum uv_tape_pool_kind {
  UV_TAPE_POOL_FS = 1,
};

/*
 * At uv__work_submit. Stamps w->tape_seq. Returns 1 if replaying -- the caller
 * must NOT post to the thread pool; the completion is served by the pump.
 * Returns 0 on record or with no tape: caller posts normally.
 */
int uv_tape_submit(struct uv__work* w, int kind, void* req);

/*
 * At an effect's completion (its `done` wrapper), on record, on the loop thread.
 * `result` is the syscall result; `payload`/`len` is the effect's output bytes
 * (a statbuf, or the bytes read; empty for open/close).
 */
void uv_tape_record_completion(unsigned long long seq, int kind, int fs_type,
                               long long result, const void* payload, size_t len);

/*
 * Deliver the next recorded completion: find the pending request by seq, fill it
 * from the tape (via uv__fs_tape_fill for fs), and call its done. Returns 1 if
 * one was delivered, 0 if none is pending. Called from uv_run under replay.
 */
int uv_tape_pump(void);

/* True while replay has pending completions -- keeps the loop alive. */
int uv_tape_has_pending(void);

/* Defined in fs.c: fill a replayed uv_fs_t from tape bytes before its done runs. */
void uv__fs_tape_fill(void* req, int fs_type, long long result,
                      const void* payload, size_t len);

/* Seal the tape. Called from Node once the program has finished. */
void uv_tape_finish(int exit_status);

/* Node-side entry points, called from node_main_instance.cc. */
void uv_tape_record_to(const char* path);
void uv_tape_replay_from(const char* path);
void uv_tape_inspect(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* UV_TAPE_H_ */
