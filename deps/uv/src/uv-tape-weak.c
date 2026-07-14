/*
 * Weak no-op fallbacks for the tape C ABI.
 *
 * libuv is linked into build-time host tools (node_js2c, mkcodecache) that do
 * NOT link Node's src/tape.cc. Without these, uv__hrtime's call to
 * uv_tape_hrtime would be an undefined symbol in those tools. These weak
 * definitions satisfy the linker there; the strong definitions in tape.cc
 * override them in the real node binary.
 *
 * Every function returns "no tape active", so an untaped tool behaves exactly as
 * stock libuv.
 */
#include "uv-tape.h"

UV_TAPE_WEAK int uv_tape_recording(void) { return 0; }
UV_TAPE_WEAK int uv_tape_replaying(void) { return 0; }
UV_TAPE_WEAK uint64_t uv_tape_hrtime(uint64_t real) { return real; }
UV_TAPE_WEAK double uv_tape_clock_millis(double real) { return real; }
UV_TAPE_WEAK void uv_tape_random(void* buf, size_t len, int ret) {
  (void)buf; (void)len; (void)ret;
}
UV_TAPE_WEAK void uv_tape_finish(int exit_status) { (void)exit_status; }
UV_TAPE_WEAK void uv_tape_record_to(const char* path) { (void)path; }
UV_TAPE_WEAK void uv_tape_replay_from(const char* path) { (void)path; }
UV_TAPE_WEAK void uv_tape_inspect(const char* path) { (void)path; }
