#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "plugins.h"
#include "decode_xa.h"

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <iopheap.h>

#include <libsd-common.h>
#include "spu2regs.h"
#include "iop/iop_spu.h"

#define EFFECT_REGISTER_WRITE(x, y)		*((volatile u16 *) (x)) = (u16)(y >> 14); *((volatile u16 *)(x) + 1) = (u16)(y << 2)

#define DMA_BUF_SIZE 4096 * 4

static u8 dmabuf[DMA_BUF_SIZE] __attribute__((aligned(64)));

static u16 spu_data_transfer_ctr_reg = 0x0004;
static volatile u32 spu_address;
static int transfer_sema = 0;

static SifRpcClientData_t iopspu;
static int rpcbuf[RPC_BUF_SIZE / 4 + 1] __attribute__((aligned(64)));
static u8 iop_spu_inited = 0;

static void init_spu2(void);
static void init_spu(void);
static void spu_iop_init(void);
static void spu_iop_init_sound(void);
static void spu_iop_set_format(int freq, int bits, int channels);
static int spu_iop_avail_space(void);
static void spu_iop_play_audio(void *ptr, int size);

static int sound_freq, sound_bits, sound_channels;

static void handler(int irq) {
	switch (irq) {
		case SBUS_INTC: Trigger_SPU_IRQ(); break;
		case SBUS_TRANSFER: iSignalSema(transfer_sema); break;
		default:
			break;
	}
	asm("sync\nei\n");
}

static u16 read_spu2_address(volatile u16 *reg) {
	u32 spu2_addr = ((U16_REGISTER_READ(reg) & 0x3F) << 16) | U16_REGISTER_READ(reg + 1);
	return (spu2_addr - (spu2_addr >= 0xc0000 ? 0xc0000 : 0)) / 4;
}

static void write_spu2_address(volatile u16 *reg, u16 addr) {
	u32 spu2_addr = addr * 4 + (addr >= 0x200 ? 0xc0000 : 0);
	U16_REGISTER_WRITE(reg, (u16)(spu2_addr >> 16) & 0x003F);
	U16_REGISTER_WRITE(reg + 1, (u16)(spu2_addr & 0xFFFF));
}

long SPU_init(void) {
	init_spu2();
	init_spu();
	AddSbusIntcHandler(SBUS_INTC, handler);
	AddSbusIntcHandler(SBUS_TRANSFER, handler);
	SifInitIopHeap();
	spu_iop_init();
	spu_iop_init_sound();
	return 0;
}

long SPU_shutdown(void) {
    return 0;
}

long SPU_open(void) {
    return 0;
}

long SPU_close(void) {
    return 0;
}

void SPU_writeRegister(unsigned long reg, unsigned short val, unsigned int unk) {
	int r = reg & 0xFFF;
	volatile u16 *reg_addr = NULL;
	int full_addr = 0;

	//printf("SPU_writeRegister 0x%08lx 0x%04x\n", reg, val);

	if (r >= 0x0C00 && r < 0x0D80) { //SPU Voice 0..23 Registers
		int ch = (r >> 4) - 0xC0;
		switch (r & 0x0F) {
			case 0x00: reg_addr = SD_VP_VOLL(SPU_CORE, ch); break;
			case 0x02: reg_addr = SD_VP_VOLR(SPU_CORE, ch); break;
			case 0x04: reg_addr = SD_VP_PITCH(SPU_CORE, ch); val = (u16)(((u32)val * 44100) / 48000); break;
			case 0x06: write_spu2_address(SD_VA_SSA_HI(SPU_CORE, ch), val); break;
			case 0x08: reg_addr = SD_VP_ADSR1(SPU_CORE, ch); break;
			case 0x0A: reg_addr = SD_VP_ADSR2(SPU_CORE, ch); break;
			case 0x0C: reg_addr = SD_VP_ENVX(SPU_CORE, ch); break;
			case 0x0E: write_spu2_address(SD_VA_LSAX_HI(SPU_CORE, ch), val); break;
		}
	}

	if (r >= 0x0D80 && r < 0x0DC0) { //SPU Control Registers
		switch (r & 0xFF) {
			case 0x80: reg_addr = SD_P_MVOLL(SPU_CORE); break;
			case 0x82: reg_addr = SD_P_MVOLR(SPU_CORE); break;
			case 0x84: reg_addr = SD_P_EVOLL(SPU_CORE); break;
			case 0x86: reg_addr = SD_P_EVOLR(SPU_CORE); break;
			case 0x88: reg_addr = SD_A_KON_HI(SPU_CORE); break;
			case 0x8A: reg_addr = SD_A_KON_LO(SPU_CORE); break;
			case 0x8C: reg_addr = SD_A_KOFF_HI(SPU_CORE); break;
			case 0x8E: reg_addr = SD_A_KOFF_LO(SPU_CORE); break;
			case 0x90: reg_addr = SD_S_PMON_HI(SPU_CORE); break;
			case 0x92: reg_addr = SD_S_PMON_LO(SPU_CORE); break;
			case 0x94: reg_addr = SD_S_NON_HI(SPU_CORE); break;
			case 0x96: reg_addr = SD_S_NON_LO(SPU_CORE); break;
			case 0x98: U16_REGISTER_WRITE(SD_S_VMIXEL_HI(SPU_CORE), val); reg_addr = SD_S_VMIXER_HI(SPU_CORE); break;
			case 0x9A: U16_REGISTER_WRITE(SD_S_VMIXEL_LO(SPU_CORE), val); reg_addr = SD_S_VMIXER_LO(SPU_CORE); break;
			case 0x9C: reg_addr = SD_S_ENDX_HI(SPU_CORE); break;
			case 0x9E: reg_addr = SD_S_ENDX_LO(SPU_CORE); break;
			case 0xA0: break;
			case 0xA2: write_spu2_address(SD_A_ESA_HI(SPU_CORE), val); break;
			case 0xA4: write_spu2_address(SD_CORE_IRQA_HI(SPU_CORE), val); break;
			case 0xA6: write_spu2_address(SD_A_TSA_HI(SPU_CORE), val); spu_address = val * 8; break;
			case 0xA8: 
				reg_addr = SD_A_STD(SPU_CORE); 
				spu_address += 2;
				if (spu_address > 0x7FFFF) {
					spu_address = 0;
					write_spu2_address(SD_A_TSA_HI(SPU_CORE), 0);
				}
				break;
			case 0xAA: reg_addr = SD_CORE_ATTR(SPU_CORE); break;
			case 0xAC: spu_data_transfer_ctr_reg = val; break;
			case 0xAE: reg_addr = SD_C_STATX(SPU_CORE); break;
			case 0xB0: reg_addr = SD_P_BVOLL(SOUND_CORE); break;
			case 0xB2: reg_addr = SD_P_BVOLR(SOUND_CORE); break;
			case 0xB4: reg_addr = SD_P_AVOLL(SPU_CORE); break;
			case 0xB6: reg_addr = SD_P_AVOLR(SPU_CORE); break;
			case 0xB8: reg_addr = SD_P_MVOLXL(SPU_CORE); break;
			case 0xBA: reg_addr = SD_P_MVOLXR(SPU_CORE); break;
			case 0xBC: break;
			case 0xBE: break;
		}
	}

	if (r >= 0x0DC0 && r < 0x0E00) { //SPU Reverb Configuration Area
		switch (r & 0xFF) {
			case 0xC0: EFFECT_REGISTER_WRITE(SD_R_FB_SRC_A(SPU_CORE), val); break;
			case 0xC2: EFFECT_REGISTER_WRITE(SD_R_FB_SRC_B(SPU_CORE), val); break;
			case 0xC4: reg_addr = SD_R_IIR_ALPHA(SPU_CORE); break;
			case 0xC6: reg_addr = SD_R_ACC_COEF_A(SPU_CORE); break;
			case 0xC8: reg_addr = SD_R_ACC_COEF_B(SPU_CORE); break;
			case 0xCA: reg_addr = SD_R_ACC_COEF_C(SPU_CORE); break;
			case 0xCC: reg_addr = SD_R_ACC_COEF_D(SPU_CORE); break;
			case 0xCE: reg_addr = SD_R_IIR_COEF(SPU_CORE); break;
			case 0xD0: reg_addr = SD_R_FB_ALPHA(SPU_CORE); break;
			case 0xD2: reg_addr = SD_R_FB_X(SPU_CORE); break;
			case 0xD4: EFFECT_REGISTER_WRITE(SD_R_IIR_DEST_A0(SPU_CORE), val); break;
			case 0xD6: EFFECT_REGISTER_WRITE(SD_R_IIR_DEST_A1(SPU_CORE), val); break;
			case 0xD8: EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_A0(SPU_CORE), val); break;
			case 0xDA: EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_A1(SPU_CORE), val); break;
			case 0xDC: EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_B0(SPU_CORE), val); break;
			case 0xDE: EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_B1(SPU_CORE), val); break;
			case 0xE0: EFFECT_REGISTER_WRITE(SD_R_IIR_SRC_A0(SPU_CORE), val); break;
			case 0xE2: EFFECT_REGISTER_WRITE(SD_R_IIR_SRC_A1(SPU_CORE), val); break;
			case 0xE4: EFFECT_REGISTER_WRITE(SD_R_IIR_DEST_B0(SPU_CORE), val); break;
			case 0xE6: EFFECT_REGISTER_WRITE(SD_R_IIR_DEST_B1(SPU_CORE), val); break;
			case 0xE8: EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_C0(SPU_CORE), val); break;
			case 0xEA: EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_C1(SPU_CORE), val); break;
			case 0xEC: EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_D0(SPU_CORE), val); break;
			case 0xEE: EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_D1(SPU_CORE), val); break;
			case 0xF0: EFFECT_REGISTER_WRITE(SD_R_IIR_SRC_B1(SPU_CORE), val); break;
			case 0xF2: EFFECT_REGISTER_WRITE(SD_R_IIR_SRC_B0(SPU_CORE), val); break;
			case 0xF4: EFFECT_REGISTER_WRITE(SD_R_MIX_DEST_A0(SPU_CORE), val); break;
			case 0xF6: EFFECT_REGISTER_WRITE(SD_R_MIX_DEST_A1(SPU_CORE), val); break;
			case 0xF8: EFFECT_REGISTER_WRITE(SD_R_MIX_DEST_B0(SPU_CORE), val); break;
			case 0xFA: EFFECT_REGISTER_WRITE(SD_R_MIX_DEST_B1(SPU_CORE), val); break;
			case 0xFC: reg_addr = SD_R_IN_COEF_L(SPU_CORE); break;
			case 0xFE: reg_addr = SD_R_IN_COEF_R(SPU_CORE); break;
		}
	}

	if (reg_addr != NULL) {
		U16_REGISTER_WRITE(reg_addr, val);
	}
}

unsigned short SPU_readRegister(unsigned long reg) {
	int r = reg & 0xFFF;
	volatile u16 *reg_addr = NULL;
	u16 ret_val = 0;
	int full_addr = 0;

	if (r >= 0x0C00 && r < 0x0D80) { //SPU Voice 0..23 Registers
		int ch = (r >> 4) - 0xC0;
		switch (r & 0x0F) {
			case 0x00: reg_addr = SD_VP_VOLXL(SPU_CORE, ch); break;
			case 0x02: reg_addr = SD_VP_VOLXR(SPU_CORE, ch); break;
			case 0x04: ret_val = U16_REGISTER_READ(SD_VP_PITCH(SPU_CORE, ch)); ret_val = (u16)(((u32)ret_val * 48000) / 44100); break;
			case 0x06: ret_val = read_spu2_address(SD_VA_SSA_HI(SPU_CORE, ch)); break;
			case 0x08: reg_addr = SD_VP_ADSR1(SPU_CORE, ch); break;
			case 0x0A: reg_addr = SD_VP_ADSR2(SPU_CORE, ch); break;
			case 0x0C: reg_addr = SD_VP_ENVX(SPU_CORE, ch); break;
			case 0x0E: ret_val = read_spu2_address(SD_VA_LSAX_HI(SPU_CORE, ch)); break;
		}
	}

	if (r >= 0x0D80 && r < 0x0DC0) { //SPU Control Registers
		switch (r & 0xFF) {
			case 0x80: reg_addr = SD_P_MVOLL(SPU_CORE); break;
			case 0x82: reg_addr = SD_P_MVOLR(SPU_CORE); break;
			case 0x84: reg_addr = SD_P_EVOLL(SPU_CORE); break;
			case 0x86: reg_addr = SD_P_EVOLR(SPU_CORE); break;
			case 0x88: reg_addr = SD_A_KON_HI(SPU_CORE); break;
			case 0x8A: reg_addr = SD_A_KON_LO(SPU_CORE); break;
			case 0x8C: reg_addr = SD_A_KOFF_HI(SPU_CORE); break;
			case 0x8E: reg_addr = SD_A_KOFF_LO(SPU_CORE); break;
			case 0x90: reg_addr = SD_S_PMON_HI(SPU_CORE); break;
			case 0x92: reg_addr = SD_S_PMON_LO(SPU_CORE); break;
			case 0x94: reg_addr = SD_S_NON_HI(SPU_CORE); break;
			case 0x96: reg_addr = SD_S_NON_LO(SPU_CORE); break;
			case 0x98: reg_addr = SD_S_VMIXEL_HI(SPU_CORE); break;
			case 0x9A: reg_addr = SD_S_VMIXEL_LO(SPU_CORE); break;
			case 0x9C: reg_addr = SD_S_ENDX_HI(SPU_CORE); break;
			case 0x9E: reg_addr = SD_S_ENDX_LO(SPU_CORE); break;
			case 0xA0: break;
			case 0xA2: ret_val = read_spu2_address(SD_A_ESA_HI(SPU_CORE)); break;
			case 0xA4: ret_val = read_spu2_address(SD_CORE_IRQA_HI(SPU_CORE)); break;
			case 0xA6: ret_val = read_spu2_address(SD_A_TSA_HI(SPU_CORE)); break;
			case 0xA8: 
				ret_val = U16_REGISTER_READ(SD_A_STD(SPU_CORE)); 
				spu_address += 2;
				if (spu_address > 0x7FFFF) {
					spu_address = 0;
					write_spu2_address(SD_A_TSA_HI(SPU_CORE), 0);
				}
				break;
			case 0xAA: reg_addr = SD_CORE_ATTR(SPU_CORE); break;
			case 0xAC: ret_val = spu_data_transfer_ctr_reg; break;
			case 0xAE: reg_addr = SD_C_STATX(SPU_CORE); break;
			case 0xB0: reg_addr = SD_P_BVOLL(SOUND_CORE); break;
			case 0xB2: reg_addr = SD_P_BVOLR(SOUND_CORE); break;
			case 0xB4: reg_addr = SD_P_AVOLL(SPU_CORE); break;
			case 0xB6: reg_addr = SD_P_AVOLR(SPU_CORE); break;
			case 0xB8: reg_addr = SD_P_MVOLXL(SPU_CORE); break;
			case 0xBA: reg_addr = SD_P_MVOLXR(SPU_CORE); break;
			case 0xBC: break;
			case 0xBE: break;
		}
	}

	if (r >= 0x0DC0 && r < 0x0E00) { //SPU Reverb Configuration Area
		printf("SPU Reverb Configuration Area read %x\n", r);
	}

	if (reg_addr != NULL) {
		ret_val = U16_REGISTER_READ(reg_addr);
	}

	//printf("SPU_readRegister 0x%08lx 0x%04x\n", reg, ret_val);

    return ret_val;
}

void SPU_readDMAMem(unsigned short *ptr, int size, unsigned int unk) {
	//printf("SPU_readDMAMem 0x%08x size %d\n", ptr, size);
	int id, full_size;
	SifDmaTransfer_t sifdma;
	void *iop_addr;

	full_size = size * 2;
	iop_addr = SifAllocIopHeap(full_size);
	if (iop_addr == 0) {
		printf("Failed to allocate iop memory\n");
		return;
	}

	U16_REGISTER_WRITE(SD_CORE_ATTR(SPU_CORE), (U16_REGISTER_READ(SD_CORE_ATTR(SPU_CORE)) & ~SD_CORE_DMA) | SD_DMA_READ);

	volatile u32 *reg = U32_REGISTER(0x1014 + (SPU_CORE << 10));
	U32_REGISTER_WRITEOR(reg, (U32_REGISTER_READ(reg) & 0xF0FFFFFF) | 0x22000000);

	U32_REGISTER_WRITE(SD_DMA_ADDR(SPU_CORE), (u32)iop_addr);
	U16_REGISTER_WRITE(SD_DMA_MODE(SPU_CORE), 0x10);
	U16_REGISTER_WRITE(SD_DMA_SIZE(SPU_CORE), (full_size/64)+((full_size&63)>0));
	U32_REGISTER_WRITE(SD_DMA_CHCR(SPU_CORE), SD_DMA_CS | SD_DMA_START | SD_DMA_DIR_SPU2IOP);
	
	WaitSema(transfer_sema);

	int size_to_read = full_size;
	int write_ptr = 0;
	while (size_to_read > 0) {
		int chunk = size_to_read > DMA_BUF_SIZE ? DMA_BUF_SIZE : size_to_read;
		rpcbuf[0] = chunk;
		size_to_read -= chunk;
		
		sifdma.src = iop_addr + write_ptr;
		sifdma.dest = dmabuf;
		sifdma.size = chunk;
		sifdma.attr = 0;
		while ((id = SifSetDma(&sifdma, 1)) == 0);
		while (SifDmaStat(id) >= 0);

		memcpy((u8 *)ptr + write_ptr, dmabuf, chunk);
		write_ptr += chunk;
	}

	SifFreeIopHeap(iop_addr);
}

void SPU_writeDMAMem(unsigned short *ptr, int size, unsigned int unk) {
	int id, full_size;
	SifDmaTransfer_t sifdma;
	void *iop_addr;

	u32 current_tsa = ((U16_REGISTER_READ(SD_A_TSA_HI(SPU_CORE)) & 0x3F) << 16) | U16_REGISTER_READ(SD_A_TSA_LO(SPU_CORE));

	printf("SPU_writeDMAMem 0x%08x size %d TSA=0x%08x\n", ptr, size, current_tsa);
	
	full_size = size * 2;
	iop_addr = SifAllocIopHeap(full_size);
	if (iop_addr == 0) {
		printf("Failed to allocate iop memory\n");
		return;
	}
	
	int size_to_write = full_size;
	int read_ptr = 0;
	while (size_to_write > 0) {
		int chunk = size_to_write > DMA_BUF_SIZE ? DMA_BUF_SIZE : size_to_write;
		size_to_write -= chunk;
		memcpy(UNCACHED_SEG(dmabuf), (u8 *)ptr + read_ptr, chunk);
			
		sifdma.src = dmabuf;
		sifdma.dest = iop_addr + read_ptr;
		sifdma.size = chunk;
		sifdma.attr = 0;

		while ((id = SifSetDma(&sifdma, 1)) == 0);
		while (SifDmaStat(id) >= 0);
		read_ptr += chunk;
	}

	if (U32_REGISTER_READ(SD_DMA_CHCR(SPU_CORE)) & SD_DMA_START) {
		printf("dma write already started\n");
		return;
	}

	U16_REGISTER_WRITE(SD_CORE_ATTR(SPU_CORE), (U16_REGISTER_READ(SD_CORE_ATTR(SPU_CORE)) & ~SD_CORE_DMA) | SD_DMA_WRITE);

	volatile u32 *reg = U32_REGISTER(0x1014 + (SPU_CORE << 10));
	U32_REGISTER_WRITEOR(reg, (U32_REGISTER_READ(reg) & 0xF0FFFFFF) | 0x20000000);

	U32_REGISTER_WRITE(SD_DMA_ADDR(SPU_CORE), (u32)iop_addr);
	U16_REGISTER_WRITE(SD_DMA_MODE(SPU_CORE), 0x10);
	U16_REGISTER_WRITE(SD_DMA_SIZE(SPU_CORE), (full_size/64)+((full_size&63)>0));
	U32_REGISTER_WRITE(SD_DMA_CHCR(SPU_CORE), SD_DMA_CS | SD_DMA_START | SD_DMA_DIR_IOP2SPU);

	WaitSema(transfer_sema);

	SifFreeIopHeap(iop_addr);
}

long SPU_freeze(unsigned long ulFreezeMode,SPUFreeze_t * pF, uint32_t unk) {
	return 1;
}

void SPU_async(uint32_t length, uint32_t unk) {
	//printf("SPU_async %d\n", length);
}

void SPU_playADPCMchannel(xa_decode_t *xap) {
	//printf("SPU_playADPCMchannel %d\n", xap->nsamples);
	int channels = xap->stereo ? 2 : 1;
	if (sound_freq != xap->freq || sound_channels != channels) {
		sound_freq = xap->freq;
		sound_channels = channels;
		spu_iop_set_format(sound_freq, 16, sound_channels);
	}
	spu_iop_play_audio(xap->pcm, xap->nsamples * channels * 2);
}

int SPU_playCDDAchannel(short *pcm, int nbytes) {
	if (!pcm)      return -1;
	if (nbytes<=0) return -1;

	//printf("SPU_playCDDAchannel %d\n", nbytes);

	if (sound_freq != 44100 || sound_channels != 2) {
		sound_freq = 44100;
		sound_channels = 2;
		spu_iop_set_format(sound_freq, 16, sound_channels);
	}
	spu_iop_play_audio(pcm, nbytes);
	return 1;
}

unsigned int SPU_getADPCMBufferRoom(void) {
   return spu_iop_avail_space() / 2;
}

static u16 VoiceDataInit[16] = { 0x707, 0x707, 0x707, 0x707, 0x707, 0x707, 0x707, 0x707, 0, 0, 0, 0, 0, 0, 0, 0 };

static void init_spu2() {
	U32_REGISTER_WRITE(U32_REGISTER(0x1404), 0xBF900000);
	U32_REGISTER_WRITE(U32_REGISTER(0x140C), 0xBF900800);
	U32_REGISTER_WRITEOR(U32_REGISTER(0x10F0), 0x80000);
	U32_REGISTER_WRITEOR(U32_REGISTER(0x1570), 8);
	U32_REGISTER_WRITE(U32_REGISTER(0x1014), 0x200B31E1);
	U32_REGISTER_WRITE(U32_REGISTER(0x1414), 0x200B31E1);

	U16_REGISTER_WRITE(SD_C_SPDIF_MODE, 0x900);
	U16_REGISTER_WRITE(SD_C_SPDIF_MEDIA, 0x200);
	U16_REGISTER_WRITE(U16_REGISTER(0x7CA), 8);

	U16_REGISTER_WRITE(SD_C_SPDIF_OUT, 0);
	nopdelay();
	U16_REGISTER_WRITE(SD_C_SPDIF_OUT, 0x8000);
	nopdelay();

	U32_REGISTER_WRITEOR(U32_REGISTER(0x10F0), 0xB0000);

	for (u32 core = 0; core < 2; core++) {
		U16_REGISTER_WRITE(SD_S_ADMAS(core), 0);
		U16_REGISTER_WRITE(SD_CORE_ATTR(core), 0);
		nopdelay();
		U16_REGISTER_WRITE(SD_CORE_ATTR(core), SD_SPU2_ON);
		U16_REGISTER_WRITE(SD_P_MVOLL(core), 0);
		U16_REGISTER_WRITE(SD_P_MVOLR(core), 0);

		while (U16_REGISTER_READ(SD_C_STATX(core)) & 0x7FF);

		U16_REGISTER_WRITE(SD_A_KOFF_HI(core), 0xFFFF);
		U16_REGISTER_WRITE(SD_A_KOFF_LO(core), 0xFFFF);
		nopdelay();

		U16_REGISTER_WRITE(SD_S_VMIXL_HI(core), 0xFFFF);
		U16_REGISTER_WRITE(SD_S_VMIXL_LO(core), 0xFF);
		U16_REGISTER_WRITE(SD_S_VMIXEL_HI(core), 0xFFFF);
		U16_REGISTER_WRITE(SD_S_VMIXEL_LO(core), 0xFF);
		U16_REGISTER_WRITE(SD_S_VMIXR_HI(core), 0xFFFF);
		U16_REGISTER_WRITE(SD_S_VMIXR_LO(core), 0xFF);
		U16_REGISTER_WRITE(SD_S_VMIXER_HI(core), 0xFFFF);
		U16_REGISTER_WRITE(SD_S_VMIXER_LO(core), 0xFF);
		nopdelay();

		U16_REGISTER_WRITE(SD_P_MVOLL(core), 0);
		U16_REGISTER_WRITE(SD_P_MVOLR(core), 0);
		U16_REGISTER_WRITE(SD_P_EVOLL(core), 0);
		U16_REGISTER_WRITE(SD_P_EVOLR(core), 0);
		U16_REGISTER_WRITE(SD_P_AVOLL(core), 0);
		U16_REGISTER_WRITE(SD_P_AVOLR(core), 0);
		U16_REGISTER_WRITE(SD_P_BVOLL(core), 0);
		U16_REGISTER_WRITE(SD_P_BVOLR(core), 0);
		U16_REGISTER_WRITE(SD_P_MVOLXL(core), 0);
		U16_REGISTER_WRITE(SD_P_MVOLXR(core), 0);
		nopdelay();

		U16_REGISTER_WRITE(SD_A_TSA_HI(core), 0);
		U16_REGISTER_WRITE(SD_A_TSA_LO(core), 0x5000 >> 1);
		nopdelay();

		for (int i = 0; i < 16; i++) {
			U16_REGISTER_WRITE(SD_A_STD(core), VoiceDataInit[i]);
		}
		U16_REGISTER_WRITE(SD_CORE_ATTR(core), (U16_REGISTER_READ(SD_CORE_ATTR(core)) & ~SD_CORE_DMA) | SD_DMA_IO);
		while (U16_REGISTER_READ(SD_C_STATX(core)) & SD_IO_IN_PROCESS);
		U16_REGISTER_WRITEAND(SD_CORE_ATTR(core), ~SD_CORE_DMA);

		for (u32 voice = 0; voice < 24; voice++) {
			U16_REGISTER_WRITE(SD_VP_VOLL(core, voice), 0);
			U16_REGISTER_WRITE(SD_VP_VOLR(core, voice), 0);
			U16_REGISTER_WRITE(SD_VP_PITCH(core, voice), 0x3FFF);
			U16_REGISTER_WRITE(SD_VP_ADSR1(core, voice), 0);
			U16_REGISTER_WRITE(SD_VP_ADSR2(core, voice), 0);
			U16_REGISTER_WRITE(SD_VA_SSA_HI(core, voice), 0);
			U16_REGISTER_WRITE(SD_VA_SSA_LO(core, voice), 0x5000 >> 1);
		}

		U16_REGISTER_WRITE(SD_A_KON_HI(core), 0xFFFF);
		U16_REGISTER_WRITE(SD_A_KON_LO(core), 0xFF);
		nopdelay();
		U16_REGISTER_WRITE(SD_A_KOFF_HI(core), 0xFFFF);
		U16_REGISTER_WRITE(SD_A_KOFF_LO(core), 0xFF);
		nopdelay();
		U16_REGISTER_WRITE(SD_S_ENDX_HI(core), 0);
		U16_REGISTER_WRITE(SD_S_ENDX_LO(core), 0);

		EFFECT_REGISTER_WRITE(SD_R_FB_SRC_A(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_FB_SRC_B(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_IIR_DEST_A0(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_IIR_DEST_A1(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_A0(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_A1(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_B0(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_B1(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_IIR_SRC_A0(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_IIR_SRC_A1(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_IIR_DEST_B0(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_IIR_DEST_B1(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_C0(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_C1(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_D0(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_ACC_SRC_D1(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_IIR_SRC_B1(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_IIR_SRC_B0(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_MIX_DEST_A0(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_MIX_DEST_A1(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_MIX_DEST_B0(core), 0);
		EFFECT_REGISTER_WRITE(SD_R_MIX_DEST_B1(core), 0);

		nopdelay();
	}

	U16_REGISTER_WRITE(SD_C_SPDIF_OUT, 0xC032);
}

static void init_spu(void) {
	ee_sema_t compSema;

	U16_REGISTER_WRITE(SD_CORE_ATTR(0), SD_SPU2_ON | SD_ENABLE_EFFECTS | SD_MUTE);
	U16_REGISTER_WRITE(SD_CORE_ATTR(1), SD_SPU2_ON | SD_ENABLE_EX_INPUT | SD_MUTE | 0xF);
	
	U16_REGISTER_WRITE(SD_P_MMIX(0), 0xFF0);
	U16_REGISTER_WRITE(SD_P_MMIX(1), 0xFFF);
#if SPU_CORE
	U16_REGISTER_WRITE(SD_P_MMIX(1), 0xFF0);
	U16_REGISTER_WRITE(SD_A_ESA_HI(0), 0x07);
	U16_REGISTER_WRITE(SD_A_ESA_LO(0), 0xFFF8);
	U16_REGISTER_WRITE(SD_A_EEA_HI(0), 0x0B);
#else
	U16_REGISTER_WRITE(SD_P_AVOLL(1), 0x7FFF);
	U16_REGISTER_WRITE(SD_P_AVOLR(1), 0x7FFF);
	U16_REGISTER_WRITE(SD_P_MVOLL(1), 0x3FFF);
	U16_REGISTER_WRITE(SD_P_MVOLR(1), 0x3FFF);

	U16_REGISTER_WRITE(SD_P_MMIX(1), 0xCC);
	U16_REGISTER_WRITE(SD_A_ESA_HI(1), 0x07);
	U16_REGISTER_WRITE(SD_A_ESA_LO(1), 0xFFF8);
	U16_REGISTER_WRITE(SD_A_EEA_HI(1), 0x0B);
#endif
	U16_REGISTER_WRITE(SD_A_EEA_HI(SPU_CORE), 0x0F);
	U16_REGISTER_WRITE(SD_S_ADMAS(0), 0);
	U16_REGISTER_WRITE(SD_S_ADMAS(1), 0);

	compSema.init_count = 0;
	compSema.max_count = 1;
	compSema.option = 0;
	transfer_sema = CreateSema(&compSema);
}

static void spu_iop_init(void) {
	iopspu.server = NULL;
	do {
		if (SifBindRpc(&iopspu, IOP_SPU_BIND_RPC_ID, 0) < 0) {
			return;
		}
		nopdelay();
	} while (!iopspu.server);
	iop_spu_inited = 1;
}

static void spu_iop_init_sound(void) {
	if (!iop_spu_inited) {
		return;
	}
	SifCallRpc(&iopspu, IOP_SPU_INIT_SOUND, 0, NULL, 0, NULL, 0, NULL, NULL);
}

static void spu_iop_set_format(int freq, int bits, int channels) {
	if (!iop_spu_inited) {
		return;
	}
	rpcbuf[0] = freq;
	rpcbuf[1] = bits;
	rpcbuf[2] = channels;
	SifCallRpc(&iopspu, IOP_SPU_SET_FORMAT, 0, rpcbuf, 4 * 3, NULL, 0, NULL, NULL);
}

static int spu_iop_avail_space(void) {
	if (!iop_spu_inited) {
		return 0;
	}
	SifCallRpc(&iopspu, IOP_SPU_AVAIL_SPACE, 0, NULL, 0, rpcbuf, 4, NULL, NULL);
	return rpcbuf[0];
}

static void spu_iop_play_audio(void *ptr, int size) {
	if (!iop_spu_inited) {
		return;
	}
	int size_to_write = size;
	int read_ptr = 0;
	while (size_to_write > 0) {
		int chunk = size_to_write > RPC_BUF_SIZE ? RPC_BUF_SIZE : size_to_write;
		rpcbuf[RPC_BUF_SIZE / 4] = chunk;
		size_to_write -= chunk;
		memcpy(UNCACHED_SEG(rpcbuf), (u8 *)ptr + read_ptr, chunk);
		SifCallRpc(&iopspu, IOP_SPU_PLAY_AUDIO, 0, rpcbuf, RPC_BUF_SIZE + 4, NULL, 0, NULL, NULL);
		read_ptr += chunk;
	}
}
