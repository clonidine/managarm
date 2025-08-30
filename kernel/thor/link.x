ENTRY(thorRtEntry)

SECTIONS {
	. = 0xFFFFFFFF80000000;

#if defined(__x86_64__)
	stubsPtr = .;
	.text.stubs : {
		*(.text.stubs)
	}
	stubsLimit = .;
#endif

	.text : { *(.text .text.*) }

	/* R data segment. */
	. += CONSTANT(COMMONPAGESIZE);
	. = ALIGN(CONSTANT(COMMONPAGESIZE)); /* Eir wants page-aligned segments. */

	.rodata : { *(.rodata .rodata.*) }

	.percpu_init : {
		percpuInitStart = .;
		*(.percpu_init)
		percpuInitEnd = .;
	}

	.eh_frame_hdr : { *(.eh_frame_hdr) }
	.eh_frame : { *(.eh_frame) }
	.note.gnu.build-id : { *(.note.gnu.build-id) }

	/* RW data segment. */
	. += CONSTANT(COMMONPAGESIZE);
	. = ALIGN(CONSTANT(COMMONPAGESIZE)); /* Eir wants page-aligned segments. */

	.init_array : {
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP (*(.init_array .ctors))
		PROVIDE_HIDDEN (__init_array_end = .);
	}

	.data : { *(.data .data.*) }
#if defined(__riscv) || defined(__aarch64__)
	/* RISC-V and AArch64 require a GOT. */
	.got : { *(.got.plt) *(.got) }
#endif
	.note.managarm : { *(.note.managarm) }

	/* BSS segment. */
	.bss : { *(.bss .bss.*) }

	/* Extra space to make a separate PHDR (so that .bss does not
	get turned into PROGBITS) */
	. += CONSTANT(COMMONPAGESIZE);
	. = ALIGN(CONSTANT(COMMONPAGESIZE));
	.percpu : {
		percpuStart = .;
		*(.percpu_head) /* Static data we want at a fixed offset (AssemblyCpuData etc) */
		*(.percpu)
		. = ALIGN(CONSTANT(COMMONPAGESIZE));
		percpuEnd = .;
	}

	/* Unallocated sections. */
	.stab 0 : { *(.stab) }
	.stabstr 0 : { *(.stabstr) }
	.stab.excl 0 : { *(.stab.excl) }
	.stab.exclstr 0 : { *(.stab.exclstr) }
	.stab.index 0 : { *(.stab.index) }
	.stab.indexstr 0 : { *(.stab.indexstr) }

	.debug 0 : { *(.debug) }
	.line 0 : { *(.line) }
	.debug_srcinfo 0 : { *(.debug_srcinfo) }
	.debug_sfnames 0 : { *(.debug_sfnames) }
	.debug_aranges 0 : { *(.debug_aranges) }
	.debug_pubnames 0 : { *(.debug_pubnames) }
	.debug_info 0 : { *(.debug_info .gnu.linkonce.wi.*) }
	.debug_abbrev 0 : { *(.debug_abbrev) }
	.debug_line 0 : { *(.debug_line) }
	.debug_frame 0 : { *(.debug_frame) }
	.debug_str 0 : { *(.debug_str) }
	.debug_loc 0 : { *(.debug_loc) }
	.debug_macinfo 0 : { *(.debug_macinfo) }
	.debug_pubtypes 0 : { *(.debug_pubtypes) }
	.debug_ranges 0 : { *(.debug_ranges) }
	.debug_weaknames 0 : { *(.debug_weaknames) }
	.debug_funcnames 0 : { *(.debug_funcnames) }
	.debug_typenames 0 : { *(.debug_typenames) }
	.debug_varnames 0 : { *(.debug_varnames) }
	.debug_gnu_pubnames 0 : { *(.debug_gnu_pubnames) }
	.debug_gnu_pubtypes 0 : { *(.debug_gnu_pubtypes) }
	.debug_types 0 : { *(.debug_types) }
	.debug_addr 0 : { *(.debug_addr) }
	.debug_line_str 0 : { *(.debug_line_str) }
	.debug_loclists 0 : { *(.debug_loclists) }
	.debug_macro 0 : { *(.debug_macro) }
	.debug_names 0 : { *(.debug_names) }
	.debug_rnglists 0 : { *(.debug_rnglists) }
	.debug_str_offsets 0 : { *(.debug_str_offsets) }

	.comment 0 : { *(.comment) }

#if !defined(__riscv) && !defined(__aarch64__)
	.got : {
		*(.got) *(.got.*) *(.igot) *(.igot.*)
	}
	ASSERT(SIZEOF(.got) == 0, "Unexpected GOT entries detected!")
#endif

	.plt : {
		*(.plt) *(.plt.*) *(.iplt)
	}
	ASSERT(SIZEOF(.plt) == 0, "Unexpected run-time procedure linkages detected!")

	.rel.dyn : {
		*(.rel.*) *(.rel_*)
	}
	ASSERT(SIZEOF(.rel.dyn) == 0, "Unexpected run-time relocations (.rel) detected!")

	.rela.dyn : {
		*(.rela.*) *(.rela_*)
	}
	ASSERT(SIZEOF(.rela.dyn) == 0, "Unexpected run-time relocations (.rela) detected!")

	/DISCARD/ : {
#if defined(__riscv)
		*(.riscv.attributes)
#endif
		*(.note.GNU-stack)
	}
}
