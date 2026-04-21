/* Linker script for a basic C project memory layout */
ENTRY(_start)

PHDRS {
  text PT_LOAD FLAGS(5); /* R+X */
  data PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS
{
  . = 0x600000;

  .text :
  {
    *(.note.gnu.build-id)
    KEEP(*(.text.main))
    *(.text*)
    *(.rodata*)
    _etext = .;
  } :text

  .data :
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

  _end = .;
}
