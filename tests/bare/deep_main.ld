ENTRY(_start)
SECTIONS {
  . = 1M;
  tb_code_start = .;
  .text : { KEEP(*(.multiboot)) *(.text*) }
  .rodata : { *(.rodata*) }
  . = ALIGN(4096);
  tb_code_end = .;
  .data : { *(.data*) }
  .tbtls : { tb_tls_start = .; KEEP(*(.tbtls)) tb_tls_end = .; }
  .bss : { *(COMMON) *(.bss*) }
  . = ALIGN(0x400000);
  tb_heap_area = .;
  .tbheap (NOLOAD) : { *(.tbheap) }
  . = ALIGN(4096);
  tb_kernel_end = .;
}
