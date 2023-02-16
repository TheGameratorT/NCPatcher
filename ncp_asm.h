#pragma once

#define __ncp_get_macro(_1, _2, _3, NAME, ...) NAME

#define __ncp_dest_set(address) .set ncp_dest, address

#define __ncp_main_label(opcode, address) .global ncp_ ##opcode## _ ##address; .type ncp_ ##opcode## _ ##address,%function; ncp_ ##opcode## _ ##address## : __ncp_dest_set(address)
#define __ncp_ovxx_label(opcode, address, overlay) .global ncp_ ##opcode## _ ##address## _ov ##overlay; .type ncp_ ##opcode## _ ##address## _ov ##overlay,%function; ncp_ ##opcode## _ ##address## _ov ##overlay## : __ncp_dest_set(address)
#define __ncp_main_section(opcode, address) .pushsection .ncp_ ##opcode## _ ##address; __ncp_dest_set(address)
#define __ncp_ovxx_section(opcode, address, overlay) .pushsection .ncp_ ##opcode## _ ##address## _ov ##overlay; __ncp_dest_set(address)

// ARM

#define __ncp_main_jump(address) __ncp_main_label(jump, address)
#define __ncp_main_call(address) __ncp_main_label(call, address)
#define __ncp_main_hook(address) __ncp_main_label(hook, address)
#define __ncp_ovxx_jump(address, overlay) __ncp_ovxx_label(jump, address, overlay)
#define __ncp_ovxx_call(address, overlay) __ncp_ovxx_label(call, address, overlay)
#define __ncp_ovxx_hook(address, overlay) __ncp_ovxx_label(hook, address, overlay)

#define ncp_jump(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_jump, __ncp_main_jump)(__VA_ARGS__)
#define ncp_call(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_call, __ncp_main_call)(__VA_ARGS__)
#define ncp_hook(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_hook, __ncp_main_hook)(__VA_ARGS__)

// THUMB

#define __ncp_main_tjump(address) __ncp_main_label(tjump, address)
#define __ncp_main_tcall(address) __ncp_main_label(tcall, address)
#define __ncp_main_thook(address) __ncp_main_label(thook, address)
#define __ncp_ovxx_tjump(address, overlay) __ncp_ovxx_label(tjump, address, overlay)
#define __ncp_ovxx_tcall(address, overlay) __ncp_ovxx_label(tcall, address, overlay)
#define __ncp_ovxx_thook(address, overlay) __ncp_ovxx_label(thook, address, overlay)

#define ncp_tjump(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_tjump, __ncp_main_tjump)(__VA_ARGS__)
#define ncp_tcall(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_tcall, __ncp_main_tcall)(__VA_ARGS__)
#define ncp_thook(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_thook, __ncp_main_thook)(__VA_ARGS__)

// OTHER

#define __ncp_main_over(address) __ncp_main_section(over, address)
#define __ncp_ovxx_over(address, overlay) __ncp_ovxx_section(over, address, overlay)

#define ncp_over(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_over, __ncp_main_over)(__VA_ARGS__)
#define ncp_endover() .popsection
