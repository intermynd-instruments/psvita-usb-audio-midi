#ifndef PSVITA_USB_AUDIO_MIDI_H
#define PSVITA_USB_AUDIO_MIDI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSVITA_USB_AUDIO_MIDI_API_VERSION 0x00020000u
#define PSVITA_USB_AUDIO_MIDI_DIAGNOSTICS_VERSION 0x00020000u
#define PSVITA_USB_AUDIO_MIDI_BUILD_ID 0x26072601u
#define PSVITA_USB_AUDIO_PROTOCOL_VERSION 0x00010010u
#define PSVITA_USB_AUDIO_SAMPLE_RATE 48000u
/* The original write call remains stereo for source compatibility. The USB
 * stream exposes master L/R followed by eight discrete track channels. */
#define PSVITA_USB_AUDIO_CHANNELS 2u
#define PSVITA_USB_AUDIO_TRACK_CHANNELS 8u
#define PSVITA_USB_AUDIO_STREAM_CHANNELS 10u
#define PSVITA_USB_AUDIO_BITS_PER_SAMPLE 16u
#define PSVITA_USB_AUDIO_RING_FRAMES 8192u
#define PSVITA_USB_AUDIO_MAX_WRITE_FRAMES 512u
#define PSVITA_USB_AUDIO_INPUT_CHANNELS 2u
#define PSVITA_USB_AUDIO_INPUT_RING_FRAMES 8192u
#define PSVITA_USB_AUDIO_INPUT_MAX_READ_FRAMES 512u
#define PSVITA_USB_AUDIO_PRIME_FRAMES 6144u
#define PSVITA_USB_AUDIO_MIDI_TRACE_CAPACITY 32u
#define PSVITA_USB_MIDI_LATENCY_BUCKETS 8u
#define PSVITA_USB_AUDIO_MIDI_ACQUIRE_REPLACE_MTP (1u << 0)
#define PSVITA_USB_AUDIO_MIDI_STATUS_STOCK_SNAPSHOT (1u << 8)
#define PSVITA_USB_AUDIO_MIDI_STATUS_CONTROLLER_STARTED (1u << 9)
#define PSVITA_USB_AUDIO_MIDI_STATUS_CONTROLLER_REGISTERED (1u << 10)
#define PSVITA_USB_AUDIO_MIDI_STATUS_MTP_STARTED (1u << 11)
#define PSVITA_USB_AUDIO_MIDI_STATUS_MTP_REGISTERED (1u << 12)
#define PSVITA_USB_AUDIO_MIDI_STATUS_USB_ACTIVATED (1u << 13)
#define PSVITA_USB_AUDIO_MIDI_STATUS_USB_DEACTIVATED (1u << 14)
#define PSVITA_USB_AUDIO_MIDI_STATUS_CABLE_CONNECTED (1u << 15)
#define PSVITA_USB_AUDIO_MIDI_STATUS_CABLE_DISCONNECTED (1u << 16)
#define PSVITA_USB_AUDIO_MIDI_STATUS_PSP_COMM_STARTED (1u << 17)
#define PSVITA_USB_AUDIO_MIDI_STATUS_SERIAL_STARTED (1u << 18)
#define PSVITA_USB_AUDIO_MIDI_STATUS_TAKEOVER_STEP_SHIFT 20u
#define PSVITA_USB_AUDIO_MIDI_STATUS_TAKEOVER_STEP_MASK (0x0fu << PSVITA_USB_AUDIO_MIDI_STATUS_TAKEOVER_STEP_SHIFT)
#define PSVITA_USB_AUDIO_MIDI_STATUS_CONFIGURED (1u << 24)
#define PSVITA_USB_AUDIO_MIDI_STATUS_RX_ARMED (1u << 25)
#define PSVITA_USB_AUDIO_MIDI_STATUS_CONNECTED (1u << 31)
#define PSVITA_USB_MIDI_MAX_EVENTS 64u

#define PSVITA_USB_AUDIO_MIDI_ERROR_NOT_READY ((int32_t)0x805d0001u)
#define PSVITA_USB_AUDIO_MIDI_ERROR_VERSION ((int32_t)0x805d0002u)
#define PSVITA_USB_AUDIO_MIDI_ERROR_FLAGS ((int32_t)0x805d0003u)
#define PSVITA_USB_AUDIO_MIDI_ERROR_BUSY ((int32_t)0x805d0004u)
#define PSVITA_USB_AUDIO_MIDI_ERROR_USB_STATE ((int32_t)0x805d0005u)
#define PSVITA_USB_AUDIO_MIDI_ERROR_CONFLICT ((int32_t)0x805d0006u)
#define PSVITA_USB_AUDIO_MIDI_ERROR_NOT_OWNER ((int32_t)0x805d0007u)
#define PSVITA_USB_AUDIO_MIDI_ERROR_BOUNDS ((int32_t)0x805d0008u)
#define PSVITA_USB_AUDIO_MIDI_ERROR_COPY ((int32_t)0x805d0009u)
#define PSVITA_USB_AUDIO_MIDI_ERROR_FORMAT ((int32_t)0x805d000au)

#define PSVITA_USB_AUDIO_STATUS_ENABLED (1u << 0)
#define PSVITA_USB_AUDIO_STATUS_HOST_CONNECTED (1u << 1)
#define PSVITA_USB_AUDIO_STATUS_CONFIGURED (1u << 2)
#define PSVITA_USB_AUDIO_STATUS_HOST_STREAMING (1u << 3)
#define PSVITA_USB_AUDIO_STATUS_REQUEST_PENDING (1u << 4)
#define PSVITA_USB_AUDIO_INPUT_STATUS_ENABLED (1u << 0)
#define PSVITA_USB_AUDIO_INPUT_STATUS_HOST_CONNECTED (1u << 1)
#define PSVITA_USB_AUDIO_INPUT_STATUS_CONFIGURED (1u << 2)
#define PSVITA_USB_AUDIO_INPUT_STATUS_HOST_STREAMING (1u << 3)
#define PSVITA_USB_AUDIO_INPUT_STATUS_REQUEST_PENDING (1u << 4)

typedef enum {
	PSVITA_USB_AUDIO_MIDI_STATE_UNAVAILABLE = 0,
	PSVITA_USB_AUDIO_MIDI_STATE_IDLE = 1,
	PSVITA_USB_AUDIO_MIDI_STATE_ACQUIRING = 2,
	PSVITA_USB_AUDIO_MIDI_STATE_ACTIVE = 3,
	PSVITA_USB_AUDIO_MIDI_STATE_RELEASING = 4,
	PSVITA_USB_AUDIO_MIDI_STATE_ERROR = 5
} PsvitaUsbAudioMidiState;

typedef enum {
	PSVITA_USB_AUDIO_STATE_DISABLED = 0,
	PSVITA_USB_AUDIO_STATE_IDLE = 1,
	PSVITA_USB_AUDIO_STATE_CONNECTED = 2,
	PSVITA_USB_AUDIO_STATE_STREAMING = 3,
	PSVITA_USB_AUDIO_STATE_ERROR = 4
} PsvitaUsbAudioState;

typedef enum {
	PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_NONE = 0,
	PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_DEACTIVATE = 1,
	PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_STOP_MTP = 2,
	PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_STOP_PSP_COMM = 3,
	PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_STOP_SERIAL = 4,
	PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_STOP_CONTROLLER = 5,
	PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_START_CONTROLLER = 6,
	PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_START_MIDI = 7,
	PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_ACTIVATE_MIDI = 8
} PsvitaUsbAudioMidiTakeoverStep;

typedef enum {
	PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_FORWARD = 1,
	PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_ROLLBACK = 2,
	PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_RESTORE = 3
} PsvitaUsbAudioMidiTracePhase;

typedef enum {
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_DEACTIVATE = 1,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_MTP = 2,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_PSP_COMM = 3,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_SERIAL = 4,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_CONTROLLER = 5,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_CONTROLLER = 6,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_MIDI = 7,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_ACTIVATE_MIDI = 8,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_MIDI = 9,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_MTP = 10,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_PSP_COMM = 11,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_SERIAL = 12,
	PSVITA_USB_AUDIO_MIDI_TRACE_OP_ACTIVATE_MTP = 13
} PsvitaUsbAudioMidiTraceOperation;

typedef struct {
	uint8_t phase;
	uint8_t operation;
	uint16_t reserved;
	int32_t result;
} PsvitaUsbAudioMidiTraceEntry;

typedef struct {
	uint64_t timestamp_us;
	uint8_t cable;
	uint8_t size;
	uint8_t data[3];
	uint8_t reserved[3];
} PsvitaUsbMidiEvent;

typedef struct {
	uint32_t size;
	uint32_t api_version;
	uint32_t state;
	uint32_t flags;
	int32_t last_error;
	uint32_t rx_dropped;
	uint32_t tx_dropped;
	uint32_t filtered;
} PsvitaUsbAudioMidiStatus;

typedef struct {
	uint32_t size;
	uint32_t diagnostics_version;
	uint32_t build_id;
	uint32_t sequence;
	int32_t acquire_result;
	int32_t release_result;
	int32_t controller_state;
	int32_t mtp_state;
	int32_t psp_comm_state;
	int32_t serial_state;
	int32_t device_state;
	uint32_t entry_count;
	PsvitaUsbAudioMidiTraceEntry entries[PSVITA_USB_AUDIO_MIDI_TRACE_CAPACITY];
	uint32_t rx_completed;
	uint32_t tx_scheduled;
	uint32_t rx_latency_histogram[PSVITA_USB_MIDI_LATENCY_BUCKETS];
	uint32_t tx_jitter_histogram[PSVITA_USB_MIDI_LATENCY_BUCKETS];
	uint32_t rx_submit_attempts;
	uint32_t rx_submit_failures;
	uint32_t rx_callback_count;
	uint32_t rx_callback_bytes;
	int32_t rx_last_submit_result;
	int32_t rx_last_callback_result;
	uint32_t rx_filtered_packets;
	uint32_t rx_malformed_packets;
	uint32_t tx_filtered_events;
	uint32_t rx_last_transfer_length;
	uint32_t rx_last_packet_word;
} PsvitaUsbAudioMidiDiagnostics;

typedef struct {
	uint32_t size;
	uint32_t protocol_version;
	uint32_t state;
	uint32_t flags;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t bit_depth;
	uint32_t ring_capacity_frames;
	uint32_t buffered_frames;
	uint32_t last_packet_frames;
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
	uint32_t maximum_buffered_frames;
	uint32_t minimum_buffered_frames;
	uint32_t packets_47_frames;
	uint32_t packets_48_frames;
	uint32_t packets_49_frames;
	int32_t last_error;
	int32_t endpoint_driver_number;
	int32_t submit_endpoint_number;
	int32_t endpoint_number;
	uint32_t endpoint_transmitted_bytes;
	uint32_t request_attributes;
	uint32_t set_interface_requests;
	uint32_t change_setting_callbacks;
	uint32_t fallback_starts;
	uint32_t stalled_requests;
	uint32_t recovered_requests;
	uint32_t cancel_failures;
	uint32_t pending_age_ms;
	int32_t requested_alternate;
	int32_t applied_alternate;
	uint32_t last_packet_peak;
	int32_t last_packet_first_left;
	int32_t last_packet_first_right;
	uint32_t nonzero_packets;
	uint32_t zero_byte_completions;
	int32_t last_completion_result;
	uint32_t last_completion_bytes;
	uint32_t last_packet_channel_nonzero_mask;
	uint32_t last_packet_channel_peaks[PSVITA_USB_AUDIO_STREAM_CHANNELS];
	uint32_t stream_primed;
	uint32_t priming_packets;
	uint32_t rebuffer_events;
	uint32_t last_completion_requested_bytes;
	uint32_t short_completions;
	uint32_t late_completions;
	uint32_t last_completion_gap_us;
	uint32_t maximum_completion_gap_us;
	uint32_t last_rearm_delay_us;
	uint32_t maximum_rearm_delay_us;
	uint32_t last_worker_lock_wait_us;
	uint32_t maximum_worker_lock_wait_us;
	uint32_t conceal_events;
	uint32_t concealed_frames;
	uint32_t consecutive_missing_frames;
} PsvitaUsbAudioStatus;

typedef struct {
	uint32_t size;
	uint32_t protocol_version;
	uint32_t state;
	uint32_t flags;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t bit_depth;
	uint32_t ring_capacity_frames;
	uint32_t buffered_frames;
	uint32_t received_frames;
	uint32_t read_frames;
	uint32_t packets_submitted;
	uint32_t packets_completed;
	uint32_t bytes_received;
	uint32_t overruns;
	uint32_t malformed_packets;
	uint32_t short_packets;
	uint32_t completion_errors;
	uint32_t submit_failures;
	uint32_t disconnects;
	uint32_t maximum_buffered_frames;
	uint32_t minimum_buffered_frames;
	uint32_t last_packet_frames;
	uint32_t last_packet_bytes;
	int32_t last_error;
	int32_t endpoint_driver_number;
	int32_t endpoint_number;
	uint32_t endpoint_received_bytes;
	int32_t requested_alternate;
	int32_t applied_alternate;
} PsvitaUsbAudioInputStatus;

int psvitaUsbAudioMidiClientLoad(const char *suprx_path);
int psvitaUsbAudioMidiGetApiVersion(void);
int psvitaUsbAudioMidiAcquire(uint32_t flags);
int psvitaUsbAudioMidiRelease(void);
int psvitaUsbMidiRead(PsvitaUsbMidiEvent *events, uint32_t capacity);
int psvitaUsbMidiReadWait(PsvitaUsbMidiEvent *events, uint32_t capacity,
	uint32_t timeout_us);
int psvitaUsbMidiWrite(const PsvitaUsbMidiEvent *events, uint32_t count);
int psvitaUsbAudioMidiGetStatus(PsvitaUsbAudioMidiStatus *status);
int psvitaUsbAudioMidiGetDiagnostics(PsvitaUsbAudioMidiDiagnostics *diagnostics);
int psvitaUsbAudioSetEnabled(int enabled);
int psvitaUsbAudioWrite(const int16_t *interleaved_stereo, uint32_t frames,
	uint32_t sample_rate);
int psvitaUsbAudioWriteMulti(const int16_t *interleaved, uint32_t frames,
	uint32_t sample_rate, uint32_t channels);
int psvitaUsbAudioGetStatus(PsvitaUsbAudioStatus *status);
int psvitaUsbAudioInputRead(int16_t *interleaved_stereo,
	uint32_t capacity_frames);
int psvitaUsbAudioGetInputStatus(PsvitaUsbAudioInputStatus *status);

#ifdef __cplusplus
}
#endif

#endif
