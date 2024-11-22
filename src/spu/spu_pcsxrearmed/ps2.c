#include "out.h"
#include <stdio.h>
#include <audsrv.h>

// SETUP SOUND
static int ps2_init(void)
{
	struct audsrv_fmt_t format;
	int ret = -1;

	ret = audsrv_init();

	if (ret != 0) {
		printf("Failed to initialize audsrv: %s\n", audsrv_get_error_string());
		return -1;
	}

	format.bits = 16;
	format.freq = 44100;
	format.channels = 2;

	ret = audsrv_set_format(&format);

	if (ret != 0) {
		printf("Set format returned: %s\n", audsrv_get_error_string());
		return -1;
	}

	audsrv_set_volume(MAX_VOLUME);

	return ret;
}

// REMOVE SOUND
static void ps2_finish(void)
{
	audsrv_stop_audio();
	audsrv_quit();
}

// GET BYTES BUFFERED
static int ps2_busy(void)
{
	return 1;
}

// FEED SOUND DATA
static void ps2_feed(void* buf, int bytes)
{
	audsrv_wait_audio(bytes);
	audsrv_play_audio(buf, bytes);
}

void out_register_ps2(struct out_driver* drv)
{
	drv->name = "ps2";
	drv->init = ps2_init;
	drv->finish = ps2_finish;
	drv->busy = ps2_busy;
	drv->feed = ps2_feed;
}
