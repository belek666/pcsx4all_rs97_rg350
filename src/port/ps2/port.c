#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "port.h"
#include "r3000a.h"
#include "plugins.h"
#include "plugin_lib.h"
#include "perfmon.h"
#include "cheat.h"
#include "cdrom_hacks.h"

#include <kernel.h>
#include <sbv_patches.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <sifrpc.h>
#include <libpad.h>
#include <gsKit.h>

//#define USE_FILEXIO

#ifdef SPU_PCSXREARMED
#include "spu/spu_pcsxrearmed/spu_config.h"		// To set spu-specific configuration
#endif

// New gpulib from Notaz's PCSX Rearmed handles duties common to GPU plugins
#ifdef USE_GPULIB
#include "gpu/gpulib/gpu.h"
#endif

#ifdef GPU_UNAI
#include "gpu/gpu_unai/gpu.h"
#endif

#ifdef RUMBLE
#include "libShake/include/shake.h"

/* Weak rumble is either off or on */
#define RUMBLE_WEAK_MAGNITUDE SHAKE_RUMBLE_WEAK_MAGNITUDE_MAX

/* Strong rumble is internally in the range [0,255] */
#define RUMBLE_STRONG_MAGNITUDE_FACTOR (SHAKE_RUMBLE_STRONG_MAGNITUDE_MAX / 255)

typedef struct
{
	Shake_Device *device;
	Shake_Effect effect;
	int id;
	uint8_t low;
	uint8_t high;
	uint8_t active;
	uint8_t initialised;
} joypad_rumble_t;

static joypad_rumble_t joypad_rumble = {0};
#endif

enum {
	DKEY_SELECT = 0,
	DKEY_L3,
	DKEY_R3,
	DKEY_START,
	DKEY_UP,
	DKEY_RIGHT,
	DKEY_DOWN,
	DKEY_LEFT,
	DKEY_L2,
	DKEY_R2,
	DKEY_L1,
	DKEY_R1,
	DKEY_TRIANGLE,
	DKEY_CIRCLE,
	DKEY_CROSS,
	DKEY_SQUARE,

	DKEY_TOTAL
};

GSFONTM *gsFontM;
GSGLOBAL* gsGlobal;
GSTEXTURE gsTexture;
static u32 textureMem[640 * 480] __attribute__((aligned(64)));
unsigned short *SCREEN;
int SCREEN_WIDTH = 320, SCREEN_HEIGHT = 240;
static float center_x, center_y, x2, y2;

static uint_fast8_t pcsx4all_initted = false;
static uint_fast8_t emu_running = false;
static char padBuf_t[2][256] __attribute__((aligned(64)));

extern u8 iomanX_irx[];
extern int size_iomanX_irx;

extern u8 usbhdfsd_irx[];
extern int size_usbhdfsd_irx;

extern u8 usbd_irx[];
extern int size_usbd_irx;

extern u8 fileXio_irx[];
extern int size_fileXio_irx;

#ifdef SPU_PCSXREARMED

extern u8 libsd_irx[];
extern int size_libsd_irx;

extern u8 audsrv_irx[];
extern int size_audsrv_irx;

#elif defined(PS2_SPU)

extern u8 sbusintr_irx[];
extern int size_sbusintr_irx;

extern u8 iop_spu_irx[];
extern int size_iop_spu_irx;

#endif


void config_load();
void config_save();
void update_window_size(int w, int h, uint_fast8_t ntsc_fix);

int Setup_Pad();
void Wait_Pad_Ready();
void init_gs(int bpp);
void init_texture(int w, int h, int bpp);

static void pcsx4all_exit(void) {
	// unload cheats
	cheat_unload();

	// Store config to file
	config_save();

#ifdef RUMBLE
	if (joypad_rumble.device)
	{
		if (joypad_rumble.active)
			Shake_Stop(joypad_rumble.device, joypad_rumble.id);

		Shake_EraseEffect(joypad_rumble.device, joypad_rumble.id);

		Shake_Close(joypad_rumble.device);
		memset(&joypad_rumble, 0, sizeof(joypad_rumble_t));
	}
	Shake_Quit();
#endif

	if (pcsx4all_initted == true) {
		ReleasePlugins();
		psxShutdown();
	}
}

static char McdPath1[MAXPATHLEN] = "";
static char McdPath2[MAXPATHLEN] = "";
static char BiosFile[MAXPATHLEN] = "";

static char homedir[MAXPATHLEN];
static char memcardsdir[MAXPATHLEN];
static char biosdir[MAXPATHLEN];
static char patchesdir[MAXPATHLEN];
char sstatesdir[MAXPATHLEN];
char cheatsdir[MAXPATHLEN];

#define HomeDirectory getcwd(buf, MAXPATHLEN)
#define MKDIR(A) if (access(A, F_OK ) == -1) { mkdir(A, 0755); }

static void setup_paths() {
	static char buf[MAXPATHLEN];
	snprintf(homedir, sizeof(homedir), "%s", getcwd(buf, MAXPATHLEN));
	
	/* 
	 * If folder does not exists then create it 
	 * This can speeds up startup if the folder already exists
	*/

	snprintf(sstatesdir, sizeof(sstatesdir), "%s/sstates", homedir);
	snprintf(memcardsdir, sizeof(memcardsdir), "%s/memcards", homedir);
	snprintf(biosdir, sizeof(biosdir), "%s/bios", homedir);
	snprintf(patchesdir, sizeof(patchesdir), "%s/patches", homedir);
	snprintf(cheatsdir, sizeof(cheatsdir), "%s/cheats", homedir);
	
	MKDIR(homedir);
	MKDIR(sstatesdir);
	MKDIR(memcardsdir);
	MKDIR(biosdir);
	MKDIR(patchesdir);
	MKDIR(cheatsdir);
}

void probe_lastdir() {
	DIR *dir;
	if (!Config.LastDir)
		return;

	dir = opendir(Config.LastDir);

	if (!dir) {
		// Fallback to home directory.
		strncpy(Config.LastDir, homedir, MAXPATHLEN);
		Config.LastDir[MAXPATHLEN-1] = '\0';
	} else {
		closedir(dir);
	}
}

#ifdef PSXREC
extern uint32_t cycle_multiplier; // in mips/recompiler.cpp
#endif

void config_load() {
	FILE *f;
	char config[MAXPATHLEN];
	char line[MAXPATHLEN + 8 + 1];
	int lineNum = 0;

	sprintf(config, "%s/pcsx4all_gb.cfg", homedir);
	f = fopen(config, "r");

	if (f == NULL) {
		printf("Failed to open config file: \"%s\" for reading.\n", config);
		return;
	}

	while (fgets(line, sizeof(line), f)) {
		char *arg = strchr(line, ' ');
		int value;

		++lineNum;

		if (!arg) {
			continue;
		}

		*arg = '\0';
		arg++;

		if (lineNum == 1) {
			if (!strcmp(line, "CONFIG_VERSION")) {
				sscanf(arg, "%d", &value);
				if (value == CONFIG_VERSION) {
					continue;
				} else {
					printf("Incompatible config version for \"%s\"."
					       "Required: %d. Found: %d. Ignoring.\n",
					       config, CONFIG_VERSION, value);
					break;
				}
			}

			printf("Incompatible config format for \"%s\"."
			       "Ignoring.\n", config);
			break;
		}

		if (!strcmp(line, "Xa")) {
			sscanf(arg, "%d", &value);
			Config.Xa = value;
		} else if (!strcmp(line, "Mdec")) {
			sscanf(arg, "%d", &value);
			Config.Mdec = value;
		} else if (!strcmp(line, "PsxAuto")) {
			sscanf(arg, "%d", &value);
			Config.PsxAuto = value;
		} else if (!strcmp(line, "Cdda")) {
			sscanf(arg, "%d", &value);
			Config.Cdda = value;
		} else if (!strcmp(line, "HLE")) {
			sscanf(arg, "%d", &value);
			Config.HLE = value;
		} else if (!strcmp(line, "SlowBoot")) {
			sscanf(arg, "%d", &value);
			Config.SlowBoot = value;
		} else if (!strcmp(line, "AnalogArrow")) {
			sscanf(arg, "%d", &value);
			Config.AnalogArrow = value;
		} else if (!strcmp(line, "Analog_Mode")) {
			sscanf(arg, "%d", &value);
			Config.AnalogMode = value;
		} else if (!strcmp(line, "AnalogDigital")) {
			sscanf(arg, "%d", &value);
			Config.AnalogDigital = value;
#ifdef RUMBLE
		} else if (!strcmp(line, "RumbleGain")) {
			sscanf(arg, "%d", &value);
			if (value < 0 || value > 100)
				value = 100;
			Config.RumbleGain = value;
#endif
		} else if (!strcmp(line, "RCntFix")) {
			sscanf(arg, "%d", &value);
			Config.RCntFix = value;
		} else if (!strcmp(line, "VSyncWA")) {
			sscanf(arg, "%d", &value);
			Config.VSyncWA = value;
		} else if (!strcmp(line, "Cpu")) {
			sscanf(arg, "%d", &value);
			Config.Cpu = value;
		} else if (!strcmp(line, "PsxType")) {
            sscanf(arg, "%d", &value);
            Config.PsxType = value;
        } else if (!strcmp(line, "McdSlot1")) {
            sscanf(arg, "%d", &value);
            Config.McdSlot1 = value;
        } else if (!strcmp(line, "McdSlot2")) {
            sscanf(arg, "%d", &value);
            Config.McdSlot2 = value;
        } else if (!strcmp(line, "SpuIrq")) {
            sscanf(arg, "%d", &value);
			Config.SpuIrq = value;
		} else if (!strcmp(line, "SyncAudio")) {
			sscanf(arg, "%d", &value);
			Config.SyncAudio = value;
		} else if (!strcmp(line, "SpuUpdateFreq")) {
			sscanf(arg, "%d", &value);
			if (value < SPU_UPDATE_FREQ_MIN || value > SPU_UPDATE_FREQ_MAX)
				value = SPU_UPDATE_FREQ_DEFAULT;
			Config.SpuUpdateFreq = value;
		} else if (!strcmp(line, "ForcedXAUpdates")) {
			sscanf(arg, "%d", &value);
			if (value < FORCED_XA_UPDATES_MIN || value > FORCED_XA_UPDATES_MAX)
				value = FORCED_XA_UPDATES_DEFAULT;
			Config.ForcedXAUpdates = value;
		} else if (!strcmp(line, "ShowFps")) {
			sscanf(arg, "%d", &value);
			Config.ShowFps = value;
		} else if (!strcmp(line, "FrameLimit")) {
			sscanf(arg, "%d", &value);
			Config.FrameLimit = value;
		} else if (!strcmp(line, "FrameSkip")) {
			sscanf(arg, "%d", &value);
			if (value < FRAMESKIP_MIN || value > FRAMESKIP_MAX)
				value = FRAMESKIP_OFF;
			Config.FrameSkip = value;
		} else if (!strcmp(line, "VideoScaling")) {
			sscanf(arg, "%d", &value);
			Config.VideoScaling = value;
		}
#ifdef SPU_PCSXREARMED
		else if (!strcmp(line, "SpuUseInterpolation")) {
			sscanf(arg, "%d", &value);
			spu_config.iUseInterpolation = value;
		} else if (!strcmp(line, "SpuUseReverb")) {
			sscanf(arg, "%d", &value);
			spu_config.iUseReverb = value;
		} else if (!strcmp(line, "SpuVolume")) {
			sscanf(arg, "%d", &value);
			if (value > 1024) value = 1024;
			if (value < 0) value = 0;
			spu_config.iVolume = value;
		}
#endif
		else if (!strcmp(line, "LastDir")) {
			int len = strlen(arg);

			if (len == 0 || len > sizeof(Config.LastDir) - 1) {
				continue;
			}

			if (arg[len-1] == '\n') {
				arg[len-1] = '\0';
			}

			strcpy(Config.LastDir, arg);
		} else if (!strcmp(line, "BiosDir")) {
			int len = strlen(arg);

			if (len == 0 || len > sizeof(Config.BiosDir) - 1) {
				continue;
			}

			if (arg[len-1] == '\n') {
				arg[len-1] = '\0';
			}

			strcpy(Config.BiosDir, arg);
		} else if (!strcmp(line, "Bios")) {
			int len = strlen(arg);

			if (len == 0 || len > sizeof(Config.Bios) - 1) {
				continue;
			}

			if (arg[len-1] == '\n') {
				arg[len-1] = '\0';
			}

			strcpy(Config.Bios, arg);
		}
#ifdef PSXREC
		else if (!strcmp(line, "CycleMultiplier")) {
			sscanf(arg, "%03x", &value);
			cycle_multiplier = value;
		}
#endif
#ifdef GPU_UNAI
		else if (!strcmp(line, "pixel_skip")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.pixel_skip = value;
		}
		else if (!strcmp(line, "lighting")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.lighting = value;
		} else if (!strcmp(line, "fast_lighting")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.fast_lighting = value;
		} else if (!strcmp(line, "blending")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.blending = value;
		} else if (!strcmp(line, "dithering")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.dithering = value;
		} else if (!strcmp(line, "interlace")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.ilace_force = value;
		} else if (!strcmp(line, "ntsc_fix")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.ntsc_fix = value;
		}
#endif
	}

	fclose(f);
}

void config_save() {
	FILE *f;
	char config[MAXPATHLEN];
	extern uint_fast8_t ishack_enabled;

	sprintf(config, "%s/pcsx4all_gb.cfg", homedir);

	f = fopen(config, "w");

	if (f == NULL) {
		printf("Failed to open config file: \"%s\" for writing.\n", config);
		return;
	}
	
	if (ishack_enabled == 1)
	{
		Config.VSyncWA = 0;
		/* Default is DualShock */
		Config.AnalogMode = 2;
		Config.RCntFix = 0;
	}

	fprintf(f, "CONFIG_VERSION %d\n"
		   "Xa %d\n"
		   "Mdec %d\n"
		   "PsxAuto %d\n"
		   "Cdda %d\n"
		   "HLE %d\n"
		   "SlowBoot %d\n"
		   "AnalogArrow %d\n"
		   "Analog_Mode %d\n"
#ifdef RUMBLE
		   "RumbleGain %d\n"
#endif
		   "RCntFix %d\n"
		   "VSyncWA %d\n"
		   "Cpu %d\n"
		   "PsxType %d\n"
		   "McdSlot1 %d\n"
		   "McdSlot2 %d\n"
		   "SpuIrq %d\n"
		   "SyncAudio %d\n"
		   "SpuUpdateFreq %d\n"
		   "ForcedXAUpdates %d\n"
		   "ShowFps %d\n"
		   "FrameLimit %d\n"
		   "FrameSkip %d\n"
		   "VideoScaling %d\n"
		   "AnalogDigital %d\n",
		   CONFIG_VERSION, Config.Xa, Config.Mdec, Config.PsxAuto, Config.Cdda,
		   Config.HLE, Config.SlowBoot, Config.AnalogArrow, Config.AnalogMode,
#ifdef RUMBLE
		   Config.RumbleGain,
#endif
		   Config.RCntFix, Config.VSyncWA, Config.Cpu, Config.PsxType,
		   Config.McdSlot1, Config.McdSlot2, Config.SpuIrq, Config.SyncAudio,
		   Config.SpuUpdateFreq, Config.ForcedXAUpdates, Config.ShowFps,
		   Config.FrameLimit, Config.FrameSkip, Config.VideoScaling, Config.AnalogDigital);

#ifdef SPU_PCSXREARMED
	fprintf(f, "SpuUseInterpolation %d\n", spu_config.iUseInterpolation);
	fprintf(f, "SpuUseReverb %d\n", spu_config.iUseReverb);
	fprintf(f, "SpuVolume %d\n", spu_config.iVolume);
#endif

#ifdef PSXREC
	fprintf(f, "CycleMultiplier %03x\n", cycle_multiplier);
#endif

#ifdef GPU_UNAI
	fprintf(f, "interlace %d\n"
		   "pixel_skip %d\n"
		   "lighting %d\n"
		   "fast_lighting %d\n"
		   "blending %d\n"
		   "dithering %d\n"
		   "ntsc_fix %d\n",
		   gpu_unai_config_ext.ilace_force,
		   gpu_unai_config_ext.pixel_skip,
		   gpu_unai_config_ext.lighting,
		   gpu_unai_config_ext.fast_lighting,
		   gpu_unai_config_ext.blending,
		   gpu_unai_config_ext.dithering,
		   gpu_unai_config_ext.ntsc_fix);
#endif

	if (Config.LastDir[0]) {
		fprintf(f, "LastDir %s\n", Config.LastDir);
	}

	if (Config.BiosDir[0]) {
		fprintf(f, "BiosDir %s\n", Config.BiosDir);
	}

	if (Config.Bios[0]) {
		fprintf(f, "Bios %s\n", Config.Bios);
	}

	fclose(f);
}

// Returns 0: success, -1: failure
int state_load(int slot) {
	char savename[512];
	sprintf(savename, "%s/%s.%d.sav", sstatesdir, CdromId, slot);

	if (FileExists(savename)) {
		return LoadState(savename);
	}

	return -1;
}

// Returns 0: success, -1: failure
int state_save(int slot) {
	char savename[512];
	sprintf(savename, "%s/%s.%d.sav", sstatesdir, CdromId, slot);

	return SaveState(savename);
}

static uint16_t pad1 = 0xFFFF;

static uint16_t pad2 = 0xFFFF;

static uint16_t pad1_buttons = 0xFFFF;

static unsigned short analog1 = 0;

#define joy_commit_range    8192
enum {
	ANALOG_UP = 1,
	ANALOG_DOWN = 2,
	ANALOG_LEFT = 4,
	ANALOG_RIGHT = 8
};

struct ps1_controller player_controller[2];

void Set_Controller_Mode() {
	switch (Config.AnalogMode) {
		/* Digital. Required for some games. */
	default: player_controller[0].id = 0x41;
		player_controller[0].pad_mode = 0;
		player_controller[0].pad_controllertype = 0;
		break;
		/* DualAnalog. Some games might misbehave with Dualshock like Descent so this is for those */
	case 1: player_controller[0].id = 0x53;
		player_controller[0].pad_mode = 1;
		player_controller[0].pad_controllertype = 1;
		break;
		/* DualShock, required for Ape Escape. */
	case 2: player_controller[0].id = 0x73;
		player_controller[0].pad_mode = 1;
		player_controller[0].pad_controllertype = 1;
		break;
	}
}

void joy_init() {
	Setup_Pad();
	Wait_Pad_Ready();
	
	player_controller[0].id = 0x41;
	player_controller[0].pad_mode = 0;
	player_controller[0].pad_controllertype = 0;
	
	player_controller[0].joy_left_ax0 = 127;
	player_controller[0].joy_left_ax1 = 127;
	player_controller[0].joy_right_ax0 = 127;
	player_controller[0].joy_right_ax1 = 127;

	player_controller[0].Vib[0] = 0;
	player_controller[0].Vib[1] = 0;
	player_controller[0].VibF[0] = 0;
	player_controller[0].VibF[1] = 0;

	player_controller[0].configmode = 0;

	Set_Controller_Mode();
}

void pad_update() {
	uint_fast8_t popup_menu = false;
	Wait_Pad_Ready();
	struct padButtonStatus pad;
	if (padRead(0, 0, &pad)) {
		if ((pad.btns ^ 0xFFFF) == (PAD_START|PAD_SELECT|PAD_L1|PAD_R1)) {
			popup_menu = true;
		}
		else {
			pad1 = pad.btns;
			player_controller[0].joy_right_ax0 = pad.rjoy_h;
			player_controller[0].joy_right_ax1 = pad.rjoy_v;
			player_controller[0].joy_left_ax0 = pad.ljoy_h;
			player_controller[0].joy_left_ax1 = pad.ljoy_v;
		}
	}

	// popup main menu
	if (popup_menu) {
		//Sync and close any memcard files opened for writing
		//TODO: Disallow entering menu until they are synced/closed
		// automatically, displaying message that write is in progress.
		sioSyncMcds();

		emu_running = false;
		pl_pause();    // Tell plugin_lib we're pausing emu
#ifndef NO_HWSCALE
		update_window_size(320, 240, false);
#endif
		GameMenu();
		emu_running = true;
		pad1_buttons |= (1 << DKEY_SELECT) | (1 << DKEY_START) | (1 << DKEY_CROSS);
#ifndef NO_HWSCALE
		update_window_size(gpu.screen.hres, gpu.screen.vres, Config.PsxType == PSX_TYPE_NTSC);
#endif
		if (Config.VideoScaling == 1) {
			video_clear();
			video_flip();
			video_clear();
		}
		emu_running = true;
		//pad1 |= (1 << DKEY_START);
		//pad1 |= (1 << DKEY_CROSS);
		pl_resume();    // Tell plugin_lib we're reentering emu
	}
}

unsigned short pad_read(int num)
{
	return (num == 0 ? pad1 : pad2);
}

void video_flip(void) {
	if (emu_running) {
		gsGlobal->PrimAlphaEnable = 0;
		SyncDCache(gsTexture.Mem, (u8*)gsTexture.Mem + gsKit_texture_size_ee(gsTexture.Width, gsTexture.Height, gsTexture.PSM));
		gsKit_texture_send_inline(gsGlobal, gsTexture.Mem, gsTexture.Width, gsTexture.Height, gsTexture.Vram, gsTexture.PSM, gsTexture.TBW, GS_CLUT_NONE);
		gsKit_prim_sprite_texture(gsGlobal, &gsTexture,
        	center_x, center_y, 0.0f, 0.0f,
        	x2, y2,
        	gsTexture.Width, gsTexture.Height,
        	3.0f, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00)
		);
		gsGlobal->PrimAlphaEnable = 1;
		if (Config.ShowFps) {
			port_printf(5, 5, pl_data.stats_msg);
		}
	}
	gsKit_queue_exec(gsGlobal);
	gsKit_sync_flip(gsGlobal);
}

void video_clear(void) {
	memset(gsTexture.Mem, 0, gsKit_texture_size_ee(gsTexture.Width, gsTexture.Height, gsTexture.PSM));
	gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00,0x00,0x00,0x80,0x00));
}

char *GetMemcardPath(int slot) {
	switch(slot) {
	case 1:
		if (Config.McdSlot1 == -1) {
			return NULL;
		} else {
			return McdPath1;
		}
	case 2:
		if (Config.McdSlot2 == -1) {
			return NULL;
		} else {
			return McdPath2;
		}
	}
	return NULL;
}

void update_memcards(int load_mcd) {

	if (Config.McdSlot1 == -1) {
		McdPath1[0] = '\0';
	} else if (Config.McdSlot1 == 0) {
		if (string_is_empty(CdromId)) {
			/* Fallback */
			sprintf(McdPath1, "%s/%s", memcardsdir, "card1.mcd");
		} else {
			sprintf(McdPath1, "%s/%s.1.mcr", memcardsdir, CdromId);
		}
	} else {
		sprintf(McdPath1, "%s/mcd%03d.mcr", memcardsdir, (int)Config.McdSlot1);
	}

	if (Config.McdSlot2 == -1) {
		McdPath2[0] = '\0';
	} else if (Config.McdSlot2 == 0) {
		if (string_is_empty(CdromId)) {
			/* Fallback */
			sprintf(McdPath2, "%s/%s", memcardsdir, "card2.mcd");
		} else {
			sprintf(McdPath2, "%s/%s.2.mcr", memcardsdir, CdromId);
		}
	} else {
		sprintf(McdPath2, "%s/mcd%03d.mcr", memcardsdir, (int)Config.McdSlot2);
	}

	if (load_mcd & 1)
		LoadMcd(MCD1, GetMemcardPath(1)); //Memcard 1
	if (load_mcd & 2)
		LoadMcd(MCD2, GetMemcardPath(2)); //Memcard 2
}

const char *bios_file_get() {
	return BiosFile;
}

// if [CdromId].bin is exsit, use the spec bios
void check_spec_bios() {
	FILE *f = NULL;
	char bios[MAXPATHLEN];
	sprintf(bios, "%s/%s.bin", Config.BiosDir, CdromId);
	f = fopen(bios, "rb");
	if (f == NULL) {
		strcpy(BiosFile, Config.Bios);
		return;
	}
	fclose(f);
	sprintf(BiosFile, "%s.bin", CdromId);
}

static char actAlign[6] = { 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF };

int set_rumble_gain(unsigned gain) { 
	return 0; 
};

void Rumble_Init() {
	padSetActAlign(0, 0, actAlign);
}

int trigger_rumble(uint8_t low, uint8_t high) { 
	actAlign[0] = low;
	actAlign[1] = high;
	return padSetActDirect(0, 0, actAlign);
}

void update_window_size(int w, int h, uint_fast8_t ntsc_fix) {
	printf("update_window_size w %d h %d fix %d\n", w, h, ntsc_fix);

	if (Config.VideoScaling != 0) return;
	SCREEN_WIDTH = w;
#ifdef GPU_UNAI
	if (gpu_unai_config_ext.ntsc_fix && ntsc_fix) {
#else
	if (ntsc_fix) {
#endif
		switch (h) {
		case 240:
		case 256: h -= 16; break;
		case 480: h -= 32; break;
		}
	}
	SCREEN_HEIGHT = h;

	init_texture(w, h, 16);

	video_clear();
	video_flip();
	video_clear();
}

int main (int argc, char **argv) {
	char filename[256];
	const char *cdrfilename = GetIsoFile();

	filename[0] = '\0'; /* Executable file name */

	/*argc = 1 + 2;
	argv[1] = "-iso";
	argv[2] = "Silent Hill (USA) (v1.1).cue";*/
	//argv[2] = "mass:/PSXISO/Chrono Cross (USA) (Disc 1).cue";
	//argv[2] = "mass:/PSXISO/Silent Hill (USA) (v1.1).cue";
	//argv[2] = "Chrono Cross (USA) (Disc 1).cue";
	//argv[2] = "Tomb Raider (Europe)/Tomb Raider (Europe).cue";
	//argv[2] = "Quake II (USA).cue";
	//argv[3] = "-bios";
	//argv[4] = "bios/SCPH1001.BIN";

#ifdef USE_FILEXIO
	SifInitRpc(0);

	while (!SifIopReset(NULL, 0)) {};
	while (!SifIopSync()) {};

	fioExit();
	SifExitIopHeap();
	SifLoadFileExit();
	SifExitRpc();
	SifExitCmd();
#endif

	SifInitRpc(0);
	FlushCache(0);
	FlushCache(2);

	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();
	sbv_patch_fileio();

	SifLoadModule("rom0:SIO2MAN", 0, NULL);
	SifLoadModule("rom0:MCMAN", 0, NULL);
	SifLoadModule("rom0:MCSERV", 0, NULL);
	SifLoadModule("rom0:PADMAN", 0, NULL);

	SifExecModuleBuffer(iomanX_irx, size_iomanX_irx, 0, NULL, NULL);

#ifdef USE_FILEXIO
	SifExecModuleBuffer(fileXio_irx, size_fileXio_irx, 0, NULL, NULL);
	fileXioInit();
#endif

	SifExecModuleBuffer(usbd_irx, size_usbd_irx, 0, NULL, NULL);
	SifExecModuleBuffer(usbhdfsd_irx, size_usbhdfsd_irx, 0, NULL, NULL);
#ifdef SPU_PCSXREARMED
	SifExecModuleBuffer(libsd_irx, size_libsd_irx, 0, NULL, NULL);
	SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, NULL);
#elif defined(PS2_SPU)
	SifExecModuleBuffer(sbusintr_irx, size_sbusintr_irx, 0, NULL, NULL);
	SifExecModuleBuffer(iop_spu_irx, size_iop_spu_irx, 0, NULL, NULL);
#endif

	nopdelay();
	nopdelay();

	Setup_Pad();
	Wait_Pad_Ready();

	setup_paths();

	// PCSX
	Config.McdSlot1 = 1;
	Config.McdSlot2 = -1;
	update_memcards(0);
	strcpy(Config.PatchesDir, patchesdir);
	strcpy(Config.BiosDir, biosdir);
	strcpy(Config.Bios, "scph1001.bin");
	
	Config.AnalogDigital = 0;
	Config.AnalogArrow = 0;
	Config.AnalogMode = 2;

#ifdef RUMBLE
	Config.RumbleGain = 100; /* [0,100]-Rumble effect strength */
#endif

	Config.Xa=0; /* 0=XA enabled, 1=XA disabled */
	Config.Mdec=0; /* 0=Black&White Mdecs Only Disabled, 1=Black&White Mdecs Only Enabled */
	Config.PsxAuto=1; /* 1=autodetect system (pal or ntsc) */
	Config.PsxType=0; /* PSX_TYPE_NTSC=ntsc, PSX_TYPE_PAL=pal */
	Config.Cdda=0; /* 0=Enable Cd audio, 1=Disable Cd audio */
	Config.HLE=1; /* 0=BIOS, 1=HLE */
#if defined (PSXREC)
	Config.Cpu=0; /* 0=recompiler, 1=interpreter */
#else
	Config.Cpu=1; /* 0=recompiler, 1=interpreter */
#endif
	Config.SlowBoot=0; /* 0=skip bios logo sequence on boot  1=show sequence (does not apply to HLE) */
	Config.RCntFix=0; /* 1=Parasite Eve 2, Vandal Hearts 1/2 Fix */
	Config.VSyncWA=0; /* 1=InuYasha Sengoku Battle Fix */
	Config.SpuIrq=1; /* 1=SPU IRQ always on, fixes some games */

	Config.SyncAudio=0;	/* 1=emu waits if audio output buffer is full
	                       (happens seldom with new auto frame limit) */

	// Number of times per frame to update SPU. Rearmed default is once per
	//  frame, but we are more flexible (for slower devices).
	//  Valid values: SPU_UPDATE_FREQ_1 .. SPU_UPDATE_FREQ_32
	Config.SpuUpdateFreq = SPU_UPDATE_FREQ_DEFAULT;

	//senquack - Added option to allow queuing CDREAD_INT interrupts sooner
	//           than they'd normally be issued when SPU's XA buffer is not
	//           full. This fixes droupouts in music/speech on slow devices.
	Config.ForcedXAUpdates = FORCED_XA_UPDATES_DEFAULT;

	Config.ShowFps=0;    // 0=don't show FPS
	Config.FrameLimit = true;
	Config.FrameSkip = FRAMESKIP_OFF;

	//zear - Added option to store the last visited directory.
	strncpy(Config.LastDir, homedir, MAXPATHLEN);
	Config.LastDir[MAXPATHLEN-1] = '\0';

	// senquack - added spu_pcsxrearmed plugin:
#ifdef SPU_PCSXREARMED
	//ORIGINAL PCSX ReARMed SPU defaults (put here for reference):
	//	spu_config.iUseReverb = 1;
	//	spu_config.iUseInterpolation = 1;
	//	spu_config.iXAPitch = 0;
	//	spu_config.iVolume = 768;
	//	spu_config.iTempo = 0;
	//	spu_config.iUseThread = 1; // no effect if only 1 core is detected
	//	// LOW-END DEVICE:
	//	#ifdef HAVE_PRE_ARMV7 /* XXX GPH hack */
	//		spu_config.iUseReverb = 0;
	//		spu_config.iUseInterpolation = 0;
	//		spu_config.iTempo = 1;
	//	#endif

	// PCSX4ALL defaults:
	// NOTE: iUseThread *will* have an effect even on a single-core device, but
	//		 results have yet to be tested. TODO: test if using iUseThread can
	//		 improve sound dropouts in any cases.
	spu_config.iHaveConfiguration = 1;    // *MUST* be set to 1 before calling SPU_Init()
	spu_config.iUseReverb = 0;
	spu_config.iUseInterpolation = 0;
	spu_config.iXAPitch = 0;
	spu_config.iVolume = 1024;            // 1024 is max volume
	spu_config.iUseThread = 0;            // no effect if only 1 core is detected
	spu_config.iUseFixedUpdates = 1;      // This is always set to 1 in libretro's pcsxReARMed
	spu_config.iTempo = 1;                // see note below
#endif

	//senquack - NOTE REGARDING iTempo config var above
	// From thread https://pyra-handheld.com/boards/threads/pcsx-rearmed-r22-now-using-the-dsp.75388/
	// Notaz says that setting iTempo=1 restores pcsxreARMed SPU's old behavior, which allows slow emulation
	// to not introduce audio dropouts (at least I *think* he's referring to iTempo config setting)
	// "Probably the main change is SPU emulation, there were issues in some games where effects were wrong,
	//  mostly Final Fantasy series, it should be better now. There were also sound sync issues where game would
	//  occasionally lock up (like Valkyrie Profile), it should be stable now.
	//  Changed sync has a side effect however - if the emulator is not fast enough (may happen with double
	//  resolution mode or while underclocking), sound will stutter more instead of slowing down the music itself.
	//  There is a new option in SPU plugin config to restore old inaccurate behavior if anyone wants it." -Notaz

	// gpu_unai
#ifdef GPU_UNAI
	gpu_unai_config_ext.ilace_force = 0;
	gpu_unai_config_ext.pixel_skip = 0;
	gpu_unai_config_ext.lighting = 1;
	gpu_unai_config_ext.fast_lighting = 1;
	gpu_unai_config_ext.blending = 1;
	gpu_unai_config_ext.dithering = 0;
	gpu_unai_config_ext.ntsc_fix = 1;
#endif

	// Load config from file.
	config_load();

	// Check if LastDir exists.
	probe_lastdir();

	// command line options
	uint_fast8_t param_parse_error = 0;
	for (int i = 1; i < argc; i++) {
		// PCSX
		// XA audio disabled
		if (strcmp(argv[i],"-noxa") == 0)
			Config.Xa = 1;

		// Black & White MDEC
		if (strcmp(argv[i],"-bwmdec") == 0)
			Config.Mdec = 1;

		// Force PAL system
		if (strcmp(argv[i],"-pal") == 0) {
			Config.PsxAuto = 0;
			Config.PsxType = 1;
		}

		// Force NTSC system
		if (strcmp(argv[i],"-ntsc") == 0) {
			Config.PsxAuto = 0;
			Config.PsxType = 0;
		}

		// CD audio disabled
		if (strcmp(argv[i],"-nocdda") == 0)
			Config.Cdda = 1;

		// BIOS enabled
		if (strcmp(argv[i],"-bios") == 0)
			Config.HLE = 0;

		// Interpreter enabled
		if (strcmp(argv[i],"-interpreter") == 0)
			Config.Cpu = 1;

		// Show BIOS logo sequence at BIOS startup (doesn't apply to HLE)
		if (strcmp(argv[i],"-slowboot") == 0)
			Config.SlowBoot = 1;

		// Parasite Eve 2, Vandal Hearts 1/2 Fix
		if (strcmp(argv[i],"-rcntfix") == 0)
			Config.RCntFix = 1;

		// InuYasha Sengoku Battle Fix
		if (strcmp(argv[i],"-vsyncwa") == 0)
			Config.VSyncWA = 1;

		// SPU IRQ always enabled (fixes audio in some games)
		if (strcmp(argv[i],"-spuirq") == 0)
			Config.SpuIrq = 1;

		// Set ISO file
		if (strcmp(argv[i],"-iso") == 0)
			SetIsoFile(argv[i + 1]);

		// Set executable file
		if (strcmp(argv[i],"-file") == 0)
			strcpy(filename, argv[i + 1]);

		// Audio synchronization option: if audio buffer full, main thread
		//  blocks. Otherwise, just drop the samples.
		if (strcmp(argv[i],"-syncaudio") == 0)
			Config.SyncAudio = 0;

		// Number of times per frame to update SPU. PCSX Rearmed default is once
		//  per frame, but we are more flexible. Valid value is 0..5, where
		//  0 is once per frame, 5 is 32 times per frame (2^5)
		if (strcmp(argv[i],"-spuupdatefreq") == 0) {
			int val = -1;
			if (++i < argc) {
				val = atoi(argv[i]);
				if (val >= SPU_UPDATE_FREQ_MIN && val <= SPU_UPDATE_FREQ_MAX) {
					Config.SpuUpdateFreq = val;
				} else val = -1;
			} else {
				printf("ERROR: missing value for -spuupdatefreq\n");
			}

			if (val == -1) {
				printf("ERROR: -spuupdatefreq value must be between %d..%d\n"
					   "(%d is once per frame)\n",
					   SPU_UPDATE_FREQ_MIN, SPU_UPDATE_FREQ_MAX, SPU_UPDATE_FREQ_1);
				param_parse_error = true;
				break;
			}
		}

		//senquack - Added option to allow queuing CDREAD_INT interrupts sooner
		//           than they'd normally be issued when SPU's XA buffer is not
		//           full. This fixes droupouts in music/speech on slow devices.
		if (strcmp(argv[i],"-forcedxaupdates") == 0) {
			int val = -1;
			if (++i < argc) {
				val = atoi(argv[i]);
				if (val >= FORCED_XA_UPDATES_MIN && val <= FORCED_XA_UPDATES_MAX) {
					Config.ForcedXAUpdates = val;
				} else val = -1;
			} else {
				printf("ERROR: missing value for -forcedxaupdates\n");
			}

			if (val == -1) {
				printf("ERROR: -forcedxaupdates value must be between %d..%d\n",
					   FORCED_XA_UPDATES_MIN, FORCED_XA_UPDATES_MAX);
				param_parse_error = true;
				break;
			}
		}

		// Performance monitoring options
		if (strcmp(argv[i],"-perfmon") == 0) {
			// Enable detailed stats and console output
			Config.PerfmonConsoleOutput = true;
			Config.PerfmonDetailedStats = true;
		}

		// GPU
		// show FPS
		if (strcmp(argv[i],"-showfps") == 0) {
			Config.ShowFps = true;
		}

		// frame limit
		if (strcmp(argv[i],"-noframelimit") == 0) {
			Config.FrameLimit = 0;
		}

		// frame skip
		if (strcmp(argv[i],"-frameskip") == 0) {
			int val = -1000;
			if (++i < argc) {
				val = atoi(argv[i]);
				if (val >= -1 && val <= 3) {
					Config.FrameSkip = val;
				}
			} else {
				printf("ERROR: missing value for -frameskip\n");
			}

			if (val == -1000) {
				printf("ERROR: -frameskip value must be between -1..3 (-1 is AUTO)\n");
				param_parse_error = true;
				break;
			}
		}

#ifdef GPU_UNAI
		// Render only every other line (looks ugly but faster)
		if (strcmp(argv[i],"-interlace") == 0) {
			gpu_unai_config_ext.ilace_force = 1;
		}

		// Allow 24bpp->15bpp dithering (only polys, only if PS1 game uses it)
		if (strcmp(argv[i],"-dither") == 0) {
			gpu_unai_config_ext.dithering = 1;
		}

		if (strcmp(argv[i],"-ntsc_fix") == 0) {
			gpu_unai_config_ext.ntsc_fix = 1;
		}

		if (strcmp(argv[i],"-nolight") == 0) {
			gpu_unai_config_ext.lighting = 0;
		}

		if (strcmp(argv[i],"-noblend") == 0) {
			gpu_unai_config_ext.blending = 0;
		}

		// Apply lighting to all primitives. Default is to only light primitives
		//  with light values below a certain threshold (for speed).
		if (strcmp(argv[i],"-nofastlight") == 0) {
			gpu_unai_config_ext.fast_lighting = 0;
		}

		// Render all pixels on a horizontal line, even when in hi-res 512,640
		//  PSX vid modes and those pixels would never appear on 320x240 screen.
		//  (when using pixel-dropping downscaler).
		//  Can cause visual artifacts, default is on for now (for speed)
		if (strcmp(argv[i],"-nopixelskip") == 0) {
		 	gpu_unai_config_ext.pixel_skip = 0;
		}

		// Settings specific to older, non-gpulib standalone gpu_unai:
#ifndef USE_GPULIB
		// Progressive interlace option - See gpu_unai/gpu.h
		// Old option left in from when PCSX4ALL ran on very slow devices.
		if (strcmp(argv[i],"-progressive") == 0) {
			gpu_unai_config_ext.prog_ilace = 1;
		}
#endif //!USE_GPULIB
#endif //GPU_UNAI


	// SPU
#ifndef SPU_NULL

	// ----- BEGIN SPU_PCSXREARMED SECTION -----
#ifdef SPU_PCSXREARMED
		// No sound
		if (strcmp(argv[i],"-silent") == 0) {
			spu_config.iDisabled = 1;
		}
		// Reverb
		if (strcmp(argv[i],"-reverb") == 0) {
			spu_config.iUseReverb = 1;
		}
		// XA Pitch change support
		if (strcmp(argv[i],"-xapitch") == 0) {
			spu_config.iXAPitch = 1;
		}

		// Enable SPU thread
		// NOTE: By default, PCSX ReARMed would not launch
		//  a thread if only one core was detected, but I have
		//  changed it to allow it under any case.
		// TODO: test if any benefit is ever achieved
		if (strcmp(argv[i],"-threaded_spu") == 0) {
			spu_config.iUseThread = 1;
		}

		// Don't output fixed number of samples per frame
		// (unknown if this helps or hurts performance
		//  or compatibility.) The default in all builds
		//  of PCSX_ReARMed is "true", so that is also the
		//  default here.
		if (strcmp(argv[i],"-nofixedupdates") == 0) {
			spu_config.iUseFixedUpdates = 0;
		}

		// Set interpolation none/simple/gaussian/cubic, default is none
		if (strcmp(argv[i],"-interpolation") == 0) {
			int val = -1;
			if (++i < argc) {
				if (strcmp(argv[i],"none") == 0) val=0;
				if (strcmp(argv[i],"simple") == 0) val=1;
				if (strcmp(argv[i],"gaussian") == 0) val=2;
				if (strcmp(argv[i],"cubic") == 0) val=3;
			} else
				printf("ERROR: missing value for -interpolation\n");


			if (val == -1) {
				printf("ERROR: -interpolation value must be one of: none,simple,gaussian,cubic\n");
				param_parse_error = true; break;
			}

			spu_config.iUseInterpolation = val;
		}

		// Set volume level of SPU, 0-1024
		//  If value is 0, sound will be disabled.
		if (strcmp(argv[i],"-volume") == 0) {
			int val = -1;
			if (++i < argc)
				val = atoi(argv[i]);
			else
				printf("ERROR: missing value for -volume\n");

			if (val < 0 || val > 1024) {
				printf("ERROR: -volume value must be between 0-1024. Value of 0 will mute sound\n"
						"        but SPU plugin will still run, ensuring best compatibility.\n"
						"        Use -silent flag to disable SPU plugin entirely.\n");
				param_parse_error = true; break;
			}

			spu_config.iVolume = val;
		}

		// SPU will issue updates at a rate that ensures better
		//  compatibility, but if the emulator runs too slowly,
		//  audio stutter will be increased. "False" is the
		//  default setting on Pandora/Pyra/Android builds of
		//  PCSX_ReARMed, but Wiz/Caanoo builds used the faster
		//  inaccurate setting, "true", so I've made our default
		//  "true" as well, since we target low-end devices.
		if (strcmp(argv[i],"-notempo") == 0) {
			spu_config.iTempo = 0;
		}

		//NOTE REGARDING ABOVE SETTING "spu_config.iTempo":
		// From thread https://pyra-handheld.com/boards/threads/pcsx-rearmed-r22-now-using-the-dsp.75388/
		// Notaz says that setting iTempo=1 restores pcsxreARMed SPU's old behavior, which allows slow emulation
		// to not introduce audio dropouts (at least I *think* he's referring to iTempo config setting)
		// "Probably the main change is SPU emulation, there were issues in some games where effects were wrong,
		//  mostly Final Fantasy series, it should be better now. There were also sound sync issues where game would
		//  occasionally lock up (like Valkyrie Profile), it should be stable now.
		//  Changed sync has a side effect however - if the emulator is not fast enough (may happen with double
		//  resolution mode or while underclocking), sound will stutter more instead of slowing down the music itself.
		//  There is a new option in SPU plugin config to restore old inaccurate behavior if anyone wants it." -Notaz

#endif //SPU_PCSXREARMED
	// ----- END SPU_PCSXREARMED SECTION -----

#endif //!SPU_NULL
	}


	if (param_parse_error) {
		printf("Failed to parse command-line parameters, exiting.\n");
		exit(1);
	}

	//NOTE: spu_pcsxrearmed will handle audio initialization
	init_gs(16);
	init_texture(SCREEN_WIDTH, SCREEN_HEIGHT, 16);
	SCREEN = (u16 *)gsTexture.Mem;

	atexit(pcsx4all_exit);


	if (argc < 2 || cdrfilename[0] == '\0') {
		// Enter frontend main-menu:
		emu_running = false;
		if (!SelectGame()) {
			printf("ERROR: missing filename for -iso\n");
			exit(1);
		}
	}

	if (psxInit() == -1) {
		printf("PSX emulator couldn't be initialized.\n");
		exit(1);
	}

	if (LoadPlugins() == -1) {
		printf("Failed loading plugins.\n");
		exit(1);
	}
	
	update_memcards(0);
	strcpy(BiosFile, Config.Bios);
	Rumble_Init();

	pcsx4all_initted = true;
	emu_running = true;

	// Initialize plugin_lib, gpulib
	pl_init();

	if (cdrfilename[0] != '\0') {
		if (CheckCdrom() == -1) {
			psxReset();
			printf("Failed checking ISO image.\n");
			SetIsoFile(NULL);
		} else {
			check_spec_bios();
			psxReset();
			printf("Running ISO image: %s.\n", cdrfilename);
			if (LoadCdrom() == -1) {
				printf("Failed loading ISO image.\n");
				SetIsoFile(NULL);
			} else {
				// load cheats
				cheat_load();
			}
		}
	} else {
		psxReset();
	}

	CheckforCDROMid_applyhacks();

	/* If we are using per-disk memory cards, load them now */
	if ((Config.McdSlot1 == 0) || (Config.McdSlot2 == 0)) {
		update_memcards(0);
		LoadMcd(MCD1, GetMemcardPath(1)); //Memcard 1
		LoadMcd(MCD2, GetMemcardPath(2)); //Memcard 2
	}

	if (filename[0] != '\0') {
		if (Load(filename) == -1) {
			printf("Failed loading executable.\n");
			filename[0]='\0';
		}

		printf("Running executable: %s.\n",filename);
	}

	if ((cdrfilename[0] == '\0') && (filename[0] == '\0') && (Config.HLE == 0)) {
		printf("Running BIOS.\n");
	}

	if ((cdrfilename[0] != '\0') || (filename[0] != '\0') || (Config.HLE == 0)) {
		psxCpu->Execute();
	}

	return 0;
}

unsigned get_ticks(void) {
	return ((((unsigned long long)clock())*1000000ULL)/((unsigned long long)CLOCKS_PER_SEC));
}

void wait_ticks(unsigned s) {
    usleep(s);
}

void port_printf(int x, int y, const char *text) {
	static u64 WhiteFont = GS_SETREG_RGBAQ(0x80,0x80,0x80,0x80,0x00);
	gsKit_fontm_print_scaled(gsGlobal, gsFontM, x * gsGlobal->Width / gsTexture.Width, y * gsGlobal->Height / gsTexture.Height, 3, 0.7f, WhiteFont, text);
}

void WaitPadReady(int port, int slot) {
	int state, lastState;
	char stateString[16];

	state = padGetState(port, slot);
	lastState = -1;
	while ((state != PAD_STATE_DISCONN)
		&& (state != PAD_STATE_STABLE)
		&& (state != PAD_STATE_FINDCTP1)) {
		if (state != lastState)
			padStateInt2String(state, stateString);
		lastState = state;
		state = padGetState(port, slot);
	}
}

void Wait_Pad_Ready(void) {
	int state_1, state_2;

	state_1 = padGetState(0, 0);
	state_2 = padGetState(1, 0);
	while ((state_1 != PAD_STATE_DISCONN) && (state_2 != PAD_STATE_DISCONN)
		&& (state_1 != PAD_STATE_STABLE) && (state_2 != PAD_STATE_STABLE)
		&& (state_1 != PAD_STATE_FINDCTP1) && (state_2 != PAD_STATE_FINDCTP1)) {
		state_1 = padGetState(0, 0);
		state_2 = padGetState(1, 0);
	}
}

int Setup_Pad(void) {
	int ret, i, port, state, modes;
	padInit(0);

	for (port = 0; port < 2; port++) {
		if ((ret = padPortOpen(port, 0, &padBuf_t[port][0])) == 0)
			return 0;
		WaitPadReady(port, 0);
		state = padGetState(port, 0);
		if (state != PAD_STATE_DISCONN) {
			modes = padInfoMode(port, 0, PAD_MODETABLE, -1);
			if (modes != 0) {
				i = 0;
				do {
					if (padInfoMode(port, 0, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK) {
						padSetMainMode(port, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
						break;
					}
					i++;
				} while (i < modes);
			}
		}
	}
	return 1;
}

void MIPS32R2_MakeCodeVisible(void* start, int size) {
	u32 inval_start = ((u32)start) & ~63;
	u32 inval_end = (((u32)start) + size + 64) & ~63;
	for (; inval_start < inval_end; inval_start += 64) {
		__asm__ __volatile__(
			".set noreorder\n\t"

			"sync.l \n"
			"cache 0x18, (%0) \n"
			"sync.l \n"
			"sync.p \n"
			"cache 0x0B, (%0) \n"
			"sync.p \n"
			".set reorder\n\t"

			: "=r" (inval_start)
			: "0" (inval_start)
		);
	}
}

void init_gs(int bpp) {
	if (gsGlobal != NULL) {
		gsKit_deinit_global(gsGlobal);
	}

	gsGlobal = gsKit_init_global();
	gsFontM = gsKit_init_fontm();

	if (bpp == 32) {
		gsGlobal->PSM = GS_PSM_CT32;
		gsGlobal->PSMZ = GS_PSMZ_32;
	}
	else {
		gsGlobal->PSM = GS_PSM_CT16S;
		gsGlobal->PSMZ = GS_PSMZ_32;
	}

	gsGlobal->DoubleBuffering = GS_SETTING_ON;
	gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
	gsGlobal->ZBuffering = GS_SETTING_ON;

	gsGlobal->Mode = GS_MODE_NTSC;
	gsGlobal->Interlace = GS_INTERLACED;
	gsGlobal->Field = GS_FIELD;
	gsGlobal->Width = 640;
	gsGlobal->Height = 448;

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	gsKit_init_screen(gsGlobal);
	gsKit_fontm_upload(gsGlobal, gsFontM);
	gsFontM->Spacing = 0.7f;

	gsKit_mode_switch(gsGlobal, GS_ONESHOT);
	gsKit_set_primalpha(gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

	gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
	gsKit_queue_exec(gsGlobal);
	gsKit_sync_flip(gsGlobal);

	gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
	gsKit_queue_exec(gsGlobal);
	gsKit_sync_flip(gsGlobal);
	
	gsTexture.Width = 640;
	gsTexture.Height = 480;
	gsTexture.PSM = GS_PSM_CT32;
	gsTexture.Vram = gsKit_vram_alloc(gsGlobal, gsKit_texture_size(gsTexture.Width, gsTexture.Height, gsTexture.PSM), GSKIT_ALLOC_USERBUFFER);
	gsTexture.Mem = textureMem;
}

void init_texture(int w, int h, int bpp) {
    float ratio, w_ratio, h_ratio, y_fix = 1.0f;
	bool fullscreen = true;

	gsTexture.Width = w;
	gsTexture.Height = h;
	if (bpp == 16) {
		gsTexture.PSM = GS_PSM_CT16;
	}
	else {
		gsTexture.PSM = GS_PSM_CT32;
	}
	gsKit_setup_tbw(&gsTexture);

	if ((gsGlobal->Interlace == GS_INTERLACED) && (gsGlobal->Field == GS_FRAME)) {
		y_fix = 0.5f;
	}
	
	w_ratio = (float)gsGlobal->Width / (float)gsTexture.Width;
	h_ratio = (float)(gsGlobal->Height / y_fix) / (float)gsTexture.Height;
	ratio = (w_ratio <= h_ratio) ? w_ratio : h_ratio;
	
	center_x = ((float)gsGlobal->Width - ((float)gsTexture.Width * ratio)) / 2;
	center_y = ((float)(gsGlobal->Height / y_fix) - ((float)gsTexture.Height * ratio)) / 2;
	
	if (fullscreen) {
		x2 = (float)gsGlobal->Width;
		y2 = (float)gsGlobal->Height;
		center_x = 0.0f;
		center_y = 0.0f;
	}
	else {
		x2 = (float)gsTexture.Width * ratio + center_x;
		y2 = (float)gsTexture.Height * y_fix * ratio + center_y;
	}
}
