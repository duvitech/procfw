#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspthreadman_kernel.h>
#include <psputilsforkernel.h>
#include <stdio.h>
#include <string.h>

#include "systemctrl.h"
#include "systemctrl_se.h"
#include "systemctrl_private.h"
#include "utils.h"
#include "printk.h"
#include "galaxy_patch_offset.h"

#define CISO_IDX_BUFFER_SIZE 0x200
#define CISO_DEC_BUFFER_SIZE 0x2000
#define SECTOR_SIZE 0x800

PSP_MODULE_INFO("M33GalaxyController", 0x1006, 1, 1);

extern int get_total_block(void);
extern int clear_cache(void);
extern int sceKernelSetQTGP3(void *unk0);

u32 psp_fw_version;

// 0x00000F24
SceUID g_SceNpUmdMount_thid = -1;

// 0x00000F28
SceUID g_iso_fd = -1;

// 0x00000F2C
SceUID g_total_blocks = -1;

// 0x00000F40
int g_iso_opened = 0;

// 0x00000F44
int g_is_ciso = 0;

// 0x00000F80
void *g_ciso_block_buf = NULL;

// 0x00000F84, size CISO_DEC_BUFFER_SIZE, align 64
void *g_ciso_dec_buf = NULL;

// 0x00000FC0
u32 g_CISO_idx_cache[CISO_IDX_BUFFER_SIZE/4] __attribute__((aligned(64)));

// 0x000011C0
int g_ciso_dec_buf_offset = -1;

// 0x000011C4
int g_CISO_cur_idx = 0;

struct CISO_header {
	u8 magic[4];  // 0
	u32 header_size;  // 4
	u64 total_bytes; // 8
	u32 block_size; // 16
	u8 ver; // 20
	u8 align;  // 21
	u8 rsv_06[2];  // 22
};

// 0x000011C8
struct CISO_header g_CISO_hdr;

// 0x000011E0
u32 ciso_total_block = 0;

// 0x0000120C
u32 g_sceNp9660_driver_text_addr = 0;

// 0x00001200
u32 g_func_1200 = 0;

// 0x00001204
u32 g_data_1204 = 0;

// 0x00001208
u32 g_func_1208 = 0;

// 0x0000121C
u32 g_func_121C = 0;

// 0x00001220
char *g_iso_fn = NULL;

// 0x00000e10
u8 g_umddata[16] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// 0x00000000
SceUID myKernelCreateThread(const char * name,
		SceKernelThreadEntry entry,
		int	initPriority,
		int	stackSize,
		SceUInt	attr,
		SceKernelThreadOptParam * option	 
		)
{
	SceUID thid;

	thid = sceKernelCreateThread(name, entry, initPriority, 
			stackSize, attr, option);

	if(!strncmp(name, "SceNpUmdMount", 13)) {
		g_SceNpUmdMount_thid = thid;
		printk("g_SceNpUmdMount_thid at 0x%08x\n", thid);
	}

	return thid;
}

// 670
int cso_open(SceUID fd)
{
	int ret;

	g_CISO_hdr.magic[0] = '\0';
	g_ciso_dec_buf_offset = 0x7FFFFFFF;

	sceIoLseek(fd, 0, PSP_SEEK_SET);
	ret = sceIoRead(fd, &g_CISO_hdr, sizeof(g_CISO_hdr));

	if(ret != sizeof(g_CISO_hdr)) {
		ret = -1;
		printk("%s: -> %d\n", __func__, ret);
		goto exit;
	}

	if(*(u32*)g_CISO_hdr.magic == 0x4F534943) { // CISO
		g_CISO_cur_idx = -1;
		ciso_total_block = g_CISO_hdr.total_bytes / g_CISO_hdr.block_size;
		printk("%s: total block %d\n", __func__, ciso_total_block);

		if(g_ciso_dec_buf == NULL) {
			g_ciso_dec_buf = oe_malloc(CISO_DEC_BUFFER_SIZE + 64);

			if(g_ciso_dec_buf == NULL) {
				ret = -2;
				printk("%s: -> %d\n", __func__, ret);
				goto exit;
			}

			if((u32)g_ciso_dec_buf & 63)
				g_ciso_dec_buf = (void*)(((u32)g_ciso_dec_buf & (~63)) + 64);
		}

		if(g_ciso_block_buf == NULL) {
			g_ciso_block_buf = oe_malloc(SECTOR_SIZE);

			if(g_ciso_block_buf == NULL) {
				ret = -3;
				printk("%s: -> %d\n", __func__, ret);
				goto exit;
			}
		}

		ret = 0;
	} else {
		ret = 0x8002012F;
	}

exit:
	return ret;
}

// 184
int open_iso(void)
{
	int ret;

	g_iso_opened = 0;
	sceIoClose(g_iso_fd);

	while( 1 ) {
		g_iso_fd = sceIoOpen(g_iso_fn, 0x000F0000 | PSP_O_RDONLY, 0);

		if(g_iso_fd >= 0) {
			break;
		}

		sceKernelDelayThread(10000);
	}

	// 6.30: 0x00004D50
	// 6.20: 0x00004C70 in sub_00004BB4
	// see 0x00004618 in sub_000045CC
	// see 0x00003428 in sub_000033F4, memset(0x00005BA4, 0, 60)
	_sw(g_iso_fd, g_sceNp9660_driver_text_addr + g_offs->StoreFd);
	g_is_ciso = 0;
	ret = cso_open(g_iso_fd);

	if(ret >= 0) {
		g_is_ciso = 1;
		printk("%s: g_is_ciso = 1\n", __func__);
	}

	ret = get_total_block();
	g_total_blocks = ret;
	g_iso_opened = 1;

	return 0;
}

int sub_00000588(void)
{
	int intr;
	void (*ptr)(u32) = (void*)g_func_1200;

	(*ptr)(0);
	open_iso();
	intr = sceKernelCpuSuspendIntr();

	/* sceUmdManGetUmdDiscInfo patch */
	_sw(0xE0000800, g_sceNp9660_driver_text_addr + g_offs->Data1);
	_sw(0x00000009, g_sceNp9660_driver_text_addr + g_offs->Data2);
	_sw(g_total_blocks, g_sceNp9660_driver_text_addr + g_offs->Data3);
	_sw(g_total_blocks, g_sceNp9660_driver_text_addr + g_offs->Data4);
	_sw(0x00000000, g_sceNp9660_driver_text_addr + g_offs->Data5);

	sceKernelCpuResumeIntr(intr);

	if(g_data_1204 == 0) {
		g_data_1204 = 1;
		sceKernelDelayThread(800000);
	}

	clear_cache();
	sceKernelSetQTGP3(g_umddata);

	return 0;
}

// d8
int get_total_block(void)
{
	SceOff offset;
	int ret;

	if(g_is_ciso) {
		ret = ciso_total_block;
	} else {
		offset = sceIoLseek(g_iso_fd, 0, PSP_SEEK_END);

		ret = ((u32)(offset >> 32) << 21) | (((u32)offset) >> 11);
	}

	printk("%s: returns %d blocks\n", __func__, ret);

	return ret;
}

// 244
int read_raw_data(u8* addr, u32 size, int offset)
{
	int ret;
	int i;

	i = 0;

	do {
		i++;
		ret = sceIoLseek32(g_iso_fd, offset, PSP_SEEK_SET);

		if(ret >= 0) {
			i = 0;
			break;
		} else {
			open_iso();
		}
	} while(i < 16);

	if(i == 16) {
		ret = 0x80010013;
		goto exit;
	}

	for(i=0; i<16; ++i) {
		ret = sceIoRead(g_iso_fd, addr, size);

		if(ret >= 0) {
			i = 0;
			break;
		} else {
			open_iso();
		}
	}

	if(i == 16) {
		ret = 0x80010013;
		goto exit;
	}

exit:
	return ret;
}

// 790
int read_cso_sector(u8 *addr, int sector)
{
	int ret;
	int n_sector;
	int offset, next_offset;
	int size;

	n_sector = sector - g_CISO_cur_idx;

	// not within sector idx cache?
	if(g_CISO_cur_idx == -1 || n_sector < 0 || n_sector >= NELEMS(g_CISO_idx_cache)) {
		ret = read_raw_data((u8*)g_CISO_idx_cache, sizeof(g_CISO_idx_cache), (sector << 2) + sizeof(struct CISO_header));

		if(ret < 0) {
			ret = -4;
			printk("%s: -> %d\n", __func__, ret);

			return ret;
		}

		g_CISO_cur_idx = sector;
		n_sector = 0;
	}

	// loc_804
	offset = (g_CISO_idx_cache[n_sector] & 0x7FFFFFFF) << g_CISO_hdr.align;

	// is plain?
	if(g_CISO_idx_cache[n_sector] & 0x80000000) {
		// loc_968
		return read_raw_data(addr, SECTOR_SIZE, offset);
	}

	sector++;
	n_sector = sector - g_CISO_cur_idx;

	if(g_CISO_cur_idx == -1 || n_sector < 0 || n_sector >= NELEMS(g_CISO_idx_cache)) {
		ret = read_raw_data((u8*)g_CISO_idx_cache, sizeof(g_CISO_idx_cache), (sector << 2) + sizeof(struct CISO_header));

		if(ret < 0) {
			ret = -5;
			printk("%s: -> %d\n", __func__, ret);

			return ret;
		}

		g_CISO_cur_idx = sector;
		n_sector = 0;
	}

	// loc_858
	next_offset = (g_CISO_idx_cache[n_sector] & 0x7FFFFFFF) << g_CISO_hdr.align;
	size = next_offset - offset;
	
	if(size <= SECTOR_SIZE)
		size = SECTOR_SIZE;

	if(offset < g_ciso_dec_buf_offset || size + offset >= g_ciso_dec_buf_offset + CISO_DEC_BUFFER_SIZE) {
		// loc_93C
		ret = read_raw_data(g_ciso_dec_buf, CISO_DEC_BUFFER_SIZE, offset);

		/* May not reach CISO_DEC_BUFFER_SIZE */	
		if(ret < 0) {
			// loc_95C
			g_ciso_dec_buf_offset = 0xFFF00000;
			ret = -6;
			printk("%s: -> %d\n", __func__, ret);

			return ret;
		}

		g_ciso_dec_buf_offset = offset;
	}

	// loc_8B8
	ret = sceKernelDeflateDecompress(addr, SECTOR_SIZE, g_ciso_dec_buf + offset - g_ciso_dec_buf_offset, 0);

	return ret < 0 ? ret : SECTOR_SIZE;
}

// 998
int read_cso_data(u8* addr, u32 size, int offset)
{
	u32 cur_block = offset / SECTOR_SIZE;
	int ret;
	int read_bytes;
	int pos = offset & 0x7FF;

	if(pos) {
		// loc_A80
		ret = read_cso_sector(g_ciso_block_buf, cur_block);

		if(ret != SECTOR_SIZE) {
			ret = -7;
			printk("%s: -> %d\n", __func__, ret);

			return ret;
		}

		read_bytes = MIN(size, (SECTOR_SIZE - pos));
		memcpy(addr, g_ciso_block_buf + pos, read_bytes);
		size -= read_bytes;
		addr += read_bytes;
		cur_block++;
	} else {
		read_bytes = 0;
	}

	// loc_9E4
	// more than 1 block left
	if(size / SECTOR_SIZE > 0) {
		int i;
		int block_cnt = size / SECTOR_SIZE;

		for(i=0; i<block_cnt; ++i) {
			ret = read_cso_sector(addr, cur_block);

			if(ret != SECTOR_SIZE) {
				ret = -8;
				printk("%s: -> %d\n", __func__, ret);

				return ret;
			}

			cur_block++;
			addr += SECTOR_SIZE;
			read_bytes += SECTOR_SIZE;
			size -= SECTOR_SIZE;
		}
	}

	if(size != 0) {
		ret = read_cso_sector(g_ciso_block_buf, cur_block);

		if(ret != SECTOR_SIZE) {
			ret = -9;
			printk("%s: -> %d\n", __func__, ret);

			return ret;
		}

		memcpy(addr, g_ciso_block_buf, size);

		return size + read_bytes;
	}

	return read_bytes;
}

struct read_data_args {
	u32 offset; // 0
	u8 *address; // 4
	u32 size; // 8
};

// 0x00001210~0x0000121C
struct read_data_args args;

// 30C
int read_data(struct read_data_args *args)
{
	if(g_is_ciso != 0) {
		// ciso decompess
		return read_cso_data(args->address, args->size, args->offset);
	} else {
		return read_raw_data(args->address, args->size, args->offset);
	}
}

int sub_00000054(u32 a0, u8 *a1, u32 a2)
{
	int (*ptr)(u32, u8*, u32) = (void*)g_func_1208;
	int (*ptr2)(void) = (void*)g_func_121C;
	int ret;

	ret = (*ptr)(a0, a1, a2);

	args.offset = a0;
	args.address = a1;
	args.size = a2;

	ret = sceKernelExtendKernelStack(0x2000, (void*)&read_data, &args);

	(*ptr2)();

	return ret;
}

int sub_00000514(int fd)
{
	int ret;
	
	ret = sceIoClose(fd);

	if(fd == g_iso_fd) {
		g_iso_fd = -1;
		_sw(-1, g_sceNp9660_driver_text_addr + g_offs->StoreFd);
		clear_cache();

		return ret;
	} else {
		return ret;
	}
}

// 0x000003D8
int myKernelStartThread(SceUID thid, SceSize arglen, void *argp)
{
	if(g_SceNpUmdMount_thid == thid) {
		SceModule2 *pMod;

		pMod = (SceModule2*) sceKernelFindModuleByName("sceNp9660_driver");
		g_sceNp9660_driver_text_addr = pMod->text_addr;

		// 6.30: 0x00003C34
		// 6.20: move to 0x00003BD8: jal InitForKernel_29DAA63F
		_sw(0x3C028000, g_sceNp9660_driver_text_addr + g_offs->InitForKernelCall); // jal InitForKernel_23458221 to lui $v0, 0x00008000
		// 6.30: 0x00003C4C
		// 6.20: move to 0x00003BF0: jal sub_00000000
		_sw(MAKE_CALL(sub_00000588), g_sceNp9660_driver_text_addr + g_offs->Func1);
		// 6.30: 0x000043B4
		// 6.20: move to 0x00004358: jal sub_00004388
		_sw(MAKE_CALL(sub_00000054), g_sceNp9660_driver_text_addr + g_offs->Func2); // jal sub_3948 to jal sub_00000054
		// 6.30: 0x0000590C
		// 6.20: move to 0x0000582C: jal sub_00004388
		_sw(MAKE_CALL(sub_00000054), g_sceNp9660_driver_text_addr + g_offs->Func3); // jal sub_3948 to jal sub_00000054
		// 6.30: 0x00007D08
		// 6.20: move to 0x00007C28
		_sw(MAKE_JUMP(sub_00000514), g_sceNp9660_driver_text_addr + g_offs->sceIoClose); // hook sceIoClose import

		// 6.30: 0x00003680
		// 6.20: move to 0x00003624
		g_func_1200 = pMod->text_addr + g_offs->Func4;
		printk("g_func_1200 0x%08x\n", g_func_1200); // sub_2f30
		// 6.30: 0x00004F8C
		// 6.20: move to 0x00004EAC
		g_func_1208 = pMod->text_addr + g_offs->Func5;
		printk("g_func_1208 0x%08x\n", g_func_1208); // sub_4494
		// 6.30: 0x00004FFC
		// 6.20: move to 0x00004F1C
		g_func_121C = pMod->text_addr + g_offs->Func6;
		printk("g_func_121C 0x%08x\n", g_func_121C); // sub_44ec

		clear_cache();
	}

	return sceKernelStartThread(thid, arglen, argp);
}

// 0x00000340
int module_start(SceSize args, void* argp)
{
	SceModule2 *pMod;
	int fd;
   
	psp_fw_version = sceKernelDevkitVersion();
	setup_patch_offset_table(psp_fw_version);

	printk_init("ms0:/LOG_GALAXY.TXT");
	printk("M33GalaxyController started: 0x%08X\n", psp_fw_version);

	g_iso_fn = sctrlSEGetUmdFile();
	pMod = (SceModule2*)sceKernelFindModuleByName("sceThreadManager");

	if(pMod != NULL) {
		// sceKernelCreateThread export
		_sw((u32)&myKernelCreateThread, pMod->text_addr + g_offs->sceKernelCreateThread); 

		// sceKernelStartThread export
		_sw((u32)&myKernelStartThread, pMod->text_addr + g_offs->sceKernelStartThread); 
	} else {
		printk("sceThreadManager cannot be found?!\n");
	}

	clear_cache();

	while( 1 ) {
		fd = sceIoOpen(g_iso_fn, PSP_O_RDONLY, 0);

		if(fd >= 0) {
			break;
		}

		sceKernelDelayThread(10000);
	}

	sceIoClose(fd);
	printk("%s: finished\n", __func__);

	return 0;
}

// 0x00000168
int clear_cache(void)
{
	sceKernelIcacheInvalidateAll();
	sceKernelDcacheWritebackInvalidateAll();

	return 0;
}
