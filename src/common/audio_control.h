#ifndef PSVITA_USB_AUDIO_CONTROL_H
#define PSVITA_USB_AUDIO_CONTROL_H

#include <stdint.h>

int psvita_usb_audio_decode_set_interface(uint8_t request_type,
	uint8_t request, uint16_t value, uint16_t index, int *alternate);

#endif
