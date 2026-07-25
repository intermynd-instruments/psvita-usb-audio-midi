#include "stock_state.h"

/* These values are part of the SceUdcd state ABI. Keeping the classifier in
 * common code makes the hardware-observed state transitions host-testable. */
#define UDCD_DRIVER_STARTED 1
#define UDCD_DRIVER_REGISTERED 2
#define UDCD_CABLE_DISCONNECTED 0x10
#define UDCD_CABLE_CONNECTED 0x20
#define UDCD_DEACTIVATED 0x100
#define UDCD_ACTIVATED 0x200

int psvita_usb_audio_midi_classify_stock_state(int controller, int mtp,
	int psp_comm, int serial, int device,
	PsvitaUsbAudioMidiStockState *snapshot, uint32_t *snapshot_flags)
{
	uint32_t flags = PSVITA_USB_AUDIO_MIDI_STATUS_STOCK_SNAPSHOT;
	int active;
	int deactivated;

	if (controller == UDCD_DRIVER_STARTED)
		flags |= PSVITA_USB_AUDIO_MIDI_STATUS_CONTROLLER_STARTED;
	else if (controller == UDCD_DRIVER_REGISTERED)
		flags |= PSVITA_USB_AUDIO_MIDI_STATUS_CONTROLLER_REGISTERED;
	if (mtp == UDCD_DRIVER_STARTED)
		flags |= PSVITA_USB_AUDIO_MIDI_STATUS_MTP_STARTED;
	else if (mtp == UDCD_DRIVER_REGISTERED)
		flags |= PSVITA_USB_AUDIO_MIDI_STATUS_MTP_REGISTERED;
	if (device >= 0) {
		if (device & UDCD_ACTIVATED)
			flags |= PSVITA_USB_AUDIO_MIDI_STATUS_USB_ACTIVATED;
		if (device & UDCD_DEACTIVATED)
			flags |= PSVITA_USB_AUDIO_MIDI_STATUS_USB_DEACTIVATED;
		if (device & UDCD_CABLE_CONNECTED)
			flags |= PSVITA_USB_AUDIO_MIDI_STATUS_CABLE_CONNECTED;
		if (device & UDCD_CABLE_DISCONNECTED)
			flags |= PSVITA_USB_AUDIO_MIDI_STATUS_CABLE_DISCONNECTED;
	}
	if (psp_comm == UDCD_DRIVER_STARTED)
		flags |= PSVITA_USB_AUDIO_MIDI_STATUS_PSP_COMM_STARTED;
	if (serial == UDCD_DRIVER_STARTED)
		flags |= PSVITA_USB_AUDIO_MIDI_STATUS_SERIAL_STARTED;
	if (snapshot_flags) *snapshot_flags = flags;

	if (!snapshot || device < 0)
		return PSVITA_USB_AUDIO_MIDI_ERROR_USB_STATE;
	/* GetDrvStateInternal for the controller returns NOT_FOUND on retail
	 * firmware even while its stock child drivers and device state are valid.
	 * Keep it as diagnostics only; the independently queried stock personality
	 * and conflict checks are the takeover safety boundary. */
	if (mtp != UDCD_DRIVER_STARTED && mtp != UDCD_DRIVER_REGISTERED)
		return PSVITA_USB_AUDIO_MIDI_ERROR_USB_STATE;
	if ((psp_comm != UDCD_DRIVER_STARTED &&
	     psp_comm != UDCD_DRIVER_REGISTERED) ||
	    (serial != UDCD_DRIVER_STARTED &&
	     serial != UDCD_DRIVER_REGISTERED))
		return PSVITA_USB_AUDIO_MIDI_ERROR_USB_STATE;

	active = (device & UDCD_ACTIVATED) != 0;
	deactivated = (device & UDCD_DEACTIVATED) != 0;
	if (active == deactivated)
		return PSVITA_USB_AUDIO_MIDI_ERROR_USB_STATE;

	snapshot->mtp_started = mtp == UDCD_DRIVER_STARTED;
	snapshot->psp_comm_started = psp_comm == UDCD_DRIVER_STARTED;
	snapshot->serial_started = serial == UDCD_DRIVER_STARTED;
	snapshot->usb_active = active;
	return 0;
}
