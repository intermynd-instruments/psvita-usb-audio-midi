#include "audio_control.h"
#include "audio_core.h"
#include "audio_diagnostics.h"
#include "midi_core.h"
#include "usb_descriptors.h"
#include "midi_timing.h"
#include "owner_guard.h"
#include "scope_text.h"
#include "stock_state.h"
#include "takeover.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); \
	return 1; } } while (0)

static void append_bytes(uint8_t *destination, size_t *length,
	const uint8_t *source, size_t count)
{
	memcpy(destination + *length, source, count);
	*length += count;
}

static int test_serialized_configuration(uint16_t midi_packet_size,
	uint8_t audio_interval)
{
	uint8_t raw[PSVITA_USB_AUDIO_MIDI_CONFIG_TOTAL_LENGTH];
	size_t length = 0;
	const uint8_t config[] = {
		9, 2, (uint8_t)PSVITA_USB_AUDIO_MIDI_CONFIG_TOTAL_LENGTH,
		(uint8_t)(PSVITA_USB_AUDIO_MIDI_CONFIG_TOTAL_LENGTH >> 8),
		PSVITA_USB_INTERFACE_COUNT, 1, 0, 0x80, 50
	};
	const uint8_t audio_control[] = {9, 4, 0, 0, 0, 1, 1, 0, 0};
	const uint8_t audio_stream_zero[] = {9, 4, 1, 0, 0, 1, 2, 0, 0};
	const uint8_t audio_stream_one[] = {9, 4, 1, 1, 1, 1, 2, 0, 0};
	const uint8_t audio_endpoint[] = {
		9, 5, PSVITA_USB_AUDIO_IN_ENDPOINT_ADDRESS,
		PSVITA_USB_AUDIO_ENDPOINT_ATTRIBUTES,
		(uint8_t)PSVITA_USB_AUDIO_MAX_PACKET_BYTES,
		(uint8_t)(PSVITA_USB_AUDIO_MAX_PACKET_BYTES >> 8), audio_interval
	};
	const uint8_t audio_input_stream_zero[] = {9, 4, 2, 0, 0, 1, 2, 0, 0};
	const uint8_t audio_input_stream_one[] = {9, 4, 2, 1, 1, 1, 2, 0, 0};
	const uint8_t audio_input_endpoint[] = {
		9, 5, PSVITA_USB_AUDIO_OUT_ENDPOINT_ADDRESS,
		PSVITA_USB_AUDIO_INPUT_ENDPOINT_ATTRIBUTES,
		(uint8_t)PSVITA_USB_AUDIO_INPUT_MAX_PACKET_BYTES,
		(uint8_t)(PSVITA_USB_AUDIO_INPUT_MAX_PACKET_BYTES >> 8), audio_interval
	};
	const uint8_t midi_stream[] = {9, 4, 3, 0, 2, 1, 3, 0, 0};
	const uint8_t midi_out_endpoint[] = {
		9, 5, PSVITA_USB_MIDI_OUT_ENDPOINT_ADDRESS, 2,
		(uint8_t)midi_packet_size, (uint8_t)(midi_packet_size >> 8), 0
	};
	const uint8_t midi_in_endpoint[] = {
		9, 5, PSVITA_USB_MIDI_IN_ENDPOINT_ADDRESS, 2,
		(uint8_t)midi_packet_size, (uint8_t)(midi_packet_size >> 8), 0
	};

	append_bytes(raw, &length, config, sizeof(config));
	append_bytes(raw, &length, audio_control, sizeof(audio_control));
	append_bytes(raw, &length, psvita_usb_audio_control_extra,
		sizeof(psvita_usb_audio_control_extra));
	append_bytes(raw, &length, audio_stream_zero, sizeof(audio_stream_zero));
	append_bytes(raw, &length, audio_stream_one, sizeof(audio_stream_one));
	append_bytes(raw, &length, psvita_usb_audio_as_extra,
		sizeof(psvita_usb_audio_as_extra));
	append_bytes(raw, &length, audio_endpoint, sizeof(audio_endpoint));
	append_bytes(raw, &length, psvita_usb_audio_in_extra, 2u);
	append_bytes(raw, &length, psvita_usb_audio_in_extra + 2u,
		sizeof(psvita_usb_audio_in_extra) - 2u);
	append_bytes(raw, &length, audio_input_stream_zero,
		sizeof(audio_input_stream_zero));
	append_bytes(raw, &length, audio_input_stream_one,
		sizeof(audio_input_stream_one));
	append_bytes(raw, &length, psvita_usb_audio_input_as_extra,
		sizeof(psvita_usb_audio_input_as_extra));
	append_bytes(raw, &length, audio_input_endpoint,
		sizeof(audio_input_endpoint));
	append_bytes(raw, &length, psvita_usb_audio_out_extra, 2u);
	append_bytes(raw, &length, psvita_usb_audio_out_extra + 2u,
		sizeof(psvita_usb_audio_out_extra) - 2u);
	append_bytes(raw, &length, midi_stream, sizeof(midi_stream));
	append_bytes(raw, &length, psvita_usb_midi_ms_extra,
		sizeof(psvita_usb_midi_ms_extra));
	append_bytes(raw, &length, midi_out_endpoint, sizeof(midi_out_endpoint));
	append_bytes(raw, &length, psvita_usb_midi_out_extra, 2u);
	append_bytes(raw, &length, psvita_usb_midi_out_extra + 2u,
		sizeof(psvita_usb_midi_out_extra) - 2u);
	append_bytes(raw, &length, midi_in_endpoint, sizeof(midi_in_endpoint));
	append_bytes(raw, &length, psvita_usb_midi_in_extra, 2u);
	append_bytes(raw, &length, psvita_usb_midi_in_extra + 2u,
		sizeof(psvita_usb_midi_in_extra) - 2u);
	CHECK(length == sizeof(raw));

	uint32_t interface_descriptors = 0;
	uint32_t endpoint_descriptors = 0;
	uint32_t audio_endpoints = 0;
	uint32_t audio_input_endpoints = 0;
	uint32_t midi_out_endpoints = 0;
	uint32_t midi_in_endpoints = 0;
	for (size_t offset = 0; offset < length;) {
		uint8_t descriptor_length = raw[offset];
		CHECK(descriptor_length >= 2u);
		CHECK(offset + descriptor_length <= length);
		uint8_t descriptor_type = raw[offset + 1u];
		if (descriptor_type == 4u) interface_descriptors++;
		if (descriptor_type == 5u) {
			endpoint_descriptors++;
			CHECK(descriptor_length == 9u);
			if (raw[offset + 2u] == PSVITA_USB_AUDIO_IN_ENDPOINT_ADDRESS) {
				audio_endpoints++;
				CHECK(raw[offset + 3u] == PSVITA_USB_AUDIO_ENDPOINT_ATTRIBUTES);
				CHECK((uint16_t)(raw[offset + 4u] |
				      ((uint16_t)raw[offset + 5u] << 8u)) ==
				      PSVITA_USB_AUDIO_MAX_PACKET_BYTES);
				CHECK(raw[offset + 6u] == audio_interval);
				CHECK(raw[offset + 7u] == 0u && raw[offset + 8u] == 0u);
			} else if (raw[offset + 2u] ==
			           PSVITA_USB_AUDIO_OUT_ENDPOINT_ADDRESS) {
				audio_input_endpoints++;
				CHECK(raw[offset + 3u] ==
				      PSVITA_USB_AUDIO_INPUT_ENDPOINT_ATTRIBUTES);
				CHECK((uint16_t)(raw[offset + 4u] |
				      ((uint16_t)raw[offset + 5u] << 8u)) ==
				      PSVITA_USB_AUDIO_INPUT_MAX_PACKET_BYTES);
				CHECK(raw[offset + 6u] == audio_interval);
				CHECK(raw[offset + 7u] == 0u && raw[offset + 8u] == 0u);
			} else if (raw[offset + 2u] == PSVITA_USB_MIDI_OUT_ENDPOINT_ADDRESS) {
				midi_out_endpoints++;
			} else if (raw[offset + 2u] == PSVITA_USB_MIDI_IN_ENDPOINT_ADDRESS) {
				midi_in_endpoints++;
			} else {
				CHECK(0);
			}
		}
		offset += descriptor_length;
	}
	CHECK(interface_descriptors == PSVITA_USB_INTERFACE_DESCRIPTOR_COUNT);
	CHECK(endpoint_descriptors == 4u);
	CHECK(audio_endpoints == 1u);
	CHECK(audio_input_endpoints == 1u);
	CHECK(midi_out_endpoints == 1u);
	CHECK(midi_in_endpoints == 1u);
	return 0;
}

static int test_descriptors(void)
{
	CHECK(psvita_usb_audio_midi_validate_descriptor_layout());
	CHECK(PSVITA_USB_AUDIO_MIDI_CONFIG_TOTAL_LENGTH == 248u);
	CHECK(PSVITA_USB_UDCD_ENDPOINT_COUNT == 5u);
	CHECK(PSVITA_USB_INTERFACE_COUNT == 4u);
	CHECK(PSVITA_USB_INTERFACE_DESCRIPTOR_COUNT == 6u);
	CHECK(PSVITA_USB_STRING_DESCRIPTOR_COUNT == 12u);
	CHECK(PSVITA_USB_AUDIO_IN_ENDPOINT_ADDRESS == 0x83u);
	CHECK(PSVITA_USB_AUDIO_OUT_ENDPOINT_ADDRESS == 0x04u);
	/* Exact 48-frame packets avoid a short tail when one UDCD request spans
	 * multiple 1 ms service intervals. */
	CHECK(PSVITA_USB_AUDIO_MAX_PACKET_BYTES == 960u);
	CHECK(PSVITA_USB_AUDIO_MAX_PACKET_BYTES <= 1023u);
	CHECK(PSVITA_USB_AUDIO_INPUT_MAX_PACKET_BYTES == 196u);
	CHECK(PSVITA_USB_AUDIO_INPUT_ENDPOINT_ATTRIBUTES == 0x09u);
	CHECK(PSVITA_USB_MIDI_OUT_ENDPOINT_ADDRESS == 0x01u);
	CHECK(PSVITA_USB_MIDI_IN_ENDPOINT_ADDRESS == 0x82u);
	CHECK(PSVITA_USB_AUDIO_DRIVER_ENDPOINT ==
	      (PSVITA_USB_AUDIO_IN_ENDPOINT_ADDRESS & 0x0fu));
	CHECK(PSVITA_USB_AUDIO_INPUT_DRIVER_ENDPOINT ==
	      (PSVITA_USB_AUDIO_OUT_ENDPOINT_ADDRESS & 0x0fu));
	CHECK(PSVITA_USB_MIDI_OUT_DRIVER_ENDPOINT ==
	      (PSVITA_USB_MIDI_OUT_ENDPOINT_ADDRESS & 0x0fu));
	CHECK(PSVITA_USB_MIDI_IN_DRIVER_ENDPOINT ==
	      (PSVITA_USB_MIDI_IN_ENDPOINT_ADDRESS & 0x0fu));
	CHECK(PSVITA_USB_AUDIO_DESCRIPTOR_ENDPOINT_INDEX == 0u);
	CHECK(PSVITA_USB_AUDIO_INPUT_DESCRIPTOR_ENDPOINT_INDEX == 1u);
	CHECK(PSVITA_USB_MIDI_OUT_DESCRIPTOR_ENDPOINT_INDEX == 2u);
	CHECK(PSVITA_USB_MIDI_IN_DESCRIPTOR_ENDPOINT_INDEX == 3u);
	CHECK(psvita_usb_audio_control_extra[0] == 10u);
	CHECK(psvita_usb_audio_control_extra[7] == 2u);
	CHECK(psvita_usb_audio_control_extra[8] == PSVITA_USB_AUDIO_STREAM_INTERFACE);
	CHECK(psvita_usb_audio_control_extra[9] == PSVITA_USB_AUDIO_INPUT_STREAM_INTERFACE);
	CHECK(psvita_usb_audio_control_extra[17] == 10u);
	CHECK(psvita_usb_audio_control_extra[18] == 0x03u);
	CHECK(psvita_usb_audio_control_extra[20] == 4u);
	CHECK(psvita_usb_audio_as_extra[11] == 10u);
	CHECK(psvita_usb_audio_as_extra[13] == 16u);
	CHECK(psvita_usb_audio_input_as_extra[3] == 3u);
	CHECK(psvita_usb_audio_input_as_extra[11] == 2u);
	CHECK(psvita_usb_audio_input_as_extra[13] == 16u);
	CHECK(psvita_usb_midi_ms_extra[5] == 37u);
	CHECK(psvita_usb_midi_ms_extra[30] == 0x03u);
	CHECK(test_serialized_configuration(64u,
	      PSVITA_USB_AUDIO_FULL_SPEED_INTERVAL) == 0);
	CHECK(test_serialized_configuration(512u,
	      PSVITA_USB_AUDIO_HIGH_SPEED_INTERVAL) == 0);
	return 0;
}

static int test_audio_set_interface_request(void)
{
	int alternate = -1;
	CHECK(psvita_usb_audio_decode_set_interface(0x01u, 0x0bu, 1u,
	      PSVITA_USB_AUDIO_STREAM_INTERFACE, &alternate) == 1);
	CHECK(alternate == 1);
	CHECK(psvita_usb_audio_decode_set_interface(0x01u, 0x0bu, 0u,
	      PSVITA_USB_AUDIO_STREAM_INTERFACE, &alternate) == 1);
	CHECK(alternate == 0);
	CHECK(psvita_usb_audio_decode_set_interface(0x01u, 0x0bu, 1u,
	      PSVITA_USB_AUDIO_INPUT_STREAM_INTERFACE, &alternate) == 1);
	CHECK(alternate == 1);
	CHECK(psvita_usb_audio_decode_set_interface(0x01u, 0x0bu, 1u,
	      PSVITA_USB_MIDI_STREAM_INTERFACE, &alternate) == 0);
	CHECK(psvita_usb_audio_decode_set_interface(0x01u, 0x0bu, 2u,
	      PSVITA_USB_AUDIO_STREAM_INTERFACE, &alternate) < 0);
	CHECK(psvita_usb_audio_decode_set_interface(0x81u, 0x0au, 0u,
	      PSVITA_USB_AUDIO_STREAM_INTERFACE, &alternate) == 0);
	return 0;
}

static int test_audio_ring_and_silence(void)
{
	PsvitaUsbAudioRing ring;
	int16_t input[PSVITA_USB_AUDIO_MAX_WRITE_FRAMES * 2u];
	int16_t output[49u * 2u];
	int16_t wrap_output[64u * 2u];
	psvita_usb_audio_ring_init(&ring, 2u);
	for (uint32_t frame = 0; frame < PSVITA_USB_AUDIO_MAX_WRITE_FRAMES; ++frame) {
		input[frame * 2u] = (int16_t)frame;
		input[frame * 2u + 1u] = (int16_t)(-((int32_t)frame));
	}
	CHECK(psvita_usb_audio_ring_write(&ring, input, 64u) == 64u);
	CHECK(psvita_usb_audio_ring_available(&ring) == 64u);
	CHECK(psvita_usb_audio_ring_read_silence(&ring, output, 48u) == 48u);
	CHECK(output[30] == 15 && output[31] == -15);
	CHECK(psvita_usb_audio_ring_read_silence(&ring, output, 48u) == 16u);
	CHECK(output[0] == 48 && output[1] == -48);
	for (uint32_t sample = 32u; sample < 96u; ++sample)
		CHECK(output[sample] == 0);
	CHECK(ring.underruns == 1u);
	CHECK(psvita_usb_audio_ring_available(&ring) == 0u);
	for (uint32_t batch = 0; batch < 7u; ++batch) {
		CHECK(psvita_usb_audio_ring_write(&ring, input, 256u) == 256u);
		for (uint32_t packet = 0; packet < 8u; ++packet)
			CHECK(psvita_usb_audio_ring_read_silence(&ring, output, 32u) == 32u);
	}
	CHECK(psvita_usb_audio_ring_write(&ring, input, 240u) == 240u);
	for (uint32_t packet = 0; packet < 5u; ++packet)
		CHECK(psvita_usb_audio_ring_read_silence(&ring, output, 48u) == 48u);
	CHECK(psvita_usb_audio_ring_write(&ring, input, 64u) == 64u);
	CHECK(psvita_usb_audio_ring_read_silence(&ring, wrap_output, 64u) == 64u);
	for (uint32_t frame = 0; frame < 64u; ++frame) {
		CHECK(wrap_output[frame * 2u] == (int16_t)frame);
		CHECK(wrap_output[frame * 2u + 1u] == (int16_t)(-((int32_t)frame)));
	}

	psvita_usb_audio_ring_init(&ring, 2u);
	for (uint32_t batch = 0;
	     batch < PSVITA_USB_AUDIO_RING_FRAMES /
	         PSVITA_USB_AUDIO_MAX_WRITE_FRAMES + 2u;
	     ++batch)
		CHECK(psvita_usb_audio_ring_write(&ring, input,
			PSVITA_USB_AUDIO_MAX_WRITE_FRAMES) == PSVITA_USB_AUDIO_MAX_WRITE_FRAMES);
	CHECK(psvita_usb_audio_ring_available(&ring) == PSVITA_USB_AUDIO_RING_FRAMES);
	CHECK(ring.overruns == 2u);
	CHECK(psvita_usb_audio_ring_read_silence(&ring, output, 49u) == 49u);
	return 0;
}

static int test_audio_packet_clock(void)
{
	PsvitaUsbAudioPacketClock clock;
	/* SceUdcd stalls when more than one isochronous request is queued. */
	CHECK(PSVITA_USB_AUDIO_MAX_IN_FLIGHT_REQUESTS == 1u);
	psvita_usb_audio_packet_clock_init(&clock);
	for (int packet = 0; packet < 256; ++packet) {
		CHECK(psvita_usb_audio_packet_frames(&clock, 0u) == 48u);
		CHECK(psvita_usb_audio_packet_frames(&clock, 1536u) == 48u);
	}
	/* Ten channels leave too little scheduling margin when every four USB
	 * frames requires a completion callback and worker rearm. Keep the same
	 * 1 ms isochronous packets, but amortize that work across eight packets. */
	CHECK(PSVITA_USB_AUDIO_REQUEST_INTERVALS == 8u);
	CHECK(PSVITA_USB_AUDIO_REQUEST_FRAMES == 384u);
	CHECK(PSVITA_USB_AUDIO_REQUEST_BYTES == 7680u);
	CHECK(PSVITA_USB_AUDIO_REQUEST_BYTES %
	      PSVITA_USB_AUDIO_MAX_PACKET_BYTES == 0u);
	CHECK(PSVITA_USB_AUDIO_REQUEST_BYTES /
	      PSVITA_USB_AUDIO_MAX_PACKET_BYTES ==
	      PSVITA_USB_AUDIO_REQUEST_INTERVALS);
	CHECK(sizeof(PsvitaUsbAudioStatus) == 292u);
	CHECK(offsetof(PsvitaUsbAudioStatus,
	      last_packet_channel_nonzero_mask) == 188u);
	CHECK(PSVITA_USB_AUDIO_REQUEST_BYTES <=
	      PSVITA_USB_AUDIO_DMA_BUFFER_BYTES);
	return 0;
}

static int test_audio_completion_health(void)
{
	PsvitaUsbAudioCompletionClock clock;
	psvita_usb_audio_completion_clock_init(&clock);
	psvita_usb_audio_completion_record(&clock, 1000u,
		PSVITA_USB_AUDIO_REQUEST_BYTES, PSVITA_USB_AUDIO_REQUEST_BYTES, 0);
	CHECK(clock.last_gap_us == 0u);
	CHECK(clock.short_completions == 0u && clock.late_completions == 0u);
	psvita_usb_audio_completion_record(&clock, 9000u,
		PSVITA_USB_AUDIO_REQUEST_BYTES, PSVITA_USB_AUDIO_REQUEST_BYTES, 0);
	CHECK(clock.last_gap_us == 8000u && clock.maximum_gap_us == 8000u);
	CHECK(clock.late_completions == 0u);
	psvita_usb_audio_completion_record(&clock, 18000u,
		PSVITA_USB_AUDIO_REQUEST_BYTES, PSVITA_USB_AUDIO_REQUEST_BYTES - 20u, 0);
	CHECK(clock.last_gap_us == 9000u && clock.maximum_gap_us == 9000u);
	CHECK(clock.short_completions == 1u && clock.late_completions == 1u);
	/* Errors are reported separately and are not mislabeled as short packets. */
	psvita_usb_audio_completion_record(&clock, 19000u,
		PSVITA_USB_AUDIO_REQUEST_BYTES, 0u, -1);
	CHECK(clock.short_completions == 1u && clock.late_completions == 1u);
	/* A six-interval rescue request has its own 7 ms lateness threshold. */
	psvita_usb_audio_completion_clock_init(&clock);
	uint32_t rescue_bytes = PSVITA_USB_AUDIO_PACKET_BYTES(6u *
		PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES);
	psvita_usb_audio_completion_record(&clock, 1000u,
		rescue_bytes, rescue_bytes, 0);
	psvita_usb_audio_completion_record(&clock, 7000u,
		rescue_bytes, rescue_bytes, 0);
	CHECK(clock.late_completions == 0u);
	psvita_usb_audio_completion_record(&clock, 14000u,
		rescue_bytes, rescue_bytes, 0);
	CHECK(clock.late_completions == 1u);
	return 0;
}

static int test_audio_packet_priming_and_rebuffer(void)
{
	PsvitaUsbAudioRing ring;
	PsvitaUsbAudioPacketClock clock;
	static int16_t input[PSVITA_USB_AUDIO_PRIME_FRAMES *
		PSVITA_USB_AUDIO_STREAM_CHANNELS];
	int16_t output[PSVITA_USB_AUDIO_MAX_PACKET_FRAMES *
		PSVITA_USB_AUDIO_STREAM_CHANNELS];
	for (uint32_t sample = 0; sample <
	     PSVITA_USB_AUDIO_PRIME_FRAMES * PSVITA_USB_AUDIO_STREAM_CHANNELS;
	     ++sample)
		input[sample] = (int16_t)(1000 + sample % 100u);
	psvita_usb_audio_ring_init(&ring, PSVITA_USB_AUDIO_STREAM_CHANNELS);
	psvita_usb_audio_packet_clock_init(&clock);
	memset(output, 0x7f, sizeof(output));
	CHECK(psvita_usb_audio_ring_read_packet(&ring, &clock, output, 48u) == 0u);
	CHECK(clock.primed == 0u && clock.priming_packets == 1u);
	CHECK(ring.underruns == 0u);
	for (uint32_t sample = 0; sample < 48u * PSVITA_USB_AUDIO_STREAM_CHANNELS;
	     ++sample)
		CHECK(output[sample] == 0);

	CHECK(psvita_usb_audio_ring_write(&ring, input,
	      PSVITA_USB_AUDIO_PRIME_FRAMES) == PSVITA_USB_AUDIO_PRIME_FRAMES);
	CHECK(psvita_usb_audio_ring_read_packet(&ring, &clock, output, 48u) == 48u);
	CHECK(clock.primed == 1u);
	CHECK(output[0] == input[0] && output[479] == input[479]);

	/* Hardware captured K42 immediately before a 48-frame request while the
	 * next 512-frame producer block was already becoming available. Six absent
	 * frames must not turn into a full 3,584-frame re-prime dropout. */
	ring.read_frame = ring.write_frame - 42u;
	memset(output, 0x7f, sizeof(output));
	CHECK(psvita_usb_audio_ring_read_packet(&ring, &clock, output, 48u) == 42u);
	CHECK(clock.primed == 1u && clock.rebuffer_events == 0u);
	CHECK(clock.conceal_events == 1u && clock.concealed_frames == 6u);
	CHECK(clock.consecutive_missing_frames == 6u);
	CHECK(ring.underruns == 1u);
	CHECK(psvita_usb_audio_ring_available(&ring) == 0u);
	for (uint32_t frame = 42u; frame < 48u; ++frame)
		for (uint32_t channel = 0; channel < PSVITA_USB_AUDIO_STREAM_CHANNELS;
		     ++channel)
			CHECK(output[frame * PSVITA_USB_AUDIO_STREAM_CHANNELS + channel] ==
			      output[41u * PSVITA_USB_AUDIO_STREAM_CHANNELS + channel]);

	CHECK(psvita_usb_audio_ring_write(&ring, input, 512u) == 512u);
	CHECK(psvita_usb_audio_ring_read_packet(&ring, &clock, output, 48u) == 48u);
	CHECK(clock.primed == 1u && clock.rebuffer_events == 0u);
	CHECK(clock.consecutive_missing_frames == 0u);

	/* A genuinely absent producer still enters the protective re-prime state,
	 * but only after eight consecutive 1 ms packets are missing. */
	ring.read_frame = ring.write_frame;
	for (uint32_t packet = 0; packet < 7u; ++packet) {
		CHECK(psvita_usb_audio_ring_read_packet(&ring, &clock, output, 48u) == 0u);
		CHECK(clock.primed == 1u && clock.rebuffer_events == 0u);
	}
	CHECK(psvita_usb_audio_ring_read_packet(&ring, &clock, output, 48u) == 0u);
	CHECK(clock.primed == 0u && clock.rebuffer_events == 1u);
	CHECK(clock.conceal_events == 9u && clock.concealed_frames == 390u);
	return 0;
}

static int test_audio_low_reservoir_uses_available_whole_packets(void)
{
	PsvitaUsbAudioRing ring;
	PsvitaUsbAudioPacketClock clock;
	int16_t input[512u * PSVITA_USB_AUDIO_STREAM_CHANNELS];
	int16_t output[PSVITA_USB_AUDIO_REQUEST_FRAMES *
		PSVITA_USB_AUDIO_STREAM_CHANNELS];
	memset(input, 0x23, sizeof(input));
	psvita_usb_audio_ring_init(&ring, PSVITA_USB_AUDIO_STREAM_CHANNELS);
	psvita_usb_audio_packet_clock_init(&clock);
	clock.primed = 1u;

	/* This is the hardware failure snapshot: the next fixed 384-frame request
	 * would rebuffer even though 290 valid frames still provide about 6 ms for
	 * the producer/feeder to recover. */
	CHECK(psvita_usb_audio_ring_write(&ring, input, 290u) == 290u);
	uint32_t intervals = psvita_usb_audio_request_intervals(
		psvita_usb_audio_ring_available(&ring));
	CHECK(intervals == 6u);
	CHECK(psvita_usb_audio_ring_read_packet(&ring, &clock, output,
	      intervals * PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES) == 288u);
	CHECK(clock.primed == 1u && clock.rebuffer_events == 0u);
	CHECK(psvita_usb_audio_ring_available(&ring) == 2u);

	/* If a 512-frame callback arrives while that rescue request is in flight,
	 * the normal eight-interval cadence resumes on its next completion. */
	CHECK(psvita_usb_audio_ring_write(&ring, input, 512u) == 512u);
	intervals = psvita_usb_audio_request_intervals(
		psvita_usb_audio_ring_available(&ring));
	CHECK(intervals == 8u);
	CHECK(psvita_usb_audio_ring_read_packet(&ring, &clock, output,
	      intervals * PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES) == 384u);
	CHECK(clock.primed == 1u && clock.rebuffer_events == 0u);
	return 0;
}

static int test_audio_failed_submit_rolls_back_prepared_frames(void)
{
	PsvitaUsbAudioRing ring;
	PsvitaUsbAudioPacketClock clock;
	PsvitaUsbAudioReadCheckpoint checkpoint;
	int16_t input[512u * PSVITA_USB_AUDIO_STREAM_CHANNELS];
	int16_t first[PSVITA_USB_AUDIO_REQUEST_FRAMES *
		PSVITA_USB_AUDIO_STREAM_CHANNELS];
	int16_t retry[PSVITA_USB_AUDIO_REQUEST_FRAMES *
		PSVITA_USB_AUDIO_STREAM_CHANNELS];
	for (uint32_t sample = 0; sample <
	     512u * PSVITA_USB_AUDIO_STREAM_CHANNELS; ++sample)
		input[sample] = (int16_t)(sample - 2048);
	psvita_usb_audio_ring_init(&ring, PSVITA_USB_AUDIO_STREAM_CHANNELS);
	psvita_usb_audio_packet_clock_init(&clock);
	clock.primed = 1u;
	CHECK(psvita_usb_audio_ring_write(&ring, input, 512u) == 512u);
	psvita_usb_audio_read_checkpoint(&ring, &clock, &checkpoint);
	CHECK(psvita_usb_audio_packet_frames(&clock, 512u) == 48u);
	CHECK(psvita_usb_audio_ring_read_packet(&ring, &clock, first,
	      PSVITA_USB_AUDIO_REQUEST_FRAMES) == PSVITA_USB_AUDIO_REQUEST_FRAMES);
	CHECK(psvita_usb_audio_ring_available(&ring) == 128u);

	/* Model ksceUdcdReqSend rejecting the prepared request. */
	psvita_usb_audio_read_rollback(&ring, &clock, &checkpoint);
	CHECK(psvita_usb_audio_ring_available(&ring) == 512u);
	CHECK(ring.consumed_frames == 0u && ring.underruns == 0u);
	CHECK(clock.packet_index == 0u && clock.concealed_frames == 0u);
	CHECK(psvita_usb_audio_ring_read_packet(&ring, &clock, retry,
	      PSVITA_USB_AUDIO_REQUEST_FRAMES) == PSVITA_USB_AUDIO_REQUEST_FRAMES);
	CHECK(memcmp(first, retry, sizeof(first)) == 0);
	return 0;
}

static int test_audio_completion_keeps_slot_owned_until_cleanup_finishes(void)
{
	PsvitaUsbAudioRequestState request;
	psvita_usb_audio_request_init(&request);
	CHECK(psvita_usb_audio_request_try_prepare(&request));
	psvita_usb_audio_request_publish(&request, 1000u);
	CHECK(psvita_usb_audio_request_state(&request) ==
	      PSVITA_USB_AUDIO_REQUEST_PENDING);

	/* Reproduce the real callback/feeder interleaving: callback entry begins,
	 * then an unrelated producer wake runs the feeder before callback cleanup. */
	int canceled = -1;
	CHECK(psvita_usb_audio_request_begin_completion(&request, &canceled));
	CHECK(canceled == 0);
	CHECK(!psvita_usb_audio_request_try_prepare(&request));
	psvita_usb_audio_request_finish_completion(&request);
	CHECK(psvita_usb_audio_request_state(&request) ==
	      PSVITA_USB_AUDIO_REQUEST_FREE);
	CHECK(request.started_us == 0u);
	CHECK(psvita_usb_audio_request_try_prepare(&request));
	return 0;
}

static int test_audio_request_cancel_and_timer_wrap_are_race_safe(void)
{
	PsvitaUsbAudioRequestState request;
	psvita_usb_audio_request_init(&request);
	CHECK(psvita_usb_audio_request_try_prepare(&request));
	psvita_usb_audio_request_publish(&request, 0xfffffff0u);
	CHECK(!psvita_usb_audio_request_is_stalled(&request, 0x20u, 0x40u));
	CHECK(psvita_usb_audio_request_is_stalled(&request, 0x40u, 0x40u));
	CHECK(psvita_usb_audio_request_try_cancel(&request));
	CHECK(!psvita_usb_audio_request_try_prepare(&request));
	int canceled = 0;
	CHECK(psvita_usb_audio_request_begin_completion(&request, &canceled));
	CHECK(canceled == 1);
	CHECK(!psvita_usb_audio_request_try_prepare(&request));
	psvita_usb_audio_request_finish_completion(&request);
	CHECK(psvita_usb_audio_request_state(&request) ==
	      PSVITA_USB_AUDIO_REQUEST_FREE);

	/* Timestamp zero is a valid instant after the 32-bit microsecond clock
	 * wraps; ownership, not a zero sentinel, decides whether it can stall. */
	CHECK(psvita_usb_audio_request_try_prepare(&request));
	psvita_usb_audio_request_publish(&request, 0u);
	CHECK(psvita_usb_audio_request_is_stalled(&request, 250000u, 250000u));
	return 0;
}

static int test_audio_ten_channel_layout(void)
{
	PsvitaUsbAudioRing ring;
	int16_t input[64u * PSVITA_USB_AUDIO_STREAM_CHANNELS];
	int16_t output[64u * PSVITA_USB_AUDIO_STREAM_CHANNELS];
	for (uint32_t frame = 0; frame < 64u; ++frame)
		for (uint32_t channel = 0; channel < PSVITA_USB_AUDIO_STREAM_CHANNELS;
		     ++channel)
			input[frame * PSVITA_USB_AUDIO_STREAM_CHANNELS + channel] =
				(int16_t)(frame * 100u + channel);
	psvita_usb_audio_ring_init(&ring, PSVITA_USB_AUDIO_STREAM_CHANNELS);
	CHECK(ring.channels == PSVITA_USB_AUDIO_STREAM_CHANNELS);
	CHECK(psvita_usb_audio_ring_write(&ring, input, 64u) == 64u);
	CHECK(psvita_usb_audio_ring_read_silence(&ring, output, 64u) == 64u);
	CHECK(memcmp(input, output, sizeof(input)) == 0);
	uint32_t peaks[PSVITA_USB_AUDIO_STREAM_CHANNELS];
	uint32_t mask = psvita_usb_audio_measure_channels(output, 64u,
		PSVITA_USB_AUDIO_STREAM_CHANNELS, peaks,
		PSVITA_USB_AUDIO_STREAM_CHANNELS);
	CHECK(mask == ((1u << PSVITA_USB_AUDIO_STREAM_CHANNELS) - 1u));
	for (uint32_t channel = 0; channel < PSVITA_USB_AUDIO_STREAM_CHANNELS;
	     ++channel)
		CHECK(peaks[channel] == 6300u + channel);

	int16_t stereo[4u * PSVITA_USB_AUDIO_CHANNELS] = {
		1, -1, 2, -2, 3, -3, 4, -4
	};
	int16_t expanded[4u * PSVITA_USB_AUDIO_STREAM_CHANNELS];
	psvita_usb_audio_expand_stereo(expanded, stereo, 4u);
	for (uint32_t frame = 0; frame < 4u; ++frame) {
		CHECK(expanded[frame * PSVITA_USB_AUDIO_STREAM_CHANNELS] ==
		      stereo[frame * 2u]);
		CHECK(expanded[frame * PSVITA_USB_AUDIO_STREAM_CHANNELS + 1u] ==
		      stereo[frame * 2u + 1u]);
		for (uint32_t channel = 2u;
		     channel < PSVITA_USB_AUDIO_STREAM_CHANNELS; ++channel)
			CHECK(expanded[frame * PSVITA_USB_AUDIO_STREAM_CHANNELS + channel] == 0);
	}
	return 0;
}

typedef struct {
	uint32_t calls;
	uint32_t bytes;
} MockAudioCopy;

static int mock_audio_copy(void *context, void *destination,
	const void *source, uint32_t bytes)
{
	MockAudioCopy *copy = (MockAudioCopy *)context;
	copy->calls++;
	copy->bytes += bytes;
	memcpy(destination, source, bytes);
	return 0;
}

static int test_audio_multichannel_ingress_is_one_transaction(void)
{
	PsvitaUsbAudioRing ring;
	static int16_t input[PSVITA_USB_AUDIO_MAX_WRITE_FRAMES *
		PSVITA_USB_AUDIO_STREAM_CHANNELS];
	static int16_t scratch[PSVITA_USB_AUDIO_MAX_WRITE_FRAMES *
		PSVITA_USB_AUDIO_STREAM_CHANNELS];
	for (uint32_t sample = 0; sample <
	     PSVITA_USB_AUDIO_MAX_WRITE_FRAMES * PSVITA_USB_AUDIO_STREAM_CHANNELS;
	     ++sample)
		input[sample] = (int16_t)(sample - 1200);
	MockAudioCopy copy = { 0, 0 };
	psvita_usb_audio_ring_init(&ring, PSVITA_USB_AUDIO_STREAM_CHANNELS);
	CHECK(psvita_usb_audio_copy_frames(scratch,
	      PSVITA_USB_AUDIO_MAX_WRITE_FRAMES, input,
	      PSVITA_USB_AUDIO_MAX_WRITE_FRAMES,
	      PSVITA_USB_AUDIO_STREAM_CHANNELS, mock_audio_copy, &copy) ==
	      (int)PSVITA_USB_AUDIO_MAX_WRITE_FRAMES);
	CHECK(psvita_usb_audio_ring_write(&ring, scratch,
	      PSVITA_USB_AUDIO_MAX_WRITE_FRAMES) ==
	      PSVITA_USB_AUDIO_MAX_WRITE_FRAMES);
	CHECK(copy.calls == 1u);
	CHECK(copy.bytes == sizeof(input));
	CHECK(ring.write_transactions == 1u);
	CHECK(memcmp(ring.pcm, input, sizeof(input)) == 0);
	return 0;
}

static int test_audio_input_ring_and_api_contract(void)
{
	PsvitaUsbAudioRing ring;
	int16_t packet_48[48u * PSVITA_USB_AUDIO_INPUT_CHANNELS];
	int16_t packet_49[49u * PSVITA_USB_AUDIO_INPUT_CHANNELS];
	int16_t output[97u * PSVITA_USB_AUDIO_INPUT_CHANNELS];
	int16_t scratch[PSVITA_USB_AUDIO_INPUT_MAX_READ_FRAMES *
		PSVITA_USB_AUDIO_INPUT_CHANNELS];
	for (uint32_t frame = 0; frame < 48u; ++frame) {
		packet_48[frame * 2u] = (int16_t)(1000u + frame);
		packet_48[frame * 2u + 1u] = (int16_t)(-1000 - (int32_t)frame);
	}
	for (uint32_t frame = 0; frame < 49u; ++frame) {
		packet_49[frame * 2u] = (int16_t)(2000u + frame);
		packet_49[frame * 2u + 1u] = (int16_t)(-2000 - (int32_t)frame);
	}
	psvita_usb_audio_ring_init(&ring, PSVITA_USB_AUDIO_INPUT_CHANNELS);
	CHECK(psvita_usb_audio_ring_write(&ring, packet_48, 48u) == 48u);
	CHECK(psvita_usb_audio_ring_write(&ring, packet_49, 49u) == 49u);
	CHECK(psvita_usb_audio_ring_read_silence(&ring, output, 97u) == 97u);
	CHECK(memcmp(output, packet_48, sizeof(packet_48)) == 0);
	CHECK(memcmp(output + 48u * 2u, packet_49, sizeof(packet_49)) == 0);

	MockAudioCopy copy = { 0, 0 };
	CHECK(psvita_usb_audio_copy_frames(scratch,
	      PSVITA_USB_AUDIO_INPUT_MAX_READ_FRAMES, output, 97u,
	      PSVITA_USB_AUDIO_INPUT_CHANNELS, mock_audio_copy, &copy) == 97);
	CHECK(copy.calls == 1u &&
	      copy.bytes == 97u * PSVITA_USB_AUDIO_INPUT_CHANNELS *
	          sizeof(int16_t));
	CHECK(psvita_usb_audio_copy_frames(scratch,
	      PSVITA_USB_AUDIO_INPUT_MAX_READ_FRAMES, output,
	      PSVITA_USB_AUDIO_INPUT_MAX_READ_FRAMES + 1u,
	      PSVITA_USB_AUDIO_INPUT_CHANNELS, mock_audio_copy, &copy) < 0);
	CHECK(sizeof(PsvitaUsbAudioInputStatus) == 120u);
	CHECK(offsetof(PsvitaUsbAudioInputStatus, applied_alternate) == 116u);
	return 0;
}

static void clean_diagnostic_status(PsvitaUsbAudioStatus *status,
	uint32_t requests, uint32_t bytes)
{
	memset(status, 0, sizeof(*status));
	status->size = sizeof(*status);
	status->protocol_version = PSVITA_USB_AUDIO_PROTOCOL_VERSION;
	status->state = PSVITA_USB_AUDIO_STATE_STREAMING;
	status->flags = PSVITA_USB_AUDIO_STATUS_ENABLED |
		PSVITA_USB_AUDIO_STATUS_HOST_CONNECTED |
		PSVITA_USB_AUDIO_STATUS_CONFIGURED |
		PSVITA_USB_AUDIO_STATUS_HOST_STREAMING;
	status->sample_rate = PSVITA_USB_AUDIO_SAMPLE_RATE;
	status->channels = PSVITA_USB_AUDIO_STREAM_CHANNELS;
	status->bit_depth = PSVITA_USB_AUDIO_BITS_PER_SAMPLE;
	status->ring_capacity_frames = PSVITA_USB_AUDIO_RING_FRAMES;
	status->buffered_frames = PSVITA_USB_AUDIO_PRIME_FRAMES;
	status->stream_primed = 1u;
	status->packets_submitted = requests + 1u;
	status->packets_completed = requests;
	status->bytes_transmitted = bytes;
	status->last_completion_requested_bytes =
		PSVITA_USB_AUDIO_DIAG_EXPECTED_REQUEST_BYTES;
	status->last_completion_bytes = PSVITA_USB_AUDIO_DIAG_EXPECTED_REQUEST_BYTES;
	status->last_completion_gap_us =
		PSVITA_USB_AUDIO_REQUEST_INTERVALS * 1000u;
	status->maximum_completion_gap_us = status->last_completion_gap_us;
	status->last_rearm_delay_us = 4u;
	status->maximum_rearm_delay_us = 4u;
	status->last_packet_channel_nonzero_mask =
		(1u << PSVITA_USB_AUDIO_STREAM_CHANNELS) - 1u;
	for (uint32_t channel = 0; channel < PSVITA_USB_AUDIO_STREAM_CHANNELS;
	     ++channel)
		status->last_packet_channel_peaks[channel] = 12000u;
}

static int test_audio_diagnostic_flight_recorder(void)
{
	PsvitaUsbAudioDiagnosticLog log;
	PsvitaUsbAudioStatus status;
	CHECK(sizeof(log) <= 128u * 1024u);
	psvita_usb_audio_diag_init(&log, 1000u);
	for (uint32_t tick = 0; tick <= 10u; ++tick) {
		clean_diagnostic_status(&status, tick * 25u, tick * 192000u);
		psvita_usb_audio_diag_record(&log, 1000u + tick * 200u, &status);
	}
	CHECK(log.timeline_count == 11u);
	CHECK(log.stream_sessions == 1u);
	CHECK(psvita_usb_audio_diag_request_rate(&log) == 125u);
	CHECK(psvita_usb_audio_diag_byte_rate(&log) == 960000u);
	CHECK(psvita_usb_audio_diag_failure_flags(&log) == 0u);
	/* A healthy low-reservoir rescue completion is a valid whole-packet
	 * request, not a short or malformed normal request. */
	status.packets_submitted++;
	status.packets_completed++;
	status.last_completion_requested_bytes =
		PSVITA_USB_AUDIO_PACKET_BYTES(6u *
			PSVITA_USB_AUDIO_NOMINAL_PACKET_FRAMES);
	status.last_completion_bytes = status.last_completion_requested_bytes;
	status.bytes_transmitted += status.last_completion_requested_bytes;
	status.last_completion_gap_us = 6000u;
	psvita_usb_audio_diag_record(&log, 3200u, &status);
	CHECK((psvita_usb_audio_diag_failure_flags(&log) &
	      PSVITA_USB_AUDIO_DIAG_FAULT_REQUEST_SIZE) == 0u);
	psvita_usb_audio_diag_annotate_producer(&log, 10u, 0u, 0u, 2u, 0u,
		1400u, 80u);
	CHECK(psvita_usb_audio_diag_failure_flags(&log) == 0u);
	psvita_usb_audio_diag_annotate_producer(&log, 10u, 1u, 0u, 2u, 0u,
		1400u, 80u);
	CHECK((psvita_usb_audio_diag_failure_flags(&log) &
	      PSVITA_USB_AUDIO_DIAG_FAULT_PRODUCER) != 0u);
	/* Continue the kernel fault test independently of the injected producer fault. */
	psvita_usb_audio_diag_init(&log, 1000u);
	for (uint32_t tick = 0; tick <= 10u; ++tick) {
		clean_diagnostic_status(&status, tick * 25u, tick * 192000u);
		psvita_usb_audio_diag_record(&log, 1000u + tick * 200u, &status);
	}

	status.packets_submitted++;
	status.packets_completed++;
	status.last_completion_requested_bytes =
		PSVITA_USB_AUDIO_DIAG_EXPECTED_REQUEST_BYTES;
	status.last_completion_bytes = 0u;
	status.last_completion_result = -7;
	status.short_completions = 1u;
	status.late_completions = 1u;
	status.last_completion_gap_us =
		PSVITA_USB_AUDIO_DIAG_LATE_COMPLETION_US + 1200u;
	status.maximum_completion_gap_us = status.last_completion_gap_us;
	status.completion_errors = 1u;
	status.output_overruns = 1u;
	status.rebuffer_events = 1u;
	status.last_packet_channel_nonzero_mask = 0x3fdu;
	status.last_packet_channel_peaks[1] = 0u;
	psvita_usb_audio_diag_record(&log, 3050u, &status);
	uint32_t failures = psvita_usb_audio_diag_failure_flags(&log);
	CHECK((failures & PSVITA_USB_AUDIO_DIAG_FAULT_REQUEST_SIZE) != 0u);
	CHECK((failures & PSVITA_USB_AUDIO_DIAG_FAULT_SHORT) != 0u);
	CHECK((failures & PSVITA_USB_AUDIO_DIAG_FAULT_LATE) != 0u);
	CHECK((failures & PSVITA_USB_AUDIO_DIAG_FAULT_COMPLETION) != 0u);
	CHECK((failures & PSVITA_USB_AUDIO_DIAG_FAULT_BUFFER) != 0u);
	CHECK((failures & PSVITA_USB_AUDIO_DIAG_FAULT_CHANNELS) != 0u);
	CHECK(log.event_count >= 2u);
	const PsvitaUsbAudioDiagnosticSample *event =
		psvita_usb_audio_diag_event_at(&log, log.event_count - 1u);
	CHECK(event != NULL && event->completion_result == -7);
	psvita_usb_audio_diag_mark(&log, 3060u);
	CHECK(log.manual_marks == 1u);
	event = psvita_usb_audio_diag_marker_at(&log, log.marker_count - 1u);
	CHECK(event != NULL &&
	      (event->event_flags & PSVITA_USB_AUDIO_DIAG_EVENT_MANUAL_MARK));

	psvita_usb_audio_diag_init(&log, 3000u);
	memset(&status, 0, sizeof(status));
	for (uint32_t tick = 0; tick < 600u; ++tick)
		psvita_usb_audio_diag_record(&log, 3000u + tick, &status);
	CHECK(log.timeline_count == PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY);
	const PsvitaUsbAudioDiagnosticSample *sample =
		psvita_usb_audio_diag_timeline_at(&log, 0u);
	CHECK(sample != NULL && sample->elapsed_ms == 88u);
	for (uint32_t mark = 0; mark < 12u; ++mark)
		psvita_usb_audio_diag_mark(&log, 4000u + mark);
	CHECK(log.marker_count == PSVITA_USB_AUDIO_DIAG_MARKER_CAPACITY);
	event = psvita_usb_audio_diag_marker_at(&log, 0u);
	CHECK(event != NULL && event->elapsed_ms == 1004u);
	event = psvita_usb_audio_diag_marker_at(&log, log.marker_count - 1u);
	CHECK(event != NULL && event->elapsed_ms == 1011u);

	psvita_usb_audio_diag_init(&log, 5000u);
	clean_diagnostic_status(&status, 0u, 0u);
	status.channels = 9u;
	psvita_usb_audio_diag_record(&log, 5000u, &status);
	clean_diagnostic_status(&status, 100u, 100000u);
	status.channels = 9u;
	psvita_usb_audio_diag_record(&log, 6000u, &status);
	failures = psvita_usb_audio_diag_failure_flags(&log);
	CHECK((failures & PSVITA_USB_AUDIO_DIAG_FAULT_FORMAT) != 0u);
	CHECK((failures & PSVITA_USB_AUDIO_DIAG_FAULT_THROUGHPUT) != 0u);

	psvita_usb_audio_diag_init(&log, 7000u);
	memset(&status, 0, sizeof(status));
	psvita_usb_audio_diag_record(&log, 7000u, &status);
	CHECK((psvita_usb_audio_diag_failure_flags(&log) &
	      PSVITA_USB_AUDIO_DIAG_FAULT_NO_DATA) != 0u);
	return 0;
}

static int test_scope_streaming_render_budget(void)
{
	static const char *const lines[] = {
		"USB AUDIO MIDI SCOPE BUILD 26072601 API 00020000",
		"LOAD 00000000 DIAG 00000000 MARK 1 EVT 3 REPORT 00000000",
		"ACQUIRE 00000000 OK RELEASE 00000000 STATE 3",
		"CTRL FFFFFFFF MTP 00000002 PSP 00000001 SER 00000001",
		"DEVICE 00002111 FLAGS 83075101 F8 0 FA 0 FB 0 FC 0",
		"LAT RXK P95 0.00MS TXJ P95 0.00MS RX 0 TX 0",
		"FILTERED 0 RX DROP 0 TX DROP 0",
		"USB CONFIG YES RX ARMED YES",
		"RX SUB 1 FAIL 0 CB 0 BYTES 0",
		"RX LAST SUB 00000000 CB 00000000",
		"RX REJECT 0 MALFORMED 0 TX REJECT 0",
		"AUDIO STREAMING RUN YES 10CH TEST TONES FL 0000001F",
		"AUDIO EP D 3 S 3 N 3 ALT 1/1 PR 1 RB 0 BUF 6144",
		"AUDIO Q 999999/999999 E 0 SH 0 LT 0 G 8000US",
		"AUDIO REQ 7680/7680 R 4 K 0 CH 3FF F 0000",
		"PEAK M 12000 12000 T1-4 12000 12000 12000 12000",
		"PEAK T5-8 12000 12000 12000 12000",
		"VERDICT 10CH PAYLOAD COMPLETED USB IN",
		"TRACE 00 TRY START CONTROLLER 00000000 OK",
		"CROSS RETRY CIRCLE RELEASE SAVE TRIANGLE MARK START EXIT",
		"REPORT UX0 DATA PSVITA USB AUDIO MIDI DIAGNOSTIC TXT"
	};
	uint32_t bitmap_quads = 0;
	uint32_t atlas_quads = 0;
	uint32_t render_quads = 0;
	for (uint32_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
		bitmap_quads += psvita_usb_scope_text_bitmap_quads(lines[i]);
		atlas_quads += psvita_usb_scope_text_glyph_quads(lines[i]);
		render_quads += psvita_usb_scope_text_render_quads(lines[i]);
	}
	CHECK(bitmap_quads > PSVITA_USB_SCOPE_SAFE_QUAD_BUDGET);
	CHECK(atlas_quads <= PSVITA_USB_SCOPE_SAFE_QUAD_BUDGET);
	CHECK(render_quads == atlas_quads);
	return 0;
}

static int test_parser_and_filtering(void)
{
	const uint8_t packets[] = {
		0x0f, 0xf8, 0, 0,
		0x0f, 0xfa, 0, 0,
		0x0f, 0xfb, 0, 0,
		0x0f, 0xfc, 0, 0,
		/* Compatibility: real-time messages are cable-independent. */
		0x1f, 0xf8, 0, 0,
		/* Compatibility: ignore a host's incorrect CIN/padding for real-time. */
		0x05, 0xfa, 0x7f, 0x7f,
		0x0f, 0xfb, 0x12, 0x34,
		0x09, 0x90, 60, 100,
		0x03, 0xf2, 0, 0
	};
	PsvitaUsbMidiRing ring;
	uint32_t dropped = 0;
	psvita_usb_midi_ring_init(&ring);
	PsvitaUsbMidiParseResult parsed = psvita_usb_midi_parse_usb_packets(
		packets, sizeof(packets), 123456u, &ring, &dropped);
	CHECK(parsed.accepted == 7u);
	CHECK(parsed.filtered == 2u);
	CHECK(parsed.malformed == 0u);
	CHECK(dropped == 0u);
	PsvitaUsbMidiEvent events[7];
	CHECK(psvita_usb_midi_ring_pop(&ring, events, 7) == 7u);
	CHECK(events[0].timestamp_us == 123456u);
	CHECK(events[0].data[0] == 0xf8u);
	CHECK(events[2].data[0] == 0xfbu);
	CHECK(events[3].data[0] == 0xfcu);
	CHECK(events[4].data[0] == 0xf8u && events[4].cable == 0u);
	CHECK(events[5].data[0] == 0xfau && events[5].cable == 0u);
	CHECK(events[6].data[0] == 0xfbu && events[6].cable == 0u);
	const uint8_t bare_realtime[] = { 0xf8u, 0xfau, 0xfcu };
	parsed = psvita_usb_midi_parse_usb_packets(bare_realtime,
		sizeof(bare_realtime), 456789u, &ring, &dropped);
	CHECK(parsed.accepted == 3u);
	CHECK(parsed.filtered == 0u);
	CHECK(parsed.malformed == 1u);
	PsvitaUsbMidiEvent bare_events[3];
	CHECK(psvita_usb_midi_ring_pop(&ring, bare_events, 3u) == 3u);
	CHECK(bare_events[0].data[0] == 0xf8u);
	CHECK(bare_events[1].data[0] == 0xfau);
	CHECK(bare_events[2].data[0] == 0xfcu);
	return 0;
}

static int test_ring_wrap_and_overflow(void)
{
	PsvitaUsbMidiRing ring;
	PsvitaUsbMidiEvent event;
	memset(&event, 0, sizeof(event));
	event.size = 1;
	event.data[0] = 0xf8;
	psvita_usb_midi_ring_init(&ring);
	for (uint32_t i = 0; i < PSVITA_USB_MIDI_RING_CAPACITY; ++i) {
		event.timestamp_us = i;
		CHECK(psvita_usb_midi_ring_push(&ring, &event));
	}
	CHECK(!psvita_usb_midi_ring_push(&ring, &event));
	PsvitaUsbMidiEvent first[64];
	CHECK(psvita_usb_midi_ring_pop(&ring, first, 64) == 64u);
	CHECK(first[0].timestamp_us == 0u && first[63].timestamp_us == 63u);
	for (uint32_t i = 0; i < 64u; ++i) {
		event.timestamp_us = PSVITA_USB_MIDI_RING_CAPACITY + i;
		CHECK(psvita_usb_midi_ring_push(&ring, &event));
	}
	PsvitaUsbMidiEvent all[PSVITA_USB_MIDI_RING_CAPACITY];
	CHECK(psvita_usb_midi_ring_pop(&ring, all, PSVITA_USB_MIDI_RING_CAPACITY) ==
	      PSVITA_USB_MIDI_RING_CAPACITY);
	CHECK(all[0].timestamp_us == 64u);
	CHECK(all[PSVITA_USB_MIDI_RING_CAPACITY - 1u].timestamp_us == 319u);
	return 0;
}

static int test_encode_and_bounds(void)
{
	PsvitaUsbMidiEvent event;
	uint8_t packet[4];
	memset(&event, 0, sizeof(event));
	event.size = 1;
	event.data[0] = 0xfa;
	CHECK(psvita_usb_midi_encode_event(&event, packet));
	CHECK(packet[0] == 0x0f && packet[1] == 0xfa && packet[2] == 0);
	event.data[0] = 0x90;
	CHECK(!psvita_usb_midi_encode_event(&event, packet));
	event.data[0] = 0xf8;
	event.cable = 1;
	CHECK(!psvita_usb_midi_encode_event(&event, packet));
	CHECK(sizeof(PsvitaUsbMidiEvent) == 16u);
	CHECK(sizeof(PsvitaUsbAudioMidiStatus) == 32u);
	CHECK(sizeof(PsvitaUsbAudioMidiTraceEntry) == 8u);
	CHECK(sizeof(PsvitaUsbAudioMidiDiagnostics) == 420u);
	return 0;
}

static int test_timestamp_deadlines_and_latency_histogram(void)
{
	uint32_t histogram[PSVITA_USB_MIDI_LATENCY_BUCKETS] = { 0 };
	CHECK(psvita_usb_midi_deadline_wait_us(1010000u, 1000000u, 100000u) == 10000u);
	CHECK(psvita_usb_midi_deadline_wait_us(999999u, 1000000u, 100000u) == 0u);
	CHECK(psvita_usb_midi_deadline_wait_us(0u, 1000000u, 100000u) == 0u);
	CHECK(psvita_usb_midi_deadline_wait_us(2000000u, 1000000u, 100000u) == 100000u);
	CHECK(psvita_usb_midi_latency_bucket(124u) == 0u);
	CHECK(psvita_usb_midi_latency_bucket(125u) == 1u);
	CHECK(psvita_usb_midi_latency_bucket(12000u) == 7u);
	histogram[0] = 95;
	histogram[3] = 5;
	CHECK(psvita_usb_midi_histogram_percentile(histogram, 95u) == 125u);
	CHECK(psvita_usb_midi_histogram_percentile(histogram, 99u) == 1000u);
	return 0;
}

typedef struct {
	int alive;
	int release_count;
	int released_pid;
} MockOwner;

static int mock_owner_alive(void *context, int owner_pid)
{
	MockOwner *owner = (MockOwner *)context;
	(void)owner_pid;
	return owner->alive;
}

static int mock_release_owner(void *context, int owner_pid)
{
	MockOwner *owner = (MockOwner *)context;
	owner->release_count++;
	owner->released_pid = owner_pid;
	return 0;
}

static int test_dead_owner_watchdog(void)
{
	MockOwner owner = { 1, 0, -1 };
	CHECK(psvita_usb_audio_midi_owner_watchdog(42, mock_owner_alive,
	      mock_release_owner, &owner) == 0);
	CHECK(owner.release_count == 0);
	owner.alive = 0;
	CHECK(psvita_usb_audio_midi_owner_watchdog(42, mock_owner_alive,
	      mock_release_owner, &owner) == 0);
	CHECK(owner.release_count == 1);
	CHECK(owner.released_pid == 42);
	return 0;
}

static int test_connected_registered_mtp_stock_state(void)
{
	PsvitaUsbAudioMidiStockState stock;
	uint32_t flags = 0;
	memset(&stock, 0, sizeof(stock));
	CHECK(psvita_usb_audio_midi_classify_stock_state(
	      -1, 2, 1, 1, 0x2222, &stock, &flags) == 0);
	CHECK(stock.mtp_started == 0);
	CHECK(stock.psp_comm_started == 1);
	CHECK(stock.serial_started == 1);
	CHECK(stock.usb_active == 1);
	CHECK((flags & PSVITA_USB_AUDIO_MIDI_STATUS_CABLE_CONNECTED) != 0);
	CHECK((flags & PSVITA_USB_AUDIO_MIDI_STATUS_USB_ACTIVATED) != 0);
	return 0;
}

typedef struct {
	int fail_step;
	int call;
	int mtp_started;
	int psp_comm_started;
	int serial_started;
	int controller_started;
	int midi_started;
	int usb_active;
	int trace_count;
	int trace_phase[PSVITA_USB_AUDIO_MIDI_TRACE_CAPACITY];
	int trace_operation[PSVITA_USB_AUDIO_MIDI_TRACE_CAPACITY];
	int trace_result[PSVITA_USB_AUDIO_MIDI_TRACE_CAPACITY];
	int fail_step_2;
	int fail_result_2;
	int current_stop_leaves_controller;
} MockUsb;

static int mock_result(MockUsb *usb)
{
	usb->call++;
	if (usb->call == usb->fail_step) return -usb->call;
	if (usb->call == usb->fail_step_2) return usb->fail_result_2;
	return 0;
}
static int mock_deactivate(void *c) { MockUsb *u = c; int r = mock_result(u); if (!r) u->usb_active = 0; return r; }
static int mock_stop_controller(void *c)
{
	MockUsb *u = c;
	int r = mock_result(u);
	if (!r) {
		u->mtp_started = 0;
		u->psp_comm_started = 0;
		u->serial_started = 0;
		if (!u->current_stop_leaves_controller) u->controller_started = 0;
	}
	return r;
}
static int mock_start_controller(void *c) { MockUsb *u = c; int r = mock_result(u); if (!r) u->controller_started = 1; return r; }
static int mock_start_midi(void *c) { MockUsb *u = c; int r = mock_result(u); if (!r) u->midi_started = 1; return r; }
static int mock_activate_midi(void *c) { MockUsb *u = c; int r = mock_result(u); if (!r) u->usb_active = 1; return r; }
static int mock_stop_midi(void *c) { MockUsb *u = c; int r = mock_result(u); if (!r) u->midi_started = 0; return r; }
static int mock_start_mtp(void *c) { MockUsb *u = c; int r = mock_result(u); if (!r) u->mtp_started = 1; return r; }
static int mock_start_psp_comm(void *c) { MockUsb *u = c; int r = mock_result(u); if (!r) u->psp_comm_started = 1; return r; }
static int mock_start_serial(void *c) { MockUsb *u = c; int r = mock_result(u); if (!r) u->serial_started = 1; return r; }
static int mock_activate_mtp(void *c) { MockUsb *u = c; int r = mock_result(u); if (!r) u->usb_active = 1; return r; }
static int mock_controller_start_indicates_running(int result) { return result == -1000; }
static void mock_trace(void *c, int phase, int operation, int result)
{
	MockUsb *u = c;
	if (u->trace_count >= (int)PSVITA_USB_AUDIO_MIDI_TRACE_CAPACITY) return;
	u->trace_phase[u->trace_count] = phase;
	u->trace_operation[u->trace_count] = operation;
	u->trace_result[u->trace_count] = result;
	u->trace_count++;
}

static int check_stock_restored(const MockUsb *usb,
	const PsvitaUsbAudioMidiStockState *stock, int expected_usb_active)
{
	CHECK(usb->mtp_started == stock->mtp_started);
	CHECK(usb->psp_comm_started == stock->psp_comm_started);
	CHECK(usb->serial_started == stock->serial_started);
	CHECK(usb->controller_started == 1);
	CHECK(usb->usb_active == expected_usb_active);
	CHECK(usb->midi_started == 0);
	return 0;
}

static int test_failed_release_retains_state_for_retry(void)
{
	PsvitaUsbAudioMidiTakeoverOps ops = {
		.deactivate = mock_deactivate,
		.stop_controller = mock_stop_controller,
		.start_controller = mock_start_controller,
		.start_midi = mock_start_midi,
		.activate_midi = mock_activate_midi,
		.stop_midi = mock_stop_midi,
		.start_mtp = mock_start_mtp,
		.start_psp_comm = mock_start_psp_comm,
		.start_serial = mock_start_serial,
		.activate_mtp = mock_activate_mtp,
		.controller_start_indicates_running = mock_controller_start_indicates_running,
		.trace = mock_trace
	};
	PsvitaUsbAudioMidiStockState stock = { 1, 1, 1, 1 };
	PsvitaUsbAudioMidiTakeoverState takeover_state;
	MockUsb usb = {
		.mtp_started = 1,
		.psp_comm_started = 1,
		.serial_started = 1,
		.controller_started = 1,
		.usb_active = 1
	};
	int failed_step = -1;
	CHECK(psvita_usb_audio_midi_takeover(&ops, &usb, &stock, &takeover_state,
	      &failed_step) == 0);
	int owner_pid = 42;
	int midi_started = 1;
	usb.call = 0;
	usb.fail_step = 4; /* Fail START MTP once, after stopping USB MIDI. */
	int result = psvita_usb_audio_midi_restore(&ops, &usb, &stock, &takeover_state);
	CHECK(result < 0);
	CHECK(psvita_usb_audio_midi_release_state_finish(result, &owner_pid,
	      &midi_started, &stock, &takeover_state) == result);
	CHECK(owner_pid == 42);
	CHECK(midi_started == 1);
	CHECK(stock.mtp_started == 1 && takeover_state.mtp_stopped == 1);

	usb.call = 0;
	usb.fail_step = 0;
	result = psvita_usb_audio_midi_restore(&ops, &usb, &stock, &takeover_state);
	CHECK(result == 0);
	CHECK(check_stock_restored(&usb, &stock, 1) == 0);
	CHECK(psvita_usb_audio_midi_release_state_finish(result, &owner_pid,
	      &midi_started, &stock, &takeover_state) == 0);
	CHECK(owner_pid == -1 && midi_started == 0);
	CHECK(stock.mtp_started == 0 && takeover_state.mtp_stopped == 0);
	return 0;
}

static int test_takeover_rollback(void)
{
	PsvitaUsbAudioMidiTakeoverOps ops = {
		.deactivate = mock_deactivate,
		.stop_controller = mock_stop_controller,
		.start_controller = mock_start_controller,
		.start_midi = mock_start_midi,
		.activate_midi = mock_activate_midi,
		.stop_midi = mock_stop_midi,
		.start_mtp = mock_start_mtp,
		.start_psp_comm = mock_start_psp_comm,
		.start_serial = mock_start_serial,
		.activate_mtp = mock_activate_mtp,
		.controller_start_indicates_running = mock_controller_start_indicates_running,
		.trace = mock_trace
	};
	const PsvitaUsbAudioMidiStockState states[] = {
		{ 1, 1, 1, 1 }, /* Active MTP. */
		{ 0, 1, 1, 0 }, /* Observed cable-disconnected Vita idle state. */
		{ 1, 1, 1, 0 }, /* Started but inactive MTP. */
		{ 0, 1, 1, 1 }  /* Active controller with registered MTP (D2222). */
	};
	const int takeover_steps[] = { 5, 4, 4, 5 };
	const int restore_steps[] = { 7, 5, 6, 5 };

	/* DS8 must be able to acquire the hardware-observed cold-boot state
	 * directly. MIDI Scope is not part of this transition. */
	{
		PsvitaUsbAudioMidiStockState stock;
		PsvitaUsbAudioMidiTakeoverState takeover_state;
		uint32_t flags = 0;
		int failed_step = -1;
		CHECK(psvita_usb_audio_midi_classify_stock_state(
		      -1, 2, 1, 1, 0x2222, &stock, &flags) == 0);
		MockUsb usb = {
			.mtp_started = stock.mtp_started,
			.psp_comm_started = stock.psp_comm_started,
			.serial_started = stock.serial_started,
			.controller_started = 1,
			.usb_active = stock.usb_active
		};
		CHECK(psvita_usb_audio_midi_takeover(&ops, &usb, &stock, &takeover_state,
			&failed_step) == 0);
		CHECK(failed_step == PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_NONE);
		CHECK(usb.controller_started && usb.midi_started && usb.usb_active);
		CHECK(psvita_usb_audio_midi_restore(&ops, &usb, &stock, &takeover_state) == 0);
		CHECK(check_stock_restored(&usb, &stock, 0) == 0);
	}

	for (unsigned state_index = 0; state_index < 4; ++state_index) {
		const PsvitaUsbAudioMidiStockState *stock = &states[state_index];
		for (int fail = 1; fail <= takeover_steps[state_index]; ++fail) {
			int failed_step = PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_NONE;
			PsvitaUsbAudioMidiTakeoverState takeover_state;
			MockUsb usb = {
				.fail_step = fail,
				.mtp_started = stock->mtp_started,
				.psp_comm_started = stock->psp_comm_started,
				.serial_started = stock->serial_started,
				.controller_started = 1,
				.usb_active = stock->usb_active
			};
			int takeover_result = psvita_usb_audio_midi_takeover(
				&ops, &usb, stock, &takeover_state, &failed_step);
			CHECK(usb.trace_count > 0);
			int failure_operation = 0;
			for (int trace = 0; trace < usb.trace_count; ++trace) {
				if (usb.trace_phase[trace] == PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_FORWARD &&
				    usb.trace_result[trace] < 0)
					failure_operation = usb.trace_operation[trace];
			}
			CHECK(failure_operation != 0);
			CHECK(takeover_result < 0);
			CHECK(failed_step == failure_operation);
			CHECK(check_stock_restored(&usb, stock,
				fail == 1 ? stock->usb_active :
				(stock->usb_active && stock->mtp_started)) == 0);
		}

		int failed_step = -1;
		PsvitaUsbAudioMidiTakeoverState takeover_state;
		MockUsb usb = {
			.mtp_started = stock->mtp_started,
			.psp_comm_started = stock->psp_comm_started,
			.serial_started = stock->serial_started,
			.controller_started = 1,
			.usb_active = stock->usb_active
		};
		CHECK(psvita_usb_audio_midi_takeover(&ops, &usb, stock, &takeover_state,
			&failed_step) == 0);
		CHECK(failed_step == PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_NONE);
		CHECK(!usb.mtp_started && !usb.psp_comm_started && !usb.serial_started);
		CHECK(usb.controller_started && usb.midi_started && usb.usb_active);
		CHECK(psvita_usb_audio_midi_restore(&ops, &usb, stock, &takeover_state) == 0);
		CHECK(check_stock_restored(&usb, stock,
			stock->usb_active && stock->mtp_started) == 0);

		for (int fail = 1; fail <= restore_steps[state_index]; ++fail) {
			MockUsb restoring = {
				.fail_step = fail,
				.controller_started = 1,
				.midi_started = 1,
				.usb_active = 1
			};
			CHECK(psvita_usb_audio_midi_restore(&ops, &restoring, stock,
				&takeover_state) < 0);
			CHECK(restoring.call == restore_steps[state_index]);
		}
	}

	/* A current-personality stop may leave the controller running. */
	{
		const PsvitaUsbAudioMidiStockState stock = { 0, 1, 1, 0 };
		PsvitaUsbAudioMidiTakeoverState takeover_state;
		int failed_step = -1;
		MockUsb usb = {
			.psp_comm_started = 1,
			.serial_started = 1,
			.controller_started = 1
		};
		usb.current_stop_leaves_controller = 1;
		usb.fail_step_2 = 2;
		usb.fail_result_2 = -1000;
		CHECK(psvita_usb_audio_midi_takeover(&ops, &usb, &stock, &takeover_state,
			&failed_step) == 0);
		CHECK(failed_step == PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_NONE);
		CHECK(takeover_state.controller_cycled);
		CHECK(usb.controller_started && usb.midi_started && usb.usb_active);
		CHECK(usb.trace_result[0] == 0);
		CHECK(usb.trace_result[1] == -1000);
		CHECK(psvita_usb_audio_midi_restore(&ops, &usb, &stock, &takeover_state) == 0);
		CHECK(check_stock_restored(&usb, &stock,
			stock.usb_active && stock.mtp_started) == 0);
	}
	return 0;
}

int main(void)
{
	if (test_descriptors()) return 1;
	if (test_audio_set_interface_request()) return 1;
	if (test_audio_ring_and_silence()) return 1;
	if (test_audio_packet_clock()) return 1;
	if (test_audio_completion_health()) return 1;
	if (test_audio_packet_priming_and_rebuffer()) return 1;
	if (test_audio_low_reservoir_uses_available_whole_packets()) return 1;
	if (test_audio_failed_submit_rolls_back_prepared_frames()) return 1;
	if (test_audio_completion_keeps_slot_owned_until_cleanup_finishes()) return 1;
	if (test_audio_request_cancel_and_timer_wrap_are_race_safe()) return 1;
	if (test_audio_ten_channel_layout()) return 1;
	if (test_audio_multichannel_ingress_is_one_transaction()) return 1;
	if (test_audio_input_ring_and_api_contract()) return 1;
	if (test_audio_diagnostic_flight_recorder()) return 1;
	if (test_scope_streaming_render_budget()) return 1;
	if (test_parser_and_filtering()) return 1;
	if (test_ring_wrap_and_overflow()) return 1;
	if (test_encode_and_bounds()) return 1;
	if (test_timestamp_deadlines_and_latency_histogram()) return 1;
	if (test_dead_owner_watchdog()) return 1;
	if (test_connected_registered_mtp_stock_state()) return 1;
	if (test_failed_release_retains_state_for_retry()) return 1;
	if (test_takeover_rollback()) return 1;
	puts("psvita-usb-audio-midi core tests passed");
	return 0;
}
