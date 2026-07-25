VITASDK ?= $(HOME)/vitasdk

.PHONY: test test-asan vita clean

test:
	cmake -S . -B build/host
	cmake --build build/host
	ctest --test-dir build/host --output-on-failure

test-asan:
	cmake -S . -B build/host-asan \
		-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
	cmake --build build/host-asan
	ctest --test-dir build/host-asan --output-on-failure

vita:
	env PATH="$(VITASDK)/bin:$(PATH)" cmake -S . -B build/vita \
		-DCMAKE_TOOLCHAIN_FILE=$(VITASDK)/share/vita.toolchain.cmake \
		-DPSVITA_USB_AUDIO_MIDI_VITA=ON
	env PATH="$(VITASDK)/bin:$(PATH)" cmake --build build/vita

clean:
	cmake -E remove_directory build
