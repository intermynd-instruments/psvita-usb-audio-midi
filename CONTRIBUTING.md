# Contributing

Thanks for taking an interest in psvita-usb-audio-midi. Bug reports, hardware
test results, measurements, documentation fixes, and well-scoped ideas are all
useful.

## Discuss changes first

Except for obvious spelling or broken-link fixes, please do not begin work or
open a pull request until the change has been discussed in a GitHub issue or on
the [Intermynd Instruments Discord](https://discord.gg/SGtAZqPVV).

Wait for a maintainer to explicitly agree that the proposed implementation is
in scope. Opening an issue, receiving general encouragement, or discussing an
idea does not guarantee that a pull request will be accepted. Unsolicited,
out-of-scope, overly broad, or unnecessary refactoring pull requests may be
closed without code review.

This policy exists because the plugin runs in the kernel, owns the USB device
controller while active, and must remain compatible with DS-8. Small changes can
have consequences that are difficult to reproduce without the same Vita and
host setup.

## Reporting a problem

Use the GitHub bug report form and include:

- Vita model, firmware, and homebrew environment
- plugin release, commit, or build ID
- host operating system, DAW or application, and USB setup
- clear reproduction steps and the expected result
- logs, measurements, or the Scope diagnostic report when available

## Proposing a change

Open a change proposal before writing code. Explain the problem, intended
behavior, alternatives considered, and any compatibility or safety impact.
Keep proposals focused on the USB audio/MIDI plugin and its public client API.

The project intentionally avoids automatic plugin installation, writes to
`config.txt`, firmware-specific offsets, hooks, and silent takeover of an
unknown USB state. Proposals that weaken those boundaries are unlikely to be
accepted.

## Pull requests

After a proposal has been accepted:

1. Keep the change as small and focused as practical.
2. Link the agreed issue or discussion.
3. Explain behavior changes and any Vita-specific risks.
4. Include tests for code that can be exercised on the host.
5. Update public documentation when the API, installation, or behavior changes.
6. Avoid unrelated formatting, renaming, generated files, and broad refactors.

If AI-assisted coding tools were materially used, disclose that in the pull
request. You remain responsible for reviewing the result, testing it, and
ensuring that you have the right to contribute it.

Run the checks relevant to the change:

```sh
make test
make test-asan
make vita
```

Vita hardware and host interoperability results are especially valuable. Do
not describe a change as hardware-tested unless it was actually tested on a
Vita, and state the hardware and host configuration used.

## Review and acceptance

Intermynd Instruments maintains the project and decides its scope, design,
release timing, and whether a contribution is merged. Maintainers may ask for
changes, close a proposal or pull request, or choose a different implementation.
Please do not treat an unmerged proposal as committed roadmap work.

By submitting a contribution, you agree to license it under the repository's
[MIT License](LICENSE).
