#include "psvita_usb_audio_midi.h"

#ifdef __vita__
#include <psp2/kernel/modulemgr.h>

int psvitaUsbAudioMidiClientLoad(const char *suprx_path)
{
	int start_status = 0;
	if (!suprx_path || !suprx_path[0]) return -1;
	SceUID module = sceKernelLoadStartModule(suprx_path, 0, NULL, 0, NULL,
					       &start_status);
	return module < 0 ? (int)module : start_status;
}
#else
int psvitaUsbAudioMidiClientLoad(const char *suprx_path)
{
	(void)suprx_path;
	return -1;
}
#endif
