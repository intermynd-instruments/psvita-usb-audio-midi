#include "kernel_internal.h"
#include "audio_control.h"
#include "usb_descriptors.h"

#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/udcd.h>
#include <string.h>

#define MIDI_RX_BYTES 512u
#define MIDI_TX_BYTES 256u
#define AUDIO_ENDPOINT_INDEX PSVITA_USB_AUDIO_DRIVER_ENDPOINT
#define AUDIO_INPUT_ENDPOINT_INDEX PSVITA_USB_AUDIO_INPUT_DRIVER_ENDPOINT
#define MIDI_OUT_ENDPOINT_INDEX PSVITA_USB_MIDI_OUT_DRIVER_ENDPOINT
#define MIDI_IN_ENDPOINT_INDEX PSVITA_USB_MIDI_IN_DRIVER_ENDPOINT
#define AUDIO_REQUEST_COUNT PSVITA_USB_AUDIO_MAX_IN_FLIGHT_REQUESTS
#define AUDIO_BUFFER_ALLOC_SIZE PSVITA_USB_AUDIO_DMA_BUFFER_BYTES
#define AUDIO_INPUT_REQUEST_COUNT 1u
#define AUDIO_INPUT_REQUEST_INTERVALS 1u
#define AUDIO_INPUT_REQUEST_FRAMES (49u * AUDIO_INPUT_REQUEST_INTERVALS)
#define AUDIO_INPUT_BUFFER_BYTES \
	(AUDIO_INPUT_REQUEST_FRAMES * PSVITA_USB_AUDIO_INPUT_CHANNELS * sizeof(int16_t))
#define AUDIO_INPUT_BUFFER_ALLOC_SIZE 4096u
/* Let SceUdcd finish SET_INTERFACE before the compatibility fallback starts. */
#define AUDIO_ALTERNATE_FALLBACK_US 20000u
/* Even changeSetting may run while the controller is finishing the EP switch. */
#define AUDIO_ALTERNATE_SETTLE_US 5000u
/* A normal batched isochronous IN request should finish in 8 ms. */
#define AUDIO_REQUEST_STALL_US 250000u

static SceUdcdEndpoint endpoints[PSVITA_USB_UDCD_ENDPOINT_COUNT] = {
	{ USB_ENDPOINT_OUT, 0, 0, 0 },
	{ USB_ENDPOINT_OUT, MIDI_OUT_ENDPOINT_INDEX, 0, 0 },
	{ USB_ENDPOINT_IN,  MIDI_IN_ENDPOINT_INDEX, 0, 0 },
	{ USB_ENDPOINT_IN,  AUDIO_ENDPOINT_INDEX, 0, 0 },
	{ USB_ENDPOINT_OUT, AUDIO_INPUT_ENDPOINT_INDEX, 0, 0 }
};
/* One UDCD allocation owns AudioControl, AudioStreaming, and MIDIStreaming. */
static SceUdcdInterface interface = { -1, 0, PSVITA_USB_INTERFACE_COUNT };

static SceUdcdEndpointDescriptor
	fs_endpoints[PSVITA_USB_UDCD_ENDPOINT_COUNT] = {
	[PSVITA_USB_AUDIO_DESCRIPTOR_ENDPOINT_INDEX] =
	{ 9, USB_DT_ENDPOINT, PSVITA_USB_AUDIO_IN_ENDPOINT_ADDRESS,
	  PSVITA_USB_AUDIO_ENDPOINT_ATTRIBUTES, PSVITA_USB_AUDIO_MAX_PACKET_BYTES,
	  PSVITA_USB_AUDIO_FULL_SPEED_INTERVAL,
	  (unsigned char *)psvita_usb_audio_in_extra,
	  PSVITA_USB_AUDIO_ENDPOINT_EXTRA_SIZE },
	[PSVITA_USB_AUDIO_INPUT_DESCRIPTOR_ENDPOINT_INDEX] =
	{ 9, USB_DT_ENDPOINT, PSVITA_USB_AUDIO_OUT_ENDPOINT_ADDRESS,
	  PSVITA_USB_AUDIO_INPUT_ENDPOINT_ATTRIBUTES,
	  PSVITA_USB_AUDIO_INPUT_MAX_PACKET_BYTES,
	  PSVITA_USB_AUDIO_FULL_SPEED_INTERVAL,
	  (unsigned char *)psvita_usb_audio_out_extra,
	  PSVITA_USB_AUDIO_ENDPOINT_EXTRA_SIZE },
	[PSVITA_USB_MIDI_OUT_DESCRIPTOR_ENDPOINT_INDEX] =
	{ 9, USB_DT_ENDPOINT, PSVITA_USB_MIDI_OUT_ENDPOINT_ADDRESS,
	  USB_ENDPOINT_TYPE_BULK, 64, 0,
	  (unsigned char *)psvita_usb_midi_out_extra,
	  PSVITA_USB_MIDI_ENDPOINT_EXTRA_SIZE },
	[PSVITA_USB_MIDI_IN_DESCRIPTOR_ENDPOINT_INDEX] =
	{ 9, USB_DT_ENDPOINT, PSVITA_USB_MIDI_IN_ENDPOINT_ADDRESS,
	  USB_ENDPOINT_TYPE_BULK, 64, 0,
	  (unsigned char *)psvita_usb_midi_in_extra,
	  PSVITA_USB_MIDI_ENDPOINT_EXTRA_SIZE },
	{ 0 }
};
static SceUdcdEndpointDescriptor
	hs_endpoints[PSVITA_USB_UDCD_ENDPOINT_COUNT] = {
	[PSVITA_USB_AUDIO_DESCRIPTOR_ENDPOINT_INDEX] =
	{ 9, USB_DT_ENDPOINT, PSVITA_USB_AUDIO_IN_ENDPOINT_ADDRESS,
	  PSVITA_USB_AUDIO_ENDPOINT_ATTRIBUTES, PSVITA_USB_AUDIO_MAX_PACKET_BYTES,
	  PSVITA_USB_AUDIO_HIGH_SPEED_INTERVAL,
	  (unsigned char *)psvita_usb_audio_in_extra,
	  PSVITA_USB_AUDIO_ENDPOINT_EXTRA_SIZE },
	[PSVITA_USB_AUDIO_INPUT_DESCRIPTOR_ENDPOINT_INDEX] =
	{ 9, USB_DT_ENDPOINT, PSVITA_USB_AUDIO_OUT_ENDPOINT_ADDRESS,
	  PSVITA_USB_AUDIO_INPUT_ENDPOINT_ATTRIBUTES,
	  PSVITA_USB_AUDIO_INPUT_MAX_PACKET_BYTES,
	  PSVITA_USB_AUDIO_HIGH_SPEED_INTERVAL,
	  (unsigned char *)psvita_usb_audio_out_extra,
	  PSVITA_USB_AUDIO_ENDPOINT_EXTRA_SIZE },
	[PSVITA_USB_MIDI_OUT_DESCRIPTOR_ENDPOINT_INDEX] =
	{ 9, USB_DT_ENDPOINT, PSVITA_USB_MIDI_OUT_ENDPOINT_ADDRESS,
	  USB_ENDPOINT_TYPE_BULK, 512, 0,
	  (unsigned char *)psvita_usb_midi_out_extra,
	  PSVITA_USB_MIDI_ENDPOINT_EXTRA_SIZE },
	[PSVITA_USB_MIDI_IN_DESCRIPTOR_ENDPOINT_INDEX] =
	{ 9, USB_DT_ENDPOINT, PSVITA_USB_MIDI_IN_ENDPOINT_ADDRESS,
	  USB_ENDPOINT_TYPE_BULK, 512, 0,
	  (unsigned char *)psvita_usb_midi_in_extra,
	  PSVITA_USB_MIDI_ENDPOINT_EXTRA_SIZE },
	{ 0 }
};

static SceUdcdInterfaceDescriptor
	fs_interfaces[PSVITA_USB_INTERFACE_DESCRIPTOR_COUNT + 1u] = {
	{ 9, USB_DT_INTERFACE, PSVITA_USB_AUDIO_CONTROL_INTERFACE,
	  0, 0, USB_CLASS_AUDIO, 1, 0, 0,
	  NULL, (unsigned char *)psvita_usb_audio_control_extra,
	  PSVITA_USB_AUDIO_CONTROL_EXTRA_SIZE },
	{ 9, USB_DT_INTERFACE, PSVITA_USB_AUDIO_STREAM_INTERFACE,
	  0, 0, USB_CLASS_AUDIO, 2, 0, 0,
	  NULL, NULL, 0 },
	{ 9, USB_DT_INTERFACE, PSVITA_USB_AUDIO_STREAM_INTERFACE,
	  1, 1, USB_CLASS_AUDIO, 2, 0, 0,
	  &fs_endpoints[PSVITA_USB_AUDIO_DESCRIPTOR_ENDPOINT_INDEX],
	  (unsigned char *)psvita_usb_audio_as_extra,
	  PSVITA_USB_AUDIO_STREAM_EXTRA_SIZE },
	{ 9, USB_DT_INTERFACE, PSVITA_USB_AUDIO_INPUT_STREAM_INTERFACE,
	  0, 0, USB_CLASS_AUDIO, 2, 0, 0,
	  NULL, NULL, 0 },
	{ 9, USB_DT_INTERFACE, PSVITA_USB_AUDIO_INPUT_STREAM_INTERFACE,
	  1, 1, USB_CLASS_AUDIO, 2, 0, 0,
	  &fs_endpoints[PSVITA_USB_AUDIO_INPUT_DESCRIPTOR_ENDPOINT_INDEX],
	  (unsigned char *)psvita_usb_audio_input_as_extra,
	  PSVITA_USB_AUDIO_STREAM_EXTRA_SIZE },
	{ 9, USB_DT_INTERFACE, PSVITA_USB_MIDI_STREAM_INTERFACE,
	  0, 2, USB_CLASS_AUDIO, 3, 0, 0,
	  &fs_endpoints[PSVITA_USB_MIDI_OUT_DESCRIPTOR_ENDPOINT_INDEX],
	  (unsigned char *)psvita_usb_midi_ms_extra,
	  PSVITA_USB_MIDI_STREAM_EXTRA_SIZE },
	{ 0 }
};
static SceUdcdInterfaceDescriptor
	hs_interfaces[PSVITA_USB_INTERFACE_DESCRIPTOR_COUNT + 1u] = {
	{ 9, USB_DT_INTERFACE, PSVITA_USB_AUDIO_CONTROL_INTERFACE,
	  0, 0, USB_CLASS_AUDIO, 1, 0, 0,
	  NULL, (unsigned char *)psvita_usb_audio_control_extra,
	  PSVITA_USB_AUDIO_CONTROL_EXTRA_SIZE },
	{ 9, USB_DT_INTERFACE, PSVITA_USB_AUDIO_STREAM_INTERFACE,
	  0, 0, USB_CLASS_AUDIO, 2, 0, 0,
	  NULL, NULL, 0 },
	{ 9, USB_DT_INTERFACE, PSVITA_USB_AUDIO_STREAM_INTERFACE,
	  1, 1, USB_CLASS_AUDIO, 2, 0, 0,
	  &hs_endpoints[PSVITA_USB_AUDIO_DESCRIPTOR_ENDPOINT_INDEX],
	  (unsigned char *)psvita_usb_audio_as_extra,
	  PSVITA_USB_AUDIO_STREAM_EXTRA_SIZE },
	{ 9, USB_DT_INTERFACE, PSVITA_USB_AUDIO_INPUT_STREAM_INTERFACE,
	  0, 0, USB_CLASS_AUDIO, 2, 0, 0,
	  NULL, NULL, 0 },
	{ 9, USB_DT_INTERFACE, PSVITA_USB_AUDIO_INPUT_STREAM_INTERFACE,
	  1, 1, USB_CLASS_AUDIO, 2, 0, 0,
	  &hs_endpoints[PSVITA_USB_AUDIO_INPUT_DESCRIPTOR_ENDPOINT_INDEX],
	  (unsigned char *)psvita_usb_audio_input_as_extra,
	  PSVITA_USB_AUDIO_STREAM_EXTRA_SIZE },
	{ 9, USB_DT_INTERFACE, PSVITA_USB_MIDI_STREAM_INTERFACE,
	  0, 2, USB_CLASS_AUDIO, 3, 0, 0,
	  &hs_endpoints[PSVITA_USB_MIDI_OUT_DESCRIPTOR_ENDPOINT_INDEX],
	  (unsigned char *)psvita_usb_midi_ms_extra,
	  PSVITA_USB_MIDI_STREAM_EXTRA_SIZE },
	{ 0 }
};
static SceUdcdInterfaceSettings fs_settings[PSVITA_USB_INTERFACE_COUNT] = {
	{ &fs_interfaces[0], 0, 1 },
	{ &fs_interfaces[1], 0, 2 },
	{ &fs_interfaces[3], 0, 2 },
	{ &fs_interfaces[5], 0, 1 }
};
static SceUdcdInterfaceSettings hs_settings[PSVITA_USB_INTERFACE_COUNT] = {
	{ &hs_interfaces[0], 0, 1 },
	{ &hs_interfaces[1], 0, 2 },
	{ &hs_interfaces[3], 0, 2 },
	{ &hs_interfaces[5], 0, 1 }
};
static SceUdcdConfigDescriptor fs_config_desc = {
	9, USB_DT_CONFIG, PSVITA_USB_AUDIO_MIDI_CONFIG_TOTAL_LENGTH,
	PSVITA_USB_INTERFACE_COUNT, 1, 0, 0x80, 50,
	fs_settings, NULL, 0
};
static SceUdcdConfigDescriptor hs_config_desc = {
	9, USB_DT_CONFIG, PSVITA_USB_AUDIO_MIDI_CONFIG_TOTAL_LENGTH,
	PSVITA_USB_INTERFACE_COUNT, 1, 0, 0x80, 50,
	hs_settings, NULL, 0
};
static SceUdcdConfiguration fs_config = {
	&fs_config_desc, fs_settings, fs_interfaces, fs_endpoints
};
static SceUdcdConfiguration hs_config = {
	&hs_config_desc, hs_settings, hs_interfaces, hs_endpoints
};

static SceUdcdDeviceDescriptor fs_device = {
	18, USB_DT_DEVICE, 0x0200, USB_CLASS_PER_INTERFACE, 0, 0, 64,
	0x054c, PSVITA_USB_AUDIO_MIDI_DEVELOPMENT_PID, 0x0107, 1, 2, 3, 1
};
static SceUdcdDeviceDescriptor hs_device = {
	18, USB_DT_DEVICE, 0x0200, USB_CLASS_PER_INTERFACE, 0, 0, 64,
	0x054c, PSVITA_USB_AUDIO_MIDI_DEVELOPMENT_PID, 0x0107, 1, 2, 3, 1
};

static SceUdcdStringDescriptor strings[PSVITA_USB_STRING_DESCRIPTOR_COUNT] = {
	{ 4, USB_DT_STRING, { 0x0409 } },
	{ 44, USB_DT_STRING, { 'I','n','t','e','r','m','y','n','d',' ','I','n','s','t','r','u','m','e','n','t','s' } },
	{ 24, USB_DT_STRING, { 'P','S',' ','V','i','t','a',' ','U','S','B' } },
	{ 20, USB_DT_STRING, { 'P','S','V','I','T','A','U','S','B' } },
	{ 16, USB_DT_STRING, { 'T','r','a','c','k',' ','1' } },
	{ 16, USB_DT_STRING, { 'T','r','a','c','k',' ','2' } },
	{ 16, USB_DT_STRING, { 'T','r','a','c','k',' ','3' } },
	{ 16, USB_DT_STRING, { 'T','r','a','c','k',' ','4' } },
	{ 16, USB_DT_STRING, { 'T','r','a','c','k',' ','5' } },
	{ 16, USB_DT_STRING, { 'T','r','a','c','k',' ','6' } },
	{ 16, USB_DT_STRING, { 'T','r','a','c','k',' ','7' } },
	{ 16, USB_DT_STRING, { 'T','r','a','c','k',' ','8' } }
};

static uint8_t rx_buffer[MIDI_RX_BYTES] __attribute__((aligned(64)));
static uint8_t tx_buffer[MIDI_TX_BYTES] __attribute__((aligned(64)));
static SceUID audio_buffer_uid = -1;
static int16_t (*audio_buffers)
	[PSVITA_USB_AUDIO_REQUEST_FRAMES * PSVITA_USB_AUDIO_STREAM_CHANNELS];
static SceUID audio_input_buffer_uid = -1;
static int16_t (*audio_input_buffers)
	[AUDIO_INPUT_REQUEST_FRAMES * PSVITA_USB_AUDIO_INPUT_CHANNELS];
static PsvitaUsbAudioRing audio_ring;
static PsvitaUsbAudioRing audio_input_ring;
static PsvitaUsbAudioPacketClock audio_clock;
static PsvitaUsbAudioCompletionClock audio_completion_clock;
static PsvitaUsbAudioStatus audio_status;
static PsvitaUsbAudioInputStatus audio_input_status;
static volatile uint32_t rx_length;
static volatile uint64_t rx_timestamp_us;
static volatile int rx_available;
static volatile int rx_pending;
static volatile int tx_pending;
static volatile uint32_t audio_pending_count;
static volatile uint32_t audio_input_pending_count;
static volatile int audio_enabled;
static volatile int audio_streaming;
static volatile int audio_input_streaming;
static volatile int audio_requested_alternate;
static volatile int audio_applied_alternate;
static volatile int audio_input_requested_alternate;
static volatile int audio_input_applied_alternate;
static volatile uint32_t audio_alternate_requested_us;
static volatile uint32_t audio_input_alternate_requested_us;
static volatile uint32_t audio_stream_started_us;
static volatile uint32_t audio_last_completion_us;
static volatile int audio_recovery_clear_fifo;
static volatile int connected;
static volatile int configured;
static volatile uint32_t rx_submit_attempts;
static volatile uint32_t rx_submit_failures;
static volatile uint32_t rx_callback_count;
static volatile uint32_t rx_callback_bytes;
static volatile int rx_last_submit_result;
static volatile int rx_last_callback_result;
static volatile uint32_t rx_last_transfer_length;
static volatile uint32_t rx_last_packet_word;
static volatile int audio_submit_endpoint_number = -1;
static volatile int audio_input_submit_endpoint_number = -1;
static SceUdcdDeviceRequest rx_request;
static SceUdcdDeviceRequest tx_request;
static SceUdcdDeviceRequest audio_requests[AUDIO_REQUEST_COUNT];
static SceUdcdDeviceRequest audio_input_requests[AUDIO_INPUT_REQUEST_COUNT];
static PsvitaUsbAudioRequestState audio_request_states[AUDIO_REQUEST_COUNT];
static PsvitaUsbAudioRequestState
	audio_input_request_states[AUDIO_INPUT_REQUEST_COUNT];
static volatile uint8_t audio_request_recovering[AUDIO_REQUEST_COUNT];

static uint32_t audio_now_us(void)
{
	return (uint32_t)ksceKernelGetSystemTimeWide();
}

static void reset_streaming_alternates(void)
{
	audio_streaming = 0;
	audio_input_streaming = 0;
	audio_requested_alternate = 0;
	audio_applied_alternate = 0;
	audio_input_requested_alternate = 0;
	audio_input_applied_alternate = 0;
	audio_input_alternate_requested_us = 0;
}

static int audio_buffer_init(void)
{
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
		SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
	opt.alignment = AUDIO_BUFFER_ALLOC_SIZE;
	audio_buffer_uid = ksceKernelAllocMemBlock("psvita_usb_audio",
		SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_NC_RW,
		AUDIO_BUFFER_ALLOC_SIZE, &opt);
	if (audio_buffer_uid < 0) return audio_buffer_uid;
	int result = ksceKernelGetMemBlockBase(audio_buffer_uid,
		(void **)&audio_buffers);
	if (result < 0) {
		ksceKernelFreeMemBlock(audio_buffer_uid);
		audio_buffer_uid = -1;
		audio_buffers = NULL;
		return result;
	}
	memset(audio_buffers, 0, AUDIO_BUFFER_ALLOC_SIZE);
	opt.alignment = AUDIO_INPUT_BUFFER_ALLOC_SIZE;
	audio_input_buffer_uid = ksceKernelAllocMemBlock("psvita_usb_audio_input",
		SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_NC_RW,
		AUDIO_INPUT_BUFFER_ALLOC_SIZE, &opt);
	if (audio_input_buffer_uid < 0) {
		ksceKernelFreeMemBlock(audio_buffer_uid);
		audio_buffer_uid = -1;
		audio_buffers = NULL;
		return audio_input_buffer_uid;
	}
	result = ksceKernelGetMemBlockBase(audio_input_buffer_uid,
		(void **)&audio_input_buffers);
	if (result < 0) {
		ksceKernelFreeMemBlock(audio_input_buffer_uid);
		ksceKernelFreeMemBlock(audio_buffer_uid);
		audio_input_buffer_uid = -1;
		audio_buffer_uid = -1;
		audio_input_buffers = NULL;
		audio_buffers = NULL;
		return result;
	}
	memset(audio_input_buffers, 0, AUDIO_INPUT_BUFFER_ALLOC_SIZE);
	return 0;
}

static void audio_buffer_term(void)
{
	if (audio_input_buffer_uid >= 0)
		ksceKernelFreeMemBlock(audio_input_buffer_uid);
	if (audio_buffer_uid >= 0) ksceKernelFreeMemBlock(audio_buffer_uid);
	audio_input_buffer_uid = -1;
	audio_buffer_uid = -1;
	audio_input_buffers = NULL;
	audio_buffers = NULL;
}

static void audio_status_init(void)
{
	memset(&audio_status, 0, sizeof(audio_status));
	audio_status.size = sizeof(audio_status);
	audio_status.protocol_version = PSVITA_USB_AUDIO_PROTOCOL_VERSION;
	audio_status.state = PSVITA_USB_AUDIO_STATE_DISABLED;
	audio_status.sample_rate = PSVITA_USB_AUDIO_SAMPLE_RATE;
	audio_status.channels = PSVITA_USB_AUDIO_STREAM_CHANNELS;
	audio_status.bit_depth = PSVITA_USB_AUDIO_BITS_PER_SAMPLE;
	audio_status.ring_capacity_frames = PSVITA_USB_AUDIO_RING_FRAMES;
	audio_status.minimum_buffered_frames = PSVITA_USB_AUDIO_RING_FRAMES;
	psvita_usb_audio_ring_init(&audio_ring, PSVITA_USB_AUDIO_STREAM_CHANNELS);
	psvita_usb_audio_ring_init(&audio_input_ring,
		PSVITA_USB_AUDIO_INPUT_CHANNELS);
	psvita_usb_audio_packet_clock_init(&audio_clock);
	psvita_usb_audio_completion_clock_init(&audio_completion_clock);
	audio_submit_endpoint_number = -1;
	audio_requested_alternate = 0;
	audio_applied_alternate = 0;
	audio_alternate_requested_us = 0;
	audio_stream_started_us = 0;
	audio_last_completion_us = 0;
	audio_recovery_clear_fifo = 0;
	memset(&audio_input_status, 0, sizeof(audio_input_status));
	audio_input_status.size = sizeof(audio_input_status);
	audio_input_status.protocol_version = PSVITA_USB_AUDIO_PROTOCOL_VERSION;
	audio_input_status.state = PSVITA_USB_AUDIO_STATE_DISABLED;
	audio_input_status.sample_rate = PSVITA_USB_AUDIO_SAMPLE_RATE;
	audio_input_status.channels = PSVITA_USB_AUDIO_INPUT_CHANNELS;
	audio_input_status.bit_depth = PSVITA_USB_AUDIO_BITS_PER_SAMPLE;
	audio_input_status.ring_capacity_frames =
		PSVITA_USB_AUDIO_INPUT_RING_FRAMES;
	audio_input_status.minimum_buffered_frames =
		PSVITA_USB_AUDIO_INPUT_RING_FRAMES;
	audio_input_submit_endpoint_number = -1;
	audio_input_requested_alternate = 0;
	audio_input_applied_alternate = 0;
	audio_input_alternate_requested_us = 0;
}

static void audio_pending_clear(void)
{
	audio_pending_count = 0;
	for (uint32_t slot = 0; slot < AUDIO_REQUEST_COUNT; ++slot)
		psvita_usb_audio_request_init(&audio_request_states[slot]);
	memset((void *)audio_request_recovering, 0, sizeof(audio_request_recovering));
	audio_recovery_clear_fifo = 0;
	audio_input_pending_count = 0;
	for (uint32_t slot = 0; slot < AUDIO_INPUT_REQUEST_COUNT; ++slot)
		psvita_usb_audio_request_init(&audio_input_request_states[slot]);
}

static void audio_cancel_requests(void)
{
	if (!audio_pending_count) return;
	int cancel_any = 0;
	for (uint32_t slot = 0; slot < AUDIO_REQUEST_COUNT; ++slot)
		if (psvita_usb_audio_request_try_cancel(&audio_request_states[slot]))
			cancel_any = 1;
	if (cancel_any) {
		int result = ksceUdcdReqCancelAll(&endpoints[AUDIO_ENDPOINT_INDEX]);
		if (result < 0)
			for (uint32_t slot = 0; slot < AUDIO_REQUEST_COUNT; ++slot)
				psvita_usb_audio_request_cancel_failed(
					&audio_request_states[slot]);
	}
}

static void audio_input_cancel_requests(void)
{
	if (!audio_input_pending_count) return;
	int cancel_any = 0;
	for (uint32_t slot = 0; slot < AUDIO_INPUT_REQUEST_COUNT; ++slot)
		if (psvita_usb_audio_request_try_cancel(
		    &audio_input_request_states[slot]))
			cancel_any = 1;
	if (cancel_any) {
		int result =
			ksceUdcdReqCancelAll(&endpoints[AUDIO_INPUT_ENDPOINT_INDEX]);
		if (result < 0) {
			for (uint32_t slot = 0; slot < AUDIO_INPUT_REQUEST_COUNT; ++slot)
				psvita_usb_audio_request_cancel_failed(
					&audio_input_request_states[slot]);
			audio_input_status.last_error = result;
		}
	}
}

static void rx_complete(SceUdcdDeviceRequest *request)
{
	rx_pending = 0;
	rx_callback_count++;
	rx_last_callback_result = request->returnCode;
	if (request->transmitted > 0)
		rx_callback_bytes += (uint32_t)request->transmitted;
	if (request->returnCode >= 0 && request->transmitted > 0) {
		rx_timestamp_us = (uint64_t)ksceKernelGetSystemTimeWide();
		rx_length = (uint32_t)request->transmitted;
		rx_available = 1;
		midi_kernel_usb_rx_ready();
	} else {
		midi_kernel_usb_rx_ready();
	}
}

static void tx_complete(SceUdcdDeviceRequest *request)
{
	(void)request;
	tx_pending = 0;
	midi_kernel_usb_tx_ready();
}

static void audio_complete(SceUdcdDeviceRequest *request)
{
	int matched = 0;
	int canceled = 0;
	int recovering = 0;
	uint32_t matched_slot = 0;
	for (uint32_t slot = 0; slot < AUDIO_REQUEST_COUNT; ++slot) {
		if (request == &audio_requests[slot] &&
		    psvita_usb_audio_request_begin_completion(
			    &audio_request_states[slot], &canceled)) {
			matched = 1;
			matched_slot = slot;
			recovering = audio_request_recovering[slot];
			break;
		}
	}
	if (!matched) {
		midi_kernel_usb_audio_ready();
		return;
	}
	if (canceled) {
		if (recovering) {
			audio_status.recovered_requests++;
			audio_recovery_clear_fifo = 1;
		}
		audio_request_recovering[matched_slot] = 0;
		if (audio_pending_count) audio_pending_count--;
		psvita_usb_audio_request_finish_completion(
			&audio_request_states[matched_slot]);
		midi_kernel_usb_audio_ready();
		return;
	}
	audio_status.last_completion_result = request->returnCode;
	audio_status.last_completion_requested_bytes = request->size > 0
		? (uint32_t)request->size : 0u;
	audio_status.last_completion_bytes = request->transmitted > 0
		? (uint32_t)request->transmitted : 0u;
	uint32_t completion_us = audio_now_us();
	audio_last_completion_us = completion_us;
	psvita_usb_audio_completion_record(&audio_completion_clock, completion_us,
		audio_status.last_completion_requested_bytes,
		audio_status.last_completion_bytes, request->returnCode);
	if (request->returnCode >= 0) {
		audio_status.packets_completed++;
		if (request->transmitted > 0)
			audio_status.bytes_transmitted += (uint32_t)request->transmitted;
		else
			audio_status.zero_byte_completions++;
	} else if (audio_enabled && connected && configured && audio_streaming) {
		audio_status.completion_errors++;
		audio_status.last_error = request->returnCode;
	}
	audio_request_recovering[matched_slot] = 0;
	if (audio_pending_count) audio_pending_count--;
	/* Publish FREE only after every callback read and cleanup is complete. The
	 * producer can wake the worker at any instruction, so clearing ownership
	 * earlier lets it reuse this request while the old callback still runs. */
	psvita_usb_audio_request_finish_completion(
		&audio_request_states[matched_slot]);
	midi_kernel_usb_audio_ready();
}

static void audio_input_complete(SceUdcdDeviceRequest *request)
{
	int matched = 0;
	int canceled = 0;
	uint32_t matched_slot = 0;
	for (uint32_t slot = 0; slot < AUDIO_INPUT_REQUEST_COUNT; ++slot) {
		if (request == &audio_input_requests[slot] &&
		    psvita_usb_audio_request_begin_completion(
			    &audio_input_request_states[slot], &canceled)) {
			matched = 1;
			matched_slot = slot;
			break;
		}
	}
	if (!matched) {
		midi_kernel_usb_audio_ready();
		return;
	}
	if (!canceled && request->returnCode >= 0) {
		uint32_t bytes = (uint32_t)request->transmitted;
		uint32_t frame_bytes =
			PSVITA_USB_AUDIO_INPUT_CHANNELS * sizeof(int16_t);
		audio_input_status.last_packet_bytes = bytes;
		audio_input_status.last_packet_frames = 0;
		if (bytes > AUDIO_INPUT_BUFFER_BYTES || bytes % frame_bytes) {
			audio_input_status.malformed_packets++;
			audio_input_status.last_error = PSVITA_USB_AUDIO_MIDI_ERROR_FORMAT;
		} else {
			uint32_t frames = bytes / frame_bytes;
			if (bytes)
				ksceKernelDcacheInvalidateRange(
					audio_input_buffers[matched_slot], bytes);
			if (frames > 0u) {
				psvita_usb_audio_ring_write(&audio_input_ring,
					audio_input_buffers[matched_slot], frames);
				audio_input_status.received_frames += frames;
				audio_input_status.bytes_received += bytes;
			}
			audio_input_status.last_packet_frames = frames;
			audio_input_status.packets_completed++;
			if (frames < 47u) audio_input_status.short_packets++;
		}
	} else if (!canceled && audio_enabled && connected && configured &&
	           audio_input_streaming) {
		audio_input_status.completion_errors++;
		audio_input_status.last_error = request->returnCode;
	}
	if (audio_input_pending_count) audio_input_pending_count--;
	psvita_usb_audio_request_finish_completion(
		&audio_input_request_states[matched_slot]);
	midi_kernel_usb_audio_ready();
}

static int arm_rx(void)
{
	if (!connected || !configured || rx_available || rx_pending) return 0;
	memset(&rx_request, 0, sizeof(rx_request));
	rx_request.endpoint = &endpoints[MIDI_OUT_ENDPOINT_INDEX];
	rx_request.data = rx_buffer;
	rx_request.size = sizeof(rx_buffer);
	rx_request.onComplete = rx_complete;
	ksceKernelDcacheCleanInvalidateRange(rx_buffer, sizeof(rx_buffer));
	rx_pending = 1;
	rx_submit_attempts++;
	int result = ksceUdcdReqRecv(&rx_request);
	rx_last_submit_result = result;
	if (result < 0) {
		rx_submit_failures++;
		rx_pending = 0;
	}
	return result;
}

static int apply_audio_streaming_alternate(int alternate)
{
	int streaming = alternate == 1;
	if (audio_streaming == streaming) {
		if (streaming) midi_kernel_usb_audio_ready();
		return 0;
	}
	audio_streaming = streaming;
	if (!audio_streaming) {
		audio_stream_started_us = 0;
		audio_last_completion_us = 0;
		audio_cancel_requests();
		ksceUdcdClearFIFO(&endpoints[AUDIO_ENDPOINT_INDEX]);
		audio_ring.read_frame = audio_ring.write_frame;
		psvita_usb_audio_packet_clock_init(&audio_clock);
	} else {
		audio_stream_started_us = audio_now_us();
		audio_last_completion_us = 0;
		psvita_usb_audio_packet_clock_init(&audio_clock);
		midi_kernel_usb_audio_ready();
	}
	return 0;
}

static void request_audio_streaming_alternate(int alternate)
{
	audio_requested_alternate = alternate;
	audio_alternate_requested_us = audio_now_us();
	if (alternate == 0) {
		/* Stop immediately; waiting for a second callback can leak a request. */
		apply_audio_streaming_alternate(0);
	} else {
		/* Submission waits for changeSetting, with a delayed compatibility path. */
		midi_kernel_usb_audio_ready();
	}
}

static int apply_audio_input_streaming_alternate(int alternate)
{
	int streaming = alternate == 1;
	if (audio_input_streaming == streaming) {
		if (streaming) midi_kernel_usb_audio_ready();
		return 0;
	}
	audio_input_streaming = streaming;
	if (!audio_input_streaming) {
		audio_input_cancel_requests();
		ksceUdcdClearFIFO(&endpoints[AUDIO_INPUT_ENDPOINT_INDEX]);
		audio_input_ring.read_frame = audio_input_ring.write_frame;
	} else {
		audio_input_ring.read_frame = audio_input_ring.write_frame;
		midi_kernel_usb_audio_ready();
	}
	return 0;
}

static void request_audio_input_streaming_alternate(int alternate)
{
	audio_input_requested_alternate = alternate;
	audio_input_alternate_requested_us = audio_now_us();
	if (alternate == 0)
		apply_audio_input_streaming_alternate(0);
	else
		midi_kernel_usb_audio_ready();
}

static int process_request(int recipient, int arg, SceUdcdEP0DeviceRequest *request,
				   void *user_data)
{
	(void)recipient; (void)arg; (void)user_data;
	if (!request) return -1;
	int alternate = 0;
	int set_interface = psvita_usb_audio_decode_set_interface(
		request->bmRequestType, request->bRequest, request->wValue,
		request->wIndex, &alternate);
	if (set_interface < 0) return -1;
	if (set_interface > 0) {
		int interface_number = request->wIndex & 0xffu;
		if (interface_number == (int)PSVITA_USB_AUDIO_STREAM_INTERFACE) {
			audio_status.set_interface_requests++;
			request_audio_streaming_alternate(alternate);
		} else {
			request_audio_input_streaming_alternate(alternate);
		}
		return 0;
	}
	/* The fixed UAC1 format advertises no endpoint or feature controls. */
	if ((request->bmRequestType & USB_CTRLTYPE_TYPE_MASK) ==
	    USB_CTRLTYPE_TYPE_CLASS)
		return -1;
	return 0;
}

static int change_setting(int interface_number, int alternate, int bus)
{
	(void)bus;
	if (interface_number != (int)PSVITA_USB_AUDIO_STREAM_INTERFACE &&
	    interface_number !=
	        (int)PSVITA_USB_AUDIO_INPUT_STREAM_INTERFACE)
		return alternate == 0 ? 0 : -1;
	if (alternate != 0 && alternate != 1) return -1;
	if (interface_number ==
	    (int)PSVITA_USB_AUDIO_INPUT_STREAM_INTERFACE) {
		audio_input_requested_alternate = alternate;
		audio_input_applied_alternate = alternate;
		audio_input_alternate_requested_us = audio_now_us();
		return apply_audio_input_streaming_alternate(alternate);
	}
	audio_status.change_setting_callbacks++;
	audio_requested_alternate = alternate;
	audio_applied_alternate = alternate;
	audio_alternate_requested_us = audio_now_us();
	return apply_audio_streaming_alternate(alternate);
}

static int attach(int usb_version, void *user_data)
{
	(void)usb_version; (void)user_data;
	connected = 1;
	reset_streaming_alternates();
	audio_alternate_requested_us = 0;
	audio_stream_started_us = 0;
	/* Preserve request ownership until a detach cancellation callback arrives. */
	audio_cancel_requests();
	audio_input_cancel_requests();
	rx_available = 0;
	rx_timestamp_us = 0;
	rx_pending = 0;
	tx_pending = 0;
	ksceUdcdReqCancelAll(&endpoints[MIDI_OUT_ENDPOINT_INDEX]);
	ksceUdcdReqCancelAll(&endpoints[MIDI_IN_ENDPOINT_INDEX]);
	ksceUdcdClearFIFO(&endpoints[AUDIO_ENDPOINT_INDEX]);
	ksceUdcdClearFIFO(&endpoints[AUDIO_INPUT_ENDPOINT_INDEX]);
	ksceUdcdClearFIFO(&endpoints[MIDI_OUT_ENDPOINT_INDEX]);
	ksceUdcdClearFIFO(&endpoints[MIDI_IN_ENDPOINT_INDEX]);
	midi_kernel_usb_attached();
	return 0;
}

static void detach(void *user_data)
{
	(void)user_data;
	if (connected) {
		audio_status.disconnects++;
		audio_input_status.disconnects++;
	}
	connected = 0;
	configured = 0;
	reset_streaming_alternates();
	audio_cancel_requests();
	audio_input_cancel_requests();
	audio_ring.read_frame = audio_ring.write_frame;
	audio_input_ring.read_frame = audio_input_ring.write_frame;
	rx_available = 0;
	rx_pending = 0;
	tx_pending = 0;
	midi_kernel_usb_detached();
}

static void configure(int usb_version, int count,
			      SceUdcdInterfaceSettings *settings, void *user_data)
{
	(void)usb_version; (void)count; (void)settings; (void)user_data;
	configured = 1;
	/* A receive submitted from attach can be too early on embedded hosts. */
	midi_kernel_usb_rx_ready();
	midi_kernel_usb_audio_ready();
}

static int driver_start(int size, void *args, void *user_data)
{
	(void)size; (void)args; (void)user_data;
	return 0;
}

static int driver_stop(int size, void *args, void *user_data)
{
	(void)size; (void)args; (void)user_data;
	connected = 0;
	configured = 0;
	reset_streaming_alternates();
	rx_pending = 0;
	return 0;
}

static SceUdcdDriver driver = {
	PSVITA_USB_AUDIO_MIDI_DRIVER_NAME, PSVITA_USB_UDCD_ENDPOINT_COUNT, endpoints, &interface,
	&hs_device, &hs_config, &fs_device, &fs_config,
	strings, &strings[2], &strings[3], process_request, change_setting,
	attach, detach, configure, driver_start, driver_stop,
	NULL, 0, NULL
};

int midi_usb_register(void)
{
	if (!psvita_usb_audio_midi_validate_descriptor_layout())
		return PSVITA_USB_AUDIO_MIDI_ERROR_NOT_READY;
	int result = audio_buffer_init();
	if (result < 0) return result;
	result = ksceUdcdRegister(&driver);
	if (result < 0) audio_buffer_term();
	return result;
}

int midi_usb_unregister(void)
{
	int result = ksceUdcdUnregister(&driver);
	audio_buffer_term();
	return result;
}
int midi_usb_start(void)
{
	configured = 0;
	reset_streaming_alternates();
	audio_pending_clear();
	rx_pending = 0;
	rx_submit_attempts = 0;
	rx_submit_failures = 0;
	rx_callback_count = 0;
	rx_callback_bytes = 0;
	rx_last_submit_result = 0;
	rx_last_callback_result = 0;
	rx_last_transfer_length = 0;
	rx_last_packet_word = 0;
	return ksceUdcdStartInternal(PSVITA_USB_AUDIO_MIDI_DRIVER_NAME, 0, NULL,
		PSVITA_USB_AUDIO_MIDI_BUS);
}

int midi_usb_stop(void)
{
	connected = 0;
	configured = 0;
	reset_streaming_alternates();
	rx_pending = 0;
	audio_cancel_requests();
	audio_input_cancel_requests();
	ksceUdcdReqCancelAll(&endpoints[MIDI_OUT_ENDPOINT_INDEX]);
	ksceUdcdReqCancelAll(&endpoints[MIDI_IN_ENDPOINT_INDEX]);
	int result = ksceUdcdStopInternal(PSVITA_USB_AUDIO_MIDI_DRIVER_NAME, 0, NULL,
		PSVITA_USB_AUDIO_MIDI_BUS);
	audio_pending_clear();
	return result;
}

int midi_usb_connected(void) { return connected; }
int midi_usb_configured(void) { return configured; }
int midi_usb_rx_pending(void) { return rx_pending; }
int midi_usb_tx_pending(void) { return tx_pending; }

void midi_usb_get_rx_diagnostics(MidiUsbRxDiagnostics *out)
{
	if (!out) return;
	out->submit_attempts = rx_submit_attempts;
	out->submit_failures = rx_submit_failures;
	out->callback_count = rx_callback_count;
	out->callback_bytes = rx_callback_bytes;
	out->last_submit_result = rx_last_submit_result;
	out->last_callback_result = rx_last_callback_result;
	out->last_transfer_length = rx_last_transfer_length;
	out->last_packet_word = rx_last_packet_word;
}

void midi_usb_audio_reset(void)
{
	audio_cancel_requests();
	audio_input_cancel_requests();
	audio_pending_clear();
	audio_enabled = 0;
	audio_streaming = 0;
	audio_input_streaming = 0;
	audio_status_init();
}

void midi_usb_audio_set_enabled(int enabled)
{
	if (!!enabled == !!audio_enabled) return;
	audio_enabled = !!enabled;
	if (audio_enabled) {
		audio_status_init();
		audio_status.state = PSVITA_USB_AUDIO_STATE_IDLE;
		audio_status.last_error = 0;
		audio_input_status.state = PSVITA_USB_AUDIO_STATE_IDLE;
		audio_input_status.last_error = 0;
		midi_kernel_usb_audio_ready();
	} else {
		audio_cancel_requests();
		audio_input_cancel_requests();
		ksceUdcdClearFIFO(&endpoints[AUDIO_ENDPOINT_INDEX]);
		ksceUdcdClearFIFO(&endpoints[AUDIO_INPUT_ENDPOINT_INDEX]);
		audio_ring.read_frame = audio_ring.write_frame;
		audio_input_ring.read_frame = audio_input_ring.write_frame;
		audio_status.state = PSVITA_USB_AUDIO_STATE_DISABLED;
		audio_input_status.state = PSVITA_USB_AUDIO_STATE_DISABLED;
	}
}

int midi_usb_audio_enabled(void) { return audio_enabled; }

uint32_t midi_usb_audio_write(const int16_t *interleaved, uint32_t frames)
{
	if (!audio_enabled) return 0;
	return psvita_usb_audio_ring_write(&audio_ring, interleaved, frames);
}

int midi_usb_audio_submit_next(void)
{
	if (!audio_enabled || !connected || !configured || !audio_buffers)
		return 0;
	uint32_t now_us = audio_now_us();
	if (!audio_streaming) {
		if (audio_requested_alternate != 1 ||
		    now_us - audio_alternate_requested_us < AUDIO_ALTERNATE_FALLBACK_US)
			return 0;
		/* Some SceUdcd revisions do not call changeSetting for SET_INTERFACE. */
		audio_status.fallback_starts++;
		apply_audio_streaming_alternate(1);
	}
	if (now_us - audio_stream_started_us < AUDIO_ALTERNATE_SETTLE_US)
		return 0;
	if (audio_recovery_clear_fifo) {
		int clear_result = ksceUdcdClearFIFO(&endpoints[AUDIO_ENDPOINT_INDEX]);
		if (clear_result < 0) {
			audio_status.last_error = clear_result;
			return clear_result;
		}
		audio_recovery_clear_fifo = 0;
	}
	for (uint32_t slot = 0; slot < AUDIO_REQUEST_COUNT; ++slot) {
		if (!psvita_usb_audio_request_is_stalled(
		    &audio_request_states[slot], now_us, AUDIO_REQUEST_STALL_US))
			continue;
		if (!psvita_usb_audio_request_try_cancel(&audio_request_states[slot]))
			continue;
		audio_request_recovering[slot] = 1;
		audio_status.stalled_requests++;
		int cancel_result = ksceUdcdReqCancelAll(&endpoints[AUDIO_ENDPOINT_INDEX]);
		if (cancel_result < 0) {
			audio_request_recovering[slot] = 0;
			psvita_usb_audio_request_cancel_failed(
				&audio_request_states[slot]);
			audio_status.cancel_failures++;
			audio_status.last_error = cancel_result;
			return cancel_result;
		}
		return 0;
	}
	for (uint32_t slot = 0; slot < AUDIO_REQUEST_COUNT; ++slot) {
		if (!psvita_usb_audio_request_try_prepare(
		    &audio_request_states[slot])) continue;
		PsvitaUsbAudioReadCheckpoint read_checkpoint;
		psvita_usb_audio_read_checkpoint(&audio_ring, &audio_clock,
			&read_checkpoint);
		uint32_t occupancy = psvita_usb_audio_ring_available(&audio_ring);
		uint32_t service_frames = psvita_usb_audio_packet_frames(
			&audio_clock, occupancy);
		/* Keep the efficient 8 ms batch during normal operation. If a producer
		 * pause leaves less than one full batch, submit all remaining whole USB
		 * packets instead of discarding up to 7 ms of valid reservoir and
		 * immediately entering the audible re-prime cycle. Initial priming
		 * stays at 8 ms so silence does not generate excessive callbacks. */
		uint32_t intervals = audio_clock.primed
			? psvita_usb_audio_request_intervals(occupancy)
			: PSVITA_USB_AUDIO_REQUEST_INTERVALS;
		uint32_t frames = service_frames * intervals;
		psvita_usb_audio_ring_read_packet(&audio_ring, &audio_clock,
			audio_buffers[slot], frames);
		uint32_t channel_mask = psvita_usb_audio_measure_channels(
			audio_buffers[slot], frames, PSVITA_USB_AUDIO_STREAM_CHANNELS,
			audio_status.last_packet_channel_peaks,
			PSVITA_USB_AUDIO_STREAM_CHANNELS);
		uint32_t peak = 0;
		for (uint32_t channel = 0;
		     channel < PSVITA_USB_AUDIO_STREAM_CHANNELS; ++channel)
			if (audio_status.last_packet_channel_peaks[channel] > peak)
				peak = audio_status.last_packet_channel_peaks[channel];
		audio_status.last_packet_channel_nonzero_mask = channel_mask;
		audio_status.last_packet_peak = peak;
		audio_status.last_packet_first_left = audio_buffers[slot][0];
		audio_status.last_packet_first_right = audio_buffers[slot][1];
		if (peak) audio_status.nonzero_packets++;
		SceUdcdDeviceRequest *request = &audio_requests[slot];
		memset(request, 0, sizeof(*request));
		request->endpoint = &endpoints[AUDIO_ENDPOINT_INDEX];
		request->data = audio_buffers[slot];
		request->attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT;
		request->size = (int)PSVITA_USB_AUDIO_PACKET_BYTES(frames);
		request->onComplete = audio_complete;
		uint32_t submit_us = audio_now_us();
		if (audio_last_completion_us) {
			uint32_t rearm_us = submit_us - audio_last_completion_us;
			audio_status.last_rearm_delay_us = rearm_us;
			if (rearm_us > audio_status.maximum_rearm_delay_us)
				audio_status.maximum_rearm_delay_us = rearm_us;
		}
		audio_submit_endpoint_number =
			endpoints[AUDIO_ENDPOINT_INDEX].endpointNumber;
		audio_request_recovering[slot] = 0;
		psvita_usb_audio_request_publish(&audio_request_states[slot], submit_us);
		audio_pending_count++;
		int result = ksceUdcdReqSend(request);
		if (result < 0) {
			/* Preparing a request advances the kernel ring. If UDCD rejects the
			 * request, put those frames and packet-clock state back so a retry
			 * sends the same audio instead of creating an artificial shortage. */
			psvita_usb_audio_read_rollback(&audio_ring, &audio_clock,
				&read_checkpoint);
			if (audio_pending_count) audio_pending_count--;
			psvita_usb_audio_request_abort_submit(
				&audio_request_states[slot]);
			audio_status.submit_failures++;
			audio_status.last_error = result;
			return result;
		}
		audio_status.packets_submitted++;
		audio_status.last_packet_frames = service_frames;
		if (service_frames == 47u)
			audio_status.packets_47_frames += intervals;
		else if (service_frames == 48u)
			audio_status.packets_48_frames += intervals;
		else if (service_frames == 49u)
			audio_status.packets_49_frames += intervals;
	}
	return 0;
}

int midi_usb_audio_input_submit_next(void)
{
	if (!audio_enabled || !connected || !configured || !audio_input_buffers)
		return 0;
	uint32_t now_us = audio_now_us();
	if (!audio_input_streaming) {
		if (audio_input_requested_alternate != 1 ||
		    now_us - audio_input_alternate_requested_us <
		        AUDIO_ALTERNATE_FALLBACK_US)
			return 0;
		audio_input_applied_alternate = 1;
		apply_audio_input_streaming_alternate(1);
	}
	if (now_us - audio_input_alternate_requested_us <
	    AUDIO_ALTERNATE_SETTLE_US)
		return 0;
	for (uint32_t slot = 0; slot < AUDIO_INPUT_REQUEST_COUNT; ++slot) {
		if (!psvita_usb_audio_request_try_prepare(
		    &audio_input_request_states[slot])) continue;
		SceUdcdDeviceRequest *request = &audio_input_requests[slot];
		memset(request, 0, sizeof(*request));
		memset(audio_input_buffers[slot], 0, AUDIO_INPUT_BUFFER_BYTES);
		ksceKernelDcacheCleanInvalidateRange(audio_input_buffers[slot],
			AUDIO_INPUT_BUFFER_BYTES);
		request->endpoint = &endpoints[AUDIO_INPUT_ENDPOINT_INDEX];
		request->data = audio_input_buffers[slot];
		request->attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT;
		request->size = AUDIO_INPUT_BUFFER_BYTES;
		request->onComplete = audio_input_complete;
		audio_input_submit_endpoint_number =
			endpoints[AUDIO_INPUT_ENDPOINT_INDEX].endpointNumber;
		psvita_usb_audio_request_publish(
			&audio_input_request_states[slot], now_us);
		audio_input_pending_count++;
		int result = ksceUdcdReqRecv(request);
		if (result < 0) {
			if (audio_input_pending_count) audio_input_pending_count--;
			psvita_usb_audio_request_abort_submit(
				&audio_input_request_states[slot]);
			audio_input_status.submit_failures++;
			audio_input_status.last_error = result;
			return result;
		}
		audio_input_status.packets_submitted++;
	}
	return 0;
}

void midi_usb_audio_note_worker_lock_wait(uint32_t wait_us)
{
	if (!audio_enabled || !audio_streaming) return;
	audio_status.last_worker_lock_wait_us = wait_us;
	if (wait_us > audio_status.maximum_worker_lock_wait_us)
		audio_status.maximum_worker_lock_wait_us = wait_us;
}

void midi_usb_audio_get_status(PsvitaUsbAudioStatus *out)
{
	if (!out) return;
	*out = audio_status;
	out->buffered_frames = psvita_usb_audio_ring_available(&audio_ring);
	out->produced_frames = audio_ring.produced_frames;
	out->consumed_frames = audio_ring.consumed_frames;
	out->output_underruns = audio_ring.underruns;
	out->output_overruns = audio_ring.overruns;
	out->maximum_buffered_frames = audio_ring.maximum_occupancy;
	out->minimum_buffered_frames = audio_ring.minimum_occupancy ==
		PSVITA_USB_AUDIO_RING_FRAMES ? 0u : audio_ring.minimum_occupancy;
	out->endpoint_driver_number =
		endpoints[AUDIO_ENDPOINT_INDEX].driverEndpointNumber;
	out->submit_endpoint_number = audio_submit_endpoint_number;
	out->endpoint_number = endpoints[AUDIO_ENDPOINT_INDEX].endpointNumber;
	out->endpoint_transmitted_bytes =
		(uint32_t)endpoints[AUDIO_ENDPOINT_INDEX].transmittedBytes;
	out->request_attributes = audio_requests[0].attributes;
	out->requested_alternate = audio_requested_alternate;
	out->applied_alternate = audio_applied_alternate;
	out->pending_age_ms = 0;
	out->stream_primed = audio_clock.primed;
	out->priming_packets = audio_clock.priming_packets;
	out->rebuffer_events = audio_clock.rebuffer_events;
	out->conceal_events = audio_clock.conceal_events;
	out->concealed_frames = audio_clock.concealed_frames;
	out->consecutive_missing_frames = audio_clock.consecutive_missing_frames;
	out->short_completions = audio_completion_clock.short_completions;
	out->late_completions = audio_completion_clock.late_completions;
	out->last_completion_gap_us = audio_completion_clock.last_gap_us;
	out->maximum_completion_gap_us = audio_completion_clock.maximum_gap_us;
	uint32_t now_us = audio_now_us();
	for (uint32_t slot = 0; slot < AUDIO_REQUEST_COUNT; ++slot) {
		uint32_t request_state = psvita_usb_audio_request_state(
			&audio_request_states[slot]);
		if (request_state != PSVITA_USB_AUDIO_REQUEST_FREE) {
			out->pending_age_ms =
				(now_us - audio_request_states[slot].started_us) / 1000u;
			break;
		}
	}
	if (audio_enabled) out->flags |= PSVITA_USB_AUDIO_STATUS_ENABLED;
	if (connected) out->flags |= PSVITA_USB_AUDIO_STATUS_HOST_CONNECTED;
	if (configured) out->flags |= PSVITA_USB_AUDIO_STATUS_CONFIGURED;
	if (audio_streaming) out->flags |= PSVITA_USB_AUDIO_STATUS_HOST_STREAMING;
	if (audio_pending_count) out->flags |= PSVITA_USB_AUDIO_STATUS_REQUEST_PENDING;
	if (!audio_enabled) out->state = PSVITA_USB_AUDIO_STATE_DISABLED;
	else if (connected && configured && audio_streaming)
		out->state = PSVITA_USB_AUDIO_STATE_STREAMING;
	else if (connected) out->state = PSVITA_USB_AUDIO_STATE_CONNECTED;
	else out->state = PSVITA_USB_AUDIO_STATE_IDLE;
}

uint32_t midi_usb_audio_input_read(int16_t *interleaved, uint32_t frames)
{
	if (!audio_enabled || !interleaved || !frames) return 0;
	return psvita_usb_audio_ring_read_silence(&audio_input_ring,
		interleaved, frames);
}

void midi_usb_audio_input_get_status(PsvitaUsbAudioInputStatus *out)
{
	if (!out) return;
	*out = audio_input_status;
	out->buffered_frames =
		psvita_usb_audio_ring_available(&audio_input_ring);
	out->read_frames = audio_input_ring.consumed_frames;
	out->overruns = audio_input_ring.overruns;
	out->maximum_buffered_frames = audio_input_ring.maximum_occupancy;
	out->minimum_buffered_frames =
		audio_input_ring.minimum_occupancy ==
		    PSVITA_USB_AUDIO_INPUT_RING_FRAMES
		? 0u : audio_input_ring.minimum_occupancy;
	out->endpoint_driver_number =
		endpoints[AUDIO_INPUT_ENDPOINT_INDEX].driverEndpointNumber;
	out->endpoint_number =
		endpoints[AUDIO_INPUT_ENDPOINT_INDEX].endpointNumber;
	out->endpoint_received_bytes =
		(uint32_t)endpoints[AUDIO_INPUT_ENDPOINT_INDEX].transmittedBytes;
	out->requested_alternate = audio_input_requested_alternate;
	out->applied_alternate = audio_input_applied_alternate;
	if (audio_enabled)
		out->flags |= PSVITA_USB_AUDIO_INPUT_STATUS_ENABLED;
	if (connected)
		out->flags |= PSVITA_USB_AUDIO_INPUT_STATUS_HOST_CONNECTED;
	if (configured)
		out->flags |= PSVITA_USB_AUDIO_INPUT_STATUS_CONFIGURED;
	if (audio_input_streaming)
		out->flags |= PSVITA_USB_AUDIO_INPUT_STATUS_HOST_STREAMING;
	if (audio_input_pending_count)
		out->flags |= PSVITA_USB_AUDIO_INPUT_STATUS_REQUEST_PENDING;
	if (!audio_enabled)
		out->state = PSVITA_USB_AUDIO_STATE_DISABLED;
	else if (connected && configured && audio_input_streaming)
		out->state = PSVITA_USB_AUDIO_STATE_STREAMING;
	else if (connected)
		out->state = PSVITA_USB_AUDIO_STATE_CONNECTED;
	else
		out->state = PSVITA_USB_AUDIO_STATE_IDLE;
}

int midi_usb_submit_tx(const uint8_t *bytes, uint32_t length)
{
	if (!bytes || !length || length > sizeof(tx_buffer) || tx_pending || !connected)
		return PSVITA_USB_AUDIO_MIDI_ERROR_BUSY;
	memcpy(tx_buffer, bytes, length);
	memset(&tx_request, 0, sizeof(tx_request));
	tx_request.endpoint = &endpoints[MIDI_IN_ENDPOINT_INDEX];
	tx_request.data = tx_buffer;
	tx_request.size = (int)length;
	tx_request.onComplete = tx_complete;
	ksceKernelDcacheCleanRange(tx_buffer, length);
	tx_pending = 1;
	int result = ksceUdcdReqSend(&tx_request);
	if (result < 0) tx_pending = 0;
	return result;
}

int midi_usb_take_rx(uint8_t *bytes, uint32_t capacity, uint32_t *length,
	uint64_t *timestamp_us)
{
	if (!bytes || !length || !timestamp_us || !rx_available) return 0;
	uint32_t received = rx_length;
	uint32_t count = received;
	if (count > sizeof(rx_buffer)) count = sizeof(rx_buffer);
	if (count > capacity) count = capacity;
	ksceKernelDcacheInvalidateRange(rx_buffer,
		received < sizeof(rx_buffer) ? received : sizeof(rx_buffer));
	rx_last_transfer_length = received;
	rx_last_packet_word = 0;
	for (uint32_t i = 0; i < received && i < sizeof(rx_last_packet_word); ++i)
		rx_last_packet_word |= (uint32_t)rx_buffer[i] << (i * 8u);
	memcpy(bytes, rx_buffer, count);
	rx_available = 0;
	*length = count;
	*timestamp_us = rx_timestamp_us;
	return 1;
}

void midi_usb_wake_worker(void) { (void)arm_rx(); }
