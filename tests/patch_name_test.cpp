// The names below are what sdk/ncp.h produces for the ncp_ and ncprt_ macros
// in C, C++ and assembly, with the "ncp_", ".ncp_" or "__ncp_" prefix removed.

#include "../source/patch/patch_name.hpp"

#include <iostream>

using namespace ncp::patch;

int main()
{
	bool passed = true;
	auto check = [&passed](bool condition, const std::string& message) {
		if (!condition)
		{
			std::cout << "FAIL: " << message << '\n';
			passed = false;
		}
	};

	auto expect = [&](std::string_view name, PatchNameForm form, PatchType type, u32 address, int overlay, const char* group) {
		PatchName out;
		std::string error;
		bool ok = parsePatchName(name, form, out, error);
		std::string what(name);
		check(ok, what + " should parse (" + error + ")");
		if (!ok)
			return;
		check(out.type == type, what + ": type");
		check(out.destAddress == address, what + ": address");
		check(out.destAddressOv == overlay, what + ": overlay");
		check(out.group == group, what + ": group is \"" + out.group + "\"");
	};

	auto reject = [&](std::string_view name, PatchNameForm form, bool warns) {
		PatchName out;
		std::string error;
		bool ok = parsePatchName(name, form, out, error);
		std::string what(name);
		check(!ok, what + " should not parse");
		check(error.empty() != warns, what + (warns ? " should warn" : " should not warn"));
	};

	const auto Label = PatchNameForm::Label;
	const auto Symver = PatchNameForm::Symver;

	// Build-time patches, unchanged by groups.
	expect("jump_0x02000000", Label, PatchType::Jump, 0x02000000, -1, "");
	expect("call_0x020A2CCC_ov12", Label, PatchType::Call, 0x020A2CCC, 12, "");
	expect("over_0x020A2768", Label, PatchType::Over, 0x020A2768, -1, "");
	expect("setthook_0x02000001_ov0", Label, PatchType::SetTHook, 0x02000001, 0, "");
	expect("call_0x020A2CCC_0_2_ABCD@0", Symver, PatchType::Call, 0x020A2CCC, -1, "");
	expect("hook_0x020A2CCC_ov3_10_200_MFRGGZDF", Symver, PatchType::Hook, 0x020A2CCC, 3, "");

	// Grouped symver patches from C and C++.
	expect("call_0x020A2CCC_gmvsl_0_2_ABCD", Symver, PatchType::Call, 0x020A2CCC, -1, "mvsl");
	expect("jump_0x020A2D00_ov0_gmvsl_menu_1_3_ABCD", Symver, PatchType::Jump, 0x020A2D00, 0, "mvsl_menu");
	expect("thook_0x02000001_gg2_2_4_ABCD", Symver, PatchType::THook, 0x02000001, -1, "g2");
	expect("call_0x02000000_g_x__0_1_ABCD", Symver, PatchType::Call, 0x02000000, -1, "_x_");

	// Grouped sections and assembly labels.
	expect("over_0x020A2768_gmvsl", Label, PatchType::Over, 0x020A2768, -1, "mvsl");
	expect("over_0x020A276C_ov5_gmvsl", Label, PatchType::Over, 0x020A276C, 5, "mvsl");
	expect("setjump_0x020CC1D4_gmvsl", Label, PatchType::SetJump, 0x020CC1D4, -1, "mvsl");
	expect("setcall_0x020CC1D8_ov3_gmvsl_menu", Label, PatchType::SetCall, 0x020CC1D8, 3, "mvsl_menu");
	expect("call_0x02000004_ov2_gmvsl", Label, PatchType::Call, 0x02000004, 2, "mvsl");

	// Addresses above 0x7FFFFFFF, which a signed parse would reject.
	expect("over_0xFFFF0000", Label, PatchType::Over, 0xFFFF0000, -1, "");

	// Names that are not patches at all are skipped silently.
	reject("dest", Label, false);
	reject("autogendata", Label, false);

	// Malformed names warn.
	reject("repl_0x02000000", Label, true);
	reject("jump_zz", Label, true);
	reject("jump_0x0200zz", Label, true);
	reject("jump_0x02000000_ovx", Label, true);
	reject("jump_0x02000000_x", Label, true);
	reject("jump_0x02000000_ov1_x", Label, true);
	reject("jump_0x02000000_g", Label, true);
	reject("jump_0x02000000_g1x", Label, true);
	reject("jump_0x02000000_gmvsl_1_2_ABCD@0", Label, true);
	reject("jump_0x02000000_x_1_2_ABCD", Symver, true);

	if (passed)
		std::cout << "all checks passed\n";
	return passed ? 0 : 1;
}
