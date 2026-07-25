#include "audio_core.h"

#include <string.h>

static void update_occupancy(PsvitaUsbAudioRing *ring)
{
	uint32_t occupancy = psvita_usb_audio_ring_available(ring);
	if (occupancy > ring->maximum_occupancy) ring->maximum_occupancy = occupancy;
	if (occupancy < ring->minimum_occupancy) ring->minimum_occupancy = occupancy;
}

void psvita_usb_audio_ring_init(PsvitaUsbAudioRing *ring, uint32_t channels)
{
	if (!ring) return;
	memset(ring, 0, sizeof(*ring));
	ring->channels = channels > 0u && channels <= PSVITA_USB_AUDIO_MAX_CHANNELS
		? channels : PSVITA_USB_AUDIO_STREAM_CHANNELS;
	ring->minimum_occupancy = PSVITA_USB_AUDIO_RING_FRAMES;
}

uint32_t psvita_usb_audio_ring_available(const PsvitaUsbAudioRing *ring)
{
	if (!ring) return 0;
	uint32_t available = ring->write_frame - ring->read_frame;
	return available <= PSVITA_USB_AUDIO_RING_FRAMES
		? available : PSVITA_USB_AUDIO_RING_FRAMES;
}

uint32_t psvita_usb_audio_ring_write(PsvitaUsbAudioRing *ring,
	const int16_t *interleaved, uint32_t frames)
{
	if (!ring || !interleaved || !frames) return 0;
	uint32_t channels = ring->channels;
	if (frames > PSVITA_USB_AUDIO_RING_FRAMES) {
		interleaved += (frames - PSVITA_USB_AUDIO_RING_FRAMES) * channels;
		frames = PSVITA_USB_AUDIO_RING_FRAMES;
		ring->overruns++;
	}
	uint32_t available = psvita_usb_audio_ring_available(ring);
	uint32_t free_frames = PSVITA_USB_AUDIO_RING_FRAMES - available;
	if (frames > free_frames) {
		ring->read_frame += frames - free_frames;
		ring->overruns++;
	}
	uint32_t write_index = ring->write_frame % PSVITA_USB_AUDIO_RING_FRAMES;
	uint32_t first = PSVITA_USB_AUDIO_RING_FRAMES - write_index;
	if (first > frames) first = frames;
	memcpy(&ring->pcm[write_index * channels], interleaved,
		first * channels * sizeof(int16_t));
	if (first < frames)
		memcpy(ring->pcm, &interleaved[first * channels],
			(frames - first) * channels * sizeof(int16_t));
	ring->write_frame += frames;
	ring->produced_frames += frames;
	ring->write_transactions++;
	update_occupancy(ring);
	return frames;
}

int psvita_usb_audio_copy_frames(int16_t *scratch, uint32_t scratch_frames,
	const int16_t *source, uint32_t frames, uint32_t channels,
	PsvitaUsbAudioCopyFn copy, void *copy_context)
{
	if (!scratch || !source || !frames || frames > scratch_frames ||
	    !channels || channels > PSVITA_USB_AUDIO_MAX_CHANNELS || !copy)
		return -1;
	uint32_t bytes = frames * channels * sizeof(scratch[0]);
	int copy_result = copy(copy_context, scratch, source, bytes);
	if (copy_result < 0) return copy_result;
	return (int)frames;
}

uint32_t psvita_usb_audio_ring_read_silence(PsvitaUsbAudioRing *ring,
	int16_t *interleaved, uint32_t frames)
{
	if (!ring || !interleaved || !frames) return 0;
	uint32_t channels = ring->channels;
	uint32_t available = psvita_usb_audio_ring_available(ring);
	uint32_t valid = available < frames ? available : frames;
	uint32_t read_index = ring->read_frame % PSVITA_USB_AUDIO_RING_FRAMES;
	uint32_t first = PSVITA_USB_AUDIO_RING_FRAMES - read_index;
	if (first > valid) first = valid;
	memcpy(interleaved, &ring->pcm[read_index * channels],
		first * channels * sizeof(int16_t));
	if (first < valid)
		memcpy(&interleaved[first * channels], ring->pcm,
			(valid - first) * channels * sizeof(int16_t));
	if (valid < frames) {
		memset(&interleaved[valid * channels], 0,
			(frames - valid) * channels * sizeof(int16_t));
		ring->underruns++;
	}
	ring->read_frame += valid;
	ring->consumed_frames += valid;
	update_occupancy(ring);
	return valid;
}

uint32_t psvita_usb_audio_ring_read_packet(PsvitaUsbAudioRing *ring,
	PsvitaUsbAudioPacketClock *clock, int16_t *interleaved, uint32_t frames)
{
	if (!ring || !clock || !interleaved || !frames) return 0;
	uint32_t channels = ring->channels;
	uint32_t available = psvita_usb_audio_ring_available(ring);
	if (!clock->primed) {
		if (available < PSVITA_USB_AUDIO_PRIME_FRAMES) {
			memset(interleaved, 0, frames * channels * sizeof(interleaved[0]));
			clock->priming_packets++;
			return 0;
		}
		clock->primed = 1u;
	}
	uint32_t valid = psvita_usb_audio_ring_read_silence(ring, interleaved,
		frames);
	if (valid) {
		for (uint32_t channel = 0; channel < channels; ++channel)
			clock->last_frame[channel] = interleaved[
				(valid - 1u) * channels + channel];
		clock->last_frame_valid = 1u;
	}
	if (valid < frames) {
		/* A callback and a 1 ms USB request can cross with only a handful of
		 * frames missing. Hold the last sample through that short race instead
		 * of amplifying it into a full re-prime gap. A genuinely absent
		 * producer still re-primes after one normal 8 ms request worth of
		 * consecutive missing audio. */
		if (clock->last_frame_valid) {
			for (uint32_t frame = valid; frame < frames; ++frame)
				for (uint32_t channel = 0; channel < channels; ++channel)
					interleaved[frame * channels + channel] =
						clock->last_frame[channel];
		}
		uint32_t missing = frames - valid;
		clock->conceal_events++;
		clock->concealed_frames += missing;
		if (clock->consecutive_missing_frames > 0xffffffffu - missing)
			clock->consecutive_missing_frames = 0xffffffffu;
		else
			clock->consecutive_missing_frames += missing;
		if (clock->consecutive_missing_frames >=
		    PSVITA_USB_AUDIO_REBUFFER_MISSING_FRAMES) {
			clock->primed = 0u;
			clock->priming_packets++;
			clock->rebuffer_events++;
			clock->consecutive_missing_frames = 0u;
		}
	} else {
		clock->consecutive_missing_frames = 0u;
	}
	return valid;
}

void psvita_usb_audio_read_checkpoint(const PsvitaUsbAudioRing *ring,
	const PsvitaUsbAudioPacketClock *clock,
	PsvitaUsbAudioReadCheckpoint *checkpoint)
{
	if (!ring || !clock || !checkpoint) return;
	checkpoint->read_frame = ring->read_frame;
	checkpoint->consumed_frames = ring->consumed_frames;
	checkpoint->underruns = ring->underruns;
	checkpoint->minimum_occupancy = ring->minimum_occupancy;
	checkpoint->clock = *clock;
}

void psvita_usb_audio_read_rollback(PsvitaUsbAudioRing *ring,
	PsvitaUsbAudioPacketClock *clock,
	const PsvitaUsbAudioReadCheckpoint *checkpoint)
{
	if (!ring || !clock || !checkpoint) return;
	ring->read_frame = checkpoint->read_frame;
	ring->consumed_frames = checkpoint->consumed_frames;
	ring->underruns = checkpoint->underruns;
	ring->minimum_occupancy = checkpoint->minimum_occupancy;
	*clock = checkpoint->clock;
}

void psvita_usb_audio_expand_stereo(int16_t *stream,
	const int16_t *stereo, uint32_t frames)
{
	if (!stream || !stereo || !frames) return;
	memset(stream, 0, frames * PSVITA_USB_AUDIO_STREAM_CHANNELS *
		sizeof(stream[0]));
	for (uint32_t frame = 0; frame < frames; ++frame) {
		stream[frame * PSVITA_USB_AUDIO_STREAM_CHANNELS] =
			stereo[frame * PSVITA_USB_AUDIO_CHANNELS];
		stream[frame * PSVITA_USB_AUDIO_STREAM_CHANNELS + 1u] =
			stereo[frame * PSVITA_USB_AUDIO_CHANNELS + 1u];
	}
}

uint32_t psvita_usb_audio_measure_channels(const int16_t *interleaved,
	uint32_t frames, uint32_t channels, uint32_t *peaks,
	uint32_t peak_capacity)
{
	if (!interleaved || !peaks || !frames || !channels ||
	    channels > peak_capacity || channels > 32u)
		return 0;
	memset(peaks, 0, peak_capacity * sizeof(peaks[0]));
	uint32_t mask = 0;
	for (uint32_t frame = 0; frame < frames; ++frame) {
		for (uint32_t channel = 0; channel < channels; ++channel) {
			int32_t value = interleaved[frame * channels + channel];
			uint32_t magnitude = value < 0
				? (uint32_t)(-value) : (uint32_t)value;
			if (magnitude > peaks[channel]) peaks[channel] = magnitude;
			if (magnitude) mask |= 1u << channel;
		}
	}
	return mask;
}

void psvita_usb_audio_packet_clock_init(PsvitaUsbAudioPacketClock *clock)
{
	if (clock) memset(clock, 0, sizeof(*clock));
}

uint32_t psvita_usb_audio_packet_frames(PsvitaUsbAudioPacketClock *clock,
	uint32_t occupancy)
{
	(void)occupancy;
	if (!clock) return PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES;
	clock->packet_index++;
	return PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES;
}

uint32_t psvita_usb_audio_request_intervals(uint32_t occupancy)
{
	uint32_t whole_packets = occupancy /
		PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES;
	if (!whole_packets) return 1u;
	return whole_packets < PSVITA_USB_AUDIO_REQUEST_INTERVALS
		? whole_packets : PSVITA_USB_AUDIO_REQUEST_INTERVALS;
}

void psvita_usb_audio_completion_clock_init(
	PsvitaUsbAudioCompletionClock *clock)
{
	if (clock) memset(clock, 0, sizeof(*clock));
}

void psvita_usb_audio_completion_record(PsvitaUsbAudioCompletionClock *clock,
	uint32_t now_us, uint32_t requested_bytes, uint32_t transmitted_bytes,
	int32_t result)
{
	if (!clock) return;
	if (clock->last_timestamp_us) {
		/* Unsigned subtraction also handles the 32-bit timer wrapping. */
		uint32_t gap_us = now_us - clock->last_timestamp_us;
		clock->last_gap_us = gap_us;
		if (gap_us > clock->maximum_gap_us) clock->maximum_gap_us = gap_us;
		uint32_t packet_bytes = PSVITA_USB_AUDIO_PACKET_BYTES(
			PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES);
		uint32_t intervals = packet_bytes ? requested_bytes / packet_bytes : 0u;
		uint32_t late_us = intervals > 0u &&
			intervals <= PSVITA_USB_AUDIO_REQUEST_INTERVALS
			? (intervals + 1u) * 1000u
			: PSVITA_USB_AUDIO_LATE_COMPLETION_US;
		if (gap_us >= late_us)
			clock->late_completions++;
	}
	clock->last_timestamp_us = now_us;
	if (result >= 0 && transmitted_bytes != requested_bytes)
		clock->short_completions++;
}

void psvita_usb_audio_request_init(PsvitaUsbAudioRequestState *request)
{
	if (request) memset(request, 0, sizeof(*request));
}

int psvita_usb_audio_request_try_prepare(PsvitaUsbAudioRequestState *request)
{
	return request && __sync_bool_compare_and_swap(&request->state,
		PSVITA_USB_AUDIO_REQUEST_FREE,
		PSVITA_USB_AUDIO_REQUEST_PREPARING);
}

void psvita_usb_audio_request_publish(PsvitaUsbAudioRequestState *request,
	uint32_t started_us)
{
	if (!request) return;
	request->started_us = started_us;
	__sync_synchronize();
	(void)__sync_bool_compare_and_swap(&request->state,
		PSVITA_USB_AUDIO_REQUEST_PREPARING,
		PSVITA_USB_AUDIO_REQUEST_PENDING);
}

int psvita_usb_audio_request_begin_completion(
	PsvitaUsbAudioRequestState *request, int *canceled)
{
	if (canceled) *canceled = 0;
	if (!request) return 0;
	if (__sync_bool_compare_and_swap(&request->state,
	    PSVITA_USB_AUDIO_REQUEST_PENDING,
	    PSVITA_USB_AUDIO_REQUEST_COMPLETING))
		return 1;
	if (__sync_bool_compare_and_swap(&request->state,
	    PSVITA_USB_AUDIO_REQUEST_CANCELING,
	    PSVITA_USB_AUDIO_REQUEST_COMPLETING)) {
		if (canceled) *canceled = 1;
		return 1;
	}
	return 0;
}

void psvita_usb_audio_request_finish_completion(
	PsvitaUsbAudioRequestState *request)
{
	if (!request) return;
	request->started_us = 0;
	__sync_synchronize();
	(void)__sync_bool_compare_and_swap(&request->state,
		PSVITA_USB_AUDIO_REQUEST_COMPLETING,
		PSVITA_USB_AUDIO_REQUEST_FREE);
}

int psvita_usb_audio_request_try_cancel(PsvitaUsbAudioRequestState *request)
{
	return request && __sync_bool_compare_and_swap(&request->state,
		PSVITA_USB_AUDIO_REQUEST_PENDING,
		PSVITA_USB_AUDIO_REQUEST_CANCELING);
}

void psvita_usb_audio_request_cancel_failed(PsvitaUsbAudioRequestState *request)
{
	if (!request) return;
	(void)__sync_bool_compare_and_swap(&request->state,
		PSVITA_USB_AUDIO_REQUEST_CANCELING,
		PSVITA_USB_AUDIO_REQUEST_PENDING);
}

void psvita_usb_audio_request_abort_submit(PsvitaUsbAudioRequestState *request)
{
	if (!request) return;
	request->started_us = 0;
	__sync_synchronize();
	(void)__sync_bool_compare_and_swap(&request->state,
		PSVITA_USB_AUDIO_REQUEST_PENDING,
		PSVITA_USB_AUDIO_REQUEST_FREE);
}

uint32_t psvita_usb_audio_request_state(
	const PsvitaUsbAudioRequestState *request)
{
	if (!request) return PSVITA_USB_AUDIO_REQUEST_FREE;
	return (uint32_t)__sync_val_compare_and_swap(
		(volatile uint32_t *)&request->state, 0u, 0u);
}

int psvita_usb_audio_request_is_stalled(
	const PsvitaUsbAudioRequestState *request, uint32_t now_us,
	uint32_t stall_us)
{
	return request &&
		psvita_usb_audio_request_state(request) ==
			PSVITA_USB_AUDIO_REQUEST_PENDING &&
		now_us - request->started_us >= stall_us;
}
