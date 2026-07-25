#ifndef PSVITA_USB_AUDIO_MIDI_OWNER_GUARD_H
#define PSVITA_USB_AUDIO_MIDI_OWNER_GUARD_H

#include "takeover.h"

typedef int (*PsvitaUsbAudioMidiOwnerAliveFn)(void *context, int owner_pid);
typedef int (*PsvitaUsbAudioMidiOwnerReleaseFn)(void *context, int owner_pid);

int psvita_usb_audio_midi_owner_watchdog(int owner_pid,
	PsvitaUsbAudioMidiOwnerAliveFn owner_alive,
	PsvitaUsbAudioMidiOwnerReleaseFn release_owner,
	void *context);
int psvita_usb_audio_midi_release_state_finish(int restore_result,
	int *owner_pid, int *midi_started,
	PsvitaUsbAudioMidiStockState *stock_state,
	PsvitaUsbAudioMidiTakeoverState *takeover_state);

#endif
