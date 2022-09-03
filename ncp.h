#pragma once

// NCP Common

#if defined __ncp_lang_c || defined __ncp_lang_cpp

asm("#include \"ncp_asm.h\"");

#ifdef __ncp_lang_cpp
#define __ncp_extern extern "C"
#else
#define __ncp_extern
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

// NCP Variables

#define __ncp_main_set(opcode, address, function) static void* ncp_set##opcode##_##address __attribute__((section(".ncp_set"), used)) = (void*)function;
#define __ncp_ovxx_set(opcode, address, overlay, function) static void* ncp_set##opcode##_##address##_ov##overlay __attribute__((section(".ncp_set"), used)) = (void*)function;

#define __ncp_main_set_jump(address, function) __ncp_main_set(jump, address, function)
#define __ncp_main_set_call(address, function) __ncp_main_set(call, address, function)
#define __ncp_main_set_hook(address, function) __ncp_main_set(hook, address, function)
#define __ncp_ovxx_set_jump(address, overlay, function) __ncp_ovxx_set(jump, address, overlay, function)
#define __ncp_ovxx_set_call(address, overlay, function) __ncp_ovxx_set(call, address, overlay, function)
#define __ncp_ovxx_set_hook(address, overlay, function) __ncp_ovxx_set(hook, address, overlay, function)

#define ncp_set_jump(...) __ncp_get_macro(__VA_ARGS__, __ncp_ovxx_set_jump, __ncp_main_set_jump, )(__VA_ARGS__)
#define ncp_set_call(...) __ncp_get_macro(__VA_ARGS__, __ncp_ovxx_set_call, __ncp_main_set_call, )(__VA_ARGS__)
#define ncp_set_hook(...) __ncp_get_macro(__VA_ARGS__, __ncp_ovxx_set_hook, __ncp_main_set_hook, )(__VA_ARGS__)

// NCP Utilities

#define __ncp_main_repl(address, assembly) __ncp_main_over(address) __attribute__((naked)) void ncp_repl_##address_main() { asm(assembly); }
#define __ncp_ovxx_repl(address, overlay, assembly) __ncp_ovxx_over(address, overlay) __attribute__((naked)) void ncp_repl_##address##_ov##overlay() { asm(assembly); }
#define ncp_repl(...) __ncp_get_macro(__VA_ARGS__, __ncp_ovxx_repl, __ncp_main_repl, )(__VA_ARGS__)

#define ncp_file(path, sym) \
asm(#sym":\n.incbin \"" path "\""); \
extern "C" const char sym[];

// NCP Real Time

#define arm_opcode_b 0xEA000000
#define arm_opcode_bl 0xEB000000
#define arm_opcode_nop 0xE1A00000

static inline void ncprt_set(int address, int value) { *(int*)address = value; }
static inline void ncprt_set_jump(int address, void* function) { *(int*)address = (arm_opcode_b | (((*(int*)function >> 2) - (address >> 2) - 2) & 0xFFFFFF)); }
static inline void ncprt_set_call(int address, void* function) { *(int*)address = (arm_opcode_bl | (((*(int*)function >> 2) - (address >> 2) - 2) & 0xFFFFFF)); }

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
