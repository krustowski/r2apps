/*
 *  linker.ld — link script for C++ programs targeting the r2 kernel.
 *
 *  Derived from c/linker.ld, with the sections a C++ program needs that a C
 *  one does not:
 *
 *    .init_array / .fini_array   Pointers to the functions that construct and
 *                                destroy objects with static storage duration.
 *                                The default link script defines the symbols
 *                                that bracket them; a custom script has to do
 *                                it by hand, or __r2_start finds an empty range
 *                                and no global constructor ever runs.
 *
 *    .data.rel.ro                Where vtables and other relocated read-only
 *                                data land.  Must be writable.
 *
 *  The image is placed at 0x600_000: the kernel maps a private 2 MiB frame
 *  there for every process, and rejects any ELF segment falling outside
 *  0x600_000-0xA00_000.  Everything the program owns — code, data, heap arena
 *  and stack — lives in that window; see README.md, "Memory map".
 */

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
    KEEP(*(.text._start))
    KEEP(*(.text.main))
    *(.text*)
    *(.rodata*)
    *(.gcc_except_table*)
    . = ALIGN(8);
    _etext = .;
  } :text

  .data :
  {
    _sdata = .;
    *(.data.rel.ro*)
    *(.got)
    *(.got.plt)
    *(.data*)
    _edata = .;
  } :data

  /*
   *  Global constructors.  KEEP() is required: --gc-sections has no live
   *  reference to these pointers and would drop every one of them.
   *
   *  Only the .init_array ABI is honoured.  GCC has emitted .init_array
   *  rather than .ctors since 4.7; a stray .ctors section from an old object
   *  file is left where the linker puts it and is never run.
   */
  .init_array :
  {
    . = ALIGN(8);
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } :data

  .fini_array :
  {
    . = ALIGN(8);
    __fini_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
    KEEP(*(.fini_array))
    __fini_array_end = .;
  } :data

  .bss :
  {
    . = ALIGN(16);
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(16);
    _ebss = .;
  } :data

  _end = .;
  __r2_image_end = .;

  /*
   *  Nothing unwinds: the runtime is built -fno-exceptions and the stubs in
   *  cxxabi.cpp make a throw a halt.  Dropping the unwind tables keeps the
   *  image small, which matters when the whole process fits in 2 MiB.
   */
  /DISCARD/ :
  {
    *(.eh_frame*)
    *(.note.*)
    *(.comment)
  }
}
