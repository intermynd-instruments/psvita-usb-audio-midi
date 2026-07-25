#include "owner_guard.h"

#include <string.h>

int psvita_usb_audio_midi_owner_watchdog(int owner_pid,
	PsvitaUsbAudioMidiOwnerAliveFn owner_alive,
	PsvitaUsbAudioMidiOwnerReleaseFn release_owner,
	void *context)
{
	if (owner_pid < 0 || !owner_alive || !release_owner) return 0;
	if (owner_alive(context, owner_pid)) return 0;
	return release_owner(context, owner_pid);
}

int psvita_usb_audio_midi_release_state_finish(int restore_result,
	int *owner_pid, int *midi_started,
	PsvitaUsbAudioMidiStockState *stock_state,
	PsvitaUsbAudioMidiTakeoverState *takeover_state)
{
	if (!owner_pid || !midi_started || !stock_state || !takeover_state)
		return -1;
	if (restore_result < 0) return restore_result;
	*midi_started = 0;
	memset(stock_state, 0, sizeof(*stock_state));
	memset(takeover_state, 0, sizeof(*takeover_state));
	*owner_pid = -1;
	return restore_result;
}
