#ifndef PSVITA_USB_AUDIO_DIAGNOSTICS_H
#define PSVITA_USB_AUDIO_DIAGNOSTICS_H

#include "audio_core.h"
#include "psvita_usb_audio_midi.h"

#include <stdint.h>

#define PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY 512u
#define PSVITA_USB_AUDIO_DIAG_EVENT_CAPACITY 64u
#define PSVITA_USB_AUDIO_DIAG_MARKER_CAPACITY 8u
#define PSVITA_USB_AUDIO_DIAG_EXPECTED_REQUEST_BYTES \
	PSVITA_USB_AUDIO_REQUEST_BYTES
#define PSVITA_USB_AUDIO_DIAG_EXPECTED_REQUESTS_PER_SECOND \
	(1000u / PSVITA_USB_AUDIO_REQUEST_INTERVALS)
#define PSVITA_USB_AUDIO_DIAG_EXPECTED_BYTES_PER_SECOND 960000u
#define PSVITA_USB_AUDIO_DIAG_LATE_COMPLETION_US \
	PSVITA_USB_AUDIO_LATE_COMPLETION_US

enum {
	PSVITA_USB_AUDIO_DIAG_EVENT_MANUAL_MARK = 1u << 0,
	PSVITA_USB_AUDIO_DIAG_EVENT_STREAM_START = 1u << 1,
	PSVITA_USB_AUDIO_DIAG_FAULT_FORMAT = 1u << 2,
	PSVITA_USB_AUDIO_DIAG_FAULT_REQUEST_SIZE = 1u << 3,
	PSVITA_USB_AUDIO_DIAG_FAULT_SHORT = 1u << 4,
	PSVITA_USB_AUDIO_DIAG_FAULT_LATE = 1u << 5,
	PSVITA_USB_AUDIO_DIAG_FAULT_SUBMIT = 1u << 6,
	PSVITA_USB_AUDIO_DIAG_FAULT_COMPLETION = 1u << 7,
	PSVITA_USB_AUDIO_DIAG_FAULT_BUFFER = 1u << 8,
	PSVITA_USB_AUDIO_DIAG_FAULT_STALL = 1u << 9,
	PSVITA_USB_AUDIO_DIAG_FAULT_CHANNELS = 1u << 10,
	PSVITA_USB_AUDIO_DIAG_FAULT_DISCONNECT = 1u << 11,
	PSVITA_USB_AUDIO_DIAG_FAULT_ZERO_BYTES = 1u << 12,
	PSVITA_USB_AUDIO_DIAG_EVENT_COUNTER_RESET = 1u << 13,
	PSVITA_USB_AUDIO_DIAG_FAULT_THROUGHPUT = 1u << 14,
	PSVITA_USB_AUDIO_DIAG_FAULT_NO_DATA = 1u << 15,
	PSVITA_USB_AUDIO_DIAG_FAULT_PRODUCER = 1u << 16
};

#define PSVITA_USB_AUDIO_DIAG_FAULT_MASK \
	(PSVITA_USB_AUDIO_DIAG_FAULT_FORMAT | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_REQUEST_SIZE | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_SHORT | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_LATE | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_SUBMIT | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_COMPLETION | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_BUFFER | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_STALL | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_CHANNELS | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_DISCONNECT | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_ZERO_BYTES | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_THROUGHPUT | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_NO_DATA | \
	 PSVITA_USB_AUDIO_DIAG_FAULT_PRODUCER)

typedef struct {
	uint32_t elapsed_ms;
	uint32_t event_flags;
	uint32_t status_flags;
	uint32_t buffered_frames;
	uint32_t produced_frames;
	uint32_t consumed_frames;
	uint32_t packets_submitted;
	uint32_t packets_completed;
	uint32_t bytes_transmitted;
	uint32_t output_underruns;
	uint32_t output_overruns;
	uint32_t submit_failures;
	uint32_t completion_errors;
	uint32_t disconnects;
	uint32_t stalled_requests;
	uint32_t recovered_requests;
	uint32_t cancel_failures;
	uint32_t zero_byte_completions;
	uint32_t short_completions;
	uint32_t late_completions;
	uint32_t rebuffer_events;
	uint32_t conceal_events;
	uint32_t concealed_frames;
	uint32_t consecutive_missing_frames;
	uint32_t requested_bytes;
	uint32_t completed_bytes;
	int32_t completion_result;
	uint32_t completion_gap_us;
	uint32_t rearm_delay_us;
	uint32_t worker_lock_wait_us;
	uint32_t channel_mask;
	uint32_t channel_peaks[PSVITA_USB_AUDIO_STREAM_CHANNELS];
	uint32_t producer_write_calls;
	uint32_t producer_write_failures;
	uint32_t producer_status_failures;
	uint32_t producer_late_wakes;
	uint32_t producer_pacing_resets;
	uint32_t producer_max_lateness_us;
	uint32_t producer_max_write_us;
} PsvitaUsbAudioDiagnosticSample;

typedef struct {
	PsvitaUsbAudioDiagnosticSample timeline[PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY];
	PsvitaUsbAudioDiagnosticSample events[PSVITA_USB_AUDIO_DIAG_EVENT_CAPACITY];
	PsvitaUsbAudioDiagnosticSample markers[PSVITA_USB_AUDIO_DIAG_MARKER_CAPACITY];
	uint32_t timeline_write;
	uint32_t timeline_count;
	uint32_t event_write;
	uint32_t event_count;
	uint32_t marker_write;
	uint32_t marker_count;
	uint32_t all_event_flags;
	uint32_t stream_sessions;
	uint32_t manual_marks;
	uint32_t start_ms;
	uint32_t steady_first_ms;
	uint32_t steady_last_ms;
	int have_previous;
	int have_steady;
	PsvitaUsbAudioStatus previous;
	PsvitaUsbAudioStatus steady_first;
	PsvitaUsbAudioStatus steady_last;
} PsvitaUsbAudioDiagnosticLog;

void psvita_usb_audio_diag_init(PsvitaUsbAudioDiagnosticLog *log,
	uint32_t start_ms);
void psvita_usb_audio_diag_record(PsvitaUsbAudioDiagnosticLog *log,
	uint32_t now_ms, const PsvitaUsbAudioStatus *status);
void psvita_usb_audio_diag_annotate_producer(PsvitaUsbAudioDiagnosticLog *log,
	uint32_t write_calls, uint32_t write_failures, uint32_t status_failures,
	uint32_t late_wakes, uint32_t pacing_resets, uint32_t max_lateness_us,
	uint32_t max_write_us);
void psvita_usb_audio_diag_mark(PsvitaUsbAudioDiagnosticLog *log,
	uint32_t now_ms);
uint32_t psvita_usb_audio_diag_failure_flags(
	const PsvitaUsbAudioDiagnosticLog *log);
uint32_t psvita_usb_audio_diag_request_rate(
	const PsvitaUsbAudioDiagnosticLog *log);
uint32_t psvita_usb_audio_diag_byte_rate(
	const PsvitaUsbAudioDiagnosticLog *log);
const PsvitaUsbAudioDiagnosticSample *psvita_usb_audio_diag_timeline_at(
	const PsvitaUsbAudioDiagnosticLog *log, uint32_t chronological_index);
const PsvitaUsbAudioDiagnosticSample *psvita_usb_audio_diag_event_at(
	const PsvitaUsbAudioDiagnosticLog *log, uint32_t chronological_index);
const PsvitaUsbAudioDiagnosticSample *psvita_usb_audio_diag_marker_at(
	const PsvitaUsbAudioDiagnosticLog *log, uint32_t chronological_index);

#endif
