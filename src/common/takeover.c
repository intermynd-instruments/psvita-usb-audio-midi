#include "takeover.h"

static int traced_call(const PsvitaUsbAudioMidiTakeoverOps *ops, void *context,
	int phase, int operation, int (*function)(void *))
{
	int result = function(context);
	if (ops->trace) ops->trace(context, phase, operation, result);
	return result;
}

static void restore_call(const PsvitaUsbAudioMidiTakeoverOps *ops, void *context,
	int phase, int operation, int (*function)(void *),
	int already_running_is_ok, int *first_error)
{
	int result = traced_call(ops, context, phase, operation, function);
	if (result >= 0 || *first_error)
		return;
	if (already_running_is_ok && ops->controller_start_indicates_running &&
	    ops->controller_start_indicates_running(result))
		return;
	*first_error = result;
}

static int restore_usb_state(const PsvitaUsbAudioMidiTakeoverOps *ops, void *context,
	const PsvitaUsbAudioMidiStockState *stock_state, int phase, int stop_midi,
	int controller_cycled, int mtp_stopped, int psp_comm_stopped,
	int serial_stopped)
{
	int first_error = 0;

	if (stop_midi) {
		restore_call(ops, context, phase,
			PSVITA_USB_AUDIO_MIDI_TRACE_OP_DEACTIVATE, ops->deactivate,
			0, &first_error);
		restore_call(ops, context, phase,
			PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_MIDI, ops->stop_midi,
			0, &first_error);
	}
	if (controller_cycled)
		restore_call(ops, context, phase,
			PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_CONTROLLER,
			ops->start_controller, 1, &first_error);
	if (mtp_stopped && stock_state->mtp_started)
		restore_call(ops, context, phase,
			PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_MTP, ops->start_mtp,
			1, &first_error);
	if (psp_comm_stopped && stock_state->psp_comm_started)
		restore_call(ops, context, phase,
			PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_PSP_COMM,
			ops->start_psp_comm, 1, &first_error);
	if (serial_stopped && stock_state->serial_started)
		restore_call(ops, context, phase,
			PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_SERIAL, ops->start_serial,
			1, &first_error);
	if (stock_state->usb_active && stock_state->mtp_started)
		restore_call(ops, context, phase,
			PSVITA_USB_AUDIO_MIDI_TRACE_OP_ACTIVATE_MTP, ops->activate_mtp,
			0, &first_error);

	return first_error;
}

int psvita_usb_audio_midi_takeover(const PsvitaUsbAudioMidiTakeoverOps *ops, void *context,
	const PsvitaUsbAudioMidiStockState *stock_state,
	PsvitaUsbAudioMidiTakeoverState *takeover_state, int *failed_step)
{
	int result;
	int current_stopped = 0;
	int midi_started = 0;
	if (failed_step) *failed_step = PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_NONE;
	if (takeover_state) {
		takeover_state->mtp_stopped = 0;
		takeover_state->psp_comm_stopped = 0;
		takeover_state->serial_stopped = 0;
		takeover_state->controller_cycled = 0;
	}
	if (!ops || !stock_state || !takeover_state || !ops->deactivate ||
	    !ops->stop_controller ||
	    !ops->start_controller || !ops->start_midi || !ops->activate_midi ||
	    !ops->stop_midi || !ops->start_mtp || !ops->start_psp_comm ||
	    !ops->start_serial || !ops->activate_mtp) return -1;
	if (stock_state->usb_active) {
		result = traced_call(ops, context, PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_FORWARD,
			PSVITA_USB_AUDIO_MIDI_TRACE_OP_DEACTIVATE, ops->deactivate);
		if (result < 0) {
			if (failed_step) *failed_step = PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_DEACTIVATE;
			return result;
		}
	}
	/* Stop the active personality as a unit. Retail firmware rejects attempts
	 * to stop its PSP, serial, and controller components independently. */
	result = traced_call(ops, context, PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_FORWARD,
		PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_CONTROLLER, ops->stop_controller);
	if (result < 0) {
		if (failed_step) *failed_step = PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_STOP_CONTROLLER;
		goto rollback;
	}
	current_stopped = 1;
	result = traced_call(ops, context, PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_FORWARD,
		PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_CONTROLLER, ops->start_controller);
	/* Stopping the current personality may leave the bus controller running. */
	if (result < 0 &&
	    (!ops->controller_start_indicates_running ||
	     !ops->controller_start_indicates_running(result))) {
		if (failed_step) *failed_step = PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_START_CONTROLLER;
		goto rollback;
	}
	result = traced_call(ops, context, PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_FORWARD,
		PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_MIDI, ops->start_midi);
	if (result < 0) {
		if (failed_step) *failed_step = PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_START_MIDI;
		goto rollback;
	}
	midi_started = 1;
	result = traced_call(ops, context, PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_FORWARD,
		PSVITA_USB_AUDIO_MIDI_TRACE_OP_ACTIVATE_MIDI, ops->activate_midi);
	if (result < 0) {
		if (failed_step) *failed_step = PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_ACTIVATE_MIDI;
		goto rollback;
	}
	/* A current-personality stop is broad. Conservatively restore every stock
	 * driver that the pre-takeover snapshot observed as started. */
	takeover_state->mtp_stopped = stock_state->mtp_started;
	takeover_state->psp_comm_stopped = stock_state->psp_comm_started;
	takeover_state->serial_stopped = stock_state->serial_started;
	takeover_state->controller_cycled = 1;
	return 0;

rollback:
	/* Keep the original takeover error; rollback results remain in the trace. */
	(void)restore_usb_state(ops, context, stock_state,
		PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_ROLLBACK, midi_started, current_stopped,
		current_stopped, current_stopped, current_stopped);
	return result;
}

int psvita_usb_audio_midi_restore(const PsvitaUsbAudioMidiTakeoverOps *ops, void *context,
	const PsvitaUsbAudioMidiStockState *stock_state,
	const PsvitaUsbAudioMidiTakeoverState *takeover_state)
{
	if (!ops || !stock_state || !takeover_state || !ops->deactivate || !ops->stop_midi ||
	    !ops->start_controller || !ops->start_mtp ||
	    !ops->start_psp_comm || !ops->start_serial || !ops->activate_mtp)
		return -1;
	return restore_usb_state(ops, context, stock_state,
		PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_RESTORE, 1,
		takeover_state->controller_cycled, takeover_state->mtp_stopped,
		takeover_state->psp_comm_stopped, takeover_state->serial_stopped);
}
