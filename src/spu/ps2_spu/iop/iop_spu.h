#ifndef _IOP_SPU_H_
#define _IOP_SPU_H_

#define SPU_CORE 0
#define SOUND_CORE 1

#define IOP_SPU_INIT_SOUND  1
#define IOP_SPU_SET_FORMAT  2
#define IOP_SPU_AVAIL_SPACE	3
#define IOP_SPU_PLAY_AUDIO 	4

#define IOP_SPU_BIND_RPC_ID 	0x18E39000

#define SBUS_INTC 0
#define SBUS_TRANSFER 1

#define RPC_BUF_SIZE (4096)

#define MIN(a,b) ((a) <= (b)) ? (a) : (b)

#endif

