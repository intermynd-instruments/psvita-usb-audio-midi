#ifndef PSVITA_USB_AUDIO_MIDI_KERNEL_INTERNAL_H
#define PSVITA_USB_AUDIO_MIDI_KERNEL_INTERNAL_H

#include <psp2kern/types.h>
#include "audio_core.h"
#include "midi_core.h"

#define PSVITA_USB_AUDIO_MIDI_DRIVER_NAME "VITAAMID"
#define PSVITA_USB_AUDIO_MIDI_DEVELOPMENT_PID 0x1338u
#define PSVITA_USB_AUDIO_MIDI_BUS 2

typedef struct {
	uint32_t submit_attempts;
	uint32_t submit_failures;
	uint32_t callback_count;
	uint32_t callback_bytes;
	int32_t last_submit_result;
	int32_t last_callback_result;
	uint32_t last_transfer_length;
	uint32_t last_packet_word;
} MidiUsbRxDiagnostics;

int midi_usb_register(void);
int midi_usb_unregister(void);
int midi_usb_start(void);
int midi_usb_stop(void);
int midi_usb_connected(void);
int midi_usb_configured(void);
int midi_usb_rx_pending(void);
int midi_usb_tx_pending(void);
void midi_usb_get_rx_diagnostics(MidiUsbRxDiagnostics *out);
void midi_usb_audio_reset(void);
void midi_usb_audio_set_enabled(int enabled);
int midi_usb_audio_enabled(void);
uint32_t midi_usb_audio_write(const int16_t *interleaved, uint32_t frames);
int midi_usb_audio_submit_next(void);
int midi_usb_audio_input_submit_next(void);
void midi_usb_audio_get_status(PsvitaUsbAudioStatus *out);
void midi_usb_audio_note_worker_lock_wait(uint32_t wait_us);
uint32_t midi_usb_audio_input_read(int16_t *interleaved, uint32_t frames);
void midi_usb_audio_input_get_status(PsvitaUsbAudioInputStatus *out);
int midi_usb_submit_tx(const uint8_t *bytes, uint32_t length);
int midi_usb_take_rx(uint8_t *bytes, uint32_t capacity, uint32_t *length,
	uint64_t *timestamp_us);
void midi_usb_wake_worker(void);

void midi_kernel_usb_attached(void);
void midi_kernel_usb_detached(void);
void midi_kernel_usb_rx_ready(void);
void midi_kernel_usb_tx_ready(void);
void midi_kernel_usb_audio_ready(void);

#endif
