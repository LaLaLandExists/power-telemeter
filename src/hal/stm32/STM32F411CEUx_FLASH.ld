/**
 * STM32F411CEUx_FLASH.ld
 *
 * Custom linker script for STM32F411CEU6 (WeAct Black Pill, 512 KB flash,
 * 128 KB SRAM).
 *
 * FLASH is limited to 384 KB (sectors 0-6, 0x08000000 - 0x0805FFFF).
 * The last 128 KB (sector 7, 0x08060000 - 0x0807FFFF) is intentionally
 * excluded from the linker region and is reserved for the NVS flash KV
 * store implemented in hal_nvs_stm32.cpp.
 *
 * Derived from the standard STM32F411CEUx_FLASH.ld shipped with
 * STM32CubeIDE / ARM GCC toolchain (ST Microelectronics, BSD-3-Clause).
 * The only functional change is FLASH LENGTH = 384K instead of 512K.
 */

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size  = 0x400;
_Min_Stack_Size = 0x800;

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 384K   /* sectors 0-6 */
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
}

SECTIONS
{
  /* Interrupt vector table -- must be at the flash origin */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  /* Program code */
  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  /* Constant data */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } >FLASH

  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } >FLASH

  /* Load address of .data initialiser image in flash */
  _sidata = LOADADDR(.data);

  /* Initialised data -- copied from flash to RAM at startup */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  /* Zero-initialised data */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* Heap + stack sanity check */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /DISCARD/ :
  {
    libc.a  (*)
    libm.a  (*)
    libgcc.a(*)
  }

  .ARM.attributes 0 :
  {
    *(.ARM.attributes)
  }
}
