#ifndef PSVITA_USB_AUDIO_MIDI_STOCK_STATE_H
#define PSVITA_USB_AUDIO_MIDI_STOCK_STATE_H

#include "psvita_usb_audio_midi.h"
#include "takeover.h"

int psvita_usb_audio_midi_classify_stock_state(int controller, int mtp,
	int psp_comm, int serial, int device,
	PsvitaUsbAudioMidiStockState *snapshot, uint32_t *snapshot_flags);

#endif
