/* core image: the loader header precedes the body in the file, so the body starts at 0x6345a010.
 * The image together with the stack must not go past CORE_CEIL: the module arena starts above it (the driver puts its start
 * right after the end of the image, rounded up to 4 KB). The core 0.53 stack ran into the arena and corrupted a module on the
 * camera, causing a reboot loop. The linker refuses an image that does not fit. CORE_CEIL is 0x634b0000: the arena's region
 * 0xa3470000..0xa3780000 was checked word by word on the camera, so the image may grow into its first 256 KB (the film look's
 * editors, core 1.03: 32 KB; its grain item, core 1.15: 32 KB more; the debug service, the test scenario and the optics
 * layer, core 1.42: 64 KB more; the optics layer's colours and the look's white left 1 KB of it, core 1.45: 64 KB more)
 * and the arena starts after it. The ceiling is a budget, not a reservation: the arena starts
 * at the image's real end, and it is 2.8 MB, of which the bundled modules use less than 1 MB */
OUTPUT_ARCH(mn10300)
ENTRY(_start)

MEMORY
{
    ram (rwx) : ORIGIN = 0x6345a010, LENGTH = 0x634b0000 - 0x6345a010
}

SECTIONS
{
    .text : { *(.text.start) *(.text .text.*) } > ram
    .rodata : { *(.rodata .rodata.*) } > ram
    .data : { *(.data .data.*) . = ALIGN(4); } > ram
    .bss (NOLOAD) : {
        ___bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        ___bss_end = .;
    } > ram
    .stack (NOLOAD) : {
        . = ALIGN(16);
        ___stack_bottom = .;
        . += 0x4000;
        ___stack_top = .;
        ___image_end = .;
    } > ram
    /DISCARD/ : { *(.comment) *(.note*) }
}
ASSERT(___image_end <= 0x634b0000, "core: image with stack does not fit below the module arena")
