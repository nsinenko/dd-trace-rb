# frozen_string_literal: true

module Datadog
  module Core
    # Publishes a per-thread OpenTelemetry context record (trace id, span id) into a
    # thread-local slot, so an out-of-process reader (e.g. the eBPF profiler) can discover it.
    # See the "OTel Thread Context" OTEP (open-telemetry/opentelemetry-specification#4947).
    #
    # APIs in this module are implemented as native code; see ext/libdatadog_api/otel_thread_ctx.c.
    # Linux-only: `supported?` is `false` everywhere else.
    module OTelThreadContext
      def self.supported?
        Datadog::Core::LIBDATADOG_API_FAILURE.nil? && _native_supported?
      end

      def self.enable!
        return false unless supported?

        _native_enable
      end

      def self.set(trace_id:, span_id:, local_root_span_id:)
        _native_set(trace_id, span_id, local_root_span_id)
      end

      # Debug helper: returns a Hash with the fields of the context record currently
      # attached to the calling thread (`trace_id`, `span_id`, `valid`, `attrs`), or
      # `nil` if no context is attached.
      def self.debug_peek
        record = _native_debug_peek
        return unless record

        record[:trace_id] = record[:trace_id].unpack1("H*").to_i(16)
        record[:span_id] = record[:span_id].unpack1("H*").to_i(16)
        record
      end
    end
  end
end
