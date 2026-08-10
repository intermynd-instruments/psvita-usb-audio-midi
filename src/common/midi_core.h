#ifndef PSVITA_USB_MIDI_CORE_H
#define PSVITA_USB_MIDI_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "psvita_usb_audio_midi.h"

#define PSVITA_USB_MIDI_RING_CAPACITY 256u

typedef struct {
	PsvitaUsbMidiEvent events[PSVITA_USB_MIDI_RING_CAPACITY];
	uint32_t read_index;
	uint32_t write_index;
	uint32_t count;
} PsvitaUsbMidiRing;

typedef struct {
	uint32_t accepted;
	uint32_t filtered;
	uint32_t malformed;
} PsvitaUsbMidiParseResult;

void psvita_usb_midi_ring_init(PsvitaUsbMidiRing *ring);
int psvita_usb_midi_ring_push(PsvitaUsbMidiRing *ring,
					 const PsvitaUsbMidiEvent *event);
int psvita_usb_midi_tx_ring_push(PsvitaUsbMidiRing *ring,
					 const PsvitaUsbMidiEvent *event);
uint32_t psvita_usb_midi_ring_pop(PsvitaUsbMidiRing *ring,
					 PsvitaUsbMidiEvent *events, uint32_t capacity);

int psvita_usb_midi_event_supported(const PsvitaUsbMidiEvent *event);
PsvitaUsbMidiParseResult psvita_usb_midi_parse_usb_packets(
	const uint8_t *bytes, size_t length, uint64_t timestamp_us,
	PsvitaUsbMidiRing *destination, uint32_t *dropped);
int psvita_usb_midi_encode_event(const PsvitaUsbMidiEvent *event,
	uint8_t packet[4]);

#endif
