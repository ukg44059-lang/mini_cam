/*
******************************************************************************
* @file      STM32U575ZITXQ_FLASH.ld
* @brief     STM32U575ZIT6Q 链接脚本（从内部 Flash 启动，非 TrustZone / TZEN=0）
*
*  内存映射（RM0456）：
*    FLASH  2048K @ 0x08000000
*    SRAM1   192K @ 0x20000000  \
*    SRAM2    64K @ 0x20030000   > 三块连续，合并成 768K 的 RAM 区
*    SRAM3   512K @ 0x20040000  /
*    SRAM4    16K @ 0x28000000   独立（低功耗域，可被 LPBAM 访问）
******************************************************************************
*/

ENTRY(Reset_Handler)

/* 栈顶放在 RAM 最高地址（向下增长） */
_estack = ORIGIN(RAM) + LENGTH(RAM);

_Min_Heap_Size  = 0x400;   /* 1 KB 堆  */
_Min_Stack_Size = 0x800;   /* 2 KB 栈  */

MEMORY
{
  RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 768K
  SRAM4  (xrw) : ORIGIN = 0x28000000, LENGTH = 16K
  FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
}

SECTIONS
{
  /* 中断向量表，必须放在 Flash 最前面 */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  /* 代码 */
  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;
  } >FLASH

  /* 只读数据 */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >FLASH

  .ARM : {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >FLASH

  .preinit_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .init_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .fini_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >FLASH

  /* .data 的初值存放在 Flash 里，启动时由 Reset_Handler 搬到 RAM */
  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    *(.RamFunc)
    *(.RamFunc*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  /* 未初始化数据，启动时清零 */
  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* 检查堆栈空间是否够用，不够直接在链接期报错 */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* SRAM4：需要时用 __attribute__((section(".sram4"))) 把变量放进来 */
  .sram4 (NOLOAD) :
  {
    . = ALIGN(4);
    _ssram4 = .;
    *(.sram4)
    *(.sram4*)
    . = ALIGN(4);
    _esram4 = .;
  } >SRAM4

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
