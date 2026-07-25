#ifndef PSVITA_USB_MIDI_TIMING_H
#define PSVITA_USB_MIDI_TIMING_H

#include "psvita_usb_audio_midi.h"

#include <stdint.h>

uint32_t psvita_usb_midi_deadline_wait_us(uint64_t target_us, uint64_t now_us,
	uint32_t maximum_us);
uint32_t psvita_usb_midi_latency_bucket(uint32_t latency_us);
uint32_t psvita_usb_midi_histogram_percentile(
	const uint32_t histogram[PSVITA_USB_MIDI_LATENCY_BUCKETS], uint32_t percentile);

#endif
