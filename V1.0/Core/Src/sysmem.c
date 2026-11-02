/**
  ******************************************************************************
  * @file    sysmem.c
  * @brief   _sbrk 实现，堆从 .bss 之后开始，向上增长直到逼近当前栈指针
  ******************************************************************************
  */
#include <errno.h>
#include <stdint.h>

extern uint8_t _end;       /* 链接脚本给出的 .bss 结束地址 */
extern uint8_t _estack;    /* 栈顶                        */
extern uint32_t _Min_Stack_Size;

static uint8_t *__sbrk_heap_end = NULL;

void *_sbrk(ptrdiff_t incr)
{
  const uint8_t *max_heap = (uint8_t *)((uint32_t)&_estack - (uint32_t)&_Min_Stack_Size);
  uint8_t *prev_heap_end;

  if (NULL == __sbrk_heap_end)
  {
    __sbrk_heap_end = &_end;
  }

  if (__sbrk_heap_end + incr > max_heap)
  {
    errno = ENOMEM;
    return (void *)-1;
  }

  prev_heap_end = __sbrk_heap_end;
  __sbrk_heap_end += incr;

  return (void *)prev_heap_end;
}
