#include "usb_descriptors.h"

/*
 * UAC1 AudioControl topology and collection:
 * DS-8 master L/R plus eight discrete track taps (terminal 1) -> USB
 * streaming output terminal (2). The host playback stream enters at USB
 * streaming terminal (3) and leaves through the Vita speaker terminal (4).
 * FL/FR describe the master pair; Track 1-8 are the eight undefined channels
 * named by string descriptors 4-11. The AudioControl collection lists only
 * its two AudioStreaming interfaces; MIDIStreaming remains independent.
 */
const uint8_t psvita_usb_audio_control_extra[
	PSVITA_USB_AUDIO_CONTROL_EXTRA_SIZE] = {
	10, 0x24, 0x01, 0x00, 0x01, PSVITA_USB_AUDIO_CONTROL_EXTRA_SIZE, 0, 2,
		PSVITA_USB_AUDIO_STREAM_INTERFACE,
		PSVITA_USB_AUDIO_INPUT_STREAM_INTERFACE,
	12, 0x24, 0x02, 1, 0x03, 0x06, 0, 10, 0x03, 0x00, 4, 0,
	9, 0x24, 0x03, 2, 0x01, 0x01, 0, 1, 0,
	12, 0x24, 0x02, 3, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0,
	9, 0x24, 0x03, 4, 0x01, 0x03, 0, 3, 0
};

/* UAC1 AS general + Type I PCM, fixed 48 kHz, 10-channel signed 16-bit. */
const uint8_t psvita_usb_audio_as_extra[
	PSVITA_USB_AUDIO_STREAM_EXTRA_SIZE] = {
	7, 0x24, 0x01, 2, 1, 0x01, 0x00,
	11, 0x24, 0x02, 1, PSVITA_USB_AUDIO_STREAM_CHANNELS, 2,
		PSVITA_USB_AUDIO_BITS_PER_SAMPLE, 1, 0x80, 0xbb, 0x00
};

/* Host-to-Vita UAC1 Type I PCM, fixed 48 kHz stereo signed 16-bit. */
const uint8_t psvita_usb_audio_input_as_extra[
	PSVITA_USB_AUDIO_STREAM_EXTRA_SIZE] = {
	7, 0x24, 0x01, 3, 1, 0x01, 0x00,
	11, 0x24, 0x02, 1, PSVITA_USB_AUDIO_INPUT_CHANNELS, 2,
		PSVITA_USB_AUDIO_BITS_PER_SAMPLE, 1, 0x80, 0xbb, 0x00
};

/* 9-byte audio endpoint extension followed by the class-specific endpoint. */
const uint8_t psvita_usb_audio_in_extra[
	PSVITA_USB_AUDIO_ENDPOINT_EXTRA_SIZE] = {
	0, 0,
	7, 0x25, 0x01, 0, 0, 0, 0
};

const uint8_t psvita_usb_audio_out_extra[
	PSVITA_USB_AUDIO_ENDPOINT_EXTRA_SIZE] = {
	0, 0,
	7, 0x25, 0x01, 0, 0, 0, 0
};

/* MIDIStreaming header and one bidirectional cable's four jacks. */
const uint8_t psvita_usb_midi_ms_extra[
	PSVITA_USB_MIDI_STREAM_EXTRA_SIZE] = {
	7, 0x24, 0x01, 0x00, 0x01, PSVITA_USB_MIDI_STREAM_EXTRA_SIZE, 0,
	6, 0x24, 0x02, 0x01, 1, 0,
	6, 0x24, 0x02, 0x02, 2, 0,
	9, 0x24, 0x03, 0x01, 3, 1, 2, 1, 0,
	9, 0x24, 0x03, 0x02, 4, 1, 1, 1, 0
};

/*
 * MIDI 1.0 uses the 9-byte Audio-class endpoint form. SceUdcd models the
 * common 7-byte prefix, so bRefresh/bSynchAddress precede the CS descriptor
 * in extra.
 */
const uint8_t psvita_usb_midi_out_extra[
	PSVITA_USB_MIDI_ENDPOINT_EXTRA_SIZE] = {
	0, 0, 5, 0x25, 0x01, 1, 1
};
const uint8_t psvita_usb_midi_in_extra[
	PSVITA_USB_MIDI_ENDPOINT_EXTRA_SIZE] = {
	0, 0, 5, 0x25, 0x01, 1, 3
};

static int descriptor_chain_valid(const uint8_t *bytes, size_t size,
	size_t offset)
{
	while (offset < size) {
		size_t descriptor_size = bytes[offset];
		if (descriptor_size < 2u || descriptor_size > size - offset)
			return 0;
		offset += descriptor_size;
	}
	return offset == size;
}

int psvita_usb_audio_midi_validate_descriptor_layout(void)
{
	const uint32_t audio_bytes = 48u * PSVITA_USB_AUDIO_STREAM_CHANNELS *
		(PSVITA_USB_AUDIO_BITS_PER_SAMPLE / 8u);
	const uint32_t audio_input_bytes = 49u * PSVITA_USB_AUDIO_INPUT_CHANNELS *
		(PSVITA_USB_AUDIO_BITS_PER_SAMPLE / 8u);

	return PSVITA_USB_AUDIO_MIDI_CONFIG_TOTAL_LENGTH == 248u &&
		PSVITA_USB_UDCD_ENDPOINT_COUNT ==
			PSVITA_USB_AUDIO_INPUT_DRIVER_ENDPOINT + 1u &&
		PSVITA_USB_INTERFACE_COUNT == 4u &&
		PSVITA_USB_INTERFACE_DESCRIPTOR_COUNT == 6u &&
		PSVITA_USB_STRING_DESCRIPTOR_COUNT == 12u &&
		(PSVITA_USB_AUDIO_IN_ENDPOINT_ADDRESS & 0x0fu) ==
			PSVITA_USB_AUDIO_DRIVER_ENDPOINT &&
		(PSVITA_USB_AUDIO_OUT_ENDPOINT_ADDRESS & 0x0fu) ==
			PSVITA_USB_AUDIO_INPUT_DRIVER_ENDPOINT &&
		(PSVITA_USB_MIDI_OUT_ENDPOINT_ADDRESS & 0x0fu) ==
			PSVITA_USB_MIDI_OUT_DRIVER_ENDPOINT &&
		(PSVITA_USB_MIDI_IN_ENDPOINT_ADDRESS & 0x0fu) ==
			PSVITA_USB_MIDI_IN_DRIVER_ENDPOINT &&
		PSVITA_USB_AUDIO_MAX_PACKET_BYTES == audio_bytes &&
		PSVITA_USB_AUDIO_INPUT_MAX_PACKET_BYTES == audio_input_bytes &&
		PSVITA_USB_AUDIO_MAX_PACKET_BYTES <= 1023u &&
		descriptor_chain_valid(psvita_usb_audio_control_extra,
			sizeof(psvita_usb_audio_control_extra), 0u) &&
		descriptor_chain_valid(psvita_usb_audio_as_extra,
			sizeof(psvita_usb_audio_as_extra), 0u) &&
		descriptor_chain_valid(psvita_usb_audio_input_as_extra,
			sizeof(psvita_usb_audio_input_as_extra), 0u) &&
		descriptor_chain_valid(psvita_usb_audio_in_extra,
			sizeof(psvita_usb_audio_in_extra), 2u) &&
		descriptor_chain_valid(psvita_usb_audio_out_extra,
			sizeof(psvita_usb_audio_out_extra), 2u) &&
		descriptor_chain_valid(psvita_usb_midi_ms_extra,
			sizeof(psvita_usb_midi_ms_extra), 0u) &&
		descriptor_chain_valid(psvita_usb_midi_out_extra,
			sizeof(psvita_usb_midi_out_extra), 2u) &&
		descriptor_chain_valid(psvita_usb_midi_in_extra,
			sizeof(psvita_usb_midi_in_extra), 2u) &&
		psvita_usb_audio_control_extra[5] ==
			sizeof(psvita_usb_audio_control_extra) &&
		psvita_usb_audio_control_extra[7] == 2u &&
		psvita_usb_audio_control_extra[8] ==
			PSVITA_USB_AUDIO_STREAM_INTERFACE &&
		psvita_usb_audio_control_extra[9] ==
			PSVITA_USB_AUDIO_INPUT_STREAM_INTERFACE &&
		psvita_usb_audio_as_extra[11] ==
			PSVITA_USB_AUDIO_STREAM_CHANNELS &&
		psvita_usb_audio_input_as_extra[11] ==
			PSVITA_USB_AUDIO_INPUT_CHANNELS &&
		psvita_usb_midi_ms_extra[5] ==
			sizeof(psvita_usb_midi_ms_extra);
}
