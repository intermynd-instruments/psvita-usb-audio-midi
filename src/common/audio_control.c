#include "audio_control.h"
#include "usb_descriptors.h"

#define USB_REQUEST_HOST_TO_DEVICE_STANDARD_INTERFACE 0x01u
#define USB_REQUEST_SET_INTERFACE 0x0bu

int psvita_usb_audio_decode_set_interface(uint8_t request_type,
	uint8_t request, uint16_t value, uint16_t index, int *alternate)
{
	uint16_t interface_number = index & 0xffu;
	if (request_type != USB_REQUEST_HOST_TO_DEVICE_STANDARD_INTERFACE ||
	    request != USB_REQUEST_SET_INTERFACE ||
	    (interface_number != PSVITA_USB_AUDIO_STREAM_INTERFACE &&
	     interface_number != PSVITA_USB_AUDIO_INPUT_STREAM_INTERFACE))
		return 0;
	if (!alternate || (index & 0xff00u) != 0u || value > 1u)
		return -1;
	*alternate = (int)value;
	return 1;
}
