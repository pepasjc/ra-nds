/* ranet.bin (include/ra_netblob.h): linked where nds-bootstrap-ra's card
   engine loads it, header first; .bss (the arena included) is cleared by
   the blob's init and must end inside RA_NET_BLOB_SIZE. */
OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(ranetHeader)

MEMORY {
	blob : ORIGIN = 0x0CE80000, LENGTH = 0x80000
}

SECTIONS
{
	.header : { KEEP(*(.header)) } >blob
	.text : {
		*(.text .text.* .stub .gnu.linkonce.t.*)
		*(.twl .twl.*)
		*(.glue_7t) *(.glue_7) *(.vfp11_veneer) *(.v4_bx)
		. = ALIGN(4);
	} >blob
	.rodata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		. = ALIGN(4);
	} >blob
	.data : {
		*(.data .data.* .gnu.linkonce.d.*)
		. = ALIGN(4);
	} >blob
	__image_end = .;

	.bss (NOLOAD) : {
		__bss_start = .;
		*(.bss .bss.* .gnu.linkonce.b.* COMMON)
		*(.twl_bss)
		. = ALIGN(32);
		__bss_end = .;
	} >blob

	/DISCARD/ : {
		*(.ARM.exidx*) *(.ARM.extab*) *(.init_array*) *(.fini_array*)
		*(.comment) *(.note*)
	}
}
