#include "audio_diagnostics.h"

#include <string.h>

static void snapshot(PsvitaUsbAudioDiagnosticSample *sample, uint32_t elapsed_ms,
	const PsvitaUsbAudioStatus *status)
{
	memset(sample, 0, sizeof(*sample));
	sample->elapsed_ms = elapsed_ms;
	sample->status_flags = status->flags;
	sample->buffered_frames = status->buffered_frames;
	sample->produced_frames = status->produced_frames;
	sample->consumed_frames = status->consumed_frames;
	sample->packets_submitted = status->packets_submitted;
	sample->packets_completed = status->packets_completed;
	sample->bytes_transmitted = status->bytes_transmitted;
	sample->output_underruns = status->output_underruns;
	sample->output_overruns = status->output_overruns;
	sample->submit_failures = status->submit_failures;
	sample->completion_errors = status->completion_errors;
	sample->disconnects = status->disconnects;
	sample->stalled_requests = status->stalled_requests;
	sample->recovered_requests = status->recovered_requests;
	sample->cancel_failures = status->cancel_failures;
	sample->zero_byte_completions = status->zero_byte_completions;
	sample->short_completions = status->short_completions;
	sample->late_completions = status->late_completions;
	sample->rebuffer_events = status->rebuffer_events;
	sample->conceal_events = status->conceal_events;
	sample->concealed_frames = status->concealed_frames;
	sample->consecutive_missing_frames = status->consecutive_missing_frames;
	sample->requested_bytes = status->last_completion_requested_bytes;
	sample->completed_bytes = status->last_completion_bytes;
	sample->completion_result = status->last_completion_result;
	sample->completion_gap_us = status->last_completion_gap_us;
	sample->rearm_delay_us = status->last_rearm_delay_us;
	sample->worker_lock_wait_us = status->last_worker_lock_wait_us;
	sample->channel_mask = status->last_packet_channel_nonzero_mask;
	memcpy(sample->channel_peaks, status->last_packet_channel_peaks,
		sizeof(sample->channel_peaks));
}

static int counter_decreased(const PsvitaUsbAudioStatus *before,
	const PsvitaUsbAudioStatus *after)
{
	return after->packets_submitted < before->packets_submitted ||
		after->packets_completed < before->packets_completed ||
		after->bytes_transmitted < before->bytes_transmitted ||
		after->output_underruns < before->output_underruns ||
		after->output_overruns < before->output_overruns ||
		after->submit_failures < before->submit_failures ||
		after->completion_errors < before->completion_errors ||
		after->short_completions < before->short_completions ||
		after->late_completions < before->late_completions ||
		after->rebuffer_events < before->rebuffer_events ||
		after->conceal_events < before->conceal_events ||
		after->concealed_frames < before->concealed_frames;
}

static int format_is_bad(const PsvitaUsbAudioStatus *status)
{
	return status->protocol_version != PSVITA_USB_AUDIO_PROTOCOL_VERSION ||
		status->sample_rate != PSVITA_USB_AUDIO_SAMPLE_RATE ||
		status->channels != PSVITA_USB_AUDIO_STREAM_CHANNELS ||
		status->bit_depth != PSVITA_USB_AUDIO_BITS_PER_SAMPLE ||
		status->ring_capacity_frames != PSVITA_USB_AUDIO_RING_FRAMES;
}

static int request_size_is_valid(uint32_t bytes)
{
	uint32_t packet_bytes = PSVITA_USB_AUDIO_PACKET_BYTES(
		PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES);
	return bytes >= packet_bytes && bytes <= PSVITA_USB_AUDIO_REQUEST_BYTES &&
		(bytes % packet_bytes) == 0u;
}

static uint32_t request_late_us(uint32_t bytes)
{
	uint32_t packet_bytes = PSVITA_USB_AUDIO_PACKET_BYTES(
		PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES);
	uint32_t intervals = packet_bytes ? bytes / packet_bytes : 0u;
	return intervals > 0u && intervals <= PSVITA_USB_AUDIO_REQUEST_INTERVALS
		? (intervals + 1u) * 1000u
		: PSVITA_USB_AUDIO_DIAG_LATE_COMPLETION_US;
}

static void push_event(PsvitaUsbAudioDiagnosticLog *log,
	const PsvitaUsbAudioDiagnosticSample *sample)
{
	log->events[log->event_write] = *sample;
	log->event_write = (log->event_write + 1u) %
		PSVITA_USB_AUDIO_DIAG_EVENT_CAPACITY;
	if (log->event_count < PSVITA_USB_AUDIO_DIAG_EVENT_CAPACITY)
		log->event_count++;
}

void psvita_usb_audio_diag_init(PsvitaUsbAudioDiagnosticLog *log,
	uint32_t start_ms)
{
	memset(log, 0, sizeof(*log));
	log->start_ms = start_ms;
}

void psvita_usb_audio_diag_record(PsvitaUsbAudioDiagnosticLog *log,
	uint32_t now_ms, const PsvitaUsbAudioStatus *status)
{
	PsvitaUsbAudioDiagnosticSample *sample = &log->timeline[log->timeline_write];
	snapshot(sample, now_ms - log->start_ms, status);
	int streaming =
		(status->flags & PSVITA_USB_AUDIO_STATUS_HOST_STREAMING) != 0u;
	int was_streaming = log->have_previous &&
		(log->previous.flags & PSVITA_USB_AUDIO_STATUS_HOST_STREAMING) != 0u;
	int reset = log->have_previous && counter_decreased(&log->previous, status);
	if (reset) {
		sample->event_flags |= PSVITA_USB_AUDIO_DIAG_EVENT_COUNTER_RESET;
		log->have_steady = 0;
	}
	if (!was_streaming && streaming) {
		sample->event_flags |= PSVITA_USB_AUDIO_DIAG_EVENT_STREAM_START;
		log->stream_sessions++;
		log->have_steady = 0;
	}
	if (streaming && format_is_bad(status) &&
	    (!was_streaming || !format_is_bad(&log->previous)))
		sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_FORMAT;

	if (log->have_previous && !reset) {
		int completion_advanced = status->packets_completed !=
			log->previous.packets_completed;
		if (completion_advanced &&
		    (!request_size_is_valid(status->last_completion_requested_bytes) ||
		     status->last_completion_bytes !=
		         status->last_completion_requested_bytes))
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_REQUEST_SIZE;
		if (completion_advanced && status->last_completion_result != 0)
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_COMPLETION;
		if (completion_advanced &&
		    status->last_completion_gap_us >
		        request_late_us(status->last_completion_requested_bytes))
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_LATE;
		if (completion_advanced &&
		    status->last_packet_channel_nonzero_mask !=
		        ((1u << PSVITA_USB_AUDIO_STREAM_CHANNELS) - 1u))
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_CHANNELS;
		if (status->short_completions != log->previous.short_completions)
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_SHORT;
		if (status->late_completions != log->previous.late_completions)
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_LATE;
		if (status->submit_failures != log->previous.submit_failures)
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_SUBMIT;
		if (status->completion_errors != log->previous.completion_errors)
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_COMPLETION;
		if (status->output_underruns != log->previous.output_underruns ||
		    status->output_overruns != log->previous.output_overruns ||
		    status->rebuffer_events != log->previous.rebuffer_events)
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_BUFFER;
		if (status->stalled_requests != log->previous.stalled_requests ||
		    status->cancel_failures != log->previous.cancel_failures ||
		    status->stalled_requests > status->recovered_requests)
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_STALL;
		if (status->disconnects != log->previous.disconnects)
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_DISCONNECT;
		if (status->zero_byte_completions !=
		    log->previous.zero_byte_completions)
			sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_ZERO_BYTES;
	}
	log->all_event_flags |= sample->event_flags;
	if (sample->event_flags != 0u) push_event(log, sample);
	log->timeline_write = (log->timeline_write + 1u) %
		PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY;
	if (log->timeline_count < PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY)
		log->timeline_count++;
	log->previous = *status;
	log->have_previous = 1;
	if (streaming && status->stream_primed) {
		if (!log->have_steady) {
			log->steady_first = *status;
			log->steady_first_ms = now_ms;
			log->have_steady = 1;
		}
		log->steady_last = *status;
		log->steady_last_ms = now_ms;
	}
}

void psvita_usb_audio_diag_annotate_producer(PsvitaUsbAudioDiagnosticLog *log,
	uint32_t write_calls, uint32_t write_failures, uint32_t status_failures,
	uint32_t late_wakes, uint32_t pacing_resets, uint32_t max_lateness_us,
	uint32_t max_write_us)
{
	if (!log->timeline_count) return;
	uint32_t current_index = (log->timeline_write +
		PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY - 1u) %
		PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY;
	PsvitaUsbAudioDiagnosticSample *sample = &log->timeline[current_index];
	uint32_t previous_write_failures = 0u;
	uint32_t previous_status_failures = 0u;
	uint32_t previous_pacing_resets = 0u;
	if (log->timeline_count > 1u) {
		uint32_t previous_index = (current_index +
			PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY - 1u) %
			PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY;
		const PsvitaUsbAudioDiagnosticSample *previous =
			&log->timeline[previous_index];
		previous_write_failures = previous->producer_write_failures;
		previous_status_failures = previous->producer_status_failures;
		previous_pacing_resets = previous->producer_pacing_resets;
	}
	sample->producer_write_calls = write_calls;
	sample->producer_write_failures = write_failures;
	sample->producer_status_failures = status_failures;
	sample->producer_late_wakes = late_wakes;
	sample->producer_pacing_resets = pacing_resets;
	sample->producer_max_lateness_us = max_lateness_us;
	sample->producer_max_write_us = max_write_us;
	if (write_failures > previous_write_failures ||
	    status_failures > previous_status_failures ||
	    pacing_resets > previous_pacing_resets) {
		sample->event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_PRODUCER;
		log->all_event_flags |= PSVITA_USB_AUDIO_DIAG_FAULT_PRODUCER;
		if (log->event_count) {
			uint32_t latest_event = (log->event_write +
				PSVITA_USB_AUDIO_DIAG_EVENT_CAPACITY - 1u) %
				PSVITA_USB_AUDIO_DIAG_EVENT_CAPACITY;
			if (log->events[latest_event].elapsed_ms == sample->elapsed_ms) {
				log->events[latest_event] = *sample;
				return;
			}
		}
		push_event(log, sample);
	}
}

void psvita_usb_audio_diag_mark(PsvitaUsbAudioDiagnosticLog *log,
	uint32_t now_ms)
{
	if (!log->have_previous) return;
	PsvitaUsbAudioDiagnosticSample sample;
	if (log->timeline_count) {
		uint32_t latest = (log->timeline_write +
			PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY - 1u) %
			PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY;
		sample = log->timeline[latest];
		sample.elapsed_ms = now_ms - log->start_ms;
	} else {
		snapshot(&sample, now_ms - log->start_ms, &log->previous);
	}
	sample.event_flags = PSVITA_USB_AUDIO_DIAG_EVENT_MANUAL_MARK;
	log->markers[log->marker_write] = sample;
	log->marker_write = (log->marker_write + 1u) %
		PSVITA_USB_AUDIO_DIAG_MARKER_CAPACITY;
	if (log->marker_count < PSVITA_USB_AUDIO_DIAG_MARKER_CAPACITY)
		log->marker_count++;
	log->all_event_flags |= sample.event_flags;
	log->manual_marks++;
}

static uint32_t rate(uint32_t first, uint32_t last, uint32_t elapsed_ms)
{
	if (!elapsed_ms || last < first) return 0u;
	return (uint32_t)(((uint64_t)(last - first) * 1000u + elapsed_ms / 2u) /
		elapsed_ms);
}

uint32_t psvita_usb_audio_diag_failure_flags(
	const PsvitaUsbAudioDiagnosticLog *log)
{
	uint32_t failures = log->all_event_flags & PSVITA_USB_AUDIO_DIAG_FAULT_MASK;
	if (!log->have_steady) return failures | PSVITA_USB_AUDIO_DIAG_FAULT_NO_DATA;
	if (format_is_bad(&log->steady_last))
		failures |= PSVITA_USB_AUDIO_DIAG_FAULT_FORMAT;
	if (log->steady_last.packets_completed > log->steady_first.packets_completed &&
	    (!request_size_is_valid(
	         log->steady_last.last_completion_requested_bytes) ||
	     log->steady_last.last_completion_bytes !=
	         log->steady_last.last_completion_requested_bytes))
		failures |= PSVITA_USB_AUDIO_DIAG_FAULT_REQUEST_SIZE;
	if (log->steady_last.packets_completed > 0u &&
	    log->steady_last.last_packet_channel_nonzero_mask !=
	        ((1u << PSVITA_USB_AUDIO_STREAM_CHANNELS) - 1u))
		failures |= PSVITA_USB_AUDIO_DIAG_FAULT_CHANNELS;
	uint32_t elapsed = log->steady_last_ms - log->steady_first_ms;
	uint32_t requests = psvita_usb_audio_diag_request_rate(log);
	uint32_t bytes = psvita_usb_audio_diag_byte_rate(log);
	if (elapsed >= 1000u) {
		if (!requests || !bytes)
			failures |= PSVITA_USB_AUDIO_DIAG_FAULT_NO_DATA;
		else if (requests < PSVITA_USB_AUDIO_DIAG_EXPECTED_REQUESTS_PER_SECOND * 85u / 100u ||
		         requests > PSVITA_USB_AUDIO_DIAG_EXPECTED_REQUESTS_PER_SECOND * 115u / 100u ||
		         bytes < PSVITA_USB_AUDIO_DIAG_EXPECTED_BYTES_PER_SECOND * 85u / 100u ||
		         bytes > PSVITA_USB_AUDIO_DIAG_EXPECTED_BYTES_PER_SECOND * 115u / 100u)
			failures |= PSVITA_USB_AUDIO_DIAG_FAULT_THROUGHPUT;
	}
	return failures;
}

uint32_t psvita_usb_audio_diag_request_rate(
	const PsvitaUsbAudioDiagnosticLog *log)
{
	if (!log->have_steady) return 0u;
	return rate(log->steady_first.packets_completed,
		log->steady_last.packets_completed,
		log->steady_last_ms - log->steady_first_ms);
}

uint32_t psvita_usb_audio_diag_byte_rate(
	const PsvitaUsbAudioDiagnosticLog *log)
{
	if (!log->have_steady) return 0u;
	return rate(log->steady_first.bytes_transmitted,
		log->steady_last.bytes_transmitted,
		log->steady_last_ms - log->steady_first_ms);
}

static const PsvitaUsbAudioDiagnosticSample *ring_at(
	const PsvitaUsbAudioDiagnosticSample *ring, uint32_t capacity,
	uint32_t write, uint32_t count, uint32_t index)
{
	if (index >= count) return NULL;
	uint32_t first = count == capacity ? write : 0u;
	return &ring[(first + index) % capacity];
}

const PsvitaUsbAudioDiagnosticSample *psvita_usb_audio_diag_timeline_at(
	const PsvitaUsbAudioDiagnosticLog *log, uint32_t chronological_index)
{
	return ring_at(log->timeline, PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY,
		log->timeline_write, log->timeline_count, chronological_index);
}

const PsvitaUsbAudioDiagnosticSample *psvita_usb_audio_diag_event_at(
	const PsvitaUsbAudioDiagnosticLog *log, uint32_t chronological_index)
{
	return ring_at(log->events, PSVITA_USB_AUDIO_DIAG_EVENT_CAPACITY,
		log->event_write, log->event_count, chronological_index);
}

const PsvitaUsbAudioDiagnosticSample *psvita_usb_audio_diag_marker_at(
	const PsvitaUsbAudioDiagnosticLog *log, uint32_t chronological_index)
{
	return ring_at(log->markers, PSVITA_USB_AUDIO_DIAG_MARKER_CAPACITY,
		log->marker_write, log->marker_count, chronological_index);
}
