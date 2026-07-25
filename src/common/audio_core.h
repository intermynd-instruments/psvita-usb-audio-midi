#ifndef PSVITA_USB_AUDIO_CORE_H
#define PSVITA_USB_AUDIO_CORE_H

#include "psvita_usb_audio_midi.h"

#include <stdint.h>

#define PSVITA_USB_AUDIO_MAX_CHANNELS PSVITA_USB_AUDIO_STREAM_CHANNELS
/* Vita SceUdcd stalls the isochronous endpoint if requests are queued ahead. */
#define PSVITA_USB_AUDIO_MAX_IN_FLIGHT_REQUESTS 1u
#define PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES 48u
#define PSVITA_USB_AUDIO_MIN_PACKET_FRAMES 48u
#define PSVITA_USB_AUDIO_MAX_PACKET_FRAMES 48u
#define PSVITA_USB_AUDIO_REQUEST_INTERVALS 8u
#define PSVITA_USB_AUDIO_REQUEST_FRAMES \
	(PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES * PSVITA_USB_AUDIO_REQUEST_INTERVALS)
#define PSVITA_USB_AUDIO_REBUFFER_MISSING_FRAMES PSVITA_USB_AUDIO_REQUEST_FRAMES
#define PSVITA_USB_AUDIO_DMA_BUFFER_BYTES 8192u
#define PSVITA_USB_AUDIO_LATE_COMPLETION_US \
	((PSVITA_USB_AUDIO_REQUEST_INTERVALS + 1u) * 1000u)
#define PSVITA_USB_AUDIO_PACKET_BYTES(frames) \
	((frames) * PSVITA_USB_AUDIO_STREAM_CHANNELS * sizeof(int16_t))
#define PSVITA_USB_AUDIO_REQUEST_BYTES \
	PSVITA_USB_AUDIO_PACKET_BYTES(PSVITA_USB_AUDIO_REQUEST_FRAMES)

typedef struct {
	int16_t pcm[PSVITA_USB_AUDIO_RING_FRAMES * PSVITA_USB_AUDIO_MAX_CHANNELS];
	uint32_t read_frame;
	uint32_t write_frame;
	uint32_t channels;
	uint32_t produced_frames;
	uint32_t consumed_frames;
	uint32_t underruns;
	uint32_t overruns;
	uint32_t maximum_occupancy;
	uint32_t minimum_occupancy;
	uint32_t write_transactions;
} PsvitaUsbAudioRing;

typedef int (*PsvitaUsbAudioCopyFn)(void *context, void *destination,
	const void *source, uint32_t bytes);

typedef struct {
	uint32_t packet_index;
	uint32_t primed;
	uint32_t priming_packets;
	uint32_t rebuffer_events;
	uint32_t consecutive_missing_frames;
	uint32_t last_frame_valid;
	int16_t last_frame[PSVITA_USB_AUDIO_MAX_CHANNELS];
	uint32_t conceal_events;
	uint32_t concealed_frames;
} PsvitaUsbAudioPacketClock;

typedef struct {
	uint32_t read_frame;
	uint32_t consumed_frames;
	uint32_t underruns;
	uint32_t minimum_occupancy;
	PsvitaUsbAudioPacketClock clock;
} PsvitaUsbAudioReadCheckpoint;

typedef struct {
	uint32_t last_timestamp_us;
	uint32_t last_gap_us;
	uint32_t maximum_gap_us;
	uint32_t short_completions;
	uint32_t late_completions;
} PsvitaUsbAudioCompletionClock;

enum {
	PSVITA_USB_AUDIO_REQUEST_FREE = 0,
	PSVITA_USB_AUDIO_REQUEST_PREPARING,
	PSVITA_USB_AUDIO_REQUEST_PENDING,
	PSVITA_USB_AUDIO_REQUEST_CANCELING,
	PSVITA_USB_AUDIO_REQUEST_COMPLETING
};

typedef struct {
	volatile uint32_t state;
	uint32_t started_us;
} PsvitaUsbAudioRequestState;

void psvita_usb_audio_ring_init(PsvitaUsbAudioRing *ring, uint32_t channels);
uint32_t psvita_usb_audio_ring_available(const PsvitaUsbAudioRing *ring);
uint32_t psvita_usb_audio_ring_write(PsvitaUsbAudioRing *ring,
	const int16_t *interleaved, uint32_t frames);
int psvita_usb_audio_copy_frames(int16_t *scratch, uint32_t scratch_frames,
	const int16_t *source, uint32_t frames, uint32_t channels,
	PsvitaUsbAudioCopyFn copy, void *copy_context);
uint32_t psvita_usb_audio_ring_read_silence(PsvitaUsbAudioRing *ring,
	int16_t *interleaved, uint32_t frames);
uint32_t psvita_usb_audio_ring_read_packet(PsvitaUsbAudioRing *ring,
	PsvitaUsbAudioPacketClock *clock, int16_t *interleaved, uint32_t frames);
void psvita_usb_audio_read_checkpoint(const PsvitaUsbAudioRing *ring,
	const PsvitaUsbAudioPacketClock *clock,
	PsvitaUsbAudioReadCheckpoint *checkpoint);
void psvita_usb_audio_read_rollback(PsvitaUsbAudioRing *ring,
	PsvitaUsbAudioPacketClock *clock,
	const PsvitaUsbAudioReadCheckpoint *checkpoint);
void psvita_usb_audio_expand_stereo(int16_t *stream,
	const int16_t *stereo, uint32_t frames);
uint32_t psvita_usb_audio_measure_channels(const int16_t *interleaved,
	uint32_t frames, uint32_t channels, uint32_t *peaks,
	uint32_t peak_capacity);
void psvita_usb_audio_packet_clock_init(PsvitaUsbAudioPacketClock *clock);
uint32_t psvita_usb_audio_packet_frames(PsvitaUsbAudioPacketClock *clock,
	uint32_t occupancy);
uint32_t psvita_usb_audio_request_intervals(uint32_t occupancy);
void psvita_usb_audio_completion_clock_init(
	PsvitaUsbAudioCompletionClock *clock);
void psvita_usb_audio_completion_record(PsvitaUsbAudioCompletionClock *clock,
	uint32_t now_us, uint32_t requested_bytes, uint32_t transmitted_bytes,
	int32_t result);
void psvita_usb_audio_request_init(PsvitaUsbAudioRequestState *request);
int psvita_usb_audio_request_try_prepare(PsvitaUsbAudioRequestState *request);
void psvita_usb_audio_request_publish(PsvitaUsbAudioRequestState *request,
	uint32_t started_us);
int psvita_usb_audio_request_begin_completion(
	PsvitaUsbAudioRequestState *request, int *canceled);
void psvita_usb_audio_request_finish_completion(
	PsvitaUsbAudioRequestState *request);
int psvita_usb_audio_request_try_cancel(PsvitaUsbAudioRequestState *request);
void psvita_usb_audio_request_cancel_failed(PsvitaUsbAudioRequestState *request);
void psvita_usb_audio_request_abort_submit(PsvitaUsbAudioRequestState *request);
uint32_t psvita_usb_audio_request_state(
	const PsvitaUsbAudioRequestState *request);
int psvita_usb_audio_request_is_stalled(
	const PsvitaUsbAudioRequestState *request, uint32_t now_us,
	uint32_t stall_us);

#endif
