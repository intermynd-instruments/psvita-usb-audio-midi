#include "kernel_internal.h"
#include "midi_timing.h"
#include "owner_guard.h"
#include "stock_state.h"
#include "takeover.h"

#include <psp2/kernel/error.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/kernel/processmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/udcd.h>
#include <string.h>

#define STOCK_MTP_DRIVER "USB_MTP_Driver"
#define STOCK_CONTROLLER_DRIVER "USBDeviceControllerDriver"
#define STOCK_PSP_COMM_DRIVER "USBPSPCommunicationDriver"
#define STOCK_SERIAL_DRIVER "USBSerDriver"
#define STOCK_MTP_PID 0x04e4u
#define WORK_RX (1u << 0)
#define WORK_TX (1u << 1)
#define WORK_ATTACH (1u << 2)
#define WORK_DETACH (1u << 3)
#define WORK_EXIT (1u << 4)
#define WORK_AUDIO (1u << 5)
#define WORK_MASK (WORK_RX | WORK_TX | WORK_ATTACH | WORK_DETACH | WORK_EXIT | WORK_AUDIO)
#define STOCK_STATE_RETRY_COUNT 50
#define STOCK_STATE_RETRY_DELAY_US 10000
#define OWNER_WATCHDOG_DELAY_US 100000

int module_start(SceSize argc, const void *args);
int _start(SceSize argc, const void *args)
	__attribute__((weak, alias("module_start")));

static SceUID state_mutex = -1;
static SceUID audio_write_mutex = -1;
static SceUID work_event = -1;
static SceUID rx_event = -1;
static SceUID worker_thread = -1;
static SceUID proc_handler_id = -1;
static int owner_pid = -1;
static int driver_registered;
static int midi_started;
static PsvitaUsbAudioMidiStockState stock_state;
static PsvitaUsbAudioMidiTakeoverState takeover_state;
static PsvitaUsbMidiRing rx_ring;
static PsvitaUsbMidiRing tx_ring;
static PsvitaUsbMidiEvent scheduled_tx_event;
static int scheduled_tx_valid;
static PsvitaUsbAudioMidiStatus status;
static PsvitaUsbAudioMidiDiagnostics diagnostics;

static void diagnostics_reset(void)
{
	uint32_t sequence = diagnostics.sequence + 1u;
	memset(&diagnostics, 0, sizeof(diagnostics));
	diagnostics.size = sizeof(diagnostics);
	diagnostics.diagnostics_version = PSVITA_USB_AUDIO_MIDI_DIAGNOSTICS_VERSION;
	diagnostics.build_id = PSVITA_USB_AUDIO_MIDI_BUILD_ID;
	diagnostics.sequence = sequence;
}

static void takeover_trace(void *context, int phase, int operation, int result)
{
	PsvitaUsbAudioMidiTraceEntry *entry;
	(void)context;
	if (diagnostics.entry_count >= PSVITA_USB_AUDIO_MIDI_TRACE_CAPACITY) return;
	entry = &diagnostics.entries[diagnostics.entry_count++];
	entry->phase = (uint8_t)phase;
	entry->operation = (uint8_t)operation;
	entry->reserved = 0;
	entry->result = result;
}

static int takeover_deactivate(void *context) { (void)context; return ksceUdcdDeactivate(); }
static int takeover_stop_controller(void *context)
{
	(void)context;
	return ksceUdcdStopCurrentInternal(PSVITA_USB_AUDIO_MIDI_BUS);
}
static int takeover_start_controller(void *context) { (void)context; return ksceUdcdStartInternal(STOCK_CONTROLLER_DRIVER, 0, NULL, PSVITA_USB_AUDIO_MIDI_BUS); }
static int controller_start_indicates_running(int result)
{
	return result == (int)SCE_UDCD_ERROR_ALREADY_DONE ||
	       result == (int)SCE_UDCD_ERROR_INVALID_ARGUMENT ||
	       result == (int)SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
}
static int takeover_start_midi(void *context) { (void)context; return midi_usb_start(); }
static int takeover_activate_midi(void *context) { (void)context; return ksceUdcdActivate(PSVITA_USB_AUDIO_MIDI_DEVELOPMENT_PID); }
static int takeover_stop_midi(void *context) { (void)context; return midi_usb_stop(); }
static int takeover_start_mtp(void *context) { (void)context; return ksceUdcdStartInternal(STOCK_MTP_DRIVER, 0, NULL, PSVITA_USB_AUDIO_MIDI_BUS); }
static int takeover_start_psp_comm(void *context) { (void)context; return ksceUdcdStartInternal(STOCK_PSP_COMM_DRIVER, 0, NULL, PSVITA_USB_AUDIO_MIDI_BUS); }
static int takeover_start_serial(void *context) { (void)context; return ksceUdcdStartInternal(STOCK_SERIAL_DRIVER, 0, NULL, PSVITA_USB_AUDIO_MIDI_BUS); }
static int takeover_activate_mtp(void *context) { (void)context; return ksceUdcdActivate(STOCK_MTP_PID); }

static const PsvitaUsbAudioMidiTakeoverOps takeover_ops = {
	.deactivate = takeover_deactivate,
	.stop_controller = takeover_stop_controller,
	.start_controller = takeover_start_controller,
	.start_midi = takeover_start_midi,
	.activate_midi = takeover_activate_midi,
	.stop_midi = takeover_stop_midi,
	.start_mtp = takeover_start_mtp,
	.start_psp_comm = takeover_start_psp_comm,
	.start_serial = takeover_start_serial,
	.activate_mtp = takeover_activate_mtp,
	.controller_start_indicates_running = controller_start_indicates_running,
	.trace = takeover_trace
};

static void lock_state(void) { if (state_mutex >= 0) ksceKernelLockMutex(state_mutex, 1, NULL); }
static void unlock_state(void) { if (state_mutex >= 0) ksceKernelUnlockMutex(state_mutex, 1); }
static void lock_audio_write(void) { if (audio_write_mutex >= 0) ksceKernelLockMutex(audio_write_mutex, 1, NULL); }
static void unlock_audio_write(void) { if (audio_write_mutex >= 0) ksceKernelUnlockMutex(audio_write_mutex, 1); }

static int driver_conflicts(void)
{
	return ksceUdcdGetDrvStateInternal("VITAUVC00", PSVITA_USB_AUDIO_MIDI_BUS) >= 0 ||
	       ksceUdcdGetDrvStateInternal("VITASTICK", PSVITA_USB_AUDIO_MIDI_BUS) >= 0;
}

static int snapshot_stock_state(PsvitaUsbAudioMidiStockState *snapshot, uint32_t *snapshot_flags)
{
	int controller = ksceUdcdGetDrvStateInternal(STOCK_CONTROLLER_DRIVER,
		PSVITA_USB_AUDIO_MIDI_BUS);
	int mtp = ksceUdcdGetDrvStateInternal(STOCK_MTP_DRIVER,
		PSVITA_USB_AUDIO_MIDI_BUS);
	int device = ksceUdcdGetDeviceStateInternal(PSVITA_USB_AUDIO_MIDI_BUS);
	int psp_comm = ksceUdcdGetDrvStateInternal(STOCK_PSP_COMM_DRIVER,
		PSVITA_USB_AUDIO_MIDI_BUS);
	int serial = ksceUdcdGetDrvStateInternal(STOCK_SERIAL_DRIVER,
		PSVITA_USB_AUDIO_MIDI_BUS);
	diagnostics.controller_state = controller;
	diagnostics.mtp_state = mtp;
	diagnostics.psp_comm_state = psp_comm;
	diagnostics.serial_state = serial;
	diagnostics.device_state = device;
	return psvita_usb_audio_midi_classify_stock_state(controller, mtp, psp_comm,
		serial, device, snapshot, snapshot_flags);
}

static int restore_stock_locked(void)
{
	int first_error = 0;
	if (midi_usb_audio_enabled()) midi_usb_audio_set_enabled(0);
	if (midi_started)
		first_error = psvita_usb_audio_midi_restore(&takeover_ops, NULL, &stock_state,
			&takeover_state);
	psvita_usb_audio_midi_release_state_finish(first_error, &owner_pid,
		&midi_started, &stock_state, &takeover_state);
	status.state = first_error ? PSVITA_USB_AUDIO_MIDI_STATE_ERROR : PSVITA_USB_AUDIO_MIDI_STATE_IDLE;
	if (!first_error) status.flags = 0;
	status.last_error = first_error;
	diagnostics.release_result = first_error;
	psvita_usb_midi_ring_init(&rx_ring);
	psvita_usb_midi_ring_init(&tx_ring);
	midi_usb_audio_reset();
	scheduled_tx_valid = 0;
	if (rx_event >= 0) ksceKernelSetEventFlag(rx_event, 1u);
	return first_error;
}

static int release_for_pid(SceUID pid, int forced)
{
	int result;
	lock_state();
	if (owner_pid < 0) {
		diagnostics.release_result = 0;
		unlock_state();
		return 0;
	}
	if (!forced && owner_pid != pid) {
		unlock_state();
		return PSVITA_USB_AUDIO_MIDI_ERROR_NOT_OWNER;
	}
	if (forced && owner_pid != pid) {
		unlock_state();
		return 0;
	}
	status.state = PSVITA_USB_AUDIO_MIDI_STATE_RELEASING;
	result = restore_stock_locked();
	unlock_state();
	return result;
}

static int process_exit(SceUID pid, SceProcEventInvokeParam1 *param, int arg)
{
	(void)param; (void)arg;
	release_for_pid(pid, 1);
	return 0;
}

static int process_stop(SceUID pid, int event_type,
			SceProcEventInvokeParam1 *param, int arg)
{
	(void)event_type; (void)param; (void)arg;
	release_for_pid(pid, 1);
	return 0;
}

static const SceProcEventHandler proc_handler = {
	sizeof(SceProcEventHandler), NULL, process_exit, process_exit,
	process_stop, NULL, NULL
};

static int watchdog_owner_alive(void *context, int pid)
{
	SceKernelProcessInfo info;
	(void)context;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	return ksceKernelGetProcessInfo((SceUID)pid, &info) >= 0 && info.pid == pid;
}

static int watchdog_release_owner(void *context, int pid)
{
	(void)context;
	return release_for_pid((SceUID)pid, 1);
}

static void check_owner_watchdog(void)
{
	SceUID pid;
	lock_state();
	pid = owner_pid;
	unlock_state();
	psvita_usb_audio_midi_owner_watchdog((int)pid, watchdog_owner_alive,
		watchdog_release_owner, NULL);
}

static void process_rx(void)
{
	uint8_t bytes[512];
	uint32_t length = 0;
	uint64_t completed_us = 0;
	while (midi_usb_take_rx(bytes, sizeof(bytes), &length, &completed_us) > 0) {
		lock_state();
		if (status.state == PSVITA_USB_AUDIO_MIDI_STATE_ACTIVE) {
			uint64_t now_us = (uint64_t)ksceKernelGetSystemTimeWide();
			uint64_t age_us = now_us > completed_us ? now_us - completed_us : 0;
			if (age_us > 0xffffffffu) age_us = 0xffffffffu;
			PsvitaUsbMidiParseResult parsed = psvita_usb_midi_parse_usb_packets(
				bytes, length, completed_us,
				&rx_ring, &status.rx_dropped);
			status.filtered += parsed.filtered + parsed.malformed;
			diagnostics.rx_filtered_packets += parsed.filtered;
			diagnostics.rx_malformed_packets += parsed.malformed;
			if (parsed.accepted) {
				diagnostics.rx_completed += parsed.accepted;
				diagnostics.rx_latency_histogram[
					psvita_usb_midi_latency_bucket((uint32_t)age_us)] += parsed.accepted;
				if (rx_event >= 0) ksceKernelSetEventFlag(rx_event, 1u);
			}
		}
		unlock_state();
	}
	midi_usb_wake_worker();
}

static void process_tx(void)
{
	uint8_t bytes[4];
	lock_state();
	if (!scheduled_tx_valid) {
		if (psvita_usb_midi_ring_pop(&tx_ring, &scheduled_tx_event, 1u) == 1u)
			scheduled_tx_valid = 1;
	}
	if (!scheduled_tx_valid || midi_usb_tx_pending()) {
		unlock_state();
		return;
	}
	uint64_t now_us = (uint64_t)ksceKernelGetSystemTimeWide();
	if (psvita_usb_midi_deadline_wait_us(scheduled_tx_event.timestamp_us,
	    now_us, OWNER_WATCHDOG_DELAY_US) > 0) {
		unlock_state();
		return;
	}
	if (!psvita_usb_midi_encode_event(&scheduled_tx_event, bytes)) {
		status.filtered++;
		diagnostics.tx_filtered_events++;
		scheduled_tx_valid = 0;
		unlock_state();
		return;
	}
	uint64_t error_us = scheduled_tx_event.timestamp_us && now_us > scheduled_tx_event.timestamp_us
		? now_us - scheduled_tx_event.timestamp_us : 0;
	if (error_us > 0xffffffffu) error_us = 0xffffffffu;
	int result = midi_usb_submit_tx(bytes, sizeof(bytes));
	if (result >= 0) {
		diagnostics.tx_scheduled++;
		diagnostics.tx_jitter_histogram[
			psvita_usb_midi_latency_bucket((uint32_t)error_us)]++;
		scheduled_tx_valid = 0;
	} else {
		status.last_error = result;
		if (result != PSVITA_USB_AUDIO_MIDI_ERROR_BUSY) {
			status.tx_dropped++;
			scheduled_tx_valid = 0;
		}
	}
	unlock_state();
}

static void process_detach(void)
{
	lock_state();
	psvita_usb_midi_ring_init(&tx_ring);
	scheduled_tx_valid = 0;
	unlock_state();
}

static void process_audio(void)
{
	uint64_t lock_started_us = (uint64_t)ksceKernelGetSystemTimeWide();
	lock_state();
	uint64_t lock_acquired_us = (uint64_t)ksceKernelGetSystemTimeWide();
	uint64_t lock_wait_us = lock_acquired_us - lock_started_us;
	if (lock_wait_us > 0xffffffffu) lock_wait_us = 0xffffffffu;
	midi_usb_audio_note_worker_lock_wait((uint32_t)lock_wait_us);
	if (status.state == PSVITA_USB_AUDIO_MIDI_STATE_ACTIVE) {
		int result = midi_usb_audio_submit_next();
		if (result < 0) status.last_error = result;
		result = midi_usb_audio_input_submit_next();
		if (result < 0) status.last_error = result;
	}
	unlock_state();
}

static uint32_t scheduled_tx_wait_us(void)
{
	uint32_t wait_us = OWNER_WATCHDOG_DELAY_US;
	lock_state();
	if (scheduled_tx_valid && !midi_usb_tx_pending()) {
		uint64_t now_us = (uint64_t)ksceKernelGetSystemTimeWide();
		wait_us = psvita_usb_midi_deadline_wait_us(
			scheduled_tx_event.timestamp_us, now_us, OWNER_WATCHDOG_DELAY_US);
		if (!wait_us) wait_us = 1u;
	}
	unlock_state();
	return wait_us;
}

static int worker_main(SceSize args, void *argp)
{
	(void)args; (void)argp;
	for (;;) {
		unsigned int bits = 0;
		SceUInt timeout = scheduled_tx_wait_us();
		int result = ksceKernelWaitEventFlag(work_event, WORK_MASK,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
		if (result == (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT) {
			process_audio();
			process_tx();
			/* Retry RX after hosts that configure the interface asynchronously. */
			midi_usb_wake_worker();
			check_owner_watchdog();
			continue;
		}
		if (result < 0 || (bits & WORK_EXIT)) break;
		if (bits & WORK_DETACH) process_detach();
		if (bits & WORK_AUDIO) process_audio();
		if (bits & (WORK_ATTACH | WORK_RX)) process_rx();
		if (bits & WORK_TX) process_tx();
		check_owner_watchdog();
	}
	return 0;
}

void midi_kernel_usb_attached(void) { if (work_event >= 0) ksceKernelSetEventFlag(work_event, WORK_ATTACH); }
void midi_kernel_usb_detached(void) { if (work_event >= 0) ksceKernelSetEventFlag(work_event, WORK_DETACH); }
void midi_kernel_usb_rx_ready(void) { if (work_event >= 0) ksceKernelSetEventFlag(work_event, WORK_RX); }
void midi_kernel_usb_tx_ready(void) { if (work_event >= 0) ksceKernelSetEventFlag(work_event, WORK_TX); }
void midi_kernel_usb_audio_ready(void) { if (work_event >= 0) ksceKernelSetEventFlag(work_event, WORK_AUDIO); }

int kscePsvitaUsbAudioMidiGetApiVersion(void)
{
	return (int)PSVITA_USB_AUDIO_MIDI_API_VERSION;
}

int kscePsvitaUsbAudioMidiAcquire(uint32_t flags)
{
	unsigned long syscall_state;
	int result = 0;
	uint32_t stock_flags = 0;
	int failed_step = PSVITA_USB_AUDIO_MIDI_TAKEOVER_STEP_NONE;
	int record_diagnostics;
	SceUID caller;
	ENTER_SYSCALL(syscall_state);
	caller = ksceKernelGetProcessId();
	lock_state();
	record_diagnostics = owner_pid < 0;
	if (record_diagnostics) diagnostics_reset();
	if (!driver_registered) result = PSVITA_USB_AUDIO_MIDI_ERROR_NOT_READY;
	else if (flags != PSVITA_USB_AUDIO_MIDI_ACQUIRE_REPLACE_MTP) result = PSVITA_USB_AUDIO_MIDI_ERROR_FLAGS;
	else if (owner_pid >= 0) result = PSVITA_USB_AUDIO_MIDI_ERROR_BUSY;
	else if (driver_conflicts()) {
		status.flags = 0;
		result = PSVITA_USB_AUDIO_MIDI_ERROR_CONFLICT;
	} else {
		for (int attempt = 0; attempt < STOCK_STATE_RETRY_COUNT; ++attempt) {
			result = snapshot_stock_state(&stock_state, &stock_flags);
			if (result != PSVITA_USB_AUDIO_MIDI_ERROR_USB_STATE) break;
			if (attempt + 1 < STOCK_STATE_RETRY_COUNT)
				ksceKernelDelayThread(STOCK_STATE_RETRY_DELAY_US);
		}
		status.flags = stock_flags;
	}
	if (result < 0) goto done;

	owner_pid = caller;
	status.state = PSVITA_USB_AUDIO_MIDI_STATE_ACQUIRING;
	status.flags = flags | stock_flags;
	status.last_error = 0;
	midi_usb_audio_reset();
	result = psvita_usb_audio_midi_takeover(&takeover_ops, NULL, &stock_state,
		&takeover_state, &failed_step);
	if (result < 0) {
		owner_pid = -1;
		memset(&stock_state, 0, sizeof(stock_state));
		memset(&takeover_state, 0, sizeof(takeover_state));
		status.state = PSVITA_USB_AUDIO_MIDI_STATE_ERROR;
		status.flags |= ((uint32_t)failed_step << PSVITA_USB_AUDIO_MIDI_STATUS_TAKEOVER_STEP_SHIFT) &
		                PSVITA_USB_AUDIO_MIDI_STATUS_TAKEOVER_STEP_MASK;
		status.last_error = result;
		goto done;
	}
	midi_started = 1;
	status.state = PSVITA_USB_AUDIO_MIDI_STATE_ACTIVE;
	ksceKernelSetEventFlag(work_event, WORK_TX);
	goto done;
done:
	if (result < 0) status.last_error = result;
	if (record_diagnostics) diagnostics.acquire_result = result;
	unlock_state();
	EXIT_SYSCALL(syscall_state);
	return result;
}

int kscePsvitaUsbAudioMidiRelease(void)
{
	unsigned long syscall_state;
	ENTER_SYSCALL(syscall_state);
	int result = release_for_pid(ksceKernelGetProcessId(), 0);
	EXIT_SYSCALL(syscall_state);
	return result;
}

int kscePsvitaUsbMidiRead(PsvitaUsbMidiEvent *user_events, uint32_t capacity)
{
	unsigned long syscall_state;
	PsvitaUsbMidiEvent events[PSVITA_USB_MIDI_MAX_EVENTS];
	uint32_t count;
	int result;
	ENTER_SYSCALL(syscall_state);
	if ((!user_events && capacity) || capacity > PSVITA_USB_MIDI_MAX_EVENTS) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_BOUNDS;
		goto done;
	}
	lock_state();
	if (owner_pid != ksceKernelGetProcessId()) {
		unlock_state();
		result = PSVITA_USB_AUDIO_MIDI_ERROR_NOT_OWNER;
		goto done;
	}
	count = psvita_usb_midi_ring_pop(&rx_ring, events, capacity);
	unlock_state();
	if (count && ksceKernelMemcpyKernelToUser(user_events, events,
						     count * sizeof(events[0])) < 0) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
		goto done;
	}
	result = (int)count;
done:
	EXIT_SYSCALL(syscall_state);
	return result;
}

int kscePsvitaUsbMidiReadWait(PsvitaUsbMidiEvent *user_events, uint32_t capacity,
	uint32_t timeout_us)
{
	unsigned long syscall_state;
	PsvitaUsbMidiEvent events[PSVITA_USB_MIDI_MAX_EVENTS];
	uint32_t count = 0;
	int result = 0;
	ENTER_SYSCALL(syscall_state);
	if ((!user_events && capacity) || capacity > PSVITA_USB_MIDI_MAX_EVENTS ||
	    timeout_us > 1000000u) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_BOUNDS;
		goto done;
	}
	lock_state();
	if (owner_pid != ksceKernelGetProcessId()) {
		unlock_state();
		result = PSVITA_USB_AUDIO_MIDI_ERROR_NOT_OWNER;
		goto done;
	}
	count = psvita_usb_midi_ring_pop(&rx_ring, events, capacity);
	unlock_state();
	if (!count && timeout_us && rx_event >= 0) {
		unsigned int bits = 0;
		SceUInt timeout = timeout_us;
		result = ksceKernelWaitEventFlag(rx_event, 1u,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
		if (result == (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT) {
			result = 0;
			goto done;
		}
		if (result < 0) goto done;
		lock_state();
		if (owner_pid != ksceKernelGetProcessId()) {
			unlock_state();
			result = PSVITA_USB_AUDIO_MIDI_ERROR_NOT_OWNER;
			goto done;
		}
		count = psvita_usb_midi_ring_pop(&rx_ring, events, capacity);
		unlock_state();
	}
	if (count && ksceKernelMemcpyKernelToUser(user_events, events,
		count * sizeof(events[0])) < 0) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
		goto done;
	}
	result = (int)count;
done:
	EXIT_SYSCALL(syscall_state);
	return result;
}

int kscePsvitaUsbMidiWrite(const PsvitaUsbMidiEvent *user_events, uint32_t count)
{
	unsigned long syscall_state;
	PsvitaUsbMidiEvent events[PSVITA_USB_MIDI_MAX_EVENTS];
	uint32_t accepted = 0;
	int result;
	ENTER_SYSCALL(syscall_state);
	if ((!user_events && count) || count > PSVITA_USB_MIDI_MAX_EVENTS) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_BOUNDS;
		goto done;
	}
	if (count && ksceKernelMemcpyUserToKernel(events, user_events,
						     count * sizeof(events[0])) < 0) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
		goto done;
	}
	lock_state();
	if (owner_pid != ksceKernelGetProcessId()) {
		unlock_state();
		result = PSVITA_USB_AUDIO_MIDI_ERROR_NOT_OWNER;
		goto done;
	}
	for (uint32_t i = 0; i < count; ++i) {
		if (!psvita_usb_midi_event_supported(&events[i])) {
			status.filtered++;
			diagnostics.tx_filtered_events++;
			continue;
		}
		if (!psvita_usb_midi_ring_push(&tx_ring, &events[i])) {
			status.tx_dropped++;
			continue;
		}
		accepted++;
	}
	unlock_state();
	if (accepted) ksceKernelSetEventFlag(work_event, WORK_TX);
	result = (int)accepted;
done:
	EXIT_SYSCALL(syscall_state);
	return result;
}

int kscePsvitaUsbAudioSetEnabled(int enabled)
{
	unsigned long syscall_state;
	int result = 0;
	ENTER_SYSCALL(syscall_state);
	if (enabled != 0 && enabled != 1) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_FLAGS;
		goto done;
	}
	lock_state();
	if (owner_pid != ksceKernelGetProcessId()) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_NOT_OWNER;
	} else if (status.state != PSVITA_USB_AUDIO_MIDI_STATE_ACTIVE) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_NOT_READY;
	} else {
		midi_usb_audio_set_enabled(enabled);
	}
	unlock_state();
done:
	EXIT_SYSCALL(syscall_state);
	return result;
}

static int16_t audio_write_scratch[PSVITA_USB_AUDIO_MAX_WRITE_FRAMES *
	PSVITA_USB_AUDIO_STREAM_CHANNELS] __attribute__((aligned(64)));

static int usb_audio_copy_from_user(void *context, void *destination,
	const void *source, uint32_t bytes)
{
	(void)context;
	return ksceKernelMemcpyUserToKernel(destination, source, bytes);
}

static int usb_audio_write_finish(uint32_t accepted, int error)
{
	if (accepted) {
		ksceKernelSetEventFlag(work_event, WORK_AUDIO);
		return (int)accepted;
	}
	return error;
}

static int usb_audio_writer_ready_locked(void)
{
	if (owner_pid != ksceKernelGetProcessId())
		return PSVITA_USB_AUDIO_MIDI_ERROR_NOT_OWNER;
	if (status.state != PSVITA_USB_AUDIO_MIDI_STATE_ACTIVE ||
	    !midi_usb_audio_enabled())
		return PSVITA_USB_AUDIO_MIDI_ERROR_NOT_READY;
	return 0;
}

static int usb_audio_write_user(const int16_t *user_interleaved,
	uint32_t frames, uint32_t sample_rate, uint32_t input_channels)
{
	if (!user_interleaved || !frames ||
	    frames > PSVITA_USB_AUDIO_MAX_WRITE_FRAMES)
		return PSVITA_USB_AUDIO_MIDI_ERROR_BOUNDS;
	if (sample_rate != PSVITA_USB_AUDIO_SAMPLE_RATE ||
	    (input_channels != PSVITA_USB_AUDIO_CHANNELS &&
	     input_channels != PSVITA_USB_AUDIO_STREAM_CHANNELS))
		return PSVITA_USB_AUDIO_MIDI_ERROR_FORMAT;

	int result;
	lock_audio_write();
	/* Check before copying, then check again before committing in case the
	 * owning process releases the lease while user memory is being copied. */
	lock_state();
	result = usb_audio_writer_ready_locked();
	unlock_state();
	if (result < 0) goto done_write;

	if (input_channels == PSVITA_USB_AUDIO_STREAM_CHANNELS) {
		result = psvita_usb_audio_copy_frames(audio_write_scratch,
			PSVITA_USB_AUDIO_MAX_WRITE_FRAMES, user_interleaved, frames,
			PSVITA_USB_AUDIO_STREAM_CHANNELS, usb_audio_copy_from_user, NULL);
		if (result < 0) {
			result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
			goto done_write;
		}
	} else {
		int16_t stereo[PSVITA_USB_AUDIO_MAX_WRITE_FRAMES *
			PSVITA_USB_AUDIO_CHANNELS];
		if (ksceKernelMemcpyUserToKernel(stereo, user_interleaved,
		    frames * PSVITA_USB_AUDIO_CHANNELS * sizeof(stereo[0])) < 0) {
			result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
			goto done_write;
		}
		psvita_usb_audio_expand_stereo(audio_write_scratch, stereo, frames);
	}

	lock_state();
	result = usb_audio_writer_ready_locked();
	if (result == 0)
		result = (int)midi_usb_audio_write(audio_write_scratch, frames);
	unlock_state();
done_write:
	unlock_audio_write();
	return usb_audio_write_finish(result > 0 ? (uint32_t)result : 0u, result);
}

int kscePsvitaUsbAudioWrite(const int16_t *user_interleaved, uint32_t frames,
	uint32_t sample_rate)
{
	unsigned long syscall_state;
	ENTER_SYSCALL(syscall_state);
	int result = usb_audio_write_user(user_interleaved, frames, sample_rate,
		PSVITA_USB_AUDIO_CHANNELS);
	EXIT_SYSCALL(syscall_state);
	return result;
}

int kscePsvitaUsbAudioWriteMulti(const int16_t *user_interleaved,
	uint32_t frames, uint32_t sample_rate, uint32_t channels)
{
	unsigned long syscall_state;
	ENTER_SYSCALL(syscall_state);
	int result = channels == PSVITA_USB_AUDIO_STREAM_CHANNELS
		? usb_audio_write_user(user_interleaved, frames, sample_rate, channels)
		: PSVITA_USB_AUDIO_MIDI_ERROR_FORMAT;
	EXIT_SYSCALL(syscall_state);
	return result;
}

int kscePsvitaUsbAudioGetStatus(PsvitaUsbAudioStatus *user_status)
{
	unsigned long syscall_state;
	PsvitaUsbAudioStatus copy;
	uint32_t requested_size;
	uint32_t copy_size;
	int result = 0;
	ENTER_SYSCALL(syscall_state);
	if (!user_status) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_BOUNDS;
		goto done;
	}
	if (ksceKernelMemcpyUserToKernel(&requested_size, user_status,
	    sizeof(requested_size)) < 0) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
		goto done;
	}
	if (requested_size < sizeof(uint32_t)) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_VERSION;
		goto done;
	}
	copy_size = requested_size < sizeof(copy) ? requested_size : sizeof(copy);
	lock_state();
	midi_usb_audio_get_status(&copy);
	unlock_state();
	if (ksceKernelMemcpyKernelToUser(user_status, &copy, copy_size) < 0)
		result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
done:
	EXIT_SYSCALL(syscall_state);
	return result;
}

static int16_t audio_input_read_scratch[
	PSVITA_USB_AUDIO_INPUT_MAX_READ_FRAMES *
	PSVITA_USB_AUDIO_INPUT_CHANNELS] __attribute__((aligned(64)));

int kscePsvitaUsbAudioInputRead(int16_t *user_interleaved,
	uint32_t capacity_frames)
{
	unsigned long syscall_state;
	int result = 0;
	ENTER_SYSCALL(syscall_state);
	if ((!user_interleaved && capacity_frames) ||
	    capacity_frames > PSVITA_USB_AUDIO_INPUT_MAX_READ_FRAMES) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_BOUNDS;
		goto done;
	}
	lock_audio_write();
	lock_state();
	if (owner_pid != ksceKernelGetProcessId()) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_NOT_OWNER;
	} else if (status.state != PSVITA_USB_AUDIO_MIDI_STATE_ACTIVE ||
	           !midi_usb_audio_enabled()) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_NOT_READY;
	} else {
		result = (int)midi_usb_audio_input_read(audio_input_read_scratch,
			capacity_frames);
	}
	unlock_state();
	if (result > 0 && ksceKernelMemcpyKernelToUser(user_interleaved,
	    audio_input_read_scratch,
	    (uint32_t)result * PSVITA_USB_AUDIO_INPUT_CHANNELS *
	        sizeof(audio_input_read_scratch[0])) < 0)
		result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
	unlock_audio_write();
done:
	EXIT_SYSCALL(syscall_state);
	return result;
}

int kscePsvitaUsbAudioGetInputStatus(
	PsvitaUsbAudioInputStatus *user_status)
{
	unsigned long syscall_state;
	PsvitaUsbAudioInputStatus copy;
	uint32_t requested_size;
	uint32_t copy_size;
	int result = 0;
	ENTER_SYSCALL(syscall_state);
	if (!user_status) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_BOUNDS;
		goto done;
	}
	if (ksceKernelMemcpyUserToKernel(&requested_size, user_status,
	    sizeof(requested_size)) < 0) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
		goto done;
	}
	if (requested_size < sizeof(uint32_t)) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_VERSION;
		goto done;
	}
	copy_size = requested_size < sizeof(copy) ? requested_size : sizeof(copy);
	lock_state();
	midi_usb_audio_input_get_status(&copy);
	unlock_state();
	if (ksceKernelMemcpyKernelToUser(user_status, &copy, copy_size) < 0)
		result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
done:
	EXIT_SYSCALL(syscall_state);
	return result;
}

int kscePsvitaUsbAudioMidiGetStatus(PsvitaUsbAudioMidiStatus *user_status)
{
	unsigned long syscall_state;
	PsvitaUsbAudioMidiStatus copy;
	int result = 0;
	ENTER_SYSCALL(syscall_state);
	if (!user_status) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_BOUNDS;
		goto done;
	}
	lock_state();
	copy = status;
	if (midi_usb_connected()) copy.flags |= PSVITA_USB_AUDIO_MIDI_STATUS_CONNECTED;
	if (midi_usb_configured()) copy.flags |= PSVITA_USB_AUDIO_MIDI_STATUS_CONFIGURED;
	if (midi_usb_rx_pending()) copy.flags |= PSVITA_USB_AUDIO_MIDI_STATUS_RX_ARMED;
	unlock_state();
	if (ksceKernelMemcpyKernelToUser(user_status, &copy, sizeof(copy)) < 0)
		result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
done:
	EXIT_SYSCALL(syscall_state);
	return result;
}

int kscePsvitaUsbAudioMidiGetDiagnostics(PsvitaUsbAudioMidiDiagnostics *user_diagnostics)
{
	unsigned long syscall_state;
	PsvitaUsbAudioMidiDiagnostics copy;
	MidiUsbRxDiagnostics rx_diagnostics;
	uint32_t requested_size;
	uint32_t copy_size;
	int result = 0;
	ENTER_SYSCALL(syscall_state);
	if (!user_diagnostics ||
	    ksceKernelMemcpyUserToKernel(&requested_size, user_diagnostics,
	                                 sizeof(requested_size)) < 0) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
		goto done;
	}
	if (requested_size < 48u) {
		result = PSVITA_USB_AUDIO_MIDI_ERROR_BOUNDS;
		goto done;
	}
	lock_state();
	copy = diagnostics;
	unlock_state();
	midi_usb_get_rx_diagnostics(&rx_diagnostics);
	copy.rx_submit_attempts = rx_diagnostics.submit_attempts;
	copy.rx_submit_failures = rx_diagnostics.submit_failures;
	copy.rx_callback_count = rx_diagnostics.callback_count;
	copy.rx_callback_bytes = rx_diagnostics.callback_bytes;
	copy.rx_last_submit_result = rx_diagnostics.last_submit_result;
	copy.rx_last_callback_result = rx_diagnostics.last_callback_result;
	copy.rx_last_transfer_length = rx_diagnostics.last_transfer_length;
	copy.rx_last_packet_word = rx_diagnostics.last_packet_word;
	copy_size = requested_size < sizeof(copy) ? requested_size : sizeof(copy);
	if (ksceKernelMemcpyKernelToUser(user_diagnostics, &copy, copy_size) < 0)
		result = PSVITA_USB_AUDIO_MIDI_ERROR_COPY;
done:
	EXIT_SYSCALL(syscall_state);
	return result;
}

int module_start(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	memset(&status, 0, sizeof(status));
	memset(&diagnostics, 0, sizeof(diagnostics));
	diagnostics.size = sizeof(diagnostics);
	diagnostics.diagnostics_version = PSVITA_USB_AUDIO_MIDI_DIAGNOSTICS_VERSION;
	diagnostics.build_id = PSVITA_USB_AUDIO_MIDI_BUILD_ID;
	status.size = sizeof(status);
	status.api_version = PSVITA_USB_AUDIO_MIDI_API_VERSION;
	status.state = PSVITA_USB_AUDIO_MIDI_STATE_UNAVAILABLE;
	psvita_usb_midi_ring_init(&rx_ring);
	psvita_usb_midi_ring_init(&tx_ring);
	midi_usb_audio_reset();
	state_mutex = ksceKernelCreateMutex("psvita_usb_audio_midi_state", 0, 1, NULL);
	if (state_mutex < 0) return SCE_KERNEL_START_FAILED;
	audio_write_mutex = ksceKernelCreateMutex("psvita_usb_audio_write", 0, 1, NULL);
	if (audio_write_mutex < 0) goto fail_state_mutex;
	work_event = ksceKernelCreateEventFlag("psvita_usb_audio_midi_work", 0, 0, NULL);
	if (work_event < 0) goto fail_audio_mutex;
	rx_event = ksceKernelCreateEventFlag("psvita_usb_midi_rx", 0, 0, NULL);
	if (rx_event < 0) goto fail_event;
	worker_thread = ksceKernelCreateThread("psvita_usb_audio_midi_worker", worker_main,
		0x3c, 0x3000, 0, 0, NULL);
	if (worker_thread < 0) goto fail_rx_event;
	int result = midi_usb_register();
	if (result < 0) goto fail_thread;
	driver_registered = 1;
	proc_handler_id = ksceKernelRegisterProcEventHandler("psvita_usb_audio_midi_owner",
		&proc_handler, 0);
	if (proc_handler_id < 0) goto fail_driver;
	result = ksceKernelStartThread(worker_thread, 0, NULL);
	if (result < 0) goto fail_handler;
	status.state = PSVITA_USB_AUDIO_MIDI_STATE_IDLE;
	return SCE_KERNEL_START_SUCCESS;

fail_handler:
	ksceKernelUnregisterProcEventHandler(proc_handler_id);
fail_driver:
	midi_usb_unregister();
	driver_registered = 0;
fail_thread:
	ksceKernelDeleteThread(worker_thread);
fail_rx_event:
	ksceKernelDeleteEventFlag(rx_event);
fail_event:
	ksceKernelDeleteEventFlag(work_event);
fail_audio_mutex:
	ksceKernelDeleteMutex(audio_write_mutex);
fail_state_mutex:
	ksceKernelDeleteMutex(state_mutex);
	return SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	lock_state();
	if (owner_pid >= 0) restore_stock_locked();
	unlock_state();
	if (proc_handler_id >= 0) ksceKernelUnregisterProcEventHandler(proc_handler_id);
	if (work_event >= 0) ksceKernelSetEventFlag(work_event, WORK_EXIT);
	if (worker_thread >= 0) {
		ksceKernelWaitThreadEnd(worker_thread, NULL, NULL);
		ksceKernelDeleteThread(worker_thread);
	}
	if (driver_registered) midi_usb_unregister();
	if (rx_event >= 0) ksceKernelDeleteEventFlag(rx_event);
	if (work_event >= 0) ksceKernelDeleteEventFlag(work_event);
	if (audio_write_mutex >= 0) ksceKernelDeleteMutex(audio_write_mutex);
	if (state_mutex >= 0) ksceKernelDeleteMutex(state_mutex);
	return SCE_KERNEL_STOP_SUCCESS;
}
