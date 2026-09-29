/* ranet: devkitARM's ds_arm7.ld with the ARM7 program run from main RAM
   instead of IWRAM (the WiFi stack and TLS do not fit in 96K): loaded at
   0x02380000 as usual, run at 0x023a8000 (below 0x023e3f00: nds-bootstrap-hb,
   which TWiLight Menu++ may launch homebrew with, keeps its SD cache and code
   from there to 4MB).  Only the stacks stay in IWRAM.
   Not in place (crt0 first copies its ARM7/ARM9 sync code to the run
   address) and not above 4MB (the ARM9 may not have enabled 16MB yet, so it
   would land on the ARM9 program). */
/*--------------------------------------------------------------------------------
	This Source Code Form is subject to the terms of the Mozilla Public License,
	v. 2.0. If a copy of the MPL was not distributed with this file, You can
	obtain one at https://mozilla.org/MPL/2.0/.
--------------------------------------------------------------------------------*/
OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)


PHDRS {
	crt0  PT_LOAD FLAGS(7);
	arm7  PT_LOAD FLAGS(7);
	arm7i PT_LOAD FLAGS(0x100007);
}


MEMORY {
	ewram  : ORIGIN = 0x02380000, LENGTH = 12M - 512K
	run7   : ORIGIN = 0x023a8000, LENGTH = 0x3bf00
	rom    : ORIGIN = 0x08000000, LENGTH = 32M
	iwram  : ORIGIN = 0x037f8000, LENGTH = 96K

	twl_ewram : ORIGIN = 0x02e80000, LENGTH = 512K - 64K
	twl_iwram : ORIGIN = 0x03000000, LENGTH = 256K
}

__iwram_start	=	ORIGIN(iwram);
__iwram_top	=	ORIGIN(iwram)+ LENGTH(iwram);

__sp_irq	=	__iwram_top - 0x100;
__sp_svc	=	__sp_irq - 0x100;
__sp_usr	=	__sp_svc - 0x100;

__irq_flags	=	0x04000000 - 8;
__irq_flagsaux	=	0x04000000 - 0x40;
__irq_vector	=	0x04000000 - 4;

SECTIONS
{

	.twl :
	{
		__arm7i_lma__ = LOADADDR(.twl);
		__arm7i_start__ = .;
		*(.twl)
		*.twl*(.text .stub .text.* .gnu.linkonce.t.*)
		*.twl*(.rodata)
		*.twl*(.roda)
		*.twl*(.rodata.*)
		*.twl*(.data)
		*.twl*(.data.*)
		*.twl*(.gnu.linkonce.d*)
		. = ALIGN(4);
		__arm7i_end__ = .;
	} >twl_iwram AT>twl_ewram :arm7i

	.twl_bss ALIGN(4) (NOLOAD) :
	{
		__twl_bss_start__ = .;
		*(.twl_bss)
		*.twl.*(.dynbss)
		*.twl.*(.gnu.linkonce.b*)
		*.twl.*(.bss*)
		*.twl.*(COMMON)
		. = ALIGN(4);
		__twl_bss_end__ = .;
	} >twl_iwram :NONE

	.crt0	:
	{
		KEEP (*(.crt0))
		. = ALIGN(4);  /* REQUIRED. LD is flaky without it. */
	} >ewram :crt0

	.text :
	{
		__arm7_lma__ = LOADADDR(.text);
		__arm7_start__ = .;
		KEEP (*(SORT_NONE(.init)))
		*(.plt)
		*(.text .stub .text.* .gnu.linkonce.t.*)
		KEEP (*(.text.*personality*))
		/* .gnu.warning sections are handled specially by elf32.em.  */
		*(.gnu.warning)
		*(.glue_7t) *(.glue_7) *(.vfp11_veneer)
		. = ALIGN(4);  /* REQUIRED. LD is flaky without it. */
	} >run7 AT>ewram :arm7

	.fini           :
	{
		KEEP (*(.fini))
	} >run7 AT>ewram

	.rodata :
	{
		*(.rodata)
		*all.rodata*(*)
		*(.roda)
		*(.rodata.*)
		*(.gnu.linkonce.r*)
		SORT(CONSTRUCTORS)
		. = ALIGN(4);   /* REQUIRED. LD is flaky without it. */
	} >run7 AT>ewram

	.ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >run7 AT>ewram

	.ARM.exidx   : {
		__exidx_start = .;
		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
		__exidx_end = .;
	 } >run7 AT>ewram

/* Ensure the __preinit_array_start label is properly aligned.  We
   could instead move the label definition inside the section, but
   the linker would then create the section even if it turns out to
   be empty, which isn't pretty.  */
	.preinit_array     : {
		. = ALIGN(32 / 8);
		PROVIDE (__preinit_array_start = .);
		KEEP (*(.preinit_array))
		PROVIDE (__preinit_array_end = .);
	} >run7 AT>ewram

	.init_array     : {
		PROVIDE (__init_array_start = .);
		KEEP (*(.init_array))
		PROVIDE (__init_array_end = .);
	} >run7 AT>ewram

	.fini_array     : {
		PROVIDE (__fini_array_start = .);
		KEEP (*(.fini_array))
		PROVIDE (__fini_array_end = .);
	} >run7 AT>ewram

	.ctors :
	{
	/* gcc uses crtbegin.o to find the start of the constructors, so
		we make sure it is first.  Because this is a wildcard, it
		doesn't matter if the user does not actually link against
		crtbegin.o; the linker won't look for a file to match a
		wildcard.  The wildcard also means that it doesn't matter which
		directory crtbegin.o is in.  */
		KEEP (*crtbegin.o(.ctors))
		KEEP (*(EXCLUDE_FILE (*crtend.o) .ctors))
		KEEP (*(SORT(.ctors.*)))
		KEEP (*(.ctors))
		. = ALIGN(4);   /* REQUIRED. LD is flaky without it. */
	} >run7 AT>ewram

	.dtors :
	{
		KEEP (*crtbegin.o(.dtors))
		KEEP (*(EXCLUDE_FILE (*crtend.o) .dtors))
		KEEP (*(SORT(.dtors.*)))
		KEEP (*(.dtors))
		. = ALIGN(4);   /* REQUIRED. LD is flaky without it. */
	} >run7 AT>ewram

	.eh_frame :
	{
		KEEP (*(.eh_frame))
		. = ALIGN(4);   /* REQUIRED. LD is flaky without it. */
	} >run7 AT>ewram

	.gcc_except_table :
	{
		*(.gcc_except_table)
		. = ALIGN(4);   /* REQUIRED. LD is flaky without it. */
	} >run7 AT>ewram
	.jcr            : { KEEP (*(.jcr)) } >run7 AT>ewram
	.got            : { *(.got.plt) *(.got) } >run7 AT>ewram

	.data ALIGN(4) : 	{
		__data_start = ABSOLUTE(.);
		*(.data)
		*(.data.*)
		*(.gnu.linkonce.d*)
		CONSTRUCTORS
		. = ALIGN(4);
		__data_end = ABSOLUTE(.) ;
	} >run7 AT>ewram

	.bss ALIGN(4) (NOLOAD) :
	{
		__arm7_end__ = .;
		__bss_start = ABSOLUTE(.);
		__bss_start__ = ABSOLUTE(.);
		*(.dynbss)
		*(.gnu.linkonce.b*)
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);    /* REQUIRED. LD is flaky without it. */
		__bss_end__ = ABSOLUTE(.);
		__end__ = ABSOLUTE(.);
	} >run7

	/* Stabs debugging sections.  */
	.stab 0 : { *(.stab) }
	.stabstr 0 : { *(.stabstr) }
	.stab.excl 0 : { *(.stab.excl) }
	.stab.exclstr 0 : { *(.stab.exclstr) }
	.stab.index 0 : { *(.stab.index) }
	.stab.indexstr 0 : { *(.stab.indexstr) }
	.comment 0 : { *(.comment) }
	/*	DWARF debug sections.
		Symbols in the DWARF debugging sections are relative to the beginning
		of the section so we begin them at 0.  */
	/* DWARF 1 */
	.debug          0 : { *(.debug) }
	.line           0 : { *(.line) }
	/* GNU DWARF 1 extensions */
	.debug_srcinfo  0 : { *(.debug_srcinfo) }
	.debug_sfnames  0 : { *(.debug_sfnames) }
	/* DWARF 1.1 and DWARF 2 */
	.debug_aranges  0 : { *(.debug_aranges) }
	.debug_pubnames 0 : { *(.debug_pubnames) }
	/* DWARF 2 */
	.debug_info     0 : { *(.debug_info) }
	.debug_abbrev   0 : { *(.debug_abbrev) }
	.debug_line     0 : { *(.debug_line) }
	.debug_frame    0 : { *(.debug_frame) }
	.debug_str      0 : { *(.debug_str) }
	.debug_loc      0 : { *(.debug_loc) }
	.debug_macinfo  0 : { *(.debug_macinfo) }
	/* SGI/MIPS DWARF 2 extensions */
	.debug_weaknames 0 : { *(.debug_weaknames) }
	.debug_funcnames 0 : { *(.debug_funcnames) }
	.debug_typenames 0 : { *(.debug_typenames) }
	.debug_varnames  0 : { *(.debug_varnames) }
	/* These must appear regardless of  .  */
}