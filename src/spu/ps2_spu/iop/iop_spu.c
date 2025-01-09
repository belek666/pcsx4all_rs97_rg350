


#include "types.h"
#include "ioman.h"
#include "intrman.h"
#include "loadcore.h"
#include "stdio.h"
#include "sifcmd.h"
#include "sifrpc.h"
#include "sysclib.h"
#include "sysmem.h"
#include "thbase.h"
#include "thevent.h"
#include "thsemap.h"
#include "sifman.h"
#include "vblank.h"
#include "iop_spu.h"
#include "spu2regs.h"
#include "libsd.h"
#include "sbusintr.h"
#include "upsamplers.h"
#include "hw.h"

#define SD_C_STATX(core)				((volatile u16*)(0xBF900344 + ((core) << 10))) 
#define SD_VA_LSAX_HI(core, voice)		SD_VA_REG((core), (voice), 0x04)
#define SD_VA_LSAX_LO(core, voice)		SD_VA_REG((core), (voice), 0x06)
#define SD_CORE_IRQA_HI(core)			SD_S_REG((core), 0x1C)
#define SD_CORE_IRQA_LO(core)			SD_S_REG((core), 0x1E)
#define SD_S_ADMAS(core) 				U16_REGISTER(0x1B0+(core*1024))


//#define DPRINTF(x...) printf(x)
#define DPRINTF(x...)

static int transfer_sema = 0;
static int queue_sema = 0;
static u8 sound_inited = 0;
static volatile int active_buffer = 0;
static u8 sound_core_buf[2][2048] __attribute__((aligned(16)));
static short rendered_left[512];
static short rendered_right[512];

#define ITERATIONS 20

static u8 sound_ringbuf[ITERATIONS * 2048];
static int ringbuf_size = sizeof(sound_ringbuf);
static volatile int read_pos = 0;
static volatile int write_pos = 0;
static int playing = 0;
static int sound_freq = 0;
static int sound_bits = 0;
static int sound_channels = 0;
static int format_changed = 0;

static upsampler_t upsampler = NULL;

static void  rpc_thread(void *data);
static void *rpc_sf(int cmd, void *data, int size);

static SifRpcDataQueue_t  rpc_que  __attribute__((aligned(16)));
static SifRpcServerData_t rpc_svr __attribute__((aligned(16)));

static u32 rpc_buf[RPC_BUF_SIZE / 4 + 1] __attribute((aligned(16)));

static int TransInterrupt_core0(void *data) {
	while((U16_REGISTER_READ(SD_C_STATX(SPU_CORE)) & 0x80) == 0);
	U16_REGISTER_WRITEAND(SD_CORE_ATTR(SPU_CORE), ~SD_CORE_DMA);
	U16_REGISTER_WRITE(SD_S_ADMAS(SPU_CORE), 0);
	while((U16_REGISTER_READ(SD_CORE_ATTR(SPU_CORE)) & 0x30) != 0);
	FlushDcache();
	sbus_intr_main_interrupt(SBUS_TRANSFER);
	return 1;
}

static int TransInterrupt_core1(void *data) {
	active_buffer = 1 - active_buffer;
	U32_REGISTER_WRITE(SD_DMA_ADDR(SOUND_CORE), (u32)sound_core_buf[active_buffer]);
	U16_REGISTER_WRITE(SD_DMA_SIZE(SOUND_CORE), 2048 / 64);
	U32_REGISTER_WRITE(SD_DMA_CHCR(SOUND_CORE), SD_DMA_START | SD_DMA_CS | SD_DMA_DIR_IOP2SPU);
	iSignalSema(transfer_sema);
	return 1;
}

static int Spu2Interrupt(void *data) {
	u16 val;

	(void)data;

	val = ((U16_REGISTER_READ(SD_C_IRQINFO))&0xc)>>2;
	if (!val)
		return 1;

	if (val&1)
		U16_REGISTER_WRITE(SD_CORE_ATTR(SPU_CORE), U16_REGISTER_READ(SD_CORE_ATTR(SPU_CORE)) & 0xffbf);

	if (val&2)
		U16_REGISTER_WRITE(SD_CORE_ATTR(SOUND_CORE), U16_REGISTER_READ(SD_CORE_ATTR(SOUND_CORE)) & 0xffbf);
		
	sbus_intr_main_interrupt(SBUS_INTC);
	return 1;
}

static void RegisterInterrupts(void) {
	s32 ret;

	DisableIntr(IOP_IRQ_DMA_SPU, (int *)&ret);
	DisableIntr(IOP_IRQ_DMA_SPU2, (int *)&ret);
	DisableIntr(IOP_IRQ_SPU, (int *)&ret);

	ReleaseIntrHandler(IOP_IRQ_DMA_SPU);
	ReleaseIntrHandler(IOP_IRQ_DMA_SPU2);
	ReleaseIntrHandler(IOP_IRQ_SPU);

	RegisterIntrHandler(IOP_IRQ_DMA_SPU, 1, TransInterrupt_core0, NULL);
	RegisterIntrHandler(IOP_IRQ_DMA_SPU2, 1, TransInterrupt_core1, NULL);
	RegisterIntrHandler(IOP_IRQ_SPU, 1, Spu2Interrupt, NULL);

	EnableIntr(IOP_IRQ_DMA_SPU);
	EnableIntr(IOP_IRQ_DMA_SPU2);
	EnableIntr(IOP_IRQ_SPU);
}

static void start_autodma(void) {
	if (U16_REGISTER_READ(SD_CORE_ATTR(SOUND_CORE)) & SD_DMA_IN_PROCESS || U32_REGISTER_READ(SD_DMA_CHCR(SOUND_CORE)) & SD_DMA_START) {
		return;
	}

	U16_REGISTER_WRITEAND(SD_CORE_ATTR(SOUND_CORE), 0xFFCF);

	U16_REGISTER_WRITE(SD_A_TSA_HI(SOUND_CORE), 0);
	U16_REGISTER_WRITE(SD_A_TSA_LO(SOUND_CORE), 0);

	U16_REGISTER_WRITE(SD_S_ADMAS(SOUND_CORE), 1 << SOUND_CORE);

	volatile u32 *reg = U32_REGISTER(0x1014 + (SOUND_CORE << 10));
	U32_REGISTER_WRITEOR(reg, (U32_REGISTER_READ(reg) & 0xF0FFFFFF) | 0x20000000);

	active_buffer = 0;

	U32_REGISTER_WRITE(SD_DMA_ADDR(SOUND_CORE), (u32)sound_core_buf[active_buffer]);
	U16_REGISTER_WRITE(SD_DMA_MODE(SOUND_CORE), 0x10);
	U16_REGISTER_WRITE(SD_DMA_SIZE(SOUND_CORE), 2048 / 64);
	U32_REGISTER_WRITE(SD_DMA_CHCR(SOUND_CORE), SD_DMA_CS | SD_DMA_START | SD_DMA_DIR_IOP2SPU);
}

static int get_available_space(void) {
	if (write_pos >= read_pos) {
        return ringbuf_size - (write_pos - read_pos) - 2;
    } else {
        return read_pos - write_pos - 2;
    }
}

static int get_available_data(void) {
    if (write_pos >= read_pos) {
        return write_pos - read_pos;
    } else {
        return ringbuf_size - (read_pos - write_pos);
    }
}

static int write_sound_buffer(u8 *ptr, int size) {
    int free_space = get_available_space();
    if (size > free_space) {
        size = free_space;
    }
    int first_chunk = ringbuf_size - write_pos;
    if (size <= first_chunk) {
        memcpy(sound_ringbuf + write_pos, ptr, size);
        write_pos = (write_pos + size) % ringbuf_size;
    } else {
        memcpy(sound_ringbuf + write_pos, ptr, first_chunk);
        memcpy(sound_ringbuf, ptr + first_chunk, size - first_chunk);
        write_pos = size - first_chunk;
    }
	return size;
}

static int play_audio(const char *buf, int buflen) {
	int sent = 0;

	DPRINTF("IOP_SPU: play audio size %d\n", buflen);

	if (sound_inited == 0) {
		return -1;
	}

	int read_pos = 0;
	while (buflen > 0) {
		read_pos = write_sound_buffer(buf + read_pos, buflen);
		buflen -= read_pos;
		if (buflen > 0) {
			WaitSema(queue_sema);
		}
	}
	return sent;
}

static void sound_thread(void *arg) {
	int intr_state;
	int step = 0;
	struct upsample_t up;

	while (1) {
		if (format_changed) {
			upsampler = find_upsampler(sound_freq, sound_bits, sound_channels);
			format_changed = 0;
		}

		if (playing && get_available_data() < ringbuf_size / ITERATIONS) {
			DPRINTF("IOP_SPU: stop playing, not enought data to play %d \n", get_available_data());
			playing = 0;
		}
		else if (!playing && get_available_data() >= ringbuf_size / ITERATIONS) {
			DPRINTF("IOP_SPU: start playing, avilable data %d\n", get_available_data());
			playing = 1;
		}

		if (playing && upsampler != NULL) {
			up.src = (const unsigned char *)sound_ringbuf + read_pos;
			up.left = rendered_left;
			up.right = rendered_right;
			step = upsampler(&up);
			read_pos = read_pos + step;
			if (read_pos >= ringbuf_size) {
				read_pos = 0;
			}
		}
		else {
			memset(rendered_left, '\0', sizeof(rendered_left));
			memset(rendered_right, '\0', sizeof(rendered_right));
		}
		WaitSema(transfer_sema);
		CpuSuspendIntr(&intr_state);
		int inactive_buffer = 1 - active_buffer;
		wmemcpy(sound_core_buf[inactive_buffer] + 0, rendered_left + 0, 512);
		wmemcpy(sound_core_buf[inactive_buffer] + 512, rendered_right + 0, 512);
		wmemcpy(sound_core_buf[inactive_buffer] + 1024, rendered_left + 256, 512);
		wmemcpy(sound_core_buf[inactive_buffer] + 1536, rendered_right + 256, 512);
		CpuResumeIntr(intr_state);
		if (get_available_space() >= (ringbuf_size / ITERATIONS)) {
			SignalSema(queue_sema);
		}
	}
}

static void init_sound() {
	iop_thread_t sound_th;
	transfer_sema = CreateMutex(0);
	if (transfer_sema < 0) {
		DPRINTF("IOP_SPU: Failed to allocate transfer sema\n");
		return;
	}
	queue_sema = CreateMutex(0);
	if (queue_sema < 0) {
		DPRINTF("IOP_SPU: Failed to allocate queue sema\n");
		return;
	}

	sound_th.attr = TH_C;
	sound_th.thread = sound_thread;
	sound_th.priority = 39;
	sound_th.stacksize = 0x800;
	sound_th.option = 0;

	int thid = CreateThread(&sound_th);
	if (thid > 0) {
		StartThread(thid, NULL);
	}
	else {
		DPRINTF("IOP_SPU: Failed to create sound thread\n");
		return;
	}
	U16_REGISTER_WRITE(SD_P_BVOLL(SOUND_CORE), 0x3FFF);
	U16_REGISTER_WRITE(SD_P_BVOLR(SOUND_CORE), 0x3FFF);

	start_autodma();
	sound_inited = 1;
}

static void set_sound_format(int freq, int bits, int channels) {
	int feed_size;

	playing = 0;

	if (find_upsampler(freq, bits, channels) == NULL) {
		DPRINTF("IOP_SPU: Failed to find upsampler for freq %d bits %d channels %d\n", freq, bits, channels);
		return;
	}

	int core1_sample_shift = 0;
	if (bits == 16) {
		core1_sample_shift++;
	}

	if (channels == 2) {
		core1_sample_shift++;
	}

	sound_freq = freq;
	sound_bits = bits;
	sound_channels = channels;

	feed_size = ((512 * sound_freq) / 48000) << core1_sample_shift;
	ringbuf_size = feed_size * ITERATIONS;

	write_pos = 0;
	read_pos = 0;//(feed_size * 5) & ~3;
	memset(sound_ringbuf, 0, ringbuf_size);

	DPRINTF("IOP_SPU: freq %d bits %d channels %d ringbuf_sz %d feed_size %d shift %d\n", freq, bits, channels, ringbuf_size, feed_size, core1_sample_shift);

	format_changed = 1;
}

void rpc_thread(void *data) {
	if (sceSifCheckInit() == 0) {
		DPRINTF("IOP_SPU: Sif not initialized \n");
		sceSifInit();
	}
	SifInitRpc(0);
	SifSetRpcQueue(&rpc_que, GetThreadId());
	SifRegisterRpc(&rpc_svr, IOP_SPU_BIND_RPC_ID, rpc_sf, rpc_buf, NULL, NULL, &rpc_que);
	SifRpcLoop(&rpc_que);
}

void *rpc_sf(int cmd, void *data, int size) {
	int *recv = (int *)data;
	switch (cmd) {
		case IOP_SPU_INIT_SOUND:
			init_sound();
			break;
		case IOP_SPU_SET_FORMAT:
			set_sound_format(recv[0], recv[1], recv[2]);
			break;
		case IOP_SPU_AVAIL_SPACE:
			recv[0] = get_available_space();
			break;
		case IOP_SPU_PLAY_AUDIO:
			play_audio(data, recv[RPC_BUF_SIZE / 4]);
			break;
		default:
			break;
	}
	return data;
}

int _start(int argc, char *argv[]) {
	iop_thread_t rpc_th;

	RegisterInterrupts();

	rpc_th.attr = TH_C;
	rpc_th.thread = rpc_thread;
	rpc_th.priority = 40;
	rpc_th.stacksize = 0x800;
	rpc_th.option = 0;

	int thid = CreateThread(&rpc_th);
	
	if (thid > 0) {
		StartThread(thid, NULL);
		return MODULE_RESIDENT_END;
	}
	
	return MODULE_NO_RESIDENT_END;
}

