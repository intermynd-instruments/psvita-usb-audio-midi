#include "psvita_usb_audio_midi.h"
#include "audio_diagnostics.h"
#include "scope_text.h"

#include <SDL.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define REPORT_DIR "ux0:data/psvita-usb-audio-midi"
#define REPORT_PATH REPORT_DIR "/diagnostic.txt"
#define AUDIO_PROBE_FRAMES 256u

typedef struct {
	SDL_Thread *thread;
	SDL_atomic_t stop_requested;
	SDL_atomic_t running;
	SDL_atomic_t write_calls;
	SDL_atomic_t write_failures;
	SDL_atomic_t last_write_result;
	SDL_atomic_t status_failures;
	SDL_atomic_t late_wakes;
	SDL_atomic_t pacing_resets;
	SDL_atomic_t maximum_lateness_us;
	SDL_atomic_t maximum_write_us;
	int start_result;
	int enable_result;
	int disable_result;
	int enabled;
} AudioProbe;

/* The Vita SDL thread stack is deliberately small. This 5 KiB probe payload
 * is single-producer state, so keep it in static storage instead. */
static int16_t audio_probe_pcm[AUDIO_PROBE_FRAMES *
	PSVITA_USB_AUDIO_STREAM_CHANNELS];
static PsvitaUsbAudioDiagnosticLog audio_diagnostic_log;

static const int16_t sine_32[32] = {
	0, 2341, 4592, 6667, 8485, 9978, 11087, 11777,
	12000, 11777, 11087, 9978, 8485, 6667, 4592, 2341,
	0, -2341, -4592, -6667, -8485, -9978, -11087, -11777,
	-12000, -11777, -11087, -9978, -8485, -6667, -4592, -2341
};

static SDL_Texture *scope_font_texture;

static SDL_Texture *create_font_texture(SDL_Renderer *renderer)
{
	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0,
		(int)(PSVITA_USB_SCOPE_GLYPH_COUNT * PSVITA_USB_SCOPE_GLYPH_WIDTH),
		PSVITA_USB_SCOPE_GLYPH_HEIGHT, 32, SDL_PIXELFORMAT_RGBA8888);
	if (!surface) return NULL;
	uint32_t transparent = SDL_MapRGBA(surface->format, 0, 0, 0, 0);
	uint32_t foreground = SDL_MapRGBA(surface->format, 220, 230, 235, 255);
	SDL_FillRect(surface, NULL, transparent);
	if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) < 0) {
		SDL_FreeSurface(surface);
		return NULL;
	}
	for (uint32_t glyph = 0; glyph < PSVITA_USB_SCOPE_GLYPH_COUNT; ++glyph) {
		const uint8_t *rows = psvita_usb_scope_glyph_rows(glyph);
		for (uint32_t row = 0; row < PSVITA_USB_SCOPE_GLYPH_HEIGHT; ++row) {
			uint32_t *pixels = (uint32_t *)((uint8_t *)surface->pixels +
				row * (uint32_t)surface->pitch);
			for (uint32_t column = 0; column < PSVITA_USB_SCOPE_GLYPH_WIDTH;
			     ++column)
				if (rows[row] & (1u << (4u - column)))
					pixels[glyph * PSVITA_USB_SCOPE_GLYPH_WIDTH + column] =
						foreground;
		}
	}
	if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
	SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_FreeSurface(surface);
	if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	return texture;
}

static void draw_text(SDL_Renderer *renderer, int x, int y, const char *text)
{
	if (!scope_font_texture || !text) return;
	for (uint32_t i = 0; text[i] && i < PSVITA_USB_SCOPE_MAX_TEXT_CHARS; ++i) {
		int glyph = psvita_usb_scope_glyph_index(text[i]);
		if (glyph < 0) continue;
		SDL_Rect source = {
			glyph * (int)PSVITA_USB_SCOPE_GLYPH_WIDTH, 0,
			PSVITA_USB_SCOPE_GLYPH_WIDTH, PSVITA_USB_SCOPE_GLYPH_HEIGHT
		};
		SDL_Rect destination = {
			x + (int)i * 12, y,
			PSVITA_USB_SCOPE_GLYPH_WIDTH * 2,
			PSVITA_USB_SCOPE_GLYPH_HEIGHT * 2
		};
		SDL_RenderCopy(renderer, scope_font_texture, &source, &destination);
	}
}

static const char *error_name(int result)
{
	switch ((uint32_t)result) {
	case 0: return "OK";
	case 0x80243001u: return "ALREADY DONE";
	case 0x80243002u: return "INVALID ARGUMENT";
	case 0x80243003u: return "ARGUMENT LIMIT";
	case 0x80243004u: return "MEMORY EXHAUSTED";
	case 0x80243005u: return "DRIVER NOT FOUND";
	case 0x80243006u: return "DRIVER IN PROGRESS";
	case 0x80243007u: return "BUS DRIVER STOPPED";
	case (uint32_t)PSVITA_USB_AUDIO_MIDI_ERROR_NOT_READY: return "PLUGIN NOT READY";
	case (uint32_t)PSVITA_USB_AUDIO_MIDI_ERROR_BUSY: return "LEASE BUSY";
	case (uint32_t)PSVITA_USB_AUDIO_MIDI_ERROR_USB_STATE: return "USB STATE";
	case (uint32_t)PSVITA_USB_AUDIO_MIDI_ERROR_CONFLICT: return "USB CONFLICT";
	case (uint32_t)PSVITA_USB_AUDIO_MIDI_ERROR_NOT_OWNER: return "NOT OWNER";
	case (uint32_t)PSVITA_USB_AUDIO_MIDI_ERROR_BOUNDS: return "BOUNDS";
	case (uint32_t)PSVITA_USB_AUDIO_MIDI_ERROR_COPY: return "COPY";
	case (uint32_t)PSVITA_USB_AUDIO_MIDI_ERROR_FORMAT: return "FORMAT";
	default: return "UNKNOWN ERROR";
	}
}

static const char *audio_state_name(unsigned state)
{
	switch (state) {
	case PSVITA_USB_AUDIO_STATE_DISABLED: return "DISABLED";
	case PSVITA_USB_AUDIO_STATE_IDLE: return "IDLE";
	case PSVITA_USB_AUDIO_STATE_CONNECTED: return "CONNECTED";
	case PSVITA_USB_AUDIO_STATE_STREAMING: return "STREAMING";
	case PSVITA_USB_AUDIO_STATE_ERROR: return "ERROR";
	default: return "UNKNOWN";
	}
}

static const char *phase_name(unsigned phase)
{
	switch (phase) {
	case PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_FORWARD: return "TRY";
	case PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_ROLLBACK: return "ROLLBACK";
	case PSVITA_USB_AUDIO_MIDI_TRACE_PHASE_RESTORE: return "RESTORE";
	default: return "UNKNOWN";
	}
}

static const char *operation_name(unsigned operation)
{
	switch (operation) {
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_DEACTIVATE: return "DEACTIVATE";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_MTP: return "STOP MTP";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_PSP_COMM: return "STOP PSP";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_SERIAL: return "STOP SERIAL";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_CONTROLLER: return "STOP CONTROLLER";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_CONTROLLER: return "START CONTROLLER";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_MIDI: return "START MIDI";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_ACTIVATE_MIDI: return "ACTIVATE MIDI";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_STOP_MIDI: return "STOP MIDI";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_MTP: return "START MTP";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_PSP_COMM: return "START PSP";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_START_SERIAL: return "START SERIAL";
	case PSVITA_USB_AUDIO_MIDI_TRACE_OP_ACTIVATE_MTP: return "ACTIVATE MTP";
	default: return "UNKNOWN OP";
	}
}

static uint32_t histogram_p95_us(
	const uint32_t histogram[PSVITA_USB_MIDI_LATENCY_BUCKETS])
{
	static const uint32_t limits[PSVITA_USB_MIDI_LATENCY_BUCKETS] = {
		125u, 250u, 500u, 1000u, 2000u, 5000u, 10000u, 10000u
	};
	uint64_t total = 0;
	for (uint32_t i = 0; i < PSVITA_USB_MIDI_LATENCY_BUCKETS; ++i)
		total += histogram[i];
	if (!total) return 0;
	uint64_t target = (total * 95u + 99u) / 100u;
	uint64_t cumulative = 0;
	for (uint32_t i = 0; i < PSVITA_USB_MIDI_LATENCY_BUCKETS; ++i) {
		cumulative += histogram[i];
		if (cumulative >= target) return limits[i];
	}
	return limits[PSVITA_USB_MIDI_LATENCY_BUCKETS - 1u];
}

static void fill_test_tones(int16_t *pcm, uint32_t frames, uint32_t *phases)
{
	static const uint32_t frequencies[PSVITA_USB_AUDIO_STREAM_CHANNELS] = {
		440u, 880u, 125u, 250u, 375u, 500u, 625u, 750u, 875u, 1000u
	};
	for (uint32_t i = 0; i < frames; ++i) {
		for (uint32_t channel = 0; channel < PSVITA_USB_AUDIO_STREAM_CHANNELS;
		     ++channel) {
			pcm[i * PSVITA_USB_AUDIO_STREAM_CHANNELS + channel] =
				sine_32[phases[channel] >> 27u];
			phases[channel] += (uint32_t)(((uint64_t)frequencies[channel] << 32u) /
				PSVITA_USB_AUDIO_SAMPLE_RATE);
		}
	}
}

static void atomic_max(SDL_atomic_t *value, uint32_t candidate)
{
	int current = SDL_AtomicGet(value);
	while (candidate > (uint32_t)current &&
	       !SDL_AtomicCAS(value, current, (int)candidate))
		current = SDL_AtomicGet(value);
}

static int audio_probe_thread(void *userdata)
{
	AudioProbe *probe = (AudioProbe *)userdata;
	uint32_t phases[PSVITA_USB_AUDIO_STREAM_CHANNELS] = { 0 };
	uint64_t frequency = SDL_GetPerformanceFrequency();
	uint64_t next_tick = SDL_GetPerformanceCounter();
	uint64_t tick_remainder = 0;
	SDL_AtomicSet(&probe->running, 1);
	while (!SDL_AtomicGet(&probe->stop_requested)) {
		PsvitaUsbAudioStatus status;
		memset(&status, 0, sizeof(status));
		status.size = sizeof(status);
		int status_result = psvitaUsbAudioGetStatus(&status);
		if (status_result < 0 ||
		    !(status.flags & PSVITA_USB_AUDIO_STATUS_HOST_STREAMING)) {
			if (status_result < 0)
				SDL_AtomicAdd(&probe->status_failures, 1);
			next_tick = SDL_GetPerformanceCounter();
			tick_remainder = 0;
			SDL_Delay(10);
			continue;
		}

		fill_test_tones(audio_probe_pcm, AUDIO_PROBE_FRAMES, phases);
		uint64_t write_started = SDL_GetPerformanceCounter();
		int result = psvitaUsbAudioWriteMulti(audio_probe_pcm, AUDIO_PROBE_FRAMES,
			PSVITA_USB_AUDIO_SAMPLE_RATE, PSVITA_USB_AUDIO_STREAM_CHANNELS);
		uint64_t write_finished = SDL_GetPerformanceCounter();
		uint32_t write_us = (uint32_t)(((write_finished - write_started) *
			1000000u) / frequency);
		atomic_max(&probe->maximum_write_us, write_us);
		SDL_AtomicSet(&probe->last_write_result, result);
		SDL_AtomicAdd(&probe->write_calls, 1);
		if (result != (int)AUDIO_PROBE_FRAMES)
			SDL_AtomicAdd(&probe->write_failures, 1);

		uint64_t now = write_finished;
		if (now > next_tick) {
			uint32_t late_us = (uint32_t)(((now - next_tick) * 1000000u) /
				frequency);
			atomic_max(&probe->maximum_lateness_us, late_us);
			if (late_us > 1000u) SDL_AtomicAdd(&probe->late_wakes, 1);
		}
		if (now > next_tick && now - next_tick > frequency / 50u) {
			SDL_AtomicAdd(&probe->pacing_resets, 1);
			next_tick = now;
			tick_remainder = 0;
		}
		tick_remainder += frequency * AUDIO_PROBE_FRAMES;
		next_tick += tick_remainder / PSVITA_USB_AUDIO_SAMPLE_RATE;
		tick_remainder %= PSVITA_USB_AUDIO_SAMPLE_RATE;
		while (!SDL_AtomicGet(&probe->stop_requested)) {
			now = SDL_GetPerformanceCounter();
			if (now >= next_tick) break;
			uint64_t wait_ms = ((next_tick - now) * 1000u + frequency - 1u) /
				frequency;
			SDL_Delay((uint32_t)(wait_ms ? wait_ms : 1u));
		}
	}
	SDL_AtomicSet(&probe->running, 0);
	return 0;
}

static int audio_probe_start(AudioProbe *probe)
{
	probe->enable_result = psvitaUsbAudioSetEnabled(1);
	if (probe->enable_result < 0) {
		probe->start_result = probe->enable_result;
		return probe->start_result;
	}
	probe->enabled = 1;
	SDL_AtomicSet(&probe->stop_requested, 0);
	probe->thread = SDL_CreateThread(audio_probe_thread, "usb-audio-probe", probe);
	if (!probe->thread) {
		probe->disable_result = psvitaUsbAudioSetEnabled(0);
		probe->enabled = 0;
		probe->start_result = -1;
		return probe->start_result;
	}
	probe->start_result = 0;
	return probe->start_result;
}

static void audio_probe_reset_diagnostics(AudioProbe *probe)
{
	SDL_AtomicSet(&probe->write_calls, 0);
	SDL_AtomicSet(&probe->write_failures, 0);
	SDL_AtomicSet(&probe->last_write_result, 0);
	SDL_AtomicSet(&probe->status_failures, 0);
	SDL_AtomicSet(&probe->late_wakes, 0);
	SDL_AtomicSet(&probe->pacing_resets, 0);
	SDL_AtomicSet(&probe->maximum_lateness_us, 0);
	SDL_AtomicSet(&probe->maximum_write_us, 0);
}

static int audio_probe_stop(AudioProbe *probe)
{
	if (probe->thread) {
		SDL_AtomicSet(&probe->stop_requested, 1);
		SDL_WaitThread(probe->thread, NULL);
		probe->thread = NULL;
	}
	if (probe->enabled) {
		probe->disable_result = psvitaUsbAudioSetEnabled(0);
		probe->enabled = 0;
	}
	return probe->disable_result;
}

static void report_line(SceUID fd, const char *format, ...)
{
	char line[320];
	va_list args;
	int length;
	va_start(args, format);
	length = vsnprintf(line, sizeof(line) - 2u, format, args);
	va_end(args);
	if (length < 0) return;
	if ((size_t)length > sizeof(line) - 2u) length = (int)sizeof(line) - 2;
	line[length++] = '\n';
	sceIoWrite(fd, line, (SceSize)length);
}

static const char *pass_fail(uint32_t failures, uint32_t mask)
{
	return (failures & mask) ? "FAIL" : "PASS";
}

static void report_audio_diagnostic_sample(SceUID fd, const char *kind,
	uint32_t index, const PsvitaUsbAudioDiagnosticSample *sample)
{
	report_line(fd,
		"%s,%u,%u,%08X,%08X,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%08X,%u,%u,%u,%03X",
		kind, index, sample->elapsed_ms, sample->event_flags,
		sample->status_flags, sample->buffered_frames,
		sample->produced_frames, sample->consumed_frames,
		sample->packets_submitted, sample->packets_completed,
		sample->bytes_transmitted, sample->output_underruns,
		sample->output_overruns, sample->submit_failures,
		sample->completion_errors, sample->disconnects,
		sample->stalled_requests, sample->recovered_requests,
		sample->cancel_failures, sample->zero_byte_completions,
		sample->short_completions, sample->late_completions,
		sample->rebuffer_events, sample->requested_bytes,
		sample->completion_result, sample->completed_bytes,
		sample->completion_gap_us, sample->rearm_delay_us,
		sample->worker_lock_wait_us, sample->channel_mask);
	report_line(fd, "%sH,%u,%u,%u,%u", kind, index,
		sample->conceal_events, sample->concealed_frames,
		sample->consecutive_missing_frames);
	report_line(fd, "%sP,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", kind, index,
		sample->channel_peaks[0], sample->channel_peaks[1],
		sample->channel_peaks[2], sample->channel_peaks[3],
		sample->channel_peaks[4], sample->channel_peaks[5],
		sample->channel_peaks[6], sample->channel_peaks[7],
		sample->channel_peaks[8], sample->channel_peaks[9]);
	report_line(fd, "%sW,%u,%u,%u,%u,%u,%u,%u,%u", kind, index,
		sample->producer_write_calls, sample->producer_write_failures,
		sample->producer_status_failures, sample->producer_late_wakes,
		sample->producer_pacing_resets, sample->producer_max_lateness_us,
		sample->producer_max_write_us);
}

static void report_audio_diagnostic_log(SceUID fd,
	const PsvitaUsbAudioDiagnosticLog *log, AudioProbe *audio_probe)
{
	uint32_t failures = psvita_usb_audio_diag_failure_flags(log);
	uint32_t producer_failures = audio_probe ?
		(uint32_t)(SDL_AtomicGet(&audio_probe->write_failures) +
		SDL_AtomicGet(&audio_probe->status_failures) +
		SDL_AtomicGet(&audio_probe->pacing_resets)) : 0u;
	report_line(fd, "");
	report_line(fd, "[10_channel_test_summary]");
	report_line(fd,
		"expected_rate=%u expected_channels=%u expected_bits=%u expected_ring_frames=%u",
		PSVITA_USB_AUDIO_SAMPLE_RATE, PSVITA_USB_AUDIO_STREAM_CHANNELS,
		PSVITA_USB_AUDIO_BITS_PER_SAMPLE, PSVITA_USB_AUDIO_RING_FRAMES);
	report_line(fd,
		"expected_request_bytes=%u expected_requests_per_second=%u expected_bytes_per_second=%u late_threshold_us=%u",
		PSVITA_USB_AUDIO_DIAG_EXPECTED_REQUEST_BYTES,
		PSVITA_USB_AUDIO_DIAG_EXPECTED_REQUESTS_PER_SECOND,
		PSVITA_USB_AUDIO_DIAG_EXPECTED_BYTES_PER_SECOND,
		PSVITA_USB_AUDIO_DIAG_LATE_COMPLETION_US);
	report_line(fd,
		"timeline_samples=%u anomaly_events=%u retained_markers=%u stream_sessions=%u manual_marks=%u all_event_flags=%08X failure_flags=%08X",
		log->timeline_count, log->event_count, log->marker_count, log->stream_sessions,
		log->manual_marks, log->all_event_flags, failures);
	report_line(fd,
		"steady_window_ms=%u observed_requests_per_second=%u observed_bytes_per_second=%u",
		log->have_steady ? log->steady_last_ms - log->steady_first_ms : 0u,
		psvita_usb_audio_diag_request_rate(log),
		psvita_usb_audio_diag_byte_rate(log));
	report_line(fd, "test_format=%s",
		pass_fail(failures, PSVITA_USB_AUDIO_DIAG_FAULT_FORMAT));
	report_line(fd, "test_request_size=%s",
		pass_fail(failures, PSVITA_USB_AUDIO_DIAG_FAULT_REQUEST_SIZE));
	report_line(fd, "test_completion_cadence=%s",
		pass_fail(failures, PSVITA_USB_AUDIO_DIAG_FAULT_LATE));
	report_line(fd, "test_transfer_integrity=%s",
		pass_fail(failures, PSVITA_USB_AUDIO_DIAG_FAULT_SHORT |
			PSVITA_USB_AUDIO_DIAG_FAULT_SUBMIT |
			PSVITA_USB_AUDIO_DIAG_FAULT_COMPLETION |
			PSVITA_USB_AUDIO_DIAG_FAULT_ZERO_BYTES));
	report_line(fd, "test_buffer_continuity=%s",
		pass_fail(failures, PSVITA_USB_AUDIO_DIAG_FAULT_BUFFER));
	report_line(fd, "test_request_stall_recovery=%s",
		pass_fail(failures, PSVITA_USB_AUDIO_DIAG_FAULT_STALL |
			PSVITA_USB_AUDIO_DIAG_FAULT_DISCONNECT));
	report_line(fd, "test_ten_channel_payload=%s",
		pass_fail(failures, PSVITA_USB_AUDIO_DIAG_FAULT_CHANNELS));
	report_line(fd, "test_throughput=%s",
		pass_fail(failures, PSVITA_USB_AUDIO_DIAG_FAULT_THROUGHPUT |
			PSVITA_USB_AUDIO_DIAG_FAULT_NO_DATA));
	report_line(fd, "test_probe_producer=%s failures=%u",
		producer_failures || (failures & PSVITA_USB_AUDIO_DIAG_FAULT_PRODUCER) ?
			"FAIL" : "PASS", producer_failures);
	report_line(fd, "overall=%s", failures || producer_failures ? "FAIL" : "PASS");
	report_line(fd,
		"event_flag_legend=00001_MARK,00002_STREAM_START,00004_FORMAT,00008_REQUEST_SIZE,00010_SHORT,00020_LATE,00040_SUBMIT,00080_COMPLETION,00100_BUFFER,00200_STALL,00400_CHANNELS,00800_DISCONNECT,01000_ZERO_BYTES,02000_COUNTER_RESET,04000_THROUGHPUT,08000_NO_DATA,10000_PRODUCER");

	report_line(fd, "");
	report_line(fd, "[sample_columns]");
	report_line(fd,
		"columns=kind,index,ms,event_flags,status_flags,buffered,produced,consumed,submitted,completed,bytes,underruns,overruns,submit_failures,completion_errors,disconnects,stalled,recovered,cancel_failures,zero_bytes,short,late,rebuffer,requested_bytes,completion_result,completed_bytes,gap_us,rearm_us,lock_wait_us,channel_mask");
	report_line(fd, "peak_columns=kindP,index,ch0,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8,ch9");
	report_line(fd, "hold_columns=kindH,index,conceal_events,concealed_frames,consecutive_missing_frames");
	report_line(fd, "producer_columns=kindW,index,write_calls,write_failures,status_failures,late_wakes,pacing_resets,max_lateness_us,max_write_us");
	report_line(fd, "");
	report_line(fd, "[manual_markers]");
	for (uint32_t i = 0; i < log->marker_count; ++i) {
		const PsvitaUsbAudioDiagnosticSample *sample =
			psvita_usb_audio_diag_marker_at(log, i);
		if (sample) report_audio_diagnostic_sample(fd, "M", i, sample);
	}

	report_line(fd, "");
	report_line(fd, "[anomaly_events]");
	for (uint32_t i = 0; i < log->event_count; ++i) {
		const PsvitaUsbAudioDiagnosticSample *sample =
			psvita_usb_audio_diag_event_at(log, i);
		if (sample) report_audio_diagnostic_sample(fd, "E", i, sample);
	}

	report_line(fd, "");
	report_line(fd, "[rolling_timeline]");
	report_line(fd, "sample_interval_target_ms=50 capacity=%u history_seconds=25.6",
		PSVITA_USB_AUDIO_DIAG_TIMELINE_CAPACITY);
	for (uint32_t i = 0; i < log->timeline_count; ++i) {
		const PsvitaUsbAudioDiagnosticSample *sample =
			psvita_usb_audio_diag_timeline_at(log, i);
		if (sample) report_audio_diagnostic_sample(fd, "T", i, sample);
	}
}

static int save_report(int load_result, int acquire_result, int release_result,
	const PsvitaUsbAudioMidiStatus *status, const PsvitaUsbAudioMidiDiagnostics *diagnostics,
	int diagnostics_result, const PsvitaUsbAudioStatus *audio_status,
	int audio_status_result, AudioProbe *audio_probe,
	const PsvitaUsbAudioDiagnosticLog *audio_log)
{
	sceIoMkdir(REPORT_DIR, 0777);
	SceUID fd = sceIoOpen(REPORT_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0) return (int)fd;
	report_line(fd, "psvita-usb-audio-midi diagnostic report");
	report_line(fd, "scope_api=%08X expected_build=%08X", PSVITA_USB_AUDIO_MIDI_API_VERSION,
	            PSVITA_USB_AUDIO_MIDI_BUILD_ID);
	report_line(fd, "load=%08X diagnostics_call=%08X acquire=%08X release=%08X",
	            load_result, diagnostics_result, acquire_result, release_result);
	if (status) {
		report_line(fd, "status_state=%u status_flags=%08X status_error=%08X",
		            status->state, status->flags, status->last_error);
		report_line(fd, "filtered=%u rx_dropped=%u tx_dropped=%u",
		            status->filtered, status->rx_dropped, status->tx_dropped);
	}
	if (diagnostics && diagnostics_result >= 0) {
		report_line(fd, "kernel_build=%08X diagnostics_version=%08X sequence=%u",
		            diagnostics->build_id, diagnostics->diagnostics_version,
		            diagnostics->sequence);
		report_line(fd, "controller=%08X mtp=%08X psp=%08X serial=%08X device=%08X",
		            diagnostics->controller_state, diagnostics->mtp_state,
		            diagnostics->psp_comm_state, diagnostics->serial_state,
		            diagnostics->device_state);
		report_line(fd, "rx_completed=%u rx_kernel_p95_us=%u tx_scheduled=%u tx_jitter_p95_us=%u",
		            diagnostics->rx_completed,
		            histogram_p95_us(diagnostics->rx_latency_histogram),
		            diagnostics->tx_scheduled,
		            histogram_p95_us(diagnostics->tx_jitter_histogram));
		report_line(fd, "rx_submit=%u rx_submit_fail=%u rx_callbacks=%u rx_bytes=%u",
		            diagnostics->rx_submit_attempts,
		            diagnostics->rx_submit_failures,
		            diagnostics->rx_callback_count,
		            diagnostics->rx_callback_bytes);
		report_line(fd, "rx_last_submit=%08X rx_last_callback=%08X",
		            diagnostics->rx_last_submit_result,
		            diagnostics->rx_last_callback_result);
		report_line(fd, "rx_filtered=%u rx_malformed=%u tx_filtered=%u",
		            diagnostics->rx_filtered_packets,
		            diagnostics->rx_malformed_packets,
		            diagnostics->tx_filtered_events);
		report_line(fd, "rx_raw_length=%u rx_raw=%02X %02X %02X %02X",
		            diagnostics->rx_last_transfer_length,
		            diagnostics->rx_last_packet_word & 0xffu,
		            (diagnostics->rx_last_packet_word >> 8u) & 0xffu,
		            (diagnostics->rx_last_packet_word >> 16u) & 0xffu,
		            (diagnostics->rx_last_packet_word >> 24u) & 0xffu);
		for (uint32_t i = 0; i < diagnostics->entry_count &&
		     i < PSVITA_USB_AUDIO_MIDI_TRACE_CAPACITY; ++i) {
			const PsvitaUsbAudioMidiTraceEntry *entry = &diagnostics->entries[i];
			report_line(fd, "%02u %-8s %-20s %08X %s", i,
			            phase_name(entry->phase), operation_name(entry->operation),
			            entry->result, error_name(entry->result));
		}
	}
	if (audio_probe) {
		report_line(fd, "audio_status_call=%08X start=%08X enable=%08X disable=%08X producer_running=%u",
		            audio_status_result, audio_probe->start_result, audio_probe->enable_result,
		            audio_probe->disable_result,
		            (unsigned)SDL_AtomicGet(&audio_probe->running));
		report_line(fd, "audio_write_calls=%u audio_write_failures=%u audio_last_write=%08X",
		            (unsigned)SDL_AtomicGet(&audio_probe->write_calls),
		            (unsigned)SDL_AtomicGet(&audio_probe->write_failures),
		            SDL_AtomicGet(&audio_probe->last_write_result));
		report_line(fd, "audio_producer_status_failures=%u late_wakes=%u pacing_resets=%u max_lateness_us=%u max_write_us=%u",
		            (unsigned)SDL_AtomicGet(&audio_probe->status_failures),
		            (unsigned)SDL_AtomicGet(&audio_probe->late_wakes),
		            (unsigned)SDL_AtomicGet(&audio_probe->pacing_resets),
		            (unsigned)SDL_AtomicGet(&audio_probe->maximum_lateness_us),
		            (unsigned)SDL_AtomicGet(&audio_probe->maximum_write_us));
	}
	if (audio_status && audio_status_result >= 0) {
		report_line(fd, "audio_protocol=%08X state=%u flags=%08X rate=%u channels=%u bits=%u",
		            audio_status->protocol_version, audio_status->state,
		            audio_status->flags, audio_status->sample_rate,
		            audio_status->channels, audio_status->bit_depth);
		report_line(fd, "audio_buffered=%u audio_buffer_capacity=%u audio_buffer_min=%u audio_buffer_max=%u",
		            audio_status->buffered_frames, audio_status->ring_capacity_frames,
		            audio_status->minimum_buffered_frames,
		            audio_status->maximum_buffered_frames);
		report_line(fd, "audio_produced=%u audio_consumed=%u packets_submitted=%u packets_completed=%u bytes=%u",
		            audio_status->produced_frames, audio_status->consumed_frames,
		            audio_status->packets_submitted, audio_status->packets_completed,
		            audio_status->bytes_transmitted);
		report_line(fd, "audio_packet_frames=%u packet_47=%u packet_48=%u packet_49=%u",
		            audio_status->last_packet_frames, audio_status->packets_47_frames,
		            audio_status->packets_48_frames, audio_status->packets_49_frames);
		report_line(fd, "audio_underruns=%u audio_overruns=%u submit_failures=%u completion_errors=%u disconnects=%u last_error=%08X",
		            audio_status->output_underruns, audio_status->output_overruns,
		            audio_status->submit_failures, audio_status->completion_errors,
		            audio_status->disconnects, audio_status->last_error);
		report_line(fd, "audio_conceal_events=%u audio_concealed_frames=%u audio_consecutive_missing_frames=%u",
		            audio_status->conceal_events,
		            audio_status->concealed_frames,
		            audio_status->consecutive_missing_frames);
		report_line(fd, "audio_endpoint_driver=%d submit_endpoint=%d endpoint=%d endpoint_bytes=%u request_attributes=%08X",
		            audio_status->endpoint_driver_number,
		            audio_status->submit_endpoint_number,
		            audio_status->endpoint_number,
		            audio_status->endpoint_transmitted_bytes,
		            audio_status->request_attributes);
		report_line(fd, "audio_set_interface=%u audio_change_setting=%u audio_fallback_starts=%u requested_alt=%d applied_alt=%d",
		            audio_status->set_interface_requests,
		            audio_status->change_setting_callbacks,
		            audio_status->fallback_starts,
		            audio_status->requested_alternate,
		            audio_status->applied_alternate);
		report_line(fd, "audio_stalled=%u audio_recovered=%u audio_cancel_failures=%u audio_pending_age_ms=%u",
		            audio_status->stalled_requests,
		            audio_status->recovered_requests,
		            audio_status->cancel_failures,
		            audio_status->pending_age_ms);
		report_line(fd, "audio_packet_peak=%u audio_first_lr=%d,%d audio_nonzero_packets=%u zero_byte_completions=%u",
		            audio_status->last_packet_peak,
		            audio_status->last_packet_first_left,
		            audio_status->last_packet_first_right,
		            audio_status->nonzero_packets,
		            audio_status->zero_byte_completions);
		report_line(fd, "audio_last_completion_result=%08X audio_last_completion_requested=%u audio_last_completion_bytes=%u",
		            audio_status->last_completion_result,
		            audio_status->last_completion_requested_bytes,
		            audio_status->last_completion_bytes);
		report_line(fd, "audio_short_completions=%u audio_late_completions=%u audio_completion_gap_us=%u audio_completion_gap_max_us=%u",
		            audio_status->short_completions,
		            audio_status->late_completions,
		            audio_status->last_completion_gap_us,
		            audio_status->maximum_completion_gap_us);
		report_line(fd, "audio_rearm_delay_us=%u audio_rearm_delay_max_us=%u audio_worker_lock_wait_us=%u audio_worker_lock_wait_max_us=%u",
		            audio_status->last_rearm_delay_us,
		            audio_status->maximum_rearm_delay_us,
		            audio_status->last_worker_lock_wait_us,
		            audio_status->maximum_worker_lock_wait_us);
		report_line(fd, "audio_channel_mask=%03X expected=%03X",
		            audio_status->last_packet_channel_nonzero_mask,
		            (1u << PSVITA_USB_AUDIO_STREAM_CHANNELS) - 1u);
		report_line(fd, "audio_channel_peaks=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
		            audio_status->last_packet_channel_peaks[0],
		            audio_status->last_packet_channel_peaks[1],
		            audio_status->last_packet_channel_peaks[2],
		            audio_status->last_packet_channel_peaks[3],
		            audio_status->last_packet_channel_peaks[4],
		            audio_status->last_packet_channel_peaks[5],
		            audio_status->last_packet_channel_peaks[6],
		            audio_status->last_packet_channel_peaks[7],
		            audio_status->last_packet_channel_peaks[8],
		            audio_status->last_packet_channel_peaks[9]);
		report_line(fd, "audio_stream_primed=%u priming_packets=%u rebuffer_events=%u",
		            audio_status->stream_primed,
		            audio_status->priming_packets,
		            audio_status->rebuffer_events);
	}
	if (audio_log) report_audio_diagnostic_log(fd, audio_log, audio_probe);
	sceIoClose(fd);
	return 0;
}

static int refresh_diagnostics(PsvitaUsbAudioMidiStatus *status,
	PsvitaUsbAudioMidiDiagnostics *diagnostics, int *diagnostics_result,
	PsvitaUsbAudioStatus *audio_status, int *audio_status_result)
{
	memset(status, 0, sizeof(*status));
	status->size = sizeof(*status);
	memset(diagnostics, 0, sizeof(*diagnostics));
	diagnostics->size = sizeof(*diagnostics);
	memset(audio_status, 0, sizeof(*audio_status));
	audio_status->size = sizeof(*audio_status);
	int status_result = psvitaUsbAudioMidiGetStatus(status);
	*diagnostics_result = psvitaUsbAudioMidiGetDiagnostics(diagnostics);
	*audio_status_result = psvitaUsbAudioGetStatus(audio_status);
	return status_result;
}

static void record_audio_diagnostics(uint32_t now_ms,
	const PsvitaUsbAudioStatus *audio_status, int audio_status_result,
	AudioProbe *audio_probe)
{
	if (audio_status_result < 0) return;
	psvita_usb_audio_diag_record(&audio_diagnostic_log, now_ms, audio_status);
	psvita_usb_audio_diag_annotate_producer(&audio_diagnostic_log,
		(uint32_t)SDL_AtomicGet(&audio_probe->write_calls),
		(uint32_t)SDL_AtomicGet(&audio_probe->write_failures),
		(uint32_t)SDL_AtomicGet(&audio_probe->status_failures),
		(uint32_t)SDL_AtomicGet(&audio_probe->late_wakes),
		(uint32_t)SDL_AtomicGet(&audio_probe->pacing_resets),
		(uint32_t)SDL_AtomicGet(&audio_probe->maximum_lateness_us),
		(uint32_t)SDL_AtomicGet(&audio_probe->maximum_write_us));
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	int module_status = 0;
	SceUID bridge = sceKernelLoadStartModule("app0:psvita_usb_audio_midi_client.suprx",
		0, NULL, 0, NULL, &module_status);
	int load_result = bridge < 0 ? (int)bridge : module_status;
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER);
	SDL_Window *window = SDL_CreateWindow("USB Audio + MIDI Scope", 0, 0, 960, 544, 0);
	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	scope_font_texture = create_font_texture(renderer);
	uint32_t clocks = 0, starts = 0, continues = 0, stops = 0;
	int acquire_result = load_result;
	int release_result = 0;
	int diagnostics_result = load_result;
	int audio_status_result = load_result;
	int report_result = 0;
	int report_pending = 0;
	int lease_owned = 0;
	PsvitaUsbAudioMidiStatus status;
	PsvitaUsbAudioMidiDiagnostics diagnostics;
	PsvitaUsbAudioStatus audio_status;
	AudioProbe audio_probe;
	memset(&status, 0, sizeof(status));
	memset(&diagnostics, 0, sizeof(diagnostics));
	memset(&audio_status, 0, sizeof(audio_status));
	memset(&audio_probe, 0, sizeof(audio_probe));
	psvita_usb_audio_diag_init(&audio_diagnostic_log,
		(uint32_t)SDL_GetTicks64());
	audio_probe.start_result = load_result;
	audio_probe.enable_result = load_result;
	if (load_result >= 0) {
		acquire_result = psvitaUsbAudioMidiAcquire(PSVITA_USB_AUDIO_MIDI_ACQUIRE_REPLACE_MTP);
		lease_owned = acquire_result >= 0;
		if (lease_owned) audio_probe_start(&audio_probe);
		refresh_diagnostics(&status, &diagnostics, &diagnostics_result,
		                    &audio_status, &audio_status_result);
		record_audio_diagnostics((uint32_t)SDL_GetTicks64(), &audio_status,
			audio_status_result, &audio_probe);
	}
	uint64_t controls_enabled_at = SDL_GetTicks64() + 750u;
	uint64_t diagnostics_refresh_at = 0;
	SceCtrlData pad;
	memset(&pad, 0, sizeof(pad));
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	sceCtrlPeekBufferPositive(0, &pad, 1);
	uint32_t previous_buttons = pad.buttons;
	int running = 1;
	while (running) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT)
				running = 0;
		}
		if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
			uint32_t pressed = pad.buttons & ~previous_buttons;
			previous_buttons = pad.buttons;
			if (SDL_GetTicks64() >= controls_enabled_at) {
				if (pressed & SCE_CTRL_START) {
					running = 0;
				} else if ((pressed & SCE_CTRL_CROSS) && load_result >= 0 &&
				           !lease_owned) {
					psvita_usb_audio_diag_init(&audio_diagnostic_log,
						(uint32_t)SDL_GetTicks64());
					audio_probe_reset_diagnostics(&audio_probe);
					acquire_result = psvitaUsbAudioMidiAcquire(
						PSVITA_USB_AUDIO_MIDI_ACQUIRE_REPLACE_MTP);
					lease_owned = acquire_result >= 0;
					if (lease_owned) audio_probe_start(&audio_probe);
					refresh_diagnostics(&status, &diagnostics, &diagnostics_result,
					                    &audio_status, &audio_status_result);
					record_audio_diagnostics((uint32_t)SDL_GetTicks64(),
						&audio_status, audio_status_result, &audio_probe);
					report_pending = 0;
				} else if ((pressed & SCE_CTRL_CIRCLE) && load_result >= 0 &&
				           lease_owned) {
					refresh_diagnostics(&status, &diagnostics, &diagnostics_result,
					                    &audio_status, &audio_status_result);
					record_audio_diagnostics((uint32_t)SDL_GetTicks64(),
						&audio_status, audio_status_result, &audio_probe);
					audio_probe_stop(&audio_probe);
					release_result = psvitaUsbAudioMidiRelease();
					lease_owned = 0;
					refresh_diagnostics(&status, &diagnostics, &diagnostics_result,
					                    &audio_status, &audio_status_result);
					record_audio_diagnostics((uint32_t)SDL_GetTicks64(),
						&audio_status, audio_status_result, &audio_probe);
					report_result = save_report(load_result, acquire_result, release_result,
					                           &status, &diagnostics, diagnostics_result,
					                           &audio_status, audio_status_result,
					                           &audio_probe, &audio_diagnostic_log);
					report_pending = 0;
				} else if (pressed & SCE_CTRL_TRIANGLE) {
					psvita_usb_audio_diag_mark(&audio_diagnostic_log,
						(uint32_t)SDL_GetTicks64());
					report_pending = 1;
				}
			}
		}
		PsvitaUsbMidiEvent events[PSVITA_USB_MIDI_MAX_EVENTS];
		int count = !lease_owned ? 0 :
			psvitaUsbMidiRead(events, PSVITA_USB_MIDI_MAX_EVENTS);
		for (int i = 0; i < count; ++i) {
			switch (events[i].data[0]) {
			case 0xf8: clocks++; break;
			case 0xfa: starts++; break;
			case 0xfb: continues++; break;
			case 0xfc: stops++; break;
			}
		}
		uint64_t now_ms = SDL_GetTicks64();
		if (load_result >= 0 && now_ms >= diagnostics_refresh_at) {
			refresh_diagnostics(&status, &diagnostics, &diagnostics_result,
			                    &audio_status, &audio_status_result);
			record_audio_diagnostics((uint32_t)now_ms, &audio_status,
				audio_status_result, &audio_probe);
			diagnostics_refresh_at = now_ms + 50u;
		}
		char line[128];
		SDL_SetRenderDrawColor(renderer, 12, 16, 22, 255);
		SDL_RenderClear(renderer);
		int y = 20;
		snprintf(line, sizeof(line), "USB AUDIO MIDI SCOPE BUILD %08X API %08X",
			diagnostics.build_id, PSVITA_USB_AUDIO_MIDI_API_VERSION);
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "LOAD %08X DIAG %08X MARK %u EVT %u REPORT %08X",
			load_result, diagnostics_result, audio_diagnostic_log.manual_marks,
			audio_diagnostic_log.event_count, report_result);
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "ACQUIRE %08X %s RELEASE %08X STATE %u",
			acquire_result, error_name(acquire_result), release_result, status.state);
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "CTRL %08X MTP %08X PSP %08X SER %08X",
			diagnostics.controller_state, diagnostics.mtp_state,
			diagnostics.psp_comm_state, diagnostics.serial_state);
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "DEVICE %08X FLAGS %08X F8 %u FA %u FB %u FC %u",
			diagnostics.device_state, status.flags, clocks, starts, continues, stops);
		draw_text(renderer, 18, y, line); y += 28;
		snprintf(line, sizeof(line), "LAT RXK P95 %.2fMS TXJ P95 %.2fMS RX %u TX %u",
			(float)histogram_p95_us(diagnostics.rx_latency_histogram) / 1000.0f,
			(float)histogram_p95_us(diagnostics.tx_jitter_histogram) / 1000.0f,
			diagnostics.rx_completed, diagnostics.tx_scheduled);
		draw_text(renderer, 18, y, line); y += 28;
		snprintf(line, sizeof(line), "FILTERED %u RX DROP %u TX DROP %u",
			status.filtered, status.rx_dropped, status.tx_dropped);
		draw_text(renderer, 18, y, line); y += 28;
		snprintf(line, sizeof(line), "USB CONFIG %s  RX ARMED %s",
			(status.flags & PSVITA_USB_AUDIO_MIDI_STATUS_CONFIGURED) ? "YES" : "NO",
			(status.flags & PSVITA_USB_AUDIO_MIDI_STATUS_RX_ARMED) ? "YES" : "NO");
		draw_text(renderer, 18, y, line); y += 28;
		snprintf(line, sizeof(line), "RX SUB %u FAIL %u CB %u BYTES %u",
			diagnostics.rx_submit_attempts, diagnostics.rx_submit_failures,
			diagnostics.rx_callback_count, diagnostics.rx_callback_bytes);
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "RX LAST SUB %08X CB %08X",
			diagnostics.rx_last_submit_result,
			diagnostics.rx_last_callback_result);
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "RX REJECT %u MALFORMED %u  TX REJECT %u",
			diagnostics.rx_filtered_packets, diagnostics.rx_malformed_packets,
			diagnostics.tx_filtered_events);
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "AUDIO %s RUN %s 10CH TEST TONES FL %08X",
			audio_state_name(audio_status.state),
			SDL_AtomicGet(&audio_probe.running) ? "YES" : "NO",
			audio_status.flags);
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "AUDIO EP D %d S %d N %d ALT %d/%d PR %u RB %u BUF %u",
			audio_status.endpoint_driver_number,
			audio_status.submit_endpoint_number,
			audio_status.endpoint_number,
			audio_status.requested_alternate,
			audio_status.applied_alternate,
			audio_status.stream_primed,
			audio_status.rebuffer_events,
			audio_status.buffered_frames);
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "AUDIO Q %u/%u S/C %u/%u HD %u/%u",
			audio_status.packets_submitted, audio_status.packets_completed,
			audio_status.submit_failures, audio_status.completion_errors,
			audio_status.conceal_events, audio_status.concealed_frames);
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "AUDIO REQ %u/%u R %u K %u CH %03X F %04X",
			audio_status.last_completion_requested_bytes,
			audio_status.last_completion_bytes,
			audio_status.maximum_rearm_delay_us,
			audio_status.maximum_worker_lock_wait_us,
			audio_status.last_packet_channel_nonzero_mask,
		psvita_usb_audio_diag_failure_flags(&audio_diagnostic_log));
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "PEAK M %u %u T1-4 %u %u %u %u",
			audio_status.last_packet_channel_peaks[0],
			audio_status.last_packet_channel_peaks[1],
			audio_status.last_packet_channel_peaks[2],
			audio_status.last_packet_channel_peaks[3],
			audio_status.last_packet_channel_peaks[4],
			audio_status.last_packet_channel_peaks[5]);
		draw_text(renderer, 18, y, line); y += 24;
		snprintf(line, sizeof(line), "PEAK T5-8 %u %u %u %u",
			audio_status.last_packet_channel_peaks[6],
			audio_status.last_packet_channel_peaks[7],
			audio_status.last_packet_channel_peaks[8],
			audio_status.last_packet_channel_peaks[9]);
		draw_text(renderer, 18, y, line); y += 24;
		const char *verdict;
		if (!(status.flags & PSVITA_USB_AUDIO_MIDI_STATUS_CONFIGURED))
			verdict = "WAITING FOR USB CONFIGURATION";
		else if (audio_status.cancel_failures > 0u)
			verdict = "AUDIO STALL CANCEL FAILED; MARK THEN RELEASE";
		else if (audio_status.stalled_requests > audio_status.recovered_requests)
			verdict = "AUDIO REQUEST STALLED; RECOVERY PENDING";
		else if ((audio_status.flags & PSVITA_USB_AUDIO_STATUS_HOST_STREAMING) &&
		         audio_status.packets_submitted > 0u &&
		         audio_status.packets_completed == 0u)
			verdict = "HOST HAS NOT COMPLETED AUDIO IN TRANSFER";
		else if (audio_status.submit_failures > 0u ||
		         audio_status.completion_errors > 0u)
			verdict = "AUDIO USB TRANSFER ERROR; MARK THEN RELEASE";
		else if (SDL_AtomicGet(&audio_probe.write_failures) > 0)
			verdict = "AUDIO PRODUCER WRITE FAILED; MARK THEN RELEASE";
		else if (audio_status.short_completions > 0u)
			verdict = "AUDIO SHORT USB TRANSFER; MARK THEN RELEASE";
		else if (audio_status.late_completions > 0u)
			verdict = "AUDIO USB REARM LATE; MARK THEN RELEASE";
		else if (audio_status.output_overruns > 0u)
			verdict = "AUDIO KERNEL RING OVERRUN; MARK THEN RELEASE";
		else if ((audio_status.flags & PSVITA_USB_AUDIO_STATUS_HOST_STREAMING) &&
		         !audio_status.stream_primed)
			verdict = "AUDIO BUFFER PRIMING";
		else if (audio_status.rebuffer_events > 0u)
			verdict = "AUDIO REBUFFER EVENT MARK THEN RELEASE";
		else if (audio_status.packets_completed > 0u &&
		         audio_status.last_packet_channel_nonzero_mask !=
		             ((1u << PSVITA_USB_AUDIO_STREAM_CHANNELS) - 1u))
			verdict = "MISSING CHANNELS BEFORE USB ENDPOINT";
		else if (audio_status.packets_completed > 0u)
			verdict = "10CH PAYLOAD COMPLETED USB IN";
		else if (diagnostics.rx_submit_attempts == 0u)
			verdict = "PROBE ERROR: RECEIVE WAS NOT SUBMITTED";
		else if (diagnostics.rx_callback_count == 0u)
			verdict = "HOST HAS NOT COMPLETED AN OUT TRANSFER";
		else if (diagnostics.rx_completed > 0u)
			verdict = diagnostics.rx_malformed_packets > 0u
				? "MIDI RECEIVED VIA SHORT-TRANSFER FALLBACK"
				: "USB-MIDI EVENTS RECEIVED";
		else if (diagnostics.rx_malformed_packets > 0u)
			verdict = "RX ARRIVED WITH NON USB-MIDI LENGTH";
		else if (diagnostics.rx_filtered_packets > 0u)
			verdict = "RX ARRIVED BUT USB-MIDI FRAMING WAS REJECTED";
		else
			verdict = "RX TRANSFER COMPLETED; WAITING FOR MIDI DATA";
		snprintf(line, sizeof(line), "VERDICT: %s", verdict);
		draw_text(renderer, 18, y, line); y += 24;
		if (diagnostics.entry_count > 0u) {
			const PsvitaUsbAudioMidiTraceEntry *entry = &diagnostics.entries[0];
			snprintf(line, sizeof(line), "TRACE 00 %s %s %08X %s",
				phase_name(entry->phase), operation_name(entry->operation),
				entry->result, error_name(entry->result));
			draw_text(renderer, 18, y, line);
		}
		draw_text(renderer, 18, 500,
			"CROSS RETRY CIRCLE RELEASE SAVE TRIANGLE MARK START EXIT");
		draw_text(renderer, 18, 522, "REPORT UX0 DATA PSVITA USB AUDIO MIDI DIAGNOSTIC TXT");
		SDL_RenderPresent(renderer);
		SDL_Delay(20);
	}
	if (lease_owned) {
		refresh_diagnostics(&status, &diagnostics, &diagnostics_result,
		                    &audio_status, &audio_status_result);
		record_audio_diagnostics((uint32_t)SDL_GetTicks64(), &audio_status,
			audio_status_result, &audio_probe);
		audio_probe_stop(&audio_probe);
		release_result = psvitaUsbAudioMidiRelease();
		refresh_diagnostics(&status, &diagnostics, &diagnostics_result,
		                    &audio_status, &audio_status_result);
		record_audio_diagnostics((uint32_t)SDL_GetTicks64(), &audio_status,
			audio_status_result, &audio_probe);
		report_result = save_report(load_result, acquire_result, release_result,
		            &status, &diagnostics, diagnostics_result,
		            &audio_status, audio_status_result, &audio_probe,
		            &audio_diagnostic_log);
		report_pending = 0;
	} else if (report_pending) {
		report_result = save_report(load_result, acquire_result, release_result,
		            &status, &diagnostics, diagnostics_result,
		            &audio_status, audio_status_result, &audio_probe,
		            &audio_diagnostic_log);
	}
	if (scope_font_texture) SDL_DestroyTexture(scope_font_texture);
	scope_font_texture = NULL;
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
