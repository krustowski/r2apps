/* Linker script for a basic C++ project memory layout */
ENTRY(main)

PHDRS {
  text PT_LOAD FLAGS(5); /* R+X */
  data PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS
{
  . = 0x600000;

  .text :
  {
    KEEP(*(.text.main))
    *(.text.)
    *(.rodata*)
    _etext = .;
  } :text

  .data : AT (ADDR(.text) + SIZEOF(.text))
  {
    _sdata = .;
    *(.data*)
    _edata = .;
  } :data

  .bss :
  {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    _ebss = .;
  } :data

  /* Stack and heap can be defined here if needed */
  _end = .;

  .stack : 
  {
    . = ALIGN(16);
    stack_bottom = .;
    . += 0x10000; /* 64 KiB stack */
    stack_top = .;
  } :data
}
