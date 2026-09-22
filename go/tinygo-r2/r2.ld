/*
 *  r2.ld --- memory layout for a Go program on rou2exOS.
 *
 *  Every userland process gets one private 2 MiB page mapped at 0x600000
 *  (mem::pages::create_user_page_table gives slot N its own physical frame
 *  there), and that page has to hold the program, its stack and its heap.
 *  Nothing above 0x7FFFFF belongs to us: it is either shared with the kernel
 *  or another process's business.
 *
 *      0x600000  .text / .rodata / .data / .bss
 *                _heap_start ... _heap_end        (the GC's arena)
 *      0x7A0000  stack, growing down from _stack_top
 *      0x7C0000  reserved --- see below
 *      0x800000  end of our frame
 *
 *  The 256 KiB below the top of the frame is left alone on purpose.  The
 *  kernel picks a process's initial stack from a fixed per-slot table
 *  (input/elf.rs), and the last two slots are at 0x7F0000 and 0x7D0000 ---
 *  inside this frame.  A program unlucky enough to land in slot 8 or 9 would
 *  otherwise have the argv frame written straight through its own heap.
 */

ENTRY(_start)

SECTIONS
{
  . = 0x600000;

  .text : ALIGN(16)
  {
    KEEP(*(.text._start))
    *(.text*)
  }

  .rodata : ALIGN(16)
  {
    *(.rodata*)
  }

  /*
   *  The conservative collector scans everything between _globals_start and
   *  _globals_end for pointers, so these have to bracket .data and .bss and
   *  nothing that matters may sit outside them.
   */
  . = ALIGN(16);
  _globals_start = .;

  .data : ALIGN(16)
  {
    *(.data*)
  }

  .bss (NOLOAD) : ALIGN(16)
  {
    *(.bss*)
    *(COMMON)
  }

  . = ALIGN(16);
  _globals_end = .;

  /*
   *  The heap takes whatever is left between the program and the stack.  It
   *  is deliberately not a section: giving it one would put it in a PT_LOAD
   *  segment and make the kernel zero 1.5 MiB on every exec.
   */
  . = ALIGN(4096);
  _heap_start = .;
  _heap_end   = 0x7A0000;

  /*
   *  128 KiB of stack.  There is no guard page to be had --- the frame is a
   *  single 2 MiB huge page, so the finest protection available is the whole
   *  page --- which means an overflow runs quietly into the top of the heap.
   */
  _stack_bottom = 0x7A0000;
  _stack_top    = 0x7C0000;

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.debug*)
  }
}

ASSERT(_heap_start < _heap_end, "r2.ld: the program has grown into its own heap")
