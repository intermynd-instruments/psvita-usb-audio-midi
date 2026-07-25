#ifndef PSVITA_USB_AUDIO_MIDI_TAKEOVER_H
#define PSVITA_USB_AUDIO_MIDI_TAKEOVER_H

#include "psvita_usb_audio_midi.h"

typedef struct {
	int (*deactivate)(void *context);
	int (*stop_controller)(void *context);
	int (*start_controller)(void *context);
	int (*start_midi)(void *context);
	int (*activate_midi)(void *context);
	int (*stop_midi)(void *context);
	int (*start_mtp)(void *context);
	int (*start_psp_comm)(void *context);
	int (*start_serial)(void *context);
	int (*activate_mtp)(void *context);
	int (*controller_start_indicates_running)(int result);
	void (*trace)(void *context, int phase, int operation, int result);
} PsvitaUsbAudioMidiTakeoverOps;

typedef struct {
	int mtp_started;
	int psp_comm_started;
	int serial_started;
	int usb_active;
} PsvitaUsbAudioMidiStockState;

typedef struct {
	int mtp_stopped;
	int psp_comm_stopped;
	int serial_stopped;
	int controller_cycled;
} PsvitaUsbAudioMidiTakeoverState;

int psvita_usb_audio_midi_takeover(const PsvitaUsbAudioMidiTakeoverOps *ops, void *context,
	const PsvitaUsbAudioMidiStockState *stock_state,
	PsvitaUsbAudioMidiTakeoverState *takeover_state, int *failed_step);
int psvita_usb_audio_midi_restore(const PsvitaUsbAudioMidiTakeoverOps *ops, void *context,
	const PsvitaUsbAudioMidiStockState *stock_state,
	const PsvitaUsbAudioMidiTakeoverState *takeover_state);

#endif
