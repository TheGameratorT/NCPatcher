// NCPatcher Runtime. Compiled into the ARM code of projects that use it.
//
// Depends on nothing from the game: the copies are plain loops and the cache
// maintenance talks to CP15 directly, so it works the same on every title.

#define __ncprt_uptr __UINTPTR_TYPE__
#define __ncprt_ptr(address) ((volatile unsigned char*)(__ncprt_uptr)(address))

#if defined(__ARM_ARCH)
_Static_assert(sizeof(ncprt_patch) == 16, "ncprt_patch must match the tables ncpatcher writes");
_Static_assert(sizeof(ncprt_group) == 8, "ncprt_group must match the tables ncpatcher writes");
#endif

// The ARM946E-S cache line size.
#define __ncprt_cache_line 32

static void __ncprt_copy(volatile unsigned char* dst, const volatile unsigned char* src, unsigned int size)
{
	for (unsigned int i = 0; i < size; i++)
		dst[i] = src[i];
}

static void __ncprt_put16(unsigned char* out, unsigned int value)
{
	out[0] = (unsigned char)value;
	out[1] = (unsigned char)(value >> 8);
}

static void __ncprt_put32(unsigned char* out, unsigned int value)
{
	__ncprt_put16(out, value);
	__ncprt_put16(out + 2, value >> 16);
}

// THUMB BL and BLX are two halfwords carrying 22 bits of halfword offset. BLX
// always lands on ARM code, so its offset is taken from the word-aligned PC.
static unsigned int __ncprt_thumb_call(int exchange, unsigned int address, unsigned int dest)
{
	unsigned int from = exchange ? (address & ~3u) : address;
	int offset = ((int)(dest - from) >> 1) - 2;
	unsigned int high = 0xF000 | (((unsigned int)offset >> 11) & 0x7FF);
	unsigned int low = (exchange ? 0xE800 : 0xF800) | ((unsigned int)offset & 0x7FF);
	return (low << 16) | high;
}

unsigned int __ncprt_encode(unsigned int kind, unsigned int address, unsigned int dest, unsigned char* out)
{
	switch (kind)
	{
	case NCPRT_ARM_B:
	case NCPRT_ARM_BL:
	{
		unsigned int opcode = kind == NCPRT_ARM_B ? 0xEA000000 : 0xEB000000;
		int offset = ((int)(dest - address) >> 2) - 2;
		__ncprt_put32(out, opcode | ((unsigned int)offset & 0xFFFFFF));
		return 4;
	}
	case NCPRT_ARM_BLX:
	{
		int offset = (int)((dest & ~1u) - address) - 8;
		unsigned int h = ((unsigned int)offset >> 1) & 1;
		__ncprt_put32(out, 0xFA000000 | (h << 24) | (((unsigned int)offset >> 2) & 0xFFFFFF));
		return 4;
	}
	case NCPRT_ARM_JUMP:
		__ncprt_put32(out, 0xE51FF004); // LDR PC, [PC,#-4]
		__ncprt_put32(out + 4, dest);
		return 8;
	case NCPRT_THUMB_BL:
		__ncprt_put32(out, __ncprt_thumb_call(0, address, dest & ~1u));
		return 4;
	case NCPRT_THUMB_BLX:
		__ncprt_put32(out, __ncprt_thumb_call(1, address, dest));
		return 4;
	case NCPRT_THUMB_JUMP:
	case NCPRT_THUMB_JUMPX:
		__ncprt_put16(out, 0xB500); // PUSH {LR}
		__ncprt_put32(out + 2, __ncprt_thumb_call(kind == NCPRT_THUMB_JUMPX, address + 2, dest & ~1u));
		__ncprt_put16(out + 6, 0xBD00); // POP {PC}
		return 8;
	default:
		return 0;
	}
}

#if defined(__ARM_ARCH) && __ARM_ARCH >= 5
__attribute__((target("arm"), noinline))
void ncprt_sync_code(const void* address, unsigned int size)
{
	__ncprt_uptr line = (__ncprt_uptr)address & ~(__ncprt_uptr)(__ncprt_cache_line - 1);
	__ncprt_uptr end = (__ncprt_uptr)address + size;

	for (__ncprt_uptr at = line; at < end; at += __ncprt_cache_line)
		__asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(at) : "memory"); // clean and invalidate D line

	__asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory"); // drain write buffer

	for (__ncprt_uptr at = line; at < end; at += __ncprt_cache_line)
		__asm__ volatile("mcr p15, 0, %0, c7, c5, 1" :: "r"(at) : "memory"); // invalidate I line
}
#else
void ncprt_sync_code(const void* address, unsigned int size)
{
	(void)address;
	(void)size;
}
#endif

void __ncprt_apply(ncprt_group* group)
{
	if (group->applied)
		return;

	for (unsigned int i = 0; i < group->count; i++)
	{
		const ncprt_patch* patch = &group->patches[i];
		volatile unsigned char* target = __ncprt_ptr(patch->address);

		__ncprt_copy(__ncprt_ptr(patch->save), target, patch->size);

		if (patch->kind == NCPRT_COPY)
		{
			__ncprt_copy(target, __ncprt_ptr(patch->value), patch->size);
		}
		else
		{
			unsigned char code[8];
			unsigned int size = __ncprt_encode(patch->kind, patch->address, patch->value, code);
			__ncprt_copy(target, code, size);
		}

		ncprt_sync_code((const void*)target, patch->size);
	}

	group->applied = 1;
}

void __ncprt_revert(ncprt_group* group)
{
	if (!group->applied)
		return;

	for (unsigned int i = group->count; i-- > 0;)
	{
		const ncprt_patch* patch = &group->patches[i];
		volatile unsigned char* target = __ncprt_ptr(patch->address);

		__ncprt_copy(target, __ncprt_ptr(patch->save), patch->size);
		ncprt_sync_code((const void*)target, patch->size);
	}

	group->applied = 0;
}
