#include "psvita_usb_audio_midi.h"
#include "kernel_imports.h"

#include <psp2/kernel/modulemgr.h>

int module_start(SceSize argc, const void *args);
int _start(SceSize argc, const void *args)
	__attribute__((weak, alias("module_start")));

static int lease_owned;

int module_start(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	if (lease_owned) {
		if (kscePsvitaUsbAudioMidiRelease() >= 0) lease_owned = 0;
	}
	return SCE_KERNEL_STOP_SUCCESS;
}

int psvitaUsbAudioMidiClientLoad(const char *suprx_path)
{
	/* When called through this bridge, the bridge is already resident. */
	(void)suprx_path;
	return psvitaUsbAudioMidiGetApiVersion() == (int)PSVITA_USB_AUDIO_MIDI_API_VERSION ? 0 : -1;
}

int psvitaUsbAudioMidiGetApiVersion(void) { return kscePsvitaUsbAudioMidiGetApiVersion(); }
int psvitaUsbAudioMidiAcquire(uint32_t flags)
{
	int result = kscePsvitaUsbAudioMidiAcquire(flags);
	if (result >= 0) lease_owned = 1;
	return result;
}
int psvitaUsbAudioMidiRelease(void)
{
	int result = kscePsvitaUsbAudioMidiRelease();
	if (result >= 0) lease_owned = 0;
	return result;
}
int psvitaUsbMidiRead(PsvitaUsbMidiEvent *events, uint32_t capacity)
{
	return kscePsvitaUsbMidiRead(events, capacity);
}
int psvitaUsbMidiReadWait(PsvitaUsbMidiEvent *events, uint32_t capacity,
	uint32_t timeout_us)
{
	return kscePsvitaUsbMidiReadWait(events, capacity, timeout_us);
}
int psvitaUsbMidiWrite(const PsvitaUsbMidiEvent *events, uint32_t count)
{
	return kscePsvitaUsbMidiWrite(events, count);
}
int psvitaUsbAudioMidiGetStatus(PsvitaUsbAudioMidiStatus *status)
{
	return kscePsvitaUsbAudioMidiGetStatus(status);
}
int psvitaUsbAudioMidiGetDiagnostics(PsvitaUsbAudioMidiDiagnostics *diagnostics)
{
	return kscePsvitaUsbAudioMidiGetDiagnostics(diagnostics);
}
int psvitaUsbAudioSetEnabled(int enabled)
{
	return kscePsvitaUsbAudioSetEnabled(enabled);
}
int psvitaUsbAudioWrite(const int16_t *interleaved_stereo, uint32_t frames,
	uint32_t sample_rate)
{
	return kscePsvitaUsbAudioWrite(interleaved_stereo, frames, sample_rate);
}
int psvitaUsbAudioWriteMulti(const int16_t *interleaved, uint32_t frames,
	uint32_t sample_rate, uint32_t channels)
{
	return kscePsvitaUsbAudioWriteMulti(interleaved, frames, sample_rate,
		channels);
}
int psvitaUsbAudioGetStatus(PsvitaUsbAudioStatus *status)
{
	return kscePsvitaUsbAudioGetStatus(status);
}
int psvitaUsbAudioInputRead(int16_t *interleaved_stereo,
	uint32_t capacity_frames)
{
	return kscePsvitaUsbAudioInputRead(interleaved_stereo, capacity_frames);
}
int psvitaUsbAudioGetInputStatus(PsvitaUsbAudioInputStatus *status)
{
	return kscePsvitaUsbAudioGetInputStatus(status);
}
