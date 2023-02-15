#pragma once

// NCP Common

#if defined __ncp_lang_c || defined __ncp_lang_cpp

#ifdef __ncp_c_asmwcpp
asm("#include \"ncp_asm.h\"");
#else
asm(
"@ ARM\n"
"\n"
".macro ncp_jump address; .global ncp_jump_\\address; .type ncp_jump_\\address,%function; ncp_jump_\\address:; .endm\n"
".macro ncp_call address; .global ncp_call_\\address; .type ncp_call_\\address,%function; ncp_call_\\address:; .endm\n"
".macro ncp_hook address; .global ncp_hook_\\address; .type ncp_hook_\\address,%function; ncp_hook_\\address:; .endm\n"
"\n"
".macro ncp_jump_ov address overlay; .global ncp_jump_\\address\\()_ov\\overlay; .type ncp_jump_\\address\\()_ov\\overlay,%function; ncp_jump_\\address\\()_ov\\overlay:; .endm\n"
".macro ncp_call_ov address overlay; .global ncp_call_\\address\\()_ov\\overlay; .type ncp_call_\\address\\()_ov\\overlay,%function; ncp_call_\\address\\()_ov\\overlay:; .endm\n"
".macro ncp_hook_ov address overlay; .global ncp_hook_\\address\\()_ov\\overlay; .type ncp_hook_\\address\\()_ov\\overlay,%function; ncp_hook_\\address\\()_ov\\overlay:; .endm\n"
"\n"
"@ THUMB\n"
"\n"
".macro ncp_tjump address; .global ncp_tjump_\\address; .type ncp_tjump_\\address,%function; ncp_tjump_\\address:; .endm\n"
".macro ncp_tcall address; .global ncp_tcall_\\address; .type ncp_tcall_\\address,%function; ncp_tcall_\\address:; .endm\n"
".macro ncp_thook address; .global ncp_thook_\\address; .type ncp_thook_\\address,%function; ncp_thook_\\address:; .endm\n"
"\n"
".macro ncp_tjump_ov address overlay; .global ncp_tjump_\\address\\()_ov\\overlay; .type ncp_tjump_\\address\\()_ov\\overlay,%function; ncp_tjump_\\address\\()_ov\\overlay:; .endm\n"
".macro ncp_tcall_ov address overlay; .global ncp_tcall_\\address\\()_ov\\overlay; .type ncp_tcall_\\address\\()_ov\\overlay,%function; ncp_tcall_\\address\\()_ov\\overlay:; .endm\n"
".macro ncp_thook_ov address overlay; .global ncp_thook_\\address\\()_ov\\overlay; .type ncp_thook_\\address\\()_ov\\overlay,%function; ncp_thook_\\address\\()_ov\\overlay:; .endm\n"
"\n"
"@ OTHER\n"
"\n"
".macro ncp_over_begin address; .pushsection .ncp_over_\\address; .set ncp_dest, \\address; .endm\n"
".macro ncp_over_ov_begin address overlay; .pushsection .ncp_over_\\address\\()_ov\\overlay; .set ncp_dest, \\address; .endm\n"
".macro ncp_over_end; .popsection; .endm\n"
"\n"
".macro ncp_repl address assembly; ncp_over_begin \\address; \\assembly; ncp_over_end; .endm\n"
".macro ncp_repl_ov address overlay assembly; ncp_over_ov_begin \\address, \\overlay; \\assembly; ncp_over_end; .endm\n"
""
);
#endif

#ifdef __ncp_lang_cpp
#define __ncp_extern extern "C"
#define __ncp_extern_var extern "C"
#else
#define __ncp_extern
#define __ncp_extern_var extern
#endif

#define __ncp_get_macro(_1, _2, _3, NAME, ...) NAME

// NCP Sections

#define __ncp_main_section(opcode, address) __attribute__((section(".ncp_" #opcode "_" #address), used))
#define __ncp_ovxx_section(opcode, address, overlay) __attribute__((section(".ncp_" #opcode "_" #address "_ov" #overlay), used))

#define __ncp_main_jump(address) __ncp_main_section(jump, address)
#define __ncp_main_call(address) __ncp_main_section(call, address)
#define __ncp_main_hook(address) __ncp_main_section(hook, address)
#define __ncp_main_over(address) __ncp_main_section(over, address)
#define __ncp_ovxx_jump(address, overlay) __ncp_ovxx_section(jump, address, overlay)
#define __ncp_ovxx_call(address, overlay) __ncp_ovxx_section(call, address, overlay)
#define __ncp_ovxx_hook(address, overlay) __ncp_ovxx_section(hook, address, overlay)
#define __ncp_ovxx_over(address, overlay) __ncp_ovxx_section(over, address, overlay)

#define ncp_jump(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_jump, __ncp_main_jump)(__VA_ARGS__)
#define ncp_call(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_call, __ncp_main_call)(__VA_ARGS__)
#define ncp_hook(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_hook, __ncp_main_hook)(__VA_ARGS__)
#define ncp_over(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_over, __ncp_main_over)(__VA_ARGS__)

// NCP Sections (thumb)

#define __ncp_main_tjump(address) __ncp_main_section(tjump, address)
#define __ncp_main_tcall(address) __ncp_main_section(tcall, address)
#define __ncp_main_thook(address) __ncp_main_section(thook, address)
#define __ncp_ovxx_tjump(address, overlay) __ncp_ovxx_section(tjump, address, overlay)
#define __ncp_ovxx_tcall(address, overlay) __ncp_ovxx_section(tcall, address, overlay)
#define __ncp_ovxx_thook(address, overlay) __ncp_ovxx_section(thook, address, overlay)

#define ncp_tjump(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_tjump, __ncp_main_tjump)(__VA_ARGS__)
#define ncp_tcall(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_tcall, __ncp_main_tcall)(__VA_ARGS__)
#define ncp_thook(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_thook, __ncp_main_thook)(__VA_ARGS__)

// NCP Variables

#define __ncp_main_set(opcode, address, function) void* ncp_set##opcode##_##address __attribute__((section(".ncp_set"), used)) = (void*)function;
#define __ncp_ovxx_set(opcode, address, overlay, function) void* ncp_set##opcode##_##address##_ov##overlay __attribute__((section(".ncp_set"), used)) = (void*)function;

#define __ncp_main_set_jump(address, function) __ncp_main_set(jump, address, function)
#define __ncp_main_set_call(address, function) __ncp_main_set(call, address, function)
#define __ncp_main_set_hook(address, function) __ncp_main_set(hook, address, function)
#define __ncp_ovxx_set_jump(address, overlay, function) __ncp_ovxx_set(jump, address, overlay, function)
#define __ncp_ovxx_set_call(address, overlay, function) __ncp_ovxx_set(call, address, overlay, function)
#define __ncp_ovxx_set_hook(address, overlay, function) __ncp_ovxx_set(hook, address, overlay, function)

#define ncp_set_jump(...) __ncp_get_macro(__VA_ARGS__, __ncp_ovxx_set_jump, __ncp_main_set_jump, )(__VA_ARGS__)
#define ncp_set_call(...) __ncp_get_macro(__VA_ARGS__, __ncp_ovxx_set_call, __ncp_main_set_call, )(__VA_ARGS__)
#define ncp_set_hook(...) __ncp_get_macro(__VA_ARGS__, __ncp_ovxx_set_hook, __ncp_main_set_hook, )(__VA_ARGS__)

// NCP Variables (thumb)

#define __ncp_main_set_tjump(address, function) __ncp_main_set(tjump, address, function)
#define __ncp_main_set_tcall(address, function) __ncp_main_set(tcall, address, function)
#define __ncp_main_set_thook(address, function) __ncp_main_set(thook, address, function)
#define __ncp_ovxx_set_tjump(address, overlay, function) __ncp_ovxx_set(tjump, address, overlay, function)
#define __ncp_ovxx_set_tcall(address, overlay, function) __ncp_ovxx_set(tcall, address, overlay, function)
#define __ncp_ovxx_set_thook(address, overlay, function) __ncp_ovxx_set(thook, address, overlay, function)

#define ncp_set_tjump(...) __ncp_get_macro(__VA_ARGS__, __ncp_ovxx_set_tjump, __ncp_main_set_tjump, )(__VA_ARGS__)
#define ncp_set_tcall(...) __ncp_get_macro(__VA_ARGS__, __ncp_ovxx_set_tcall, __ncp_main_set_tcall, )(__VA_ARGS__)
#define ncp_set_thook(...) __ncp_get_macro(__VA_ARGS__, __ncp_ovxx_set_thook, __ncp_main_set_thook, )(__VA_ARGS__)

// NCP Utilities

#define __ncp_main_repl(address, assembly) __attribute__((naked)) void __ncp_repl_##address() { asm( ".pushsection .ncp_over_" #address "\n" assembly "\n.popsection" ); }
#define __ncp_ovxx_repl(address, overlay, assembly) __attribute__((naked)) void __ncp_repl_##address##_ov##overlay() { asm( ".pushsection .ncp_over_" #address "_ov" #overlay "\n" assembly "\n.popsection" ); }
#define ncp_repl(...) __ncp_get_macro(__VA_ARGS__, __ncp_ovxx_repl, __ncp_main_repl, )(__VA_ARGS__)

#define ncp_file(path, sym) \
asm(#sym":\n.incbin \"" path"\"\n__"#sym"_end:"); \
__ncp_extern_var char sym[]; \
__ncp_extern_var const char __##sym##_end[];

#define ncp_filez(path, sym) \
asm(#sym":\n.incbin \"" path"\"\n.byte 0\n__"#sym"_end:"); \
__ncp_extern_var char sym[]; \
__ncp_extern_var const char __##sym##_end[];

#define ncp_filesize(sym) ((unsigned long)(__##sym##_end - sym))

#define arm __attribute__((target("arm")))
#define thumb __attribute__((target("thumb")))
#define always_inline __attribute__((always_inline))
#define noinline __attribute__((noinline))
#define asm_func __attribute__((naked))

// NCP Real Time

#define arm_opcode_b 0xEA000000
#define arm_opcode_bl 0xEB000000
#define arm_opcode_nop 0xE1A00000

static inline void __ncprt_set(int address, int value) { *(int*)address = value; }
static inline void __ncprt_set_jump(int address, void* function) { *(int*)address = (arm_opcode_b | (((((int)function - address) >> 2) - 2) & 0xFFFFFF)); }
static inline void __ncprt_set_call(int address, void* function) { *(int*)address = (arm_opcode_bl | (((((int)function - address) >> 2) - 2) & 0xFFFFFF)); }

#define ncprt_set(address, value) __ncprt_set(address, value)
#define ncprt_set_jump(address, function) __ncprt_set_jump(address, (void*)(function))
#define ncprt_set_call(address, function) __ncprt_set_call(address, (void*)(function))

#define ncprt_repl_type(name) __attribute__((section(".ncp_rtrepl_" #name)))

__ncp_extern void __ncp_ncprt_repl(void* dest, void* start, void* end);

#define ncprt_repl(address, name) { \
extern char ncp_rtrepl_##name##_start[]; \
extern char ncp_rtrepl_##name##_end[]; \
__ncp_ncprt_repl((void*)address, ncp_rtrepl_##name##_start, ncp_rtrepl_##name##_end); \
}

#elif defined __ncp_lang_asm

#include "ncp_asm.h"

#else
	
#error "Fatal NCPatcher error: No language target set"

#endif
