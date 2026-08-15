/*
******************************************************************************
**

**  File        : LinkerScript.ld
**
**  Author		: STM32CubeMX
**
**  Abstract    : STM32F407ZGTx 系列链接脚本
**                1024KB FLASH 和 192KB RAM
**
**                堆大小、栈大小和栈位置请按应用需求修改。
**
**                如果使用了外部存储器，请修改对应的存储区基地址和大小。
**
**  Target      : STMicroelectronics STM32
**
**  Distribution: The file is distributed “as is,” without any warranty
**                of any kind.
**
*****************************************************************************
** @attention
**
** <h2><center>&copy; COPYRIGHT(c) 2025 STMicroelectronics</center></h2>
**
** Redistribution and use in source and binary forms, with or without modification,
** are permitted provided that the following conditions are met:
**   1. Redistributions of source code must retain the above copyright notice,
**      this list of conditions and the following disclaimer.
**   2. Redistributions in binary form must reproduce the above copyright notice,
**      this list of conditions and the following disclaimer in the documentation
**      and/or other materials provided with the distribution.
**   3. Neither the name of STMicroelectronics nor the names of its contributors
**      may be used to endorse or promote products derived from this software
**      without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
** DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
** FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
** SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
** CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
** OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**
*****************************************************************************
*/

/* 入口点 */
ENTRY(Reset_Handler)

/* 指定存储区 */
MEMORY
{
RAM (xrw)      : ORIGIN = 0x20000000, LENGTH = 128K
CCMRAM (xrw)      : ORIGIN = 0x10000000, LENGTH = 64K
FLASH (rx)      : ORIGIN = 0x8000000, LENGTH = 1024K
}

/* 用户栈的最高地址 */
_estack = ORIGIN(RAM) + LENGTH(RAM);    /* RAM 末尾 */
_sstack = _estack - _Min_Stack_Size;
/* 如果堆和栈超出 RAM 范围，则生成链接错误 */
_Min_Heap_Size = 0x200;      /* 所需堆大小 */
_Min_Stack_Size = 0x400; /* 所需栈大小 */

/* 定义输出段 */
SECTIONS
{
  /* 启动代码先放进 FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* 中断向量表 */
    . = ALIGN(4);
  } >FLASH

  /* 程序代码和其他数据放进 FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text 段（代码） */
    *(.text*)          /* .text* 段（代码） */
    *(.glue_7)         /* ARM 到 Thumb 的跳转桥接代码 */
    *(.glue_7t)        /* Thumb 到 ARM 的跳转桥接代码 */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* 定义全局符号：代码段末尾地址 */
  } >FLASH

  /* 常量数据放进 FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata 段（常量、字符串等） */
    *(.rodata*)        /* .rodata* 段（常量、字符串等） */
    . = ALIGN(4);
  } >FLASH

  .ARM.extab (READONLY) : /* 该 "READONLY" 关键字仅在 GCC11 及以后版本支持，若使用 GCC10 或更早版本请删除 */
  {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >FLASH

  .ARM (READONLY) : /* 该 "READONLY" 关键字仅在 GCC11 及以后版本支持，若使用 GCC10 或更早版本请删除 */
  {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >FLASH

  .preinit_array (READONLY) : /* 该 "READONLY" 关键字仅在 GCC11 及以后版本支持，若使用 GCC10 或更早版本请删除 */
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .init_array (READONLY) : /* 该 "READONLY" 关键字仅在 GCC11 及以后版本支持，若使用 GCC10 或更早版本请删除 */
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .fini_array (READONLY) : /* 该 "READONLY" 关键字仅在 GCC11 及以后版本支持，若使用 GCC10 或更早版本请删除 */
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >FLASH

  _siccmram = LOADADDR(.ccmram);

  /* CCM-RAM 段
  *
  * 重要提示！
  * 如果要把带初值的变量放到该段，
  * 需要修改启动代码，让它把初始化值拷到 CCM-RAM 中。
  */
  .ccmram :
  {
    . = ALIGN(4);
    _sccmram = .;       /* 定义全局符号：ccmram 起始地址 */
    *(.ccmram)
    *(.ccmram*)

    . = ALIGN(4);
    _eccmram = .;       /* 定义全局符号：ccmram 末尾地址 */
  } >CCMRAM AT> FLASH

  /* 供启动代码初始化 .data 段使用 */
  _sidata = LOADADDR(.data);

  /* 已初始化数据段放进 RAM，初值的加载地址(LMA)存放在 FLASH 的拷贝中 */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* 定义全局符号：data 段起始地址 */
    *(.data)           /* .data 段 */
    *(.data*)          /* .data* 段 */
    *(.RamFunc)        /* .RamFunc 段 */
    *(.RamFunc*)       /* .RamFunc* 段 */

    . = ALIGN(4);
  } >RAM AT> FLASH

 /* 已初始化的 TLS 数据段 */
  .tdata : ALIGN(4)
  {
    *(.tdata .tdata.* .gnu.linkonce.td.*)
    . = ALIGN(4);
    _edata = .;        /* 定义全局符号：data 段末尾地址 */
    PROVIDE(__data_end = .);
    PROVIDE(__tdata_end = .);
  } >RAM AT> FLASH

  PROVIDE( __tdata_start = ADDR(.tdata) );
  PROVIDE( __tdata_size = __tdata_end - __tdata_start );

  PROVIDE( __data_start = ADDR(.data) );
  PROVIDE( __data_size = __data_end - __data_start );

  PROVIDE( __tdata_source = LOADADDR(.tdata) );
  PROVIDE( __tdata_source_end = LOADADDR(.tdata) + SIZEOF(.tdata) );
  PROVIDE( __tdata_source_size = __tdata_source_end - __tdata_source );

  PROVIDE( __data_source = LOADADDR(.data) );
  PROVIDE( __data_source_end = __tdata_source_end );
  PROVIDE( __data_source_size = __data_source_end - __data_source );
  /* 未初始化数据段 */
  .tbss (NOLOAD) : ALIGN(4)
  {
     /* 供启动代码初始化 .bss 段使用 */
     _sbss = .;         /* 定义全局符号：bss 段起始地址 */
    __bss_start__ = _sbss;
    *(.tbss .tbss.*)
    . = ALIGN(4);
    PROVIDE( __tbss_end = . );
  } >RAM

  PROVIDE( __tbss_start = ADDR(.tbss) );
  PROVIDE( __tbss_size = __tbss_end - __tbss_start );
  PROVIDE( __tbss_offset = ADDR(.tbss) - ADDR(.tdata) );

  PROVIDE( __tls_base = __tdata_start );
  PROVIDE( __tls_end = __tbss_end );
  PROVIDE( __tls_size = __tls_end - __tls_base );
  PROVIDE( __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) );
  PROVIDE( __tls_size_align = (__tls_size + __tls_align - 1) & ~(__tls_align - 1) );
  PROVIDE( __arm32_tls_tcb_offset = MAX(8, __tls_align) );
  PROVIDE( __arm64_tls_tcb_offset = MAX(16, __tls_align) );

  .bss (NOLOAD) : ALIGN(4)
  {
    *(.bss)
    *(.bss*)
    *(COMMON)

      . = ALIGN(4);
    _ebss = .;         /* 定义全局符号：bss 段末尾地址 */
    __bss_end__ = _ebss;
      PROVIDE( __bss_end = .);
  } >RAM
  PROVIDE( __non_tls_bss_start = ADDR(.bss) );

  PROVIDE( __bss_start = __tbss_start );
  PROVIDE( __bss_size = __bss_end - __bss_start );

  /* 用户堆栈段，用于检查 RAM 中是否有足够的剩余空间 */
  ._user_heap_stack (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM



  /* 从标准库中去除无用信息 */
  /DISCARD/ :
  {
    libc.a:* ( * )
    libm.a:* ( * )
    libgcc.a:* ( * )
  }

}
