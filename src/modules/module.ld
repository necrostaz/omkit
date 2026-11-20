/* OMKit module: image at address 0, relocations are kept (-q), core relocates the image at load. tools/mkmod.py */
OUTPUT_ARCH(mn10300)
ENTRY(_module_init)

SECTIONS
{
    . = 0;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) . = ALIGN(4); }
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) . = ALIGN(4); }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
