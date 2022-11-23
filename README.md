# NCPatcher
A universal Nintendo DS code maker/patcher.

NCPatcher is a program that modifies the executable binaries of a Nintendo DS ROM. \
It was created because of the need to have move flexible patching features that other patchers did not have.

## Credits
This program was made with the help of the [Mamma Mia Team](https://github.com/MammaMiaTeam) members. \
NCPatcher was heavily inspired by Fireflower.

## Running

Follow the steps on how to configure, after that execute NCPatcher in your current directory which contains the ncpatcher.json file. \
NCPatcher does NOT build the ROM, it requires an extracted ROM to work with, you can use these tools to pack and unpack ROMs: \
`nds-build` and `nds-extract` included with Fireflower: https://github.com/MammaMiaTeam/Fireflower/releases/latest \
This design choice was made to allow modders to choose how they want to pack their ROMs.

## Configuration

For the program to run at least one configuration file must exist with at least one target specified.
This configuration file must be named "ncpatcher.json" and looks somewhat like this:
```json
{
  "$arm_flags": "-masm-syntax-unified -mno-unaligned-access -mfloat-abi=soft -mabi=aapcs",
  "$c_flags": "-Os -fomit-frame-pointer -ffast-math -fno-builtin -nostdlib -nodefaultlibs -nostartfiles -DSDK_GCC -DSDK_FINALROM -Dthumb=ncp_thumb",
  "$cpp_flags": "-fno-rtti -fno-exceptions -std=c++20",
  "$asm_flags": "-Os -x assembler-with-cpp -fomit-frame-pointer",
  "$ld_flags": "-lgcc,-lc,-lstdc++",
  
  "backup": "backup",
  "filesystem": "fs-data",
  "toolchain": "arm-none-eabi-",
  
  "arm7": {},
  "arm9": {
  	"target": "arm9.json",
  	"build": "build"
  },
  
  "pre-build": [],
  "post-build": [],
  
  "thread-count": 0
}
```

Structure:
 - backup - The folder to where files needed to re-patch are stored.
 - filesystem - The folder that contains the ROM data to patch.
 - toolchain - The location/prefix of your GCC toolchain executable.
 - arm7 - The ARM7 target.
   - target - The location of the target configuration.
   - build - The folder to where files generated from the build are stored.
 - arm9 - The ARM9 target.
   - target - The location of the target configuration.
   - build - The folder to where files generated from the build are stored.
 - pre-build - An array of commands to run before building.
 - post-build - An array of commands to run after building.
 - thread-count - The amount of jobs to use simultaneously while building. (Use 0 for maximum)

The target configuration file, which is specified in the ncpatcher.json looks somewhat like this:
```json
{
  "$arm_flags": "-mcpu=arm946e-s $${arm_flags}",
  "$c_flags": "${arm_flags} $${c_flags} -DSDK_ARM9 -Darm9_start=0x021901E0",
  "$cpp_flags": "${c_flags} $${cpp_flags}",
  "$asm_flags": "${arm_flags} $${asm_flags}",
  "$ld_flags": "$${ld_flags}",
  
  "c_flags": "${c_flags}",
  "cpp_flags": "${cpp_flags}",
  "asm_flags": "${asm_flags}",
  "ld_flags": "${ld_flags}",
  
  "includes": [
    ["include", false],
    ["source", false]
  ],
  "regions": [{
    "dest": "main",
    "compress": false,
    "sources": [["source/main", true]]
  }, {
    "dest": "ov9",
    "mode": "append",
    "compress": false,
    "sources": [["source/ov9", true]],
    "c_flags": "${c_flags} -DOVERLAY_ID=9",
    "cpp_flags": "${cpp_flags} -DOVERLAY_ID=9",
    "asm_flags": "${asm_flags} -DOVERLAY_ID=9"
  }],
  
  "arenaLo": "0x02065F10",
  "symbols": "symbols9.x"
}
```

Structure:
 - c_flags - The flags used when building C source files. (Can be overwritten per region)
 - cpp_flags - The flags used when building C++ source files. (Can be overwritten per region)
 - asm_flags - The flags used when building Assembly files. (Can be overwritten per region)
 - ld_flags - The flags used when linking.
 - includes - Array of paths containing the include files. (`[string path, bool searchRecursive]`)
 - regions - An array of sections to build separately.
   - dest - "main" if the code should go in the main binary, "ovX" if the code should go in overlay X.
   - mode - The mode that specifies how code should be inserted.
     - "append" adds code to the end of an existing overlay (Only option for "main").
     - "replace" deletes all the contents of an existing overlay and places your code instead.
     - "create" creates a new overlay with your code.
   - address - The address in memory for this overlay. (Optional, except for "create" mode. In "replace" mode it can be used to set a new address for the overlay)
   - length - The max length that this overlay can have. (Optional)
   - compress - If the binary should be Backwards LZ compressed.
   - sources - Array of paths containing the source files. (`[string path, bool searchRecursive]`)
   - c_flags, cpp_flags, asm_flags - Region overwriteable flags. (Optional)
 - arenaLo - The address of the value holding the address end of the main binary code in memory. (Usually the value being loaded in the first LDR of OS_GetInitArenaLo)
 - symbols - A file containing symbol definitions to include when linking.

The "$" symbol allows to define or access a variable that is for its own file scope. \
The "$$" symbol allows a target to access a variable that is defined in the ncpatcher.json file scope.

## Patches

Some patch types come defined to allow the programmer to define how the code should be modified. \
Such patches are defined by using one of the following patterns on a source file. \
*NOTE: The word "hook" can be used to refer both to any patch that causes a jump to your code and to the hook patch type.*

Patches are divided into 2 types, static and dynamic patches. \
**Static patches**: Patches that allow you to change the instructions at "compile time". These are directly applied to the binary. \
**Dynamic patches**: Patches that can be stored in the binary unapplied and only be applied at runtime when requested by the code.

### Static Patching

**To replace instructions in the binary with jumps, calls or hooks use:**

Note: \
If the code you want to hook from is THUMB then for `ncp_jump` and `ncp_call`,
you can prefix them with a "t" (eg. `ncp_tjump`, `ncp_thook`) or use the address+1. \
This is valid for the `ncp_set` variants as well (eg. `ncp_set_tjump`, `ncp_set_tcall`). \
"hook" patch type only supports hooking from ARM mode.

〇 Function tags (not stackable - only 1 hook per function)

C/C++:

```
ncp_jump(int address, [int overlay])
ncp_call(int address, [int overlay])
ncp_hook(int address, [int overlay])
```

Example:
```
ncp_jump(0x02000000)
void MyFunction1() {
    // A jump to this function will be placed at 0x02000000
}

ncp_call(0x02010000, 0)
void MyFunction2() {
    // A call to this function will be placed at 0x02010000 in overlay 0
}
```

〇 Labels (stackable - multiple hooks per function)

C/C++:

```
ncp_set_jump(int address, [int overlay], void* function)
ncp_set_call(int address, [int overlay], void* function)
ncp_set_hook(int address, [int overlay], void* function)
```

Example:
```
void MyFunction() {}

ncp_set_jump(0x02000000, MyFunction)    // A jump to MyFunction will be placed at 0x02000000
ncp_set_jump(0x02000004, MyFunction)    // A jump to MyFunction will be placed at 0x02000004
ncp_set_call(0x02010000, 0, MyFunction) // A call to MyFunction will be placed at 0x02010000 in overlay 0
ncp_set_call(0x02010004, 0, MyFunction) // A call to MyFunction will be placed at 0x02010004 in overlay 0
```

Assembly:

```
ncp_jump(int address, [int overlay])
ncp_call(int address, [int overlay])
ncp_hook(int address, [int overlay])
```

Example:
```
ncp_jump(0x02000000)    // A jump to MyFunction will be placed at 0x02000000
ncp_jump(0x02000004)    // A jump to MyFunction will be placed at 0x02000004
ncp_jump(0x02010000, 0) // A call to MyFunction will be placed at 0x02010000 in overlay 0
MyFunction:
    BX      LR
```

**To overwrite chunks of the binary with raw data or setting the destination address of a function use:**

C/C++:

```
ncp_over(int address, [int overlay])
ncp_repl(int address, [int overlay], char assembly[])
```

Example:
```
ncp_over(0x02000000)
void MyFunction() {} // This function will be placed at 0x02000000

ncp_over(0x02010000, 4)
int MyArray[] = {}; // This array will be placed at 0x02010000 in overlay 4

ncp_repl(0x02000000, "MOV R0, R0") // This instruction will be placed at 0x02000000

ncp_repl(0x02010000, 0, R"(
    MOV     R0, R0
    BX      LR
    .int    0
    .int    0
)") // This assembly code will be placed at 0x02010000 in overlay 0
```

Assembly:

```
ncp_over_begin(int address, [int overlay])
ncp_over_end()
```

Example:
```
ncp_over_begin(0x02000000) // Places the following code at 0x02000000
    MOV     R0, #1
    MOV     R1, R0
    BX      LR
ncp_over_end():
```

### Dynamic Patching

C/C++:
```
void ncprt_set(int address, int value);
void ncprt_set_jump(int address, void* function);
void ncprt_set_call(int address, void* function);
void ncprt_repl(int address, char name[]);

ncprt_repl_type(name)
```
	
Example:
```
class MyClass
{
public:
    void MyFunction(); // -Wno-pmf-conversions flag must be set to be used in ncprt_set_jump/call/hook
    static void MyStaticFunction();
};

void MyFunction() {}
void MyClass::MyFunction() {}
void MyClass::MyStaticFunction() {}

ncprt_repl_type(patch0) // Must be a unique name for each patch
void MyPatch()
{
asm(R"(
    MOV     R0, #0
    BX      LR
)");
}

void MyPatcher()
{
    ncprt_set(0x02000000, 0);                              // Sets the value at 0x02000000 to 0
    ncprt_set_jump(0x02000004, MyFunction);                // Sets the value at 0x02000004 to a jump to MyFunction
    ncprt_set_call(0x02000008, &MyClass::MyFunction);      // Sets the value at 0x02000008 to a jump to MyClass::MyFunction
    ncprt_set_call(0x0200000C, MyClass::MyStaticFunction); // Sets the value at 0x0200000C to a jump to MyClass::MyStaticFunction
    ncprt_repl(0x02000010, patch0);                        // Writes the contents of patch0 (MyPatch) to 0x02000010
}
```

### Technical Details

These are the sizes occupied by branch type patches: \
The number of bytes is how many bytes are overwritten at the specified address, the ones after
the "+" sign are bridge bytes. Those are automatically generated instructions from which the
address to patch will branch to.

| Patch Type | ARM->ARM                  | ARM->THUMB               | THUMB->ARM | THUMB->THUMB |
|------------|---------------------------|--------------------------|------------|--------------|
| jump       | 4 bytes                   | 4 bytes + 8 bridge bytes | 6 bytes    | 6 bytes      |
| call       | 4 bytes                   | 4 bytes                  | 4 bytes    | 4 bytes      |
| hook       | 4 bytes + 20 bridge bytes | -                        | -          | -            |

**IMPORTANT NOTE** \
This means that all hooks made from ARM mode to any target will always be safe because it only
ever overwrites one instruction, but when hooking from THUMB, 4 or 6 bytes are always
overwritten depending on the hook type used and not just 2 bytes. So be careful because
you might accidentally overwrite more instructions than you intended to!

If the patch type is a ARM->THUMB jump, the instruction at
`destAddr` becomes a jump to a ARM->THUMB jump bridge generated
by NCPatcher and it should look as such:

```
arm2thumb_jump_bridge:
    LDR   PC, [PC,#-4]
    .int: srcAddr+1
```

If the patch type is a hook, the instruction at
`destAddr` becomes a jump to a hook bridge generated
by NCPatcher and it should look as such:

```
hook_bridge:
    PUSH {R0-R3,R12}
    BL   srcAddr
    POP  {R0-R3,R12}
    @<unpatched destAddr's instruction>
    B    (destAddr + 4)
```
