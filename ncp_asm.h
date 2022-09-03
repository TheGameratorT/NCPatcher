#pragma once

#define __ncp_get_macro(_1, _2, _3, NAME, ...) NAME

#define __ncp_main_label(opcode, address) ncp_ ##opcode## _ ##address## :
#define __ncp_ovxx_label(opcode, address, overlay) ncp_ ##opcode## _ ##address## _ov ##overlay## :

#define __ncp_main_jump(address) __ncp_main_label(jump, address)
#define __ncp_main_call(address) __ncp_main_label(call, address)
#define __ncp_main_hook(address) __ncp_main_label(hook, address)
#define __ncp_main_over(address) __ncp_main_label(over, address)
#define __ncp_ovxx_jump(address, overlay) __ncp_ovxx_label(jump, address, overlay)
#define __ncp_ovxx_call(address, overlay) __ncp_ovxx_label(call, address, overlay)
#define __ncp_ovxx_hook(address, overlay) __ncp_ovxx_label(hook, address, overlay)
#define __ncp_ovxx_over(address, overlay) __ncp_ovxx_label(over, address, overlay)

#define ncp_jump(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_jump, __ncp_main_jump)(__VA_ARGS__)
#define ncp_call(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_call, __ncp_main_call)(__VA_ARGS__)
#define ncp_hook(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_hook, __ncp_main_hook)(__VA_ARGS__)
#define ncp_over(...) __ncp_get_macro(__VA_ARGS__, , __ncp_ovxx_over, __ncp_main_over)(__VA_ARGS__)
