/*
 * vitaps2_core3.skprx: lets VitaPS2 run threads on core 3 (the system core). Game-mode apps
 * are refused affinity masks with core 3: SceKernelThreadMgr's "illegal affinity" check (text
 * offset 0x114C, Thumb) says so. This module makes that check pass, the same hook as the
 * fourth-core part of GrapheneCt's CapUnlocker (MIT), and nothing else.
 *
 * VitaPS2 loads it at start with taiLoadStartKernelModule (setting core3_unlock), so it lasts
 * until the next reboot and is never in the boot configuration. It patches only when the code
 * at that offset starts with a Thumb push {..., lr} (a function prologue): on an unexpected
 * firmware it refuses instead of patching. What it found: ux0:data/VitaPS2/core3.txt.
 */
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/io/fcntl.h>
#include <taihen.h>
#include <stdint.h>

#define ILLEGAL_AFFINITY_OFFSET 0x114C

int module_get_offset(SceUID pid, SceUID modid, int segidx, size_t offset, uintptr_t *addr);

static SceUID g_hook = -1;
static tai_hook_ref_t g_hookRef;

static int isIllegalAffinity_patched(void)
{
	return 0;
}

static void write_status(const char *text, const uint8_t *bytes, int count, int result)
{
	static const char hex[] = "0123456789ABCDEF";
	char line[96];
	int length = 0, index;
	while(text[length] != '\0' && length < 40)
	{
		line[length] = text[length];
		length++;
	}
	for(index = 0; index < count; index++)
	{
		line[length++] = ' ';
		line[length++] = hex[bytes[index] >> 4];
		line[length++] = hex[bytes[index] & 15];
	}
	line[length++] = ' ';
	for(index = 28; index >= 0; index -= 4)
		line[length++] = hex[((uint32_t)result >> index) & 15];
	line[length++] = '\n';
	SceUID fd = ksceIoOpen("ux0:data/VitaPS2/core3.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if(fd >= 0)
	{
		ksceIoWrite(fd, line, length);
		ksceIoClose(fd);
	}
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp)
{
	tai_module_info_t info;
	uintptr_t address = 0;
	uint8_t code[4] = {0, 0, 0, 0};
	int index;

	(void)args;
	(void)argp;
	info.size = sizeof(info);
	if(taiGetModuleInfoForKernel(KERNEL_PID, "SceKernelThreadMgr", &info) < 0)
	{
		write_status("no SceKernelThreadMgr", code, 0, -1);
		return SCE_KERNEL_START_FAILED;
	}
	if(module_get_offset(KERNEL_PID, info.modid, 0, ILLEGAL_AFFINITY_OFFSET, &address) < 0 || address == 0)
	{
		write_status("no text segment", code, 0, -1);
		return SCE_KERNEL_START_FAILED;
	}
	for(index = 0; index < 4; index++)
		code[index] = ((const uint8_t *)address)[index];
	// Thumb push {..., lr}: 0xB5xx (little endian: xx B5), or push.w: E92D 4xxx (2D E9 xx 4x)
	int prologue = (code[1] == 0xB5) || (code[0] == 0x2D && code[1] == 0xE9 && (code[3] & 0x40));
	if(!prologue)
	{
		write_status("unexpected code, not patched:", code, 4, (int)info.module_nid);
		return SCE_KERNEL_START_FAILED;
	}
	g_hook = taiHookFunctionOffsetForKernel(KERNEL_PID, &g_hookRef, info.modid, 0, ILLEGAL_AFFINITY_OFFSET, 1, isIllegalAffinity_patched);
	write_status(g_hook >= 0 ? "patched, code was:" : "hook failed, code was:", code, 4, g_hook >= 0 ? (int)info.module_nid : g_hook);
	return (g_hook >= 0) ? SCE_KERNEL_START_SUCCESS : SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	if(g_hook >= 0)
		taiHookReleaseForKernel(g_hook, g_hookRef);
	g_hook = -1;
	return SCE_KERNEL_STOP_SUCCESS;
}
