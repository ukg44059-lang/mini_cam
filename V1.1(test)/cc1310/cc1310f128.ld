/*
 * CC1310F128 链接脚本（GCC）
 *
 * 芯片：CC1310F128RGZ（LAUNCHXL-CC1310 板载），Cortex-M3
 *   Flash 128 KB @ 0x00000000
 *   SRAM   20 KB @ 0x20000000
 *
 * ★ CCFG 是这颗芯片最容易踩的地方 ★
 *
 * CC13xx/CC26xx 的 ROM bootloader 每次复位都会去读 flash 最后 88 字节的
 * CCFG(Customer Configuration)，用它决定：要不要允许 JTAG 调试、bootloader
 * 行为、image 是否有效、DCDC 设置等等。
 *
 *   - CCFG 全是 0xFF（也就是擦干净没写）时，多数字段的"默认"含义是允许调试，
 *     所以空片能连上；但一旦你烧了一个【不带 CCFG】的程序，最后那一页仍是
 *     0xFF 还好，怕的是烧进去一份**错误**的 CCFG —— 比如把 TI_FA_ENABLE /
 *     bootloader backdoor 关掉、把 debug 权限位写成禁止，
 *     **芯片会直接锁死，XDS110 再也连不上，只能整片擦除甚至报废**。
 *
 *   - 所以这里显式给 CCFG 划一段独立区域，并在 Makefile 里强制链接
 *     Drivers/cc13xxware/startup_files/ccfg.c（TI 官方的默认值，调试是开着的）。
 *     千万不要图省事把 .ccfg 段丢掉。
 *
 * CCFG 占 flash 最后 88(0x58) 字节：0x0001FFA8 ~ 0x0001FFFF。
 */

MEMORY
{
    /* 正常代码区：128K 减掉最后给 CCFG 的 88 字节 */
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 128K - 0x58
    CCFG  (rx)  : ORIGIN = 0x0001FFA8, LENGTH = 0x58
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

/* 栈顶放在 SRAM 最高地址，向下生长。startup_gcc.c 的向量表第 0 项要用它 */
_estack = ORIGIN(SRAM) + LENGTH(SRAM);

ENTRY(ResetISR)

SECTIONS
{
    .text :
    {
        /* 向量表必须在 flash 最开头，KEEP 防止被 --gc-sections 当无用代码删掉 */
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)

        KEEP(*(.init))
        KEEP(*(.fini))

        /* C 运行时的构造/析构表 */
        . = ALIGN(4);
        KEEP(*(.preinit_array*))
        KEEP(*(.init_array*))
        KEEP(*(.fini_array*))

        . = ALIGN(4);
    } > FLASH

    /* ARM 的异常展开表，用不到但留着不占多少 */
    .ARM.exidx :
    {
        *(.ARM.exidx*)
    } > FLASH

    /*
     * _etext 必须紧跟在 flash 里所有只读内容之后：
     * startup_gcc.c 的 ResetISR 就是从 _etext 开始，把 .data 的初值搬进 SRAM 的。
     * 位置写错的话全局变量初值全是垃圾，而且现象很怪（有的变量对有的不对）。
     */
    . = ALIGN(4);
    _etext = .;

    .data : AT (_etext)
    {
        . = ALIGN(4);
        _data = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM

    .bss :
    {
        . = ALIGN(4);
        _bss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM

    /* 剩下的 SRAM 全给堆和栈，链接时检查一下够不够 */
    ._user_heap_stack :
    {
        . = ALIGN(8);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > SRAM

    /* CCFG：见文件头的说明，绝对不能省 */
    .ccfg :
    {
        KEEP(*(.ccfg))
    } > CCFG

    /DISCARD/ :
    {
        libc.a ( * )
        libm.a ( * )
        libgcc.a ( * )
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}

_Min_Heap_Size  = 0x200;    /* 512 B，printf 之类要用 */
_Min_Stack_Size = 0x400;    /* 1 KB */
