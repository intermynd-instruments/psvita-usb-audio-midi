# API and wire contract

The public API is declared in `include/psvita_usb_audio_midi.h`. Applications
should load the client bridge and compare
`psvitaUsbAudioMidiGetApiVersion()` with
`PSVITA_USB_AUDIO_MIDI_API_VERSION` before acquiring USB.

## MIDI

`PsvitaUsbMidiEvent` is a fixed 16-byte record containing a completion or
scheduled timestamp, cable number, byte count, and up to three MIDI bytes.
Public read and write calls accept at most 64 events.

Only these one-byte system real-time messages are supported:

| Message | Status |
| --- | --- |
| Timing Clock | `F8` |
| Start | `FA` |
| Continue | `FB` |
| Stop | `FC` |

Transmit uses canonical USB-MIDI 1.0 Event Packets: cable zero, CIN `0xF`, and
zero padding. Receive normalizes supported real-time messages to cable zero.
For compatibility with small embedded hosts, a transfer shorter than one
four-byte Event Packet may contain bare supported status bytes. Nonzero cable
nibbles and stale CIN/padding are also ignored when the first MIDI byte is one
of the four supported statuses.

Notes, CC, SysEx, Song Position Pointer, and every other message are filtered.
Malformed, filtered, and dropped traffic is counted in status and diagnostics.

Receive timestamps are captured in the USB completion callback before the
kernel worker parses the transfer. `psvitaUsbMidiReadWait()` blocks for up to one
second instead of requiring an application poll loop.

Transmit timestamps are absolute Vita system times. The worker waits until each
event's deadline and never sends a stale queue after cable detach.

## Audio

Audio shares the same lease and USB configuration as MIDI. The current audio
protocol is version `0x00010010`.

- Vita to host: 48 kHz, ten-channel S16, master L/R followed by Track 1-8
- host to Vita: 48 kHz stereo S16
- maximum user write/read: 512 frames
- kernel rings: 8,192 frames in each direction

`psvitaUsbAudioWriteMulti()` requires exactly ten channels. The stereo
`psvitaUsbAudioWrite()` call is kept for source compatibility and zero-fills the
track channels.

The audio status records are append-only. A caller sets the leading `size`
field; the kernel copies at most that many bytes and returns the current full
size in the copied record. This lets an older client read the prefix it knows
without overflowing its allocation.

See [AUDIO.md](AUDIO.md) for endpoint and buffering details.

## Ownership and restoration

The kernel module registers `VITAAMID` at boot but leaves the active USB
personality alone. `psvitaUsbAudioMidiAcquire()` accepts only
`PSVITA_USB_AUDIO_MIDI_ACQUIRE_REPLACE_MTP` and binds the lease to the calling
process. Another process receives `PSVITA_USB_AUDIO_MIDI_ERROR_BUSY`.

Before takeover, the plugin:

1. rejects known conflicting UDCD drivers;
2. records the MTP, PSP communication, serial, controller, and device states;
3. accepts only the known stock MTP state;
4. deactivates USB when necessary;
5. stops the current stock personality as one unit;
6. starts the custom driver and activates its PID.

Stopping the current personality is deliberately broad. Retail firmware does
not reliably allow the stock child drivers to be stopped one at a time. The
captured snapshot records which stock drivers must be restarted.

Release deactivates and stops the custom driver, restarts the stock controller
and captured child drivers, and reactivates MTP only when it was active before
takeover. If restoration fails, the owner PID and snapshot are retained so a
later release or the dead-owner watchdog can retry.

Explicit release, client module stop, application exit, forced kill, and the
owner watchdog all use the same restoration path.

## Syscall boundary

Kernel exports validate counts and formats before copying user memory. Audio
writes use one aligned scratch buffer protected by a writer mutex, then commit
to the USB ring while holding the shorter state lock. No user pointer is retained
after a syscall returns.

Status and diagnostics are copied back to user memory. The diagnostic record is
size-versioned and includes:

- API/build identity and raw SceUdcd state
- acquire, rollback, and restore operations with raw return values
- MIDI receive submission/completion counters
- receive latency and scheduled-transmit jitter histograms
- filtering and malformed-transfer counters

The trace is bounded and read-only. It is intended for hardware diagnosis, not
as a logging channel from USB callbacks.
