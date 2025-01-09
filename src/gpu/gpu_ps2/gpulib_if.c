#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpulib/gpu.h"
#include "port.h"
#include "gpu_ps2.h"

//#define DPRINTF(x...) DPRINTF(x)
#define DPRINTF(x...)

#define FIXED_BITS 10

extern const unsigned char cmd_lengths[256];

int renderer_init(void) {
    memset((void*)&gpu_ps2, 0, sizeof(gpu_ps2));
    gpu_ps2.vram = (uint16_t*)gpu.vram;

    // Original standalone gpu_ps2 initialized TextureWindow[]. I added the
    //  same behavior here, since it seems unsafe to leave [2],[3] unset when
    //  using HLE and Rearmed gpu_neon sets this similarly on init. -senquack
    gpu_ps2.TextureWindow[0] = 0;
    gpu_ps2.TextureWindow[1] = 0;
    gpu_ps2.TextureWindow[2] = 255;
    gpu_ps2.TextureWindow[3] = 255;
    //senquack - new vars must be updated whenever texture window is changed:
    //           (used for polygon-drawing in gpu_inner.h, gpu_raster_polygon.h)
    const uint32_t fb = FIXED_BITS;  // # of fractional fixed-pt bits of u4/v4
    gpu_ps2.u_msk = (((uint32_t)gpu_ps2.TextureWindow[2]) << fb) | ((1 << fb) - 1);
    gpu_ps2.v_msk = (((uint32_t)gpu_ps2.TextureWindow[3]) << fb) | ((1 << fb) - 1);
    return 0;
}

// Handles GP0 draw settings commands 0xE1...0xE6
static void gpuGP0Cmd_0xEx(uint32_t cmd_word) {
    // Assume incoming GP0 command is 0xE1..0xE6, convert to 1..6
    uint8_t num = (cmd_word >> 24) & 7;
    gpu.ex_regs[num] = cmd_word; // Update gpulib register
    switch (num) {
        case 1: {
        // GP0(E1h) - Draw Mode setting (aka "Texpage")
        uint32_t cur_texpage = gpu_ps2.GPU_GP1 & 0x7FF;
        uint32_t new_texpage = cmd_word & 0x7FF;
        if (cur_texpage != new_texpage) {
            gpu_ps2.GPU_GP1 = (gpu_ps2.GPU_GP1 & ~0x7FF) | new_texpage;
            //gpuSetTexture(gpu_ps2.GPU_GP1);
        }
    } break;

    case 2: {
        // GP0(E2h) - Texture Window setting
        if (cmd_word != gpu_ps2.TextureWindowCur) {
            static const uint8_t TextureMask[32] = {
            255, 7, 15, 7, 31, 7, 15, 7, 63, 7, 15, 7, 31, 7, 15, 7,
            127, 7, 15, 7, 31, 7, 15, 7, 63, 7, 15, 7, 31, 7, 15, 7
            };
            gpu_ps2.TextureWindowCur = cmd_word;
            gpu_ps2.TextureWindow[0] = ((cmd_word >> 10) & 0x1F) << 3;
            gpu_ps2.TextureWindow[1] = ((cmd_word >> 15) & 0x1F) << 3;
            gpu_ps2.TextureWindow[2] = TextureMask[(cmd_word >> 0) & 0x1F];
            gpu_ps2.TextureWindow[3] = TextureMask[(cmd_word >> 5) & 0x1F];
            gpu_ps2.TextureWindow[0] &= ~gpu_ps2.TextureWindow[2];
            gpu_ps2.TextureWindow[1] &= ~gpu_ps2.TextureWindow[3];

            // Inner loop vars must be updated whenever texture window is changed:
            const uint32_t fb = FIXED_BITS;  // # of fractional fixed-pt bits of u4/v4
            gpu_ps2.u_msk = (((uint32_t)gpu_ps2.TextureWindow[2]) << fb) | ((1 << fb) - 1);
            gpu_ps2.v_msk = (((uint32_t)gpu_ps2.TextureWindow[3]) << fb) | ((1 << fb) - 1);

            //gpuSetTexture(gpu_ps2.GPU_GP1);
        }
    } break;

    case 3: {
        // GP0(E3h) - Set Drawing Area top left (X1,Y1)
        gpu_ps2.DrawingArea[0] = cmd_word         & 0x3FF;
        gpu_ps2.DrawingArea[1] = (cmd_word >> 10) & 0x3FF;
    } break;

    case 4: {
        // GP0(E4h) - Set Drawing Area bottom right (X2,Y2)
        gpu_ps2.DrawingArea[2] = (cmd_word         & 0x3FF) + 1;
        gpu_ps2.DrawingArea[3] = ((cmd_word >> 10) & 0x3FF) + 1;
    } break;

    case 5: {
        // GP0(E5h) - Set Drawing Offset (X,Y)
        gpu_ps2.DrawingOffset[0] = ((int32_t)cmd_word<<(32-11))>>(32-11);
        gpu_ps2.DrawingOffset[1] = ((int32_t)cmd_word<<(32-22))>>(32-11);
    } break;

    case 6: {
        // GP0(E6h) - Mask Bit Setting
        gpu_ps2.Masking  = (cmd_word & 0x2) <<  1;
        gpu_ps2.PixelMSB = (cmd_word & 0x1) <<  8;
    } break;
  }
}

int do_cmd_list(uint32_t *list, int list_len, int *last_cmd) {
    uint32_t cmd = 0, len, i;
    uint32_t *list_start = list;
    uint32_t *list_end = list + list_len;

    for (; list < list_end; list += 1 + len) {
        cmd = *list >> 24;
        len = cmd_lengths[cmd];
        if (list + 1 + len > list_end) {
            cmd = -1;
            break;
        }

        gpu_ps2.PacketBuffer.U4[0] = list[0];
        for (i = 1; i <= len; i++) {
            gpu_ps2.PacketBuffer.U4[i] = list[i];
        }

        switch (cmd) {
            case 0x02:
                DPRINTF("gpuClearImage\n");
                break;
            case 0x20:
            case 0x21:
            case 0x22:
            case 0x23: // Monochrome 3-pt poly
                DPRINTF("gpuDrawPolyF\n");
                break;
            case 0x24:
            case 0x25:
            case 0x26:
            case 0x27: // Textured 3-pt poly
                DPRINTF("gpuDrawPolyFT\n");
                break;
            case 0x28:
            case 0x29:
            case 0x2A:
            case 0x2B: // Monochrome 4-pt poly
                DPRINTF("gpuDrawPolyF\n");
                break;
            case 0x2C:
            case 0x2D:
            case 0x2E:
            case 0x2F: // Textured 4-pt poly
                DPRINTF("gpuDrawPolyFT\n");
                break;
            case 0x30:
            case 0x31:
            case 0x32:
            case 0x33: // Gouraud-shaded 3-pt poly
                DPRINTF("gpuDrawPolyG\n");
                break;
            case 0x34:
            case 0x35:
            case 0x36:
            case 0x37: // Gouraud-shaded, textured 3-pt poly
                DPRINTF("gpuDrawPolyGT\n");
                break;
            case 0x38:
            case 0x39:
            case 0x3A:
            case 0x3B: // Gouraud-shaded 4-pt poly
                DPRINTF("gpuDrawPolyG\n");
                break;
            case 0x3C:
            case 0x3D:
            case 0x3E:
            case 0x3F: // Gouraud-shaded, textured 4-pt poly
                DPRINTF("gpuDrawPolyGT\n");
                break;
            case 0x40:
            case 0x41:
            case 0x42:
            case 0x43: // Monochrome line
                DPRINTF("gpuDrawLineF\n");
                break;
            case 0x48 ... 0x4F: // Monochrome line strip
                DPRINTF("gpuDrawLineF\n");
                break;
            case 0x50:
            case 0x51:
            case 0x52:
            case 0x53: // Gouraud-shaded line
                DPRINTF("gpuDrawLineG\n");
                break;
            case 0x58 ... 0x5F: // Gouraud-shaded line strip
                DPRINTF("gpuDrawLineG\n");
                break;
            case 0x60:
            case 0x61:
            case 0x62:
            case 0x63: // Monochrome rectangle (variable size)
                DPRINTF("gpuDrawT\n");
                break;
            case 0x64:
            case 0x65:
            case 0x66:
            case 0x67: // Textured rectangle (variable size)
                DPRINTF("gpuDrawS\n");
                break;
            case 0x68:
            case 0x69:
            case 0x6A:
            case 0x6B: // Monochrome rectangle (1x1 dot)
                DPRINTF("gpuDrawT\n");
                break;
            case 0x70:
            case 0x71:
            case 0x72:
            case 0x73: // Monochrome rectangle (8x8)
                DPRINTF("gpuDrawT\n");
                break;
            case 0x74:
            case 0x75:
            case 0x76:
            case 0x77: // Textured rectangle (8x8)
                DPRINTF("gpuDrawS\n");
                break;
            case 0x78:
            case 0x79:
            case 0x7A:
            case 0x7B: // Monochrome rectangle (16x16)
                DPRINTF("gpuDrawT\n");
                break;
            case 0x7C:
            case 0x7D:
            case 0x7E:
            case 0x7F: // Textured rectangle (16x16)
                DPRINTF("gpuDrawS\n");
                break;
            case 0x80: //  vid -> vid
                DPRINTF("gpuMoveImage\n");
                break;
            case 0xA0: //  sys ->vid
            case 0xC0: //  vid -> sys
                // Handled by gpulib
                goto breakloop;
            case 0xE1 ... 0xE6: // Draw settings
                DPRINTF("gpuGP0Cmd_0xEx\n");
                gpuGP0Cmd_0xEx(gpu_ps2.PacketBuffer.U4[0]);
                break;
        }
    }

breakloop:
    gpu.ex_regs[1] &= ~0x1ff;
    gpu.ex_regs[1] |= gpu_ps2.GPU_GP1 & 0x1ff;

    *last_cmd = cmd;
    return list - list_start;
}

void renderer_finish(void) {
}

void renderer_set_config(const struct gpulib_config_t *config) {
    gpu_ps2.vram = (uint16_t*)gpu.vram;
}

void renderer_flush_queues(void) {
}

void renderer_update_caches(int x, int y, int w, int h) {
}

void renderer_sync_ecmds(uint32_t *ecmds) {
    int dummy;
    do_cmd_list(&ecmds[1], 6, &dummy);
}

void renderer_notify_res_change(void) {
}

void renderer_set_interlace(int enable, int is_odd) {
}
