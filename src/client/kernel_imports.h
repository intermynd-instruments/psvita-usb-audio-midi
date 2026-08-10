#ifndef PSVITA_USB_AUDIO_MIDI_KERNEL_IMPORTS_H
#define PSVITA_USB_AUDIO_MIDI_KERNEL_IMPORTS_H

#include <stdint.h>
#include "psvita_usb_audio_midi.h"

int kscePsvitaUsbAudioMidiGetApiVersion(void);
int kscePsvitaUsbAudioMidiAcquire(uint32_t flags);
int kscePsvitaUsbAudioMidiRelease(void);
int kscePsvitaUsbMidiRead(PsvitaUsbMidiEvent *events, uint32_t capacity);
int kscePsvitaUsbMidiReadWait(PsvitaUsbMidiEvent *events, uint32_t capacity,
	uint32_t timeout_us);
int kscePsvitaUsbMidiWrite(const PsvitaUsbMidiEvent *events, uint32_t count);
int kscePsvitaUsbAudioMidiGetStatus(PsvitaUsbAudioMidiStatus *status);
int kscePsvitaUsbAudioMidiGetDiagnostics(PsvitaUsbAudioMidiDiagnostics *diagnostics);
int kscePsvitaUsbAudioSetEnabled(int enabled);
int kscePsvitaUsbAudioSetOutputEnabled(int enabled);
int kscePsvitaUsbAudioSetInputEnabled(int enabled);
int kscePsvitaUsbAudioWrite(const int16_t *interleaved_stereo, uint32_t frames,
	uint32_t sample_rate);
int kscePsvitaUsbAudioWriteMulti(const int16_t *interleaved, uint32_t frames,
	uint32_t sample_rate, uint32_t channels);
int kscePsvitaUsbAudioGetStatus(PsvitaUsbAudioStatus *status);
int kscePsvitaUsbAudioInputRead(int16_t *interleaved_stereo,
	uint32_t capacity_frames);
int kscePsvitaUsbAudioGetInputStatus(PsvitaUsbAudioInputStatus *status);

#endif
