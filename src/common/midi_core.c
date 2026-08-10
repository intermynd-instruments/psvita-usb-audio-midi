#include "midi_core.h"

#include <string.h>

void psvita_usb_midi_ring_init(PsvitaUsbMidiRing *ring)
{
	if (ring) memset(ring, 0, sizeof(*ring));
}

int psvita_usb_midi_ring_push(PsvitaUsbMidiRing *ring,
					 const PsvitaUsbMidiEvent *event)
{
	if (!ring || !event || ring->count >= PSVITA_USB_MIDI_RING_CAPACITY)
		return 0;
	ring->events[ring->write_index] = *event;
	ring->write_index = (ring->write_index + 1u) % PSVITA_USB_MIDI_RING_CAPACITY;
	ring->count++;
	return 1;
}

int psvita_usb_midi_tx_ring_push(PsvitaUsbMidiRing *ring,
	const PsvitaUsbMidiEvent *event)
{
	if (!ring || !event) return 0;
	/* START, CONTINUE, and STOP supersede clock look-ahead from the previous
	 * transport state. Otherwise those future events hold the transport edge
	 * behind them and are emitted as a burst when their deadlines expire. */
	if (event->size == 1u &&
	    (event->data[0] == 0xfau || event->data[0] == 0xfbu ||
	     event->data[0] == 0xfcu))
		psvita_usb_midi_ring_init(ring);
	return psvita_usb_midi_ring_push(ring, event);
}

uint32_t psvita_usb_midi_ring_pop(PsvitaUsbMidiRing *ring,
					 PsvitaUsbMidiEvent *events, uint32_t capacity)
{
	uint32_t count = 0;
	if (!ring || (!events && capacity)) return 0;
	while (count < capacity && ring->count) {
		events[count++] = ring->events[ring->read_index];
		ring->read_index = (ring->read_index + 1u) % PSVITA_USB_MIDI_RING_CAPACITY;
		ring->count--;
	}
	return count;
}

static int realtime_status_supported(uint8_t status)
{
	return status == 0xf8u || status == 0xfau ||
	       status == 0xfbu || status == 0xfcu;
}

int psvita_usb_midi_event_supported(const PsvitaUsbMidiEvent *event)
{
	if (!event || event->cable != 0 || event->size != 1) return 0;
	return realtime_status_supported(event->data[0]);
}

PsvitaUsbMidiParseResult psvita_usb_midi_parse_usb_packets(
	const uint8_t *bytes, size_t length, uint64_t timestamp_us,
	PsvitaUsbMidiRing *destination, uint32_t *dropped)
{
	PsvitaUsbMidiParseResult result = {0, 0, 0};
	if (!bytes || !destination) {
		result.malformed = 1;
		return result;
	}
	/*
	 * USB-MIDI 1.0 requires four-byte Event Packets, but a small embedded host
	 * may forward a short system-real-time transfer as bare MIDI bytes. Keep
	 * this compatibility path limited to transfers too short to contain even
	 * one valid Event Packet, and only accept the four real-time statuses DS-8
	 * supports. The malformed count preserves evidence that the host did this.
	 */
	if (length > 0u && length < 4u) {
		result.malformed = 1;
		for (size_t offset = 0; offset < length; ++offset) {
			PsvitaUsbMidiEvent event;
			memset(&event, 0, sizeof(event));
			event.timestamp_us = timestamp_us;
			event.size = 1;
			event.data[0] = bytes[offset];
			if (!realtime_status_supported(event.data[0])) {
				result.filtered++;
				continue;
			}
			if (!psvita_usb_midi_ring_push(destination, &event)) {
				if (dropped) (*dropped)++;
				continue;
			}
			result.accepted++;
		}
		return result;
	}
	if ((length & 3u) != 0) result.malformed++;
	for (size_t offset = 0; offset + 3u < length; offset += 4u) {
		uint8_t header = bytes[offset];
		uint8_t cable = header >> 4;
		uint8_t cin = header & 0x0fu;
		PsvitaUsbMidiEvent event;
		memset(&event, 0, sizeof(event));
		event.timestamp_us = timestamp_us;
		/*
		 * System real-time messages are global, not channel/cable scoped. Some
		 * embedded USB hosts have been observed to preserve a non-zero virtual
		 * cable nibble or stale CIN/padding bytes even though byte 1 contains a
		 * valid one-byte real-time message. Accept that harmless framing variance
		 * and normalize it to DS-8's single cable.
		 */
		event.cable = 0;
		event.size = 1;
		event.data[0] = bytes[offset + 1u];
		if (!realtime_status_supported(event.data[0])) {
			result.filtered++;
			continue;
		}
		(void)cable;
		(void)cin;
		if (!psvita_usb_midi_ring_push(destination, &event)) {
			if (dropped) (*dropped)++;
			continue;
		}
		result.accepted++;
	}
	return result;
}

int psvita_usb_midi_encode_event(const PsvitaUsbMidiEvent *event,
	uint8_t packet[4])
{
	if (!packet || !psvita_usb_midi_event_supported(event)) return 0;
	packet[0] = 0x0fu;
	packet[1] = event->data[0];
	packet[2] = 0;
	packet[3] = 0;
	return 1;
}
