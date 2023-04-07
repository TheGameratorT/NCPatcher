#include "nitro.h"

void __ncp_ncprt_repl(void* dest, void* start, void* end)
{
	int data_size = (int)(start - end);
	IC_InvalidateRange(dest, data_size);
	DC_InvalidateRange(dest, data_size);
	MI_CpuCopy8(start, dest, data_size);
}
