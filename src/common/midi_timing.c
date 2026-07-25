#include "midi_timing.h"

#include <limits.h>

static const uint32_t latency_limits_us[PSVITA_USB_MIDI_LATENCY_BUCKETS] = {
	125u, 250u, 500u, 1000u, 2000u, 5000u, 10000u, UINT32_MAX
};

uint32_t psvita_usb_midi_deadline_wait_us(uint64_t target_us, uint64_t now_us,
	uint32_t maximum_us)
{
	if (!target_us || target_us <= now_us) return 0;
	uint64_t wait = target_us - now_us;
	if (wait > maximum_us) wait = maximum_us;
	return (uint32_t)wait;
}

uint32_t psvita_usb_midi_latency_bucket(uint32_t latency_us)
{
	for (uint32_t i = 0; i < PSVITA_USB_MIDI_LATENCY_BUCKETS - 1u; ++i) {
		if (latency_us < latency_limits_us[i]) return i;
	}
	return PSVITA_USB_MIDI_LATENCY_BUCKETS - 1u;
}

uint32_t psvita_usb_midi_histogram_percentile(
	const uint32_t histogram[PSVITA_USB_MIDI_LATENCY_BUCKETS], uint32_t percentile)
{
	uint64_t total = 0;
	uint64_t cumulative = 0;
	if (!histogram) return 0;
	if (percentile < 1u) percentile = 1u;
	if (percentile > 100u) percentile = 100u;
	for (uint32_t i = 0; i < PSVITA_USB_MIDI_LATENCY_BUCKETS; ++i)
		total += histogram[i];
	if (!total) return 0;
	uint64_t target = (total * percentile + 99u) / 100u;
	for (uint32_t i = 0; i < PSVITA_USB_MIDI_LATENCY_BUCKETS; ++i) {
		cumulative += histogram[i];
		if (cumulative >= target) return latency_limits_us[i];
	}
	return latency_limits_us[PSVITA_USB_MIDI_LATENCY_BUCKETS - 1u];
}
