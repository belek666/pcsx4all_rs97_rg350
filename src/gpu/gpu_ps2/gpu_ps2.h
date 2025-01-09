#include <psxcommon.h>
#include "gpu.h"

union PtrUnion
{
	uint32_t  *U4;
	int32_t  *S4;
	uint16_t  *U2;
	int16_t  *S2;
	uint8_t   *U1;
	int8_t   *S1;
	void *ptr;
};

typedef union
{
	uint32_t U4[16];
	int32_t S4[16];
	uint16_t U2[32];
	int16_t S2[32];
	uint8_t  U1[64];
	int8_t  S1[64];
} GPUPacket;

typedef struct {
	uint32_t GPU_GP1;
	GPUPacket PacketBuffer;
	uint16_t *vram;
	uint32_t TextureWindowCur;  // Current setting from last GP0(0xE2) cmd (raw form)
	uint8_t  TextureWindow[4];  // [0] : Texture window offset X
	                       // [1] : Texture window offset Y
	                       // [2] : Texture window mask X
	                       // [3] : Texture window mask Y

	uint16_t DrawingArea[4];    // [0] : Drawing area top left X
	                       // [1] : Drawing area top left Y
	                       // [2] : Drawing area bottom right X
	                       // [3] : Drawing area bottom right Y

	int16_t DrawingOffset[2];  // [0] : Drawing offset X (signed)
	                       // [1] : Drawing offset Y (signed)

	uint16_t* TBA;              // Ptr to current texture in VRAM
	uint16_t* CBA;              // Ptr to current CLUT in VRAM

	////////////////////////////////////////////////////////////////////////////
	//  Inner Loop parameters

	// 22.10 Fixed-pt texture coords, mask, scanline advance
	// NOTE: U,V are no longer packed together into one uint32_t, this proved to be
	//  too imprecise, leading to pixel dropouts.  Example: NFS3's skybox.
	uint32_t u, v;
	uint32_t u_msk, v_msk;
	int32_t u_inc, v_inc;

	// Color for Gouraud-shaded prims
	// Packed fixed-pt 8.3:8.3:8.2 rgb triplet
	//  layout:  rrrrrrrrXXXggggggggXXXbbbbbbbbXX
	//           ^ bit 31                       ^ bit 0
	uint32_t gCol;
	uint32_t gInc;          // Increment along scanline for gCol

	// Color for flat-shaded, texture-blended prims
	uint8_t  r5, g5, b5;    // 5-bit light for undithered prims
	uint8_t  r8, g8, b8;    // 8-bit light for dithered prims

	// Color for flat-shaded, untextured prims
	uint16_t PixelData;      // bgr555 color for untextured flat-shaded polys

	// End of inner Loop parameters
	////////////////////////////////////////////////////////////////////////////


	uint8_t blit_mask;           // Determines what pixels to skip when rendering.
	                        //  Only useful on low-resolution devices using
	                        //  a simple pixel-dropping downscaler for PS1
	                        //  high-res modes. See 'pixel_skip' option.

	uint8_t ilace_mask;          // Determines what lines to skip when rendering.
	                        //  Normally 0 when PS1 240 vertical res is in
	                        //  use and ilace_force is 0. When running in
	                        //  PS1 480 vertical res on a low-resolution
	                        //  device (320x240), will usually be set to 1
	                        //  so odd lines are not rendered. (Unless future
	                        //  full-screen scaling option is in use ..TODO)

	uint_fast8_t prog_ilace_flag;   // Tracks successive frames for 'prog_ilace' option

	uint8_t BLEND_MODE;
	uint8_t TEXT_MODE;
	uint8_t Masking;

	uint16_t PixelMSB;

	uint8_t  LightLUT[32*32];    // 5-bit lighting LUT (gpu_inner_light.h)
	uint32_t DitherMatrix[64];   // Matrix of dither coefficients
} gpu_ps2_t;

static gpu_ps2_t gpu_ps2;
