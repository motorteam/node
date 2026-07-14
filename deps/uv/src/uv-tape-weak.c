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
UV_TAPE_WEAK void uv_tape_go_live(void) {}
UV_TAPE_WEAK uint64_t uv_tape_hrtime(uint64_t real) { return real; }
UV_TAPE_WEAK double uv_tape_clock_millis(double real) { return real; }
UV_TAPE_WEAK void uv_tape_random(void* buf, size_t len, int ret) {
  (void)buf; (void)len; (void)ret;
}
UV_TAPE_WEAK void uv_tape_finish(int exit_status) { (void)exit_status; }
UV_TAPE_WEAK int uv_tape_submit(struct uv__work* w, int kind, void* req) {
  (void)w; (void)kind; (void)req; return 0;
}
UV_TAPE_WEAK void uv_tape_record_completion(unsigned long long seq, int kind,
    int fs_type, long long result, const void* payload, size_t len) {
  (void)seq; (void)kind; (void)fs_type; (void)result; (void)payload; (void)len;
}
UV_TAPE_WEAK int uv_tape_pump(void) { return 0; }
UV_TAPE_WEAK int uv_tape_has_pending(void) { return 0; }
UV_TAPE_WEAK void uv_tape_fs_sync_record(int a, long long b, const void* c, size_t d) {
  (void)a;(void)b;(void)c;(void)d;
}
UV_TAPE_WEAK void uv_tape_fs_sync_next(int a, long long* b, const void** c, size_t* d) {
  (void)a; if(b)*b=0; if(c)*c=0; if(d)*d=0;
}
UV_TAPE_WEAK void uv_tape_check_write(const void* a, size_t b, const void* c, size_t d) {
  (void)a;(void)b;(void)c;(void)d;
}
UV_TAPE_WEAK int uv_tape_stream_submit(unsigned long long* seq, int kind, void* req) {
  (void)kind; (void)req; if (seq) *seq = 0; return 0;
}
UV_TAPE_WEAK void uv_tape_stream_connect_record(unsigned long long seq, int status) {
  (void)seq; (void)status;
}
UV_TAPE_WEAK void uv_tape_stream_write_record(unsigned long long seq, int status,
    const void* bytes, size_t len) {
  (void)seq; (void)status; (void)bytes; (void)len;
}
UV_TAPE_WEAK unsigned long long uv_tape_stream_read_start(void* stream, int* replay) {
  (void)stream; if (replay) *replay = 0; return 0;
}
UV_TAPE_WEAK void uv_tape_stream_read_stop(void* stream) { (void)stream; }
UV_TAPE_WEAK void uv_tape_stream_read_record(unsigned long long id, long long nread,
    const void* bytes, size_t len) {
  (void)id; (void)nread; (void)bytes; (void)len;
}
UV_TAPE_WEAK void uv_tape_stream_write_sync_record(long long result,
    const void* bytes, size_t len) {
  (void)result; (void)bytes; (void)len;
}
UV_TAPE_WEAK long long uv_tape_stream_write_sync_check(const void* presented, size_t plen) {
  (void)presented; (void)plen; return 0;
}
/* uv__stream_tape_deliver_* are defined in stream.c (always part of libuv), so
 * they never need a weak fallback. */
UV_TAPE_WEAK void uv_tape_record_to(const char* path) { (void)path; }
UV_TAPE_WEAK void uv_tape_replay_from(const char* path) { (void)path; }
UV_TAPE_WEAK void uv_tape_inspect(const char* path) { (void)path; }
