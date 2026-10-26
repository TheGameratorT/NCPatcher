# Patches

Some patch types come defined to allow the programmer to define how the code should be modified. \
Such patches are defined by using one of the following patterns on a source file. \
*NOTE: The word "hook" can be used to refer both to any patch that causes a jump to your code and to the hook patch type.*

Patches are divided into 2 types, static and dynamic patches. \
**Static patches**: Patches that allow you to change the instructions at "compile time". These are directly applied to the binary. \
**Dynamic patches**: Patches that can be stored in the binary unapplied and only be applied at runtime when requested by the code.

## Static Patching

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
ncp_over(int address, [int overlay])
ncp_endover()
```

Example:
```
ncp_over(0x02000000) // Places the following code at 0x02000000
    MOV     R0, #1
    MOV     R1, R0
    BX      LR
ncp_endover():
```

## Dynamic Patching

The `ncprt_*` names stand for the NCPatcher Runtime: unlike the static patches
above, which are spliced into the binary at build time, these write to memory
while the ROM is running.

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

## Technical Details

These are the sizes occupied by branch type patches: \
The number of bytes is how many bytes are overwritten at the specified address, the ones after
the "+" sign are bridge bytes. Those are automatically generated instructions from which the
address to patch will branch to.

| Patch Type | ARM->ARM                  | ARM->THUMB                | THUMB->ARM | THUMB->THUMB |
|------------|---------------------------|---------------------------|------------|--------------|
| jump       | 4 bytes                   | 4 bytes + 8 bridge bytes  | 8 bytes    | 8 bytes      |
| call       | 4 bytes                   | 4 bytes                   | 4 bytes    | 4 bytes      |
| hook       | 4 bytes + 20 bridge bytes | 4 bytes + 20 bridge bytes | -          | -            |

**IMPORTANT NOTE** \
This means that all hooks made from ARM mode to any target will always be safe because it only
ever overwrites one instruction, but when hooking from THUMB, 4 or 8 bytes are always
overwritten depending on the hook type used and not just 2 bytes. So be careful because
you might accidentally overwrite more instructions than you intended to!

A jump patch is equivalent to a branch instruction (`B srcAddr`). \
A call patch is equivalent to a linked branch instruction (`BL srcAddr`).

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
    BL   srcAddr        @ BLX if srcAddr is THUMB
    POP  {R0-R3,R12}
    @<unpatched destAddr's instruction>
    B    (destAddr + 4)
```
