// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <iomanip>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "src/execution/arguments-inl.h"
#include "src/execution/frames-inl.h"
#include "src/execution/isolate-inl.h"
#include "src/interpreter/bytecode-array-iterator.h"
#include "src/interpreter/bytecode-decoder.h"
#include "src/interpreter/bytecode-flags-and-tokens.h"
#include "src/interpreter/bytecode-register.h"
#include "src/interpreter/bytecodes.h"
#include "src/interpreter/interpreter.h"
#include "src/logging/counters.h"
#include "src/objects/heap-number-inl.h"
#include "src/objects/js-function.h"
#include "src/objects/script-inl.h"
#include "src/objects/shared-function-info.h"
#include "src/objects/string-inl.h"
#include "src/runtime/runtime-utils.h"
#include "src/snapshot/snapshot.h"
#ifdef V8_DUMPLING
#include "src/dumpling/dumpling-manager.h"
#endif
#include "src/utils/ostreams.h"

namespace v8 {
namespace internal {

namespace {

#if defined(V8_TRACE_UNOPTIMIZED) || defined(V8_DUMPLING)

void AdvanceToOffsetForTracing(
    interpreter::BytecodeArrayIterator& bytecode_iterator, int offset) {
  while (bytecode_iterator.current_offset() +
             bytecode_iterator.current_bytecode_size() <=
         offset) {
    bytecode_iterator.Advance();
  }
  DCHECK(bytecode_iterator.current_offset() == offset ||
         ((bytecode_iterator.current_offset() + 1) == offset &&
          bytecode_iterator.current_operand_scale() >
              interpreter::OperandScale::kSingle));
}

#endif  // V8_TRACE_UNOPTIMIZED || V8_DUMPLING

#ifdef V8_TRACE_UNOPTIMIZED

void PrintRegisterRange(UnoptimizedJSFrame* frame, std::ostream& os,
                        interpreter::BytecodeArrayIterator& bytecode_iterator,
                        const int& reg_field_width, const char* arrow_direction,
                        interpreter::Register first_reg, int range) {
  for (int reg_index = first_reg.index(); reg_index < first_reg.index() + range;
       reg_index++) {
    Tagged<Object> reg_object = frame->ReadInterpreterRegister(reg_index);
    os << "      [ " << std::setw(reg_field_width)
       << interpreter::Register(reg_index).ToString() << arrow_direction;
    ShortPrint(reg_object, os);
    os << " ]" << std::endl;
  }
}

void PrintRegisters(UnoptimizedJSFrame* frame, std::ostream& os, bool is_input,
                    interpreter::BytecodeArrayIterator& bytecode_iterator,
                    Handle<Object> accumulator) {
  static const char kAccumulator[] = "accumulator";
  static const int kRegFieldWidth = static_cast<int>(sizeof(kAccumulator) - 1);
  static const char* kInputColourCode = "\033[0;36m";
  static const char* kOutputColourCode = "\033[0;35m";
  static const char* kNormalColourCode = "\033[0;m";
  const char* kArrowDirection = is_input ? " -> " : " <- ";
  if (v8_flags.log_colour) {
    os << (is_input ? kInputColourCode : kOutputColourCode);
  }

  interpreter::Bytecode bytecode = bytecode_iterator.current_bytecode();

  // Print accumulator.
  if ((is_input && interpreter::Bytecodes::ReadsAccumulator(bytecode)) ||
      (!is_input &&
       interpreter::Bytecodes::WritesOrClobbersAccumulator(bytecode))) {
    os << "      [ " << kAccumulator << kArrowDirection;
    ShortPrint(*accumulator, os);
    os << " ]" << std::endl;
  }

  // Print the registers.
  int operand_count = interpreter::Bytecodes::NumberOfOperands(bytecode);
  for (int operand_index = 0; operand_index < operand_count; operand_index++) {
    interpreter::OperandType operand_type =
        interpreter::Bytecodes::GetOperandType(bytecode, operand_index);
    bool should_print =
        is_input
            ? interpreter::Bytecodes::IsRegisterInputOperandType(operand_type)
            : interpreter::Bytecodes::IsRegisterOutputOperandType(operand_type);
    if (should_print) {
      interpreter::Register first_reg =
          bytecode_iterator.GetRegisterOperand(operand_index);
      int range = bytecode_iterator.GetRegisterOperandRange(operand_index);
      PrintRegisterRange(frame, os, bytecode_iterator, kRegFieldWidth,
                         kArrowDirection, first_reg, range);
    }
  }
  if (!is_input && interpreter::Bytecodes::IsShortStar(bytecode)) {
    PrintRegisterRange(frame, os, bytecode_iterator, kRegFieldWidth,
                       kArrowDirection,
                       interpreter::Register::FromShortStar(bytecode), 1);
  }
  if (v8_flags.log_colour) {
    os << kNormalColourCode;
  }
}

#endif  // V8_TRACE_UNOPTIMIZED

}  // namespace

#ifdef V8_TRACE_UNOPTIMIZED

RUNTIME_FUNCTION(Runtime_TraceUnoptimizedBytecodeEntry) {
  if (!v8_flags.trace_ignition && !v8_flags.trace_baseline_exec) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  JavaScriptStackFrameIterator frame_iterator(isolate);
  UnoptimizedJSFrame* frame =
      reinterpret_cast<UnoptimizedJSFrame*>(frame_iterator.frame());

  if (frame->is_interpreted() && !v8_flags.trace_ignition) {
    return ReadOnlyRoots(isolate).undefined_value();
  }
  if (frame->is_baseline() && !v8_flags.trace_baseline_exec) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  SealHandleScope shs(isolate);
  DCHECK_EQ(3, args.length());
  Handle<BytecodeArray> bytecode_array = CheckedCast<BytecodeArray>(args.at(0));
  int bytecode_offset = args.smi_value_at(1);
  Handle<Object> accumulator = args.at(2);

  int offset = bytecode_offset - BytecodeArray::kHeaderSize + kHeapObjectTag;
  interpreter::BytecodeArrayIterator bytecode_iterator(bytecode_array);
  AdvanceToOffsetForTracing(bytecode_iterator, offset);
  if (offset == bytecode_iterator.current_offset()) {
    StdoutStream os;

    // Print bytecode.
    const uint8_t* bytecode_address = bytecode_iterator.current_address();

    if (frame->is_baseline()) {
      os << "B-> ";
    } else {
      os << " -> ";
    }
    os << static_cast<const void*>(bytecode_address) << " @ " << std::setw(4)
       << offset << " : ";
    bytecode_iterator.PrintCurrentBytecodeTo(os);
    os << std::endl;
    // Print all input registers and accumulator.
    PrintRegisters(frame, os, true, bytecode_iterator, accumulator);

    os << std::flush;
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_TraceUnoptimizedBytecodeExit) {
  if (!v8_flags.trace_ignition && !v8_flags.trace_baseline_exec) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  JavaScriptStackFrameIterator frame_iterator(isolate);
  UnoptimizedJSFrame* frame =
      reinterpret_cast<UnoptimizedJSFrame*>(frame_iterator.frame());

  if (frame->is_interpreted() && !v8_flags.trace_ignition) {
    return ReadOnlyRoots(isolate).undefined_value();
  }
  if (frame->is_baseline() && !v8_flags.trace_baseline_exec) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  SealHandleScope shs(isolate);
  DCHECK_EQ(3, args.length());
  Handle<BytecodeArray> bytecode_array = CheckedCast<BytecodeArray>(args.at(0));
  int bytecode_offset = args.smi_value_at(1);
  Handle<Object> accumulator = args.at(2);

  int offset = bytecode_offset - BytecodeArray::kHeaderSize + kHeapObjectTag;
  interpreter::BytecodeArrayIterator bytecode_iterator(bytecode_array);
  AdvanceToOffsetForTracing(bytecode_iterator, offset);
  // The offset comparison here ensures registers only printed when the
  // (potentially) widened bytecode has completed. The iterator reports
  // the offset as the offset of the prefix bytecode.
  if (bytecode_iterator.current_operand_scale() ==
          interpreter::OperandScale::kSingle ||
      offset > bytecode_iterator.current_offset()) {
    StdoutStream os;

    // Print all output registers and accumulator.
    PrintRegisters(frame, os, false, bytecode_iterator, accumulator);
    os << std::flush;
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

#endif  // V8_TRACE_UNOPTIMIZED

#ifdef V8_TRACE_FEEDBACK_UPDATES

RUNTIME_FUNCTION(Runtime_TraceUpdateFeedback) {
  if (!v8_flags.trace_feedback_updates) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  SealHandleScope shs(isolate);
  DCHECK_EQ(3, args.length());
  Handle<FeedbackVector> vector = args.at<FeedbackVector>(0);
  int slot = args.smi_value_at(1);
  auto reason = Cast<String>(args[2]);

  FeedbackVector::TraceFeedbackChange(isolate, *vector, FeedbackSlot(slot),
                                      reason->ToCString().get());

  return ReadOnlyRoots(isolate).undefined_value();
}

#endif  // V8_TRACE_FEEDBACK_UPDATES

#ifdef V8_DUMPLING

RUNTIME_FUNCTION(Runtime_DumpExecutionFrame) {
  if (!isolate->dumpling_manager()->IsDumpingEnabled()) {
    return ReadOnlyRoots(isolate).undefined_value();
  }
  DCHECK_EQ(3, args.length());

  SealHandleScope shs(isolate);

  DisallowGarbageCollection no_gc;

  JavaScriptStackFrameIterator frame_iterator(isolate);
  UnoptimizedJSFrame* frame =
      reinterpret_cast<UnoptimizedJSFrame*>(frame_iterator.frame());

  Tagged<JSFunction> function = frame->function();
  bool is_sparkplug = frame->is_baseline();
  bool is_interpreter = !is_sparkplug;

  if ((is_sparkplug && !v8_flags.sparkplug_dumping) ||
      (is_interpreter && !v8_flags.interpreter_dumping)) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  Handle<BytecodeArray> bytecode_array = CheckedCast<BytecodeArray>(args.at(0));
  int bytecode_offset = args.smi_value_at(1);

  Handle<Object> accumulator = args.at(2);

  int offset = bytecode_offset - BytecodeArray::kHeaderSize + kHeapObjectTag;
  interpreter::BytecodeArrayIterator bytecode_iterator(bytecode_array);
  AdvanceToOffsetForTracing(bytecode_iterator, offset);

  if (offset == bytecode_iterator.current_offset()) {
    int function_local_bytecode_offset = bytecode_iterator.current_offset();
    DCHECK_GE(function_local_bytecode_offset, 0);
    DumpFrameType frame_dump_type =
        is_sparkplug ? kSparkplugFrame : kInterpreterFrame;
    DumplingUnoptimizedJSFrame frame_view(frame);
    isolate->dumpling_manager()->DoPrint(
        &frame_view, function, function_local_bytecode_offset, frame_dump_type,
        bytecode_array, accumulator);
  }

  return ReadOnlyRoots(isolate).undefined_value();
}

#endif  //  V8_DUMPLING

// ---- Node tape view -------------------------------------------------------
//
// The call tree of a replayed run. Node's --tape-view replays a tape (so every
// effect is served from the tape, no real IO) and turns on v8_flags.tape_view;
// the interpreter then calls Runtime_TapeViewRecordBytecode at every bytecode
// (gated by a cheap flag branch emitted in interpreter-assembler.cc). Here we
// keep only the calls and returns, tagged with the live JS stack depth, and
// render them as a tree once the run is done. This is the V8 analogue of the
// sys.monitoring viewer in the Python port and the rb_tracepoint one in Ruby.

// Set true by Node at the first event-loop iteration (uv_tape_go_live), so the
// synchronous module-loading calls before the loop stay off the tree.
bool g_tape_view_live = false;

namespace {

struct TapeViewEvent {
  bool is_call;
  int depth;
  std::string name;  // callee (call)
  std::string args;  // rendered arguments (call)
  std::string file;  // call-site file basename (call)
  int line;          // call-site line, 1-based (call)
  std::string ret;   // rendered return value (return)
};

std::vector<TapeViewEvent>* g_tape_view_events = nullptr;

bool IsTapeViewCall(interpreter::Bytecode bc) {
  return bc == interpreter::Bytecode::kCallAnyReceiver ||
         bc == interpreter::Bytecode::kCallProperty ||
         bc == interpreter::Bytecode::kCallProperty0 ||
         bc == interpreter::Bytecode::kCallProperty1 ||
         bc == interpreter::Bytecode::kCallProperty2 ||
         bc == interpreter::Bytecode::kCallUndefinedReceiver ||
         bc == interpreter::Bytecode::kCallUndefinedReceiver0 ||
         bc == interpreter::Bytecode::kCallUndefinedReceiver1 ||
         bc == interpreter::Bytecode::kCallUndefinedReceiver2 ||
         bc == interpreter::Bytecode::kCallWithSpread;
}

int TapeViewDepth(Isolate* isolate) {
  int depth = 0;
  for (JavaScriptStackFrameIterator it(isolate); !it.done(); it.Advance())
    depth++;
  return depth;
}

const char* Basename(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// Render a value cleanly for the tree: literals as themselves, strings quoted
// and truncated, objects as their class name. Never leaks a heap address (the
// bug that dogged the Ruby and Python ports) -- this is display only, but the
// principle holds: a tape view must read the same on every run.
std::string TapeViewValue(Isolate* isolate, Tagged<Object> v) {
  ReadOnlyRoots roots(isolate);
  if (v == roots.undefined_value()) return "undefined";
  if (v == roots.null_value()) return "null";
  if (v == roots.true_value()) return "true";
  if (v == roots.false_value()) return "false";
  if (IsSmi(v)) return std::to_string(Smi::ToInt(v));
  if (IsHeapNumber(v)) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", Cast<HeapNumber>(v)->value());
    return buf;
  }
  if (IsString(v)) {
    std::string s = Cast<String>(v)->ToCString().get();
    for (char& c : s) {
      if (c == '\n') c = '.';
      else if (c == '\t') c = ' ';
    }
    if (s.size() > 32) {
      size_t cut = 32;
      // Don't slice a UTF-8 sequence mid-byte (continuation bytes are 10xxxxxx).
      while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        cut--;
      s = s.substr(0, cut) + "…";
    }
    return "\"" + s + "\"";
  }
  if (IsJSFunction(v)) {
    std::string n = Cast<JSFunction>(v)->shared()->DebugNameCStr().get();
    return n.empty() ? "<function>" : ("<fn " + n + ">");
  }
  // Fallback: the class name from ShortPrint's "<ClassName ...>" form, minus
  // any address it would otherwise print.
  std::ostringstream os;
  ShortPrint(v, os);
  std::string sp = os.str();
  size_t lt = sp.find('<');
  if (lt != std::string::npos) {
    size_t start = lt + 1, end = start;
    while (end < sp.size() && sp[end] != ' ' && sp[end] != '[' &&
           sp[end] != ':' && sp[end] != '>')
      end++;
    return "<" + sp.substr(start, end - start) + ">";
  }
  return "?";
}

// A node in the reconstructed call tree. Node 0 is a synthetic root holding the
// top-level calls as children; it is never displayed.
struct TVNode {
  std::string name, args, ret, file;  // file is the full path (for the source pane)
  int line = 0;
  int parent = -1;
  int depth = -1;   // tree nesting level; root's children are 0
  std::vector<int> kids;
};

// Rebuild the tree from the flat event log. A call at caller-depth D runs its
// callee at depth D+1, so open[D+1] tracks whoever executes there; a return at
// depth R closes open[R]. Robust to native callees, which reach no Return.
std::vector<TVNode> TapeViewBuildTree() {
  std::vector<TVNode> nodes;
  nodes.push_back(TVNode{});  // root
  if (g_tape_view_events == nullptr) return nodes;

  std::vector<int> open;
  auto set_open = [&](size_t d, int idx) {
    if (open.size() <= d) open.resize(d + 1, -1);
    open[d] = idx;
  };
  auto get_open = [&](size_t d) -> int {
    return (d < open.size() && open[d] >= 0) ? open[d] : 0;
  };
  for (const TapeViewEvent& e : *g_tape_view_events) {
    if (e.is_call) {
      int parent = get_open(e.depth);
      TVNode n;
      n.name = e.name;
      n.args = e.args;
      n.file = e.file;
      n.line = e.line;
      n.parent = parent;
      n.depth = nodes[parent].depth + 1;
      int idx = static_cast<int>(nodes.size());
      nodes.push_back(std::move(n));
      nodes[parent].kids.push_back(idx);
      set_open(e.depth + 1, idx);
    } else if (e.depth < static_cast<int>(open.size()) && open[e.depth] >= 0) {
      nodes[open[e.depth]].ret = e.ret;
      open[e.depth] = -1;
    }
  }
  return nodes;
}

// One row's text: indent, expand marker, name(args), ↦ return, and basename:line.
std::string TapeViewRowText(const std::vector<TVNode>& nodes, int id,
                            const std::vector<char>& expanded) {
  const TVNode& n = nodes[id];
  std::string s;
  for (int i = 0; i < n.depth; i++) s += "  ";
  bool kids = !n.kids.empty();
  s += kids ? (expanded[id] ? "▾ " : "▸ ") : "  ";
  s += n.name + "(" + n.args + ")";
  if (!n.ret.empty()) s += " ↦ " + n.ret;
  if (!n.file.empty() && n.line > 0) {
    s += "   ";
    s += Basename(n.file.c_str());
    s += ":" + std::to_string(n.line);
  }
  return s;
}

// The non-interactive dump (piped output, or no tty): the whole tree, expanded.
void TapeViewRenderPlain() {
  std::vector<TVNode> nodes = TapeViewBuildTree();
  std::vector<char> expanded(nodes.size(), 1);
  printf("call tree (%zu calls)\n", nodes.size() - 1);
  std::vector<int> stack;
  for (auto it = nodes[0].kids.rbegin(); it != nodes[0].kids.rend(); ++it)
    stack.push_back(*it);
  while (!stack.empty()) {
    int id = stack.back();
    stack.pop_back();
    printf("%s\n", TapeViewRowText(nodes, id, expanded).c_str());
    for (auto it = nodes[id].kids.rbegin(); it != nodes[id].kids.rend(); ++it)
      stack.push_back(*it);
  }
}

// ---- Interactive TUI ------------------------------------------------------
// A ratatui-style viewer, matching the Ruby and Python ports: a scrollable tree
// pane over a source pane for the selected call. Keys: j/k move, l/h (or arrows)
// expand/collapse, g/G top/bottom, q quit.

constexpr int kSourceContext = 4;

struct termios g_saved_term;

void TapeViewRowsWalk(const std::vector<TVNode>& nodes, int id,
                      const std::vector<char>& expanded, std::vector<int>& rows) {
  rows.push_back(id);
  if (!expanded[id]) return;
  for (int k : nodes[id].kids) TapeViewRowsWalk(nodes, k, expanded, rows);
}

void TapeViewRowsRebuild(const std::vector<TVNode>& nodes,
                         const std::vector<char>& expanded,
                         std::vector<int>& rows) {
  rows.clear();
  for (int k : nodes[0].kids) TapeViewRowsWalk(nodes, k, expanded, rows);
}

void TapeViewTermSize(int* w, int* h) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
    *w = ws.ws_col;
    *h = ws.ws_row;
  } else {
    *w = 80;
    *h = 24;
  }
}

// Draw the source of the selected call, its definition line highlighted. Read
// with plain stdio -- the replay is over, so there is no tape to disturb.
void TapeViewDrawSource(const std::vector<TVNode>& nodes, int id, int top,
                        int height, int width) {
  while (id > 0 && nodes[id].file.empty()) id = nodes[id].parent;
  if (id <= 0) {
    printf("\x1b[%d;1H\x1b[7m %-*s\x1b[0m", top, width - 1, "(no source)");
    return;
  }
  const TVNode& n = nodes[id];
  int row = top;
  char header[1024];
  snprintf(header, sizeof(header), "%s:%d", n.file.c_str(), n.line);
  printf("\x1b[%d;1H\x1b[7m %-*s\x1b[0m", row++, width - 1, header);
  if (n.line < 1) return;

  FILE* f = fopen(n.file.c_str(), "r");
  if (f == nullptr) {
    printf("\x1b[%d;1H  (cannot read %s)", row++, n.file.c_str());
    return;
  }
  int from = n.line - kSourceContext;
  if (from < 1) from = 1;
  int to = n.line + kSourceContext;
  char line[512];
  int lineno = 0;
  while (fgets(line, sizeof(line), f) && lineno < to && row < top + height) {
    lineno++;
    if (lineno < from) continue;
    line[strcspn(line, "\n")] = '\0';
    const char* color = (lineno == n.line) ? "\x1b[1;33m" : "\x1b[2m";
    printf("\x1b[%d;1H%s%5d \xe2\x94\x82 %.*s\x1b[0m", row++, color, lineno,
           width - 9, line);
  }
  fclose(f);
}

void TapeViewDraw(const std::vector<TVNode>& nodes, const std::vector<int>& rows,
                  const std::vector<char>& expanded, int cursor, int scroll) {
  int w, h;
  TapeViewTermSize(&w, &h);
  int tree_h = h * 2 / 3;
  int src_h = h - tree_h - 1;

  fputs("\x1b[2J\x1b[?25l", stdout);
  printf("\x1b[1;1H\x1b[7m node tape view — %zu calls   "
         "j/k move  l/h expand/collapse  g/G ends  q quit \x1b[0m",
         nodes.size() - 1);

  for (int i = 0; i < tree_h - 1; i++) {
    size_t r = static_cast<size_t>(scroll + i);
    if (r >= rows.size()) break;
    std::string text = TapeViewRowText(nodes, rows[r], expanded);
    printf("\x1b[%d;1H", i + 2);
    if (static_cast<int>(r) == cursor) fputs("\x1b[7m", stdout);
    // Truncate to width on a byte basis is unsafe for UTF-8; print and let the
    // terminal clip. Clear to end of line first so stale text is gone.
    fputs("\x1b[K", stdout);
    printf("%s\x1b[0m", text.c_str());
  }
  if (!rows.empty())
    TapeViewDrawSource(nodes, rows[cursor], tree_h + 1, src_h, w);
  fflush(stdout);
}

void TapeViewRenderTUI() {
  std::vector<TVNode> nodes = TapeViewBuildTree();
  if (nodes.size() <= 1) {
    printf("[tape] no calls recorded\n");
    return;
  }
  std::vector<char> expanded(nodes.size(), 0);
  for (size_t i = 0; i < nodes.size(); i++)
    if (nodes[i].depth < 2) expanded[i] = 1;  // open the first two levels

  std::vector<int> rows;
  TapeViewRowsRebuild(nodes, expanded, rows);

  if (tcgetattr(STDIN_FILENO, &g_saved_term) < 0) {
    TapeViewRenderPlain();
    return;
  }
  struct termios t = g_saved_term;
  t.c_lflag &= ~(ICANON | ECHO);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
  // Alternate screen, and disable autowrap so long rows clip at the edge instead
  // of wrapping and shifting every row below them.
  fputs("\x1b[?1049h\x1b[?7l", stdout);

  int cursor = 0, scroll = 0;
  for (;;) {
    int w, h;
    TapeViewTermSize(&w, &h);
    int page = h * 2 / 3 - 1;
    if (page < 1) page = 1;
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + page) scroll = cursor - page + 1;
    TapeViewDraw(nodes, rows, expanded, cursor, scroll);

    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) break;
    if (c == 27) {  // escape: could be a bare ESC or an arrow sequence
      unsigned char seq[2];
      if (read(STDIN_FILENO, &seq[0], 1) != 1) break;
      if (seq[0] != '[' || read(STDIN_FILENO, &seq[1], 1) != 1) continue;
      switch (seq[1]) {
        case 'A': c = 'k'; break;
        case 'B': c = 'j'; break;
        case 'C': c = 'l'; break;
        case 'D': c = 'h'; break;
        default: continue;
      }
    }
    int id = rows.empty() ? 0 : rows[cursor];
    if (c == 'q') break;
    switch (c) {
      case 'j':
        if (static_cast<size_t>(cursor) + 1 < rows.size()) cursor++;
        break;
      case 'k':
        if (cursor > 0) cursor--;
        break;
      case 'l':
        if (!nodes[id].kids.empty() && !expanded[id]) {
          expanded[id] = 1;
          TapeViewRowsRebuild(nodes, expanded, rows);
        } else if (static_cast<size_t>(cursor) + 1 < rows.size()) {
          cursor++;
        }
        break;
      case 'h':
        if (expanded[id] && !nodes[id].kids.empty()) {
          expanded[id] = 0;
          TapeViewRowsRebuild(nodes, expanded, rows);
        } else if (nodes[id].parent > 0) {
          for (size_t r = 0; r < rows.size(); r++)
            if (rows[r] == nodes[id].parent) { cursor = static_cast<int>(r); break; }
        }
        break;
      case 'g': cursor = 0; break;
      case 'G': cursor = static_cast<int>(rows.size()) - 1; break;
      default: break;
    }
    if (cursor < 0) cursor = 0;
    if (cursor >= static_cast<int>(rows.size()))
      cursor = static_cast<int>(rows.size()) - 1;
  }

  // Restore autowrap, leave the alternate screen, show the cursor.
  fputs("\x1b[?7h\x1b[?1049l\x1b[?25h", stdout);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_term);
  fflush(stdout);
}

}  // namespace

RUNTIME_FUNCTION(Runtime_TapeViewRecordBytecode) {
  if (!v8_flags.tape_view || !g_tape_view_live) {
    return ReadOnlyRoots(isolate).undefined_value();
  }
  SealHandleScope shs(isolate);
  DisallowGarbageCollection no_gc;
  DCHECK_EQ(3, args.length());

  Handle<BytecodeArray> bytecode_array = CheckedCast<BytecodeArray>(args.at(0));
  int bytecode_offset = args.smi_value_at(1);
  int offset = bytecode_offset - BytecodeArray::kHeaderSize + kHeapObjectTag;

  interpreter::BytecodeArrayIterator it(bytecode_array);
  while (it.current_offset() + it.current_bytecode_size() <= offset)
    it.Advance();
  if (it.current_offset() != offset) {
    return ReadOnlyRoots(isolate).undefined_value();
  }
  interpreter::Bytecode bc = it.current_bytecode();
  bool is_call = IsTapeViewCall(bc);
  bool is_return = (bc == interpreter::Bytecode::kReturn);
  if (!is_call && !is_return) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  JavaScriptStackFrameIterator frames(isolate);
  UnoptimizedJSFrame* frame =
      reinterpret_cast<UnoptimizedJSFrame*>(frames.frame());
  int depth = TapeViewDepth(isolate);

  if (g_tape_view_events == nullptr)
    g_tape_view_events = new std::vector<TapeViewEvent>();

  TapeViewEvent e;
  e.depth = depth;
  e.line = 0;
  if (is_call) {
    e.is_call = true;
    interpreter::Register callee_reg = it.GetRegisterOperand(0);
    Tagged<Object> callee = frame->ReadInterpreterRegister(callee_reg.index());
    if (IsJSFunction(callee)) {
      e.name = Cast<JSFunction>(callee)->shared()->DebugNameCStr().get();
    } else {
      e.name = TapeViewValue(isolate, callee);
    }
    if (e.name.empty()) e.name = "<anonymous>";

    // Arguments: every register-input operand past the callee (operand 0),
    // expanding register lists. The feedback-slot operand is a uimm, so it is
    // naturally skipped. This includes the receiver for property calls.
    int shown = 0;
    int nops = interpreter::Bytecodes::NumberOfOperands(bc);
    for (int op = 1; op < nops && shown < 6; op++) {
      if (!interpreter::Bytecodes::IsRegisterInputOperandType(
              interpreter::Bytecodes::GetOperandType(bc, op)))
        continue;
      interpreter::Register first = it.GetRegisterOperand(op);
      int range = it.GetRegisterOperandRange(op);
      for (int r = 0; r < range && shown < 6; r++) {
        Tagged<Object> val =
            frame->ReadInterpreterRegister(first.index() + r);
        if (shown) e.args += ", ";
        e.args += TapeViewValue(isolate, val);
        shown++;
      }
    }

    // Call-site location, from the caller's current source position.
    Tagged<Object> script_obj = frame->function()->shared()->script();
    if (IsScript(script_obj)) {
      Tagged<Script> script = Cast<Script>(script_obj);
      Script::PositionInfo info;
      if (script->GetPositionInfo(frame->position(), &info))
        e.line = info.line + 1;
      Tagged<Object> nm = script->name();
      if (IsString(nm)) e.file = Cast<String>(nm)->ToCString().get();
    }
  } else {
    e.is_call = false;
    e.ret = TapeViewValue(isolate, *args.at(2));
  }
  g_tape_view_events->push_back(std::move(e));
  return ReadOnlyRoots(isolate).undefined_value();
}

}  // namespace internal
}  // namespace v8

// C ABI called from Node (src/tape.cc, src/node_main_instance.cc). Defined in
// V8 so the interpreter hook and the renderer share one event log; Node just
// flips "live" at go-live and asks for the render once the run is done.
extern "C" void v8_tape_view_set_live(int live) {
  v8::internal::g_tape_view_live = (live != 0);
}
extern "C" void v8_tape_view_render(void) {
  // A real terminal on both ends gets the interactive TUI; anything piped (a
  // file, a grep, the CI) gets the plain tree, which is also what makes the
  // viewer verifiable headlessly.
  if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
    v8::internal::TapeViewRenderTUI();
  else
    v8::internal::TapeViewRenderPlain();
}
