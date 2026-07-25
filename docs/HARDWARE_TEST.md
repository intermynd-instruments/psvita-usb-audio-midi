# Hardware release checklist

A host build and Vita cross-build do not prove that takeover, restoration, or
isochronous scheduling is safe on hardware. Repeat this checklist for each
release candidate. Do not call a binary stable until the lifecycle tests pass
on Vita 1000 and 2000. PSTV remains provisional.

## Quick diagnostic run

1. Build the project and install `psvita_usb_audio_midi.skprx` and
   `audio-midi-scope.vpk` from the same build.
2. Disable VitaShell USB mode and every other UDCD takeover plugin. Disconnect
   the USB cable, launch **USB Audio + MIDI Scope**, and let it acquire the
   lease. **Cross** retries a failed acquisition.
3. Connect directly to the host. Confirm that **PS Vita USB** appears with MIDI,
   a ten-channel recording stream, and a stereo playback stream.
4. Open the recording stream as 48 kHz, ten-channel S16. The Scope should move
   from connected to streaming and its submitted, completed, and byte counters
   should advance continuously.
5. Confirm the last-packet channel mask is `0x3ff` and all ten channel peaks are
   nonzero.
6. Record at least ten seconds. The expected probe tones are:

   | Input | Tone |
   | --- | ---: |
   | Master L | 440 Hz |
   | Master R | 880 Hz |
   | Track 1-8 | 125, 250, 375, 500, 625, 750, 875, 1000 Hz |

7. Send isolated left and right tones to the stereo playback stream. Confirm
   channel identity through the Vita speakers and headphones.
8. Send MIDI Clock, Start, Continue, and Stop in both directions while both
   audio streams are active.
9. Stop host audio, then press **Circle** or **Start**. Scope stops its producer,
   releases the lease, restores the previous USB state, and writes
   `ux0:data/psvita-usb-audio-midi/diagnostic.txt`.
10. Confirm that Content Manager/MTP works in the same active or inactive state
    observed before acquisition.

Press **Triangle** close to any audible glitch. It adds a protected in-memory
marker without writing storage during streaming. Keep recording for ten seconds
after the marker before saving the report.

## What to inspect

During steady capture:

- submitted and completed request counts advance together;
- requested and completed byte counts match;
- the normal request size is 7,680 bytes;
- shorter healthy requests are multiples of 960 bytes;
- completion gaps stay close to the request duration;
- submit, completion, and cancellation errors remain zero;
- underrun, overrun, rebuffer, and conceal counters stop growing after startup;
- the ten-channel mask remains `0x3ff`.

One request that remains pending for 250 ms enters cancellation recovery. Do not
reuse the request slot until SceUdcd acknowledges that cancellation.

Use the saved report to localize a failure:

- no audio endpoint: descriptor or enumeration problem;
- connected but never streaming: the host did not select alternate 1;
- one submitted request that never completes: SceUdcd endpoint or buffer issue;
- `0x3ff` in the plugin but silent host channels: host routing issue;
- missing bits in the channel mask: data disappeared before USB submission;
- rising producer failures: application feeder or syscall problem;
- rising rebuffer/conceal counts: producer scheduling or clock mismatch;
- requested/completed mismatch: short USB completion;
- large completion-to-rearm delay: worker scheduling or lock contention.

## Lifecycle matrix

- Boot with the SKPRX enabled but unused; MTP must behave normally.
- Acquire with the cable disconnected, connect later, then release back to the
  original inactive MTP state.
- Acquire while MTP is active and release back to active MTP.
- Verify PSP communication and serial drivers are restored in the normal
  cable-disconnected state.
- Exercise cable removal, suspend/resume, normal application exit, forced kill,
  and application crash.
- Test an absent client bridge, an incompatible API version, VitaShell USB mode,
  and known conflicting plugins.
- Inject a failure after each takeover step and verify rollback.
- Inject one transient release failure and verify that the owner and stock
  snapshot remain available for a successful retry.
- Complete 100 acquire/release cycles and 100 cable reconnects.

## Host and timing matrix

- Enumerate on current macOS/CoreMIDI, Windows 10/11, and Linux ALSA without a
  device-specific driver.
- Test a standalone class-compliant MIDI host.
- Capture the raw USB configuration at full and high speed.
- Verify endpoints `0x83`, `0x04`, `0x01`, and `0x82`, all alternate settings,
  packet sizes, channel order, and little-endian S16 data.
- Run simultaneous audio and MIDI for 10 minutes, 1 hour, and 4 hours.
- Test MIDI clock at 40, 120, and 300 BPM in both directions. Record drift,
  steady-state p95 jitter, and clock-loss stop latency.
- Repeat streaming after every cable reconnect and lease cycle. Reject
  progressive latency, unbounded ring occupancy, stale data after underrun,
  endpoint freezes, or unexplained transfer errors.

Finally, capture the enumerated VID/PID. The development descriptor requests
Sony VID `0x054c` and experimental PID `0x1338`; neither this checklist nor a
successful enumeration makes that PID assigned or collision-free.

Keep the Scope report and host-side USB descriptor dump with the release test
record.
