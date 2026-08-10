# USB Audio

## Format

The plugin adds USB Audio Class 1.0 capture and playback to the same USB
configuration as MIDI.

Vita to host:

- interface 1, alternate 1
- asynchronous isochronous IN endpoint `0x83`
- 48 kHz, signed 16-bit little-endian PCM
- ten channels: master L/R, then Track 1-8
- 960-byte maximum packet: 48 frames × 10 channels × 2 bytes

Host to Vita:

- interface 2, alternate 1
- adaptive isochronous OUT endpoint `0x04`
- 48 kHz stereo, signed 16-bit little-endian PCM
- 196-byte maximum packet: up to 49 stereo frames

Alternate 0 disables the corresponding stream. MIDI remains on interface 3 and
keeps endpoint addresses `0x01` and `0x82`.

There are no volume, mute, pitch, or selectable sample-rate controls. An
application must resample or convert its audio before calling the plugin.

## Application contract

1. Load `psvita_usb_audio_midi_client.suprx` and check
   `PSVITA_USB_AUDIO_MIDI_API_VERSION`.
2. Acquire `PSVITA_USB_AUDIO_MIDI_ACQUIRE_REPLACE_MTP`.
3. Enable the direction or directions the application needs with
   `psvitaUsbAudioSetOutputEnabled(1)` and/or
   `psvitaUsbAudioSetInputEnabled(1)`.
4. If output is enabled, feed Vita output with
   `psvitaUsbAudioWriteMulti(pcm, frames, 48000, 10)`.
5. If input is enabled, read host playback with `psvitaUsbAudioInputRead()`.
6. Disable the enabled directions or release the shared MIDI/audio lease.

Writes accept 1–512 frames. `psvitaUsbAudioWrite()` remains available for
stereo callers; it writes master L/R and clears the eight track channels.
`psvitaUsbAudioInputRead()` returns up to 512 complete stereo frames and does
not pad the return count with silence.

`psvitaUsbAudioSetEnabled()` remains available for compatibility and toggles
both directions together. Output and input status are otherwise independent;
disabling one direction does not reset or stop the other.

The syscalls copy user memory into bounded kernel buffers. They briefly lock
plugin state, so a real-time audio callback should normally publish to an
application-owned queue and let a regular worker call the plugin.

## Transport

Vita-to-host audio uses an 8,192-frame, drop-oldest ring. The endpoint starts
consuming live samples after 6,144 frames are buffered, leaving 2,048 frames of
write headroom. A normal SceUdcd request contains eight 960-byte service
intervals, giving an 8 ms completion cadence. When a primed ring has fewer than
384 frames left, the worker sends the remaining whole 48-frame packets as one
shorter request.

A short producer gap holds the last complete frame. After 384 consecutive
missing frames the stream returns to priming. A failed request submission rolls
back the ring read and packet-clock state so a retry sends the same audio.

Only one isochronous IN request is in flight. Hardware testing showed that
queueing several requests ahead can stall SceUdcd. The request buffer is an
8 KiB physical-contiguous, non-cacheable kernel memblock and is submitted with
`SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT`.

Host-to-Vita audio has its own physical receive buffer and 8,192-frame
drop-oldest ring. The completion callback rejects partial frames and transfers
larger than the advertised packet size before invalidating cache or touching the
ring.

SET_INTERFACE is handled in two phases. The control request records the desired
alternate setting, while SceUdcd's `changeSetting` callback enables the
endpoint. A delayed fallback covers firmware that omits that callback. Alternate
0, disconnect, audio disable, lease release, and driver stop cancel requests
and discard buffered input.

`PsvitaUsbAudioStatus` and `PsvitaUsbAudioInputStatus` expose endpoint state,
buffer occupancy, packet counts, transfer errors, disconnects, and timing
diagnostics. The input record includes completion-gap and request-rearm maxima
so an app can distinguish irregular host packets from delayed kernel servicing.
Both records are append-only: callers set the leading `size` field and the
kernel copies no more than that size.

## References

- [USB Audio Device Class 1.0](https://www.usb.org/sites/default/files/audio10.pdf)
- [USB Audio Data Formats 1.0](https://www.usb.org/sites/default/files/frmts10.pdf)
- [VitaSDK SceUdcd header](https://github.com/vitasdk/vita-headers/blob/master/include/psp2kern/udcd.h)
- [Apple USB audio device design considerations](https://developer.apple.com/documentation/technotes/tn3190-usb-audio-device-design-considerations)

Enumeration, alternate-setting behavior, simultaneous MIDI/audio traffic, and
long-duration clock drift still need hardware coverage on each supported Vita
and host platform. Use the [hardware checklist](HARDWARE_TEST.md) before
publishing a stable binary.
