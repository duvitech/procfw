#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <pspsdk.h>
#include <pspdebug.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <psputility.h>

#include "systemctrl.h"
#include "systemctrl_se.h"
#include "vshctrl.h"
#include "utils.h"
#include "vpl.h"

static SceUID g_vpl_uid = -1;

void vpl_init(void)
{
	g_vpl_uid = sceKernelCreateVpl("OurVPL", 2, 0, VPL_POOL_SIZE, NULL);
}

void vpl_finish(void)
{
	sceKernelDeleteVpl(g_vpl_uid);
}

void *vpl_alloc(int size)
{
	void *p;
	int ret;

	ret = sceKernelAllocateVpl(g_vpl_uid, size, &p, NULL);

	if(ret == 0)
		return p;

	return NULL;
}

char *vpl_strdup(const char *str)
{
	int len;
	char *p;

	len = strlen(str) + 1;
	p = vpl_alloc(len);

	if(p == NULL) {
		return p;
	}

	strcpy(p, str);

	return p;
}

void vpl_free(void *p)
{
	int ret;

	ret = sceKernelFreeVpl(g_vpl_uid, p);

#ifdef DEBUG
	if(ret != 0) {
		asm("break 0x8492");
	}
#endif
}

void *vpl_realloc(void *ptr, size_t size)
{
	void *p;

	p = vpl_alloc(size);

	if(p == NULL) {
		return p;
	}

	if(ptr == NULL) {
		memset(p, 0, size);
	} else {
		memcpy(p, ptr, size);
		vpl_free(ptr);
	}

	return p;
}
