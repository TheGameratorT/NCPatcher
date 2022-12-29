#pragma once

#ifdef __cplusplus
#define __ncp_lang_cpp
#define __ncp_extern extern "C"
#define __ncp_extern_var extern "C"
#else
#define __ncp_lang_c
#define __ncp_extern
#define __ncp_extern_var extern
#endif

// ncp_jump(int address, [int overlay])
#define ncp_jump(...)
// ncp_call(int address, [int overlay])
#define ncp_call(...)
// ncp_hook(int address, [int overlay])
#define ncp_hook(...)
// ncp_over(int address, [int overlay])
#define ncp_over(...)

// ncp_tjump(int address, [int overlay])
#define ncp_tjump(...)
// ncp_tcall(int address, [int overlay])
#define ncp_tcall(...)
// ncp_thook(int address, [int overlay])
#define ncp_thook(...)

// ncp_set_jump(int address, [int overlay], void* function)
#define ncp_set_jump(...)
// ncp_set_call(int address, [int overlay], void* function)
#define ncp_set_call(...)
// ncp_set_hook(int address, [int overlay], void* function)
#define ncp_set_hook(...)

// ncp_set_tjump(int address, [int overlay], void* function)
#define ncp_set_tjump(...)
// ncp_set_tcall(int address, [int overlay], void* function)
#define ncp_set_tcall(...)
// ncp_set_thook(int address, [int overlay], void* function)
#define ncp_set_thook(...)

// ncp_repl(int address, [int overlay], const char* asm)
#define ncp_repl(...)

// Includes a file as binary data
#define ncp_file(path, sym) \
asm(#sym":\n.incbin \""path"\"\n__"#sym"_end:"); \
__ncp_extern_var const char sym[]; \
__ncp_extern_var const char __##sym##_end[];

// Includes a file as binary data and null terminates it
#define ncp_filez(path, sym) \
asm(#sym":\n.incbin \""path"\"\n.byte 0\n__"#sym"_end:"); \
__ncp_extern_var const char sym[]; \
__ncp_extern_var const char __##sym##_end[];

// Returns the size of a file imported with ncp_file
#define ncp_filesize(sym) ((unsigned long)(__##sym##_end - sym))

// Makes the function compile in ARM mode
#define arm __attribute__((target("arm")))
// Makes the function compile in THUMB mode
#define thumb __attribute__((target("thumb")))
// Makes the function always be inlined
#define always_inline __attribute__((always_inline))
// Makes the function never be inlined
#define noinline __attribute__((noinline))
// Prevents the compiler from modifying the assembly inside the function 
#define asm_func __attribute__((naked))
