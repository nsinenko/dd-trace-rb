#include <ruby.h>
#include <ruby/thread.h>
#include <ruby/version.h>
#include <stdbool.h>

#include "datadog_ruby_common.h"
#include "otel_thread_ctx.h"

// This binding is Linux-only: the underlying libdatadog crate is gated on
// `target_os = "linux"` (it relies on the TLSDESC TLS dialect so an
// out-of-process eBPF profiler can read the record), so the header/symbols
// are absent on other platforms. `HAVE_DATADOG_OTEL_THREAD_CTX_H` is set by
// `have_header` in extconf.rb.
#ifdef HAVE_DATADOG_OTEL_THREAD_CTX_H
#include <datadog/otel-thread-ctx.h>
#endif

static VALUE native_set(VALUE _self, VALUE trace_id, VALUE span_id, VALUE local_root_span_id);
static VALUE native_enable(VALUE _self);
static VALUE native_detach_and_free(VALUE _self);
static VALUE native_supported_p(VALUE _self);
static VALUE native_debug_peek(VALUE _self);

void otel_thread_ctx_init(VALUE core_module) {
  VALUE otel_thread_ctx_module = rb_define_module_under(core_module, "OTelThreadContext");

  rb_define_singleton_method(otel_thread_ctx_module, "_native_set", native_set, 3);
  rb_define_singleton_method(otel_thread_ctx_module, "_native_enable", native_enable, 0);
  rb_define_singleton_method(otel_thread_ctx_module, "_native_detach_and_free", native_detach_and_free, 0);
  rb_define_singleton_method(otel_thread_ctx_module, "_native_supported?", native_supported_p, 0);
  rb_define_singleton_method(otel_thread_ctx_module, "_native_debug_peek", native_debug_peek, 0);
}

#ifdef HAVE_DATADOG_OTEL_THREAD_CTX_H

// The current fiber's context, kept as the packed ids the wire format expects.
// It is the source of truth we republish from on a fiber switch; the OS-thread
// record itself is written in-place by ddog_otel_thread_ctx_update.
typedef struct {
  uint8_t trace_id[16];
  uint8_t span_id[8];
  uint8_t local_root_span_id[8];
} otel_fiber_context;

static const rb_data_type_t otel_fiber_context_type = {
  .wrap_struct_name = "Datadog::Core::OTelThreadContext fiber context",
  .function = {.dfree = RUBY_TYPED_DEFAULT_FREE},
  .flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

// Fiber-local storage key. Thread#[] is fiber-local despite the name, so the
// context is scoped to the fiber and freed by GC when the fiber dies.
static ID fiber_context_slot(void) {
  static ID id = 0;
  if (id == 0) id = rb_intern("__datadog_otel_fiber_context");
  return id;
}

// Returns `thread`'s current-fiber context, or NULL if it has none yet.
static otel_fiber_context *fiber_context_of(VALUE thread) {
  VALUE existing = rb_thread_local_aref(thread, fiber_context_slot());
  if (NIL_P(existing)) return NULL;

  otel_fiber_context *ctx;
  TypedData_Get_Struct(existing, otel_fiber_context, &otel_fiber_context_type, ctx);
  return ctx;
}

// Returns the current fiber's context, or NULL if it has none yet.
static otel_fiber_context *get_current_fiber_context(void) {
  return fiber_context_of(rb_thread_current());
}

// Returns the current fiber's context, creating and attaching one if needed.
static otel_fiber_context *ensure_current_fiber_context(void) {
  otel_fiber_context *ctx = get_current_fiber_context();
  if (ctx) return ctx;

  VALUE obj = TypedData_Make_Struct(rb_cObject, otel_fiber_context, &otel_fiber_context_type, ctx);
  rb_thread_local_aset(rb_thread_current(), fiber_context_slot(), obj);
  return ctx;
}

// Publishes `ctx` (or all-zero "no trace" if NULL) into the current OS thread's
// TLS record, updating it in-place (created + attached on first use).
static void publish_context(const otel_fiber_context *ctx) {
  static const uint8_t zero_trace_id[16] = {0};
  static const uint8_t zero_span_id[8] = {0};

  if (ctx) {
    ddog_otel_thread_ctx_update(&ctx->trace_id, &ctx->span_id, &ctx->local_root_span_id);
  } else {
    ddog_otel_thread_ctx_update(&zero_trace_id, &zero_span_id, &zero_span_id);
  }
}

// trace_id is expected to fit in 16 bytes (<= 128-bit), span_id and
// local_root_span_id in 8 bytes each (<= 64-bit).
static void pack_id_big_endian(VALUE id, uint8_t *buffer, size_t size) {
  rb_integer_pack(id, buffer, size, 1, 0, INTEGER_PACK_MSWORD_FIRST | INTEGER_PACK_BIG_ENDIAN);
}

// Fires with the switched-to fiber already current, so we republish that
// fiber's stored context onto the OS thread it now runs on.
static void on_fiber_switch(
  DDTRACE_UNUSED rb_event_flag_t evflag,
  DDTRACE_UNUSED VALUE data,
  DDTRACE_UNUSED VALUE self,
  DDTRACE_UNUSED ID mid,
  DDTRACE_UNUSED VALUE klass
) {
  publish_context(get_current_fiber_context());
}

static void detach_and_free_current_record(void) {
  struct ddog_ThreadContextHandle *ctx = ddog_otel_thread_ctx_detach();
  if (ctx) ddog_otel_thread_ctx_free(ctx);
}

// event_data->thread, needed to find the resuming thread's context, exists on
// Ruby 3.3+ (on 3.2 the event data is void). M:N migration only happens on 3.3+
// too, so the resume hook is compiled out below that.
#if RUBY_API_VERSION_MAJOR > 3 || (RUBY_API_VERSION_MAJOR == 3 && RUBY_API_VERSION_MINOR >= 3)
  #define OTEL_HAVE_THREAD_EVENT_DATA 1
#endif

#ifdef OTEL_HAVE_THREAD_EVENT_DATA
// A Ruby thread was scheduled onto this OS thread (possibly a different one than
// last time, under M:N): republish its current fiber's context so a running OS
// thread always shows the running thread's context. We intentionally do NOT clear
// on suspend -- a thread blocked mid-span still belongs to that span (off-CPU),
// and a running OS thread is always corrected by the next resume.
static void on_thread_resumed(
  DDTRACE_UNUSED rb_event_flag_t event,
  const rb_internal_thread_event_data_t *event_data,
  DDTRACE_UNUSED void *user_data
) {
  publish_context(fiber_context_of(event_data->thread));
}
#endif

#ifdef RUBY_INTERNAL_THREAD_EVENT_EXITED
// Frees the OS-thread record when a Ruby thread exits. In 1:1 the OS thread dies
// with the Ruby thread, so this reclaims it; under M:N the record is recreated by
// the next resume on the (shared) OS thread.
static void on_thread_exited(
  DDTRACE_UNUSED rb_event_flag_t event,
  DDTRACE_UNUSED const rb_internal_thread_event_data_t *event_data,
  DDTRACE_UNUSED void *user_data
) {
  detach_and_free_current_record();
}
#endif

static VALUE native_enable(DDTRACE_UNUSED VALUE _self) {
  static bool enabled = false;
  if (enabled) return Qfalse;

  // Follow fiber switches within a thread (these don't touch the GVL)...
  rb_add_event_hook(on_fiber_switch, RUBY_EVENT_FIBER_SWITCH, Qnil);
  // ...and a thread being scheduled onto an OS thread (covers M:N migration).
  #ifdef OTEL_HAVE_THREAD_EVENT_DATA
    rb_internal_thread_add_event_hook(on_thread_resumed, RUBY_INTERNAL_THREAD_EVENT_RESUMED, NULL);
  #endif
  #ifdef RUBY_INTERNAL_THREAD_EVENT_EXITED
    rb_internal_thread_add_event_hook(on_thread_exited, RUBY_INTERNAL_THREAD_EVENT_EXITED, NULL);
  #endif

  enabled = true;
  return Qtrue;
}

static VALUE native_set(DDTRACE_UNUSED VALUE _self, VALUE trace_id, VALUE span_id, VALUE local_root_span_id) {
  otel_fiber_context *ctx = ensure_current_fiber_context();

  pack_id_big_endian(trace_id, ctx->trace_id, sizeof(ctx->trace_id));
  pack_id_big_endian(span_id, ctx->span_id, sizeof(ctx->span_id));
  pack_id_big_endian(local_root_span_id, ctx->local_root_span_id, sizeof(ctx->local_root_span_id));

  publish_context(ctx);

  return Qtrue;
}

// Detaches the thread context record currently attached to the calling
// thread (if any) and frees it.
static VALUE native_detach_and_free(VALUE _self) {
  detach_and_free_current_record();

  return Qtrue;
}

static VALUE native_supported_p(VALUE _self) {
  return Qtrue;
}

// Decodes the packed `attrs_data` blob into a Hash. Each entry is a 1-byte key
// index, a 1-byte value length, then that many value bytes.
static VALUE decode_attrs(const uint8_t *data, uint16_t size) {
  VALUE attrs = rb_hash_new();

  uint16_t offset = 0;
  while (offset + 2 <= size) {
    uint8_t key_index = data[offset];
    uint8_t value_len = data[offset + 1];

    if (offset + 2 + value_len > size) break;

    VALUE value = rb_str_new((const char *) (data + offset + 2), value_len);

    rb_hash_aset(attrs, INT2FIX(key_index), value);
    offset += 2 + value_len;
  }

  return attrs;
}

// Debug-only helper: reads back the record currently attached to the calling
// thread, without disturbing it (detach, read, re-attach). Returns nil if no
// context is attached.
//
// There is no libdatadog API to read a record's fields -- the whole point of
// this feature is that an out-of-process reader (the eBPF profiler) parses the
// raw bytes directly. We do the same here, using the documented, stable wire
// layout (see the `ThreadContextRecord` doc comment in
// `libdd-otel-thread-ctx/src/lib.rs`): trace_id at offset 0 (16 bytes), span_id
// at offset 16 (8 bytes), valid at offset 24 (1 byte), attrs_data_size at
// offset 26 (2 bytes, little-endian), attrs_data at offset 28.
static VALUE native_debug_peek(VALUE _self) {
  struct ddog_ThreadContextHandle *ctx = ddog_otel_thread_ctx_detach();

  if (!ctx) return Qnil;

  const uint8_t *raw = (const uint8_t *) ctx;

  VALUE trace_id = rb_str_new((const char *) raw, 16);
  VALUE span_id = rb_str_new((const char *) (raw + 16), 8);
  VALUE valid = raw[24] ? Qtrue : Qfalse;
  uint16_t attrs_data_size = (uint16_t) raw[26] | ((uint16_t) raw[27] << 8);
  VALUE attrs = decode_attrs(raw + 28, attrs_data_size);

  // Must return NULL: we just detached the only attached context, and nothing else touches
  // this thread's slot in between.
  struct ddog_ThreadContextHandle *previous = ddog_otel_thread_ctx_attach(ctx);
  if (previous) raise_error(rb_eRuntimeError, "Internal: unexpected context already attached during debug_peek");

  VALUE result = rb_hash_new();
  rb_hash_aset(result, ID2SYM(rb_intern("trace_id")), trace_id);
  rb_hash_aset(result, ID2SYM(rb_intern("span_id")), span_id);
  rb_hash_aset(result, ID2SYM(rb_intern("valid")), valid);
  rb_hash_aset(result, ID2SYM(rb_intern("attrs")), attrs);

  return result;
}

#else

static VALUE native_set(DDTRACE_UNUSED VALUE _self, DDTRACE_UNUSED VALUE trace_id, DDTRACE_UNUSED VALUE span_id, DDTRACE_UNUSED VALUE local_root_span_id) {
  return Qfalse;
}

static VALUE native_enable(DDTRACE_UNUSED VALUE _self) {
  return Qfalse;
}

static VALUE native_detach_and_free(VALUE _self) {
  return Qfalse;
}

static VALUE native_supported_p(VALUE _self) {
  return Qfalse;
}

static VALUE native_debug_peek(VALUE _self) {
  return Qnil;
}

#endif
