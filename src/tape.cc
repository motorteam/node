/*
 * tape.cc - deterministic record/replay of observable behavior
 *
 * See deps/uv/src/uv-tape.h for the model, NODEJS.md for the design, RUBY.md for
 * the wire format (shared byte for byte with the Ruby and Python ports and Watt):
 *
 *     byte 0   : format version (2)
 *     bytes 1..: zstd( postcard( SpooledTape ) )
 *
 * postcard is non-self-describing: fields emit in declaration order, integers
 * are LEB128 varints (zigzag for signed), Vec/String are varint-length-prefixed,
 * Option is 0x00 or 0x01 ++ payload.
 *
 * The effect chokepoints are in libuv (C), so the interface in uv-tape.h is a C
 * ABI; the implementation here is C++ but touches no V8 -- it runs from inside
 * the libuv loop, where the isolate may not be in a callable state.
 *
 * This first cut covers the clock, entropy and lifecycle. The async schedule --
 * recording which request completes next -- is the genuinely new part and lands
 * next; see NODEJS.md.
 */

#include "../deps/uv/src/uv-tape.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include <zstd.h>

namespace {

constexpr uint8_t kFormatVersion = 2;    // matches Watt's CURRENT_SPOOL_FORMAT
constexpr int kZstdLevel = 3;

// Bytes a tape may reach before it is dropped whole. A tape over this can't be
// partially kept -- a truncated observation diverges on replay. Matches Watt.
constexpr size_t kCeilingBytes = 64u * 1024 * 1024;

const char* const kEffectFqn[] = {
    "clock.realtime", "clock.monotonic", "random.bytes",
    "io.read", "io.write", "kernel.halt", "kernel.abort",
};

const char* const kEffectSig[] = {
    "() -> int",                 // clock.realtime  -- Date.now millis
    "() -> int",                 // clock.monotonic -- hrtime nanos
    "(size, [[byte]]) -> int",   // random.bytes    -- scatter: the filled buffer
    "(int, [[byte]]) -> int",    // io.read
    "(int, [[byte]]) -> int",    // io.write
    "() -> never",               // kernel.halt
    "() -> never",               // kernel.abort
};

// ---- Growable byte buffer -------------------------------------------------

struct Buf {
  uint8_t* ptr = nullptr;
  size_t len = 0;
  size_t cap = 0;
};

void buf_reserve(Buf* b, size_t extra) {
  if (b->len + extra <= b->cap) return;
  size_t cap = b->cap ? b->cap : 64;
  while (cap < b->len + extra) cap *= 2;
  b->ptr = static_cast<uint8_t*>(realloc(b->ptr, cap));
  if (b->ptr == nullptr) abort();
  b->cap = cap;
}

void buf_push(Buf* b, const void* p, size_t n) {
  if (n == 0) return;
  buf_reserve(b, n);
  memcpy(b->ptr + b->len, p, n);
  b->len += n;
}

void buf_byte(Buf* b, uint8_t v) {
  buf_reserve(b, 1);
  b->ptr[b->len++] = v;
}

void buf_free(Buf* b) {
  free(b->ptr);
  b->ptr = nullptr;
  b->len = b->cap = 0;
}

void sb_printf(Buf* b, const char* fmt, ...) {
  va_list args;
  char chunk[512];
  va_start(args, fmt);
  int n = vsnprintf(chunk, sizeof(chunk), fmt, args);
  va_end(args);
  if (n > 0) {
    size_t m = static_cast<size_t>(n) < sizeof(chunk) ? static_cast<size_t>(n)
                                                      : sizeof(chunk) - 1;
    buf_push(b, chunk, m);
  }
}

// ---- postcard primitives --------------------------------------------------

void pc_uvarint(Buf* b, uint64_t v) {
  do {
    uint8_t byte = v & 0x7f;
    v >>= 7;
    if (v) byte |= 0x80;
    buf_byte(b, byte);
  } while (v);
}

void pc_svarint(Buf* b, int64_t v) {  // zigzag then LEB128
  pc_uvarint(b, (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63));
}

void pc_bytes(Buf* b, const void* p, size_t n) {
  pc_uvarint(b, n);
  buf_push(b, p, n);
}

void pc_str(Buf* b, const char* s) { pc_bytes(b, s, strlen(s)); }

void put_u32(Buf* b, uint32_t v) {
  uint8_t le[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                   static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
  buf_push(b, le, 4);
}

void put_i64(Buf* b, int64_t v) {
  uint8_t le[8];
  for (int i = 0; i < 8; i++) le[i] = static_cast<uint8_t>(static_cast<uint64_t>(v) >> (i * 8));
  buf_push(b, le, 8);
}

uint32_t get_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int64_t get_i64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (i * 8);
  return static_cast<int64_t>(v);
}

// ---- Recorder -------------------------------------------------------------

struct Iov {
  uint8_t arg_index;
  Buf bytes;
};

struct Entry {
  int32_t func_index;
  uint8_t action;
  Buf args;
  Buf ret;
  Iov* iovs = nullptr;
  size_t n_iovs = 0;
};

struct Tape {
  int recording = 0;
  int replaying = 0;
  char* path = nullptr;

  Entry* entries = nullptr;
  size_t n_entries = 0;
  size_t cap_entries = 0;
  size_t bytes = 0;
  int dropped = 0;

  size_t cursor = 0;
};

Tape g_tape;

void tape_discard() {
  g_tape.dropped = 1;
  g_tape.n_entries = 0;
  fprintf(stderr, "[tape] exceeded %zu bytes; recording dropped\n", kCeilingBytes);
}

Entry* entry_begin(int func_index) {
  if (!uv_tape_recording()) return nullptr;
  if (g_tape.n_entries == g_tape.cap_entries) {
    size_t cap = g_tape.cap_entries ? g_tape.cap_entries * 2 : 256;
    Entry* grown = static_cast<Entry*>(realloc(g_tape.entries, cap * sizeof(Entry)));
    if (grown == nullptr) { tape_discard(); return nullptr; }
    g_tape.entries = grown;
    g_tape.cap_entries = cap;
  }
  Entry* e = &g_tape.entries[g_tape.n_entries];
  memset(e, 0, sizeof(*e));
  e->func_index = func_index;
  e->action = UV_TAPE_ACTION_RESUME;
  return e;
}

void entry_iov(Entry* e, uint8_t arg_index, const void* p, size_t n) {
  if (e == nullptr) return;
  if (g_tape.bytes + n > kCeilingBytes) { tape_discard(); return; }
  Iov* grown = static_cast<Iov*>(realloc(e->iovs, (e->n_iovs + 1) * sizeof(Iov)));
  if (grown == nullptr) { tape_discard(); return; }
  e->iovs = grown;
  Iov* iov = &e->iovs[e->n_iovs++];
  iov->arg_index = arg_index;
  memset(&iov->bytes, 0, sizeof(iov->bytes));
  buf_push(&iov->bytes, p, n);
  g_tape.bytes += n;
}

void entry_commit(Entry* e) {
  if (e == nullptr) return;
  g_tape.bytes += e->args.len + e->ret.len;
  if (g_tape.bytes > kCeilingBytes) { tape_discard(); return; }
  g_tape.n_entries++;
}

// ---- Replayer -------------------------------------------------------------

// Report a divergence and stop. Deliberately not a JS exception: an effect can
// be replayed while the loop is winding down, where a throw would be swallowed
// -- so the run would falsely report success. A divergence means the replay is
// invalid; say why and leave.
[[noreturn]] void tape_diverged(const char* fmt, ...) {
  va_list args;
  fputs("[tape] divergence: ", stderr);
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fputc('\n', stderr);
  fflush(stderr);
  _exit(EXIT_FAILURE);
}

const Entry* tape_next(int func_index) {
  if (g_tape.cursor >= g_tape.n_entries) {
    tape_diverged("ran off the end of the tape at entry %zu; expected no more effects, got %s",
                  g_tape.cursor, kEffectFqn[func_index]);
  }
  const Entry* e = &g_tape.entries[g_tape.cursor];
  if (e->func_index != func_index) {
    tape_diverged("entry %zu: recorded %s, but the program called %s",
                  g_tape.cursor, kEffectFqn[e->func_index], kEffectFqn[func_index]);
  }
  g_tape.cursor++;
  return e;
}

const Iov* entry_find_iov(const Entry* e, uint8_t arg_index) {
  for (size_t i = 0; i < e->n_iovs; i++)
    if (e->iovs[i].arg_index == arg_index) return &e->iovs[i];
  return nullptr;
}

// ---- Encode ---------------------------------------------------------------

int64_t now_millis() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);  // the recorder's own clock, off-tape
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void encode_tape(Buf* out) {
  pc_str(out, "nodejs");         // assembly_hash -- see NODEJS.md
  pc_str(out, "main");           // entry_fqn
  pc_svarint(out, now_millis());
  buf_byte(out, 0);              // session_id: None
  pc_str(out, "27");             // build_id
  pc_uvarint(out, g_tape.n_entries);
  for (size_t i = 0; i < g_tape.n_entries; i++) {
    const Entry* e = &g_tape.entries[i];
    pc_svarint(out, e->func_index);
    buf_byte(out, e->action);
    pc_bytes(out, e->args.ptr, e->args.len);
    pc_uvarint(out, e->n_iovs);
    for (size_t j = 0; j < e->n_iovs; j++) {
      buf_byte(out, e->iovs[j].arg_index);
      pc_bytes(out, e->iovs[j].bytes.ptr, e->iovs[j].bytes.len);
    }
    pc_bytes(out, e->ret.ptr, e->ret.len);
  }
}

void tape_write_file() {
  Buf body;
  encode_tape(&body);

  size_t bound = ZSTD_compressBound(body.len);
  uint8_t* z = static_cast<uint8_t*>(malloc(bound));
  if (z == nullptr) { buf_free(&body); return; }
  size_t zlen = ZSTD_compress(z, bound, body.ptr, body.len, kZstdLevel);
  if (ZSTD_isError(zlen)) {
    fprintf(stderr, "[tape] zstd failed: %s\n", ZSTD_getErrorName(zlen));
    free(z);
    buf_free(&body);
    return;
  }

  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s.tmp", g_tape.path);
  FILE* f = fopen(tmp, "wb");
  if (f == nullptr) {
    fprintf(stderr, "[tape] cannot open %s: %s\n", tmp, strerror(errno));
    free(z);
    buf_free(&body);
    return;
  }
  uint8_t version = kFormatVersion;
  bool ok = fwrite(&version, 1, 1, f) == 1 && fwrite(z, 1, zlen, f) == zlen;
  ok = (fclose(f) == 0) && ok;
  if (!ok || rename(tmp, g_tape.path) != 0) {
    fprintf(stderr, "[tape] cannot write %s: %s\n", g_tape.path, strerror(errno));
    unlink(tmp);
  } else {
    fprintf(stderr, "[tape] %zu entries -> %s (%zu bytes)\n",
            g_tape.n_entries, g_tape.path, zlen + 1);
  }
  free(z);
  buf_free(&body);
}

// ---- Decode ---------------------------------------------------------------

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
};

uint64_t rd_uvarint(Reader* r) {
  uint64_t v = 0;
  int shift = 0;
  while (r->p < r->end) {
    uint8_t byte = *r->p++;
    v |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if (!(byte & 0x80)) return v;
    shift += 7;
  }
  tape_diverged("truncated varint");
}

int64_t rd_svarint(Reader* r) {
  uint64_t v = rd_uvarint(r);
  return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
}

uint8_t rd_byte(Reader* r) {
  if (r->p >= r->end) tape_diverged("truncated tape");
  return *r->p++;
}

void rd_bytes(Reader* r, Buf* out) {
  size_t n = static_cast<size_t>(rd_uvarint(r));
  if (static_cast<size_t>(r->end - r->p) < n) tape_diverged("truncated payload");
  memset(out, 0, sizeof(*out));
  buf_push(out, r->p, n);
  r->p += n;
}

void rd_skip_str(Reader* r) {
  size_t n = static_cast<size_t>(rd_uvarint(r));
  if (static_cast<size_t>(r->end - r->p) < n) tape_diverged("truncated string");
  r->p += n;
}

void tape_read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    fprintf(stderr, "[tape] cannot open %s: %s\n", path, strerror(errno));
    exit(EXIT_FAILURE);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 1) { fprintf(stderr, "[tape] %s is empty\n", path); exit(EXIT_FAILURE); }

  uint8_t* raw = static_cast<uint8_t*>(malloc(static_cast<size_t>(size)));
  if (raw == nullptr || fread(raw, 1, static_cast<size_t>(size), f) != static_cast<size_t>(size)) {
    fprintf(stderr, "[tape] short read on %s\n", path);
    exit(EXIT_FAILURE);
  }
  fclose(f);
  if (raw[0] != kFormatVersion) {
    fprintf(stderr, "[tape] unsupported format version %d\n", raw[0]);
    exit(EXIT_FAILURE);
  }

  unsigned long long dlen = ZSTD_getFrameContentSize(raw + 1, static_cast<size_t>(size) - 1);
  if (dlen == ZSTD_CONTENTSIZE_ERROR || dlen == ZSTD_CONTENTSIZE_UNKNOWN) {
    fprintf(stderr, "[tape] %s is not a valid zstd frame\n", path);
    exit(EXIT_FAILURE);
  }
  uint8_t* body = static_cast<uint8_t*>(malloc(static_cast<size_t>(dlen)));
  size_t got = ZSTD_decompress(body, static_cast<size_t>(dlen), raw + 1, static_cast<size_t>(size) - 1);
  if (ZSTD_isError(got)) {
    fprintf(stderr, "[tape] zstd decode failed: %s\n", ZSTD_getErrorName(got));
    exit(EXIT_FAILURE);
  }
  free(raw);

  Reader r{body, body + got};
  rd_skip_str(&r);          // assembly_hash
  rd_skip_str(&r);          // entry_fqn
  (void)rd_svarint(&r);     // recorded_at
  if (rd_byte(&r)) rd_skip_str(&r);  // session_id
  rd_skip_str(&r);          // build_id

  g_tape.n_entries = static_cast<size_t>(rd_uvarint(&r));
  g_tape.entries = static_cast<Entry*>(calloc(g_tape.n_entries ? g_tape.n_entries : 1, sizeof(Entry)));
  for (size_t i = 0; i < g_tape.n_entries; i++) {
    Entry* e = &g_tape.entries[i];
    e->func_index = static_cast<int32_t>(rd_svarint(&r));
    e->action = rd_byte(&r);
    rd_bytes(&r, &e->args);
    e->n_iovs = static_cast<size_t>(rd_uvarint(&r));
    e->iovs = e->n_iovs ? static_cast<Iov*>(calloc(e->n_iovs, sizeof(Iov))) : nullptr;
    for (size_t j = 0; j < e->n_iovs; j++) {
      e->iovs[j].arg_index = rd_byte(&r);
      rd_bytes(&r, &e->iovs[j].bytes);
    }
    rd_bytes(&r, &e->ret);
  }
  free(body);
}

// ---- Inspect --------------------------------------------------------------

constexpr size_t kMaxCell = 48;
constexpr int kCols = 5;

bool utf8_cont(uint8_t b) { return (b & 0xc0) == 0x80; }

size_t cell_width(const Buf* b) {
  size_t w = 0;
  for (size_t i = 0; i < b->len; i++) if (!utf8_cont(b->ptr[i])) w++;
  return w;
}

void render_payload(Buf* out, const uint8_t* p, size_t len) {
  size_t textish = 0;
  for (size_t i = 0; i < len; i++)
    if (p[i] == '\n' || p[i] == '\t' || p[i] == '\r' ||
        (p[i] >= 0x20 && p[i] < 0x7f) || p[i] >= 0x80) textish++;
  bool as_text = len > 0 && textish * 10 >= len * 9;
  size_t show = len < kMaxCell ? len : kMaxCell;
  if (as_text) {
    while (show > 0 && show < len && utf8_cont(p[show])) show--;
    buf_byte(out, '"');
    for (size_t i = 0; i < show; i++) {
      switch (p[i]) {
        case '\n': sb_printf(out, "\\n"); break;
        case '\t': sb_printf(out, "\\t"); break;
        case '\r': sb_printf(out, "\\r"); break;
        case '"':  sb_printf(out, "\\\""); break;
        case '\\': sb_printf(out, "\\\\"); break;
        default:
          if (p[i] >= 0x80 || (p[i] >= 0x20 && p[i] < 0x7f)) buf_byte(out, p[i]);
          else sb_printf(out, "\\x%02x", p[i]);
      }
    }
    buf_byte(out, '"');
  } else {
    buf_byte(out, '<');
    for (size_t i = 0; i < show; i++) sb_printf(out, i ? " %02x" : "%02x", p[i]);
    buf_byte(out, '>');
  }
  if (show < len) sb_printf(out, " (+%zu bytes)", len - show);
}

void render_args(Buf* out, const Entry* e) {
  const uint8_t* a = e->args.ptr;
  switch (e->func_index) {
    case UV_TAPE_IO_READ:
    case UV_TAPE_IO_WRITE:
      if (e->args.len >= 8) sb_printf(out, "fd %u, %u", get_u32(a), get_u32(a + 4));
      break;
    case UV_TAPE_RANDOM_BYTES:
      if (e->args.len >= 4) sb_printf(out, "%u", get_u32(a));
      break;
    default: break;
  }
}

void render_action(Buf* out, const Entry* e) {
  if (e->action == UV_TAPE_ACTION_HALT || e->action == UV_TAPE_ACTION_ABORT) {
    long long status = e->ret.len >= 8 ? static_cast<long long>(get_i64(e->ret.ptr)) : 0;
    sb_printf(out, "%s(%lld)", e->action == UV_TAPE_ACTION_HALT ? "halt" : "abort", status);
    return;
  }
  sb_printf(out, "resume(");
  if (e->ret.len >= 8) sb_printf(out, "%lld", static_cast<long long>(get_i64(e->ret.ptr)));
  for (size_t i = 0; i < e->n_iovs; i++) {
    sb_printf(out, ", iov[%u]=", e->iovs[i].arg_index);
    render_payload(out, e->iovs[i].bytes.ptr, e->iovs[i].bytes.len);
  }
  buf_byte(out, ')');
}

void rule(size_t width, char fill) {
  putchar('+');
  for (size_t i = 0; i < width; i++) putchar(fill);
  puts("+");
}

}  // namespace

// ---- The C ABI declared in uv-tape.h --------------------------------------

extern "C" {

int uv_tape_recording(void) { return g_tape.recording && !g_tape.dropped; }
int uv_tape_replaying(void) { return g_tape.replaying; }

uint64_t uv_tape_hrtime(uint64_t real) {
  if (g_tape.replaying) {
    const Entry* e = tape_next(UV_TAPE_CLOCK_MONOTONIC);
    return static_cast<uint64_t>(get_i64(e->ret.ptr));
  }
  if (uv_tape_recording()) {
    Entry* e = entry_begin(UV_TAPE_CLOCK_MONOTONIC);
    if (e) { put_i64(&e->ret, static_cast<int64_t>(real)); entry_commit(e); }
  }
  return real;
}

double uv_tape_clock_millis(double real) {
  if (g_tape.replaying) {
    const Entry* e = tape_next(UV_TAPE_CLOCK_REALTIME);
    return static_cast<double>(get_i64(e->ret.ptr));
  }
  if (uv_tape_recording()) {
    Entry* e = entry_begin(UV_TAPE_CLOCK_REALTIME);
    if (e) { put_i64(&e->ret, static_cast<int64_t>(real)); entry_commit(e); }
  }
  return real;
}

void uv_tape_random(void* buf, size_t len, int ret) {
  if (g_tape.replaying) {
    const Entry* e = tape_next(UV_TAPE_RANDOM_BYTES);
    const Iov* iov = entry_find_iov(e, 0);
    if (iov) memcpy(buf, iov->bytes.ptr, iov->bytes.len < len ? iov->bytes.len : len);
    return;
  }
  if (uv_tape_recording()) {
    Entry* e = entry_begin(UV_TAPE_RANDOM_BYTES);
    if (e) {
      put_u32(&e->args, static_cast<uint32_t>(len));
      if (ret == 0) entry_iov(e, 0, buf, len);
      put_i64(&e->ret, ret);
      entry_commit(e);
    }
  }
}

void uv_tape_finish(int exit_status) {
  int aborted = exit_status != 0;
  if (g_tape.replaying) {
    fprintf(stderr, "[tape] replayed %zu of %zu entries\n", g_tape.cursor, g_tape.n_entries);
    g_tape.replaying = 0;
    return;
  }
  if (!g_tape.recording || g_tape.dropped) return;

  Entry* e = entry_begin(aborted ? UV_TAPE_KERNEL_ABORT : UV_TAPE_KERNEL_HALT);
  if (e) {
    e->action = aborted ? UV_TAPE_ACTION_ABORT : UV_TAPE_ACTION_HALT;
    put_i64(&e->ret, exit_status);
    entry_commit(e);
  }
  g_tape.recording = 0;
  tape_write_file();
}

// Called from Node's option handling (src/tape_node.cc) once the paths are
// parsed. Kept in this file so the Tape state stays file-local.
void uv_tape_record_to(const char* path) {
  g_tape.recording = 1;
  g_tape.path = strdup(path);
}

void uv_tape_replay_from(const char* path) {
  g_tape.replaying = 1;
  g_tape.path = strdup(path);
  tape_read_file(path);
}

void uv_tape_inspect(const char* path) {
  tape_read_file(path);
  static const char* const header[kCols] = {"#", "Func", "Signature", "Args", "Action"};

  size_t nrows = g_tape.n_entries + 1;
  Buf* cells = static_cast<Buf*>(calloc(nrows * kCols, sizeof(Buf)));
  for (int c = 0; c < kCols; c++) sb_printf(&cells[c], "%s", header[c]);
  for (size_t i = 0; i < g_tape.n_entries; i++) {
    const Entry* e = &g_tape.entries[i];
    Buf* row = &cells[(i + 1) * kCols];
    sb_printf(&row[0], "%zu", i);
    sb_printf(&row[1], "%s", kEffectFqn[e->func_index]);
    sb_printf(&row[2], "%s", kEffectSig[e->func_index]);
    render_args(&row[3], e);
    render_action(&row[4], e);
  }
  size_t w[kCols] = {0};
  for (size_t r = 0; r < nrows; r++)
    for (int c = 0; c < kCols; c++) {
      size_t len = cell_width(&cells[r * kCols + c]);
      if (len > w[c]) w[c] = len;
    }
  size_t inner = 2;
  for (int c = 0; c < kCols; c++) inner += w[c] + (c ? 3 : 0);

  rule(inner, '-');
  for (size_t r = 0; r < nrows; r++) {
    fputs("| ", stdout);
    for (int c = 0; c < kCols; c++) {
      if (c) fputs("   ", stdout);
      Buf* cell = &cells[r * kCols + c];
      fwrite(cell->ptr, 1, cell->len, stdout);
      for (size_t pad = cell_width(cell); pad < w[c]; pad++) putchar(' ');
    }
    puts(" |");
    if (r == 0) rule(inner, '=');
  }
  rule(inner, '-');
  for (size_t i = 0; i < nrows * kCols; i++) buf_free(&cells[i]);
  free(cells);
}

}  // extern "C"
