// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// 每个 CPU 一条 freelist
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

static struct run *steal_freelist(int cpu);

void
kinit()
{
  // 初始化每个 CPU 的锁
  for(int i = 0; i < NCPU; i++)
    initlock(&kmem[i].lock, "kmem");

  // 初始空闲页入链
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;

  // 按页释放整段物理内存
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  int cpu;
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  // 转成 freelist 节点
  r = (struct run*)pa;

  // 挂回当前 CPU 的链表
  push_off();
  cpu = cpuid();
  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  release(&kmem[cpu].lock);
  pop_off();
}

static struct run *
steal_freelist(int cpu)
{
  struct run *slow, *fast, *prev;

  // 轮询其他 CPU
  for(int i = 1; i < NCPU; i++){
    int victim = (cpu + i) % NCPU;

    acquire(&kmem[victim].lock);
    if(kmem[victim].freelist == 0){
      release(&kmem[victim].lock);
      continue;
    }

    // 快慢指针拆一半
    slow = kmem[victim].freelist;
    fast = kmem[victim].freelist;
    prev = 0;
    while(fast != 0 && fast->next != 0){
      prev = slow;
      slow = slow->next;
      fast = fast->next->next;
    }

    // 断开被偷走的部分
    if(prev == 0)
      kmem[victim].freelist = 0;
    else
      prev->next = 0;

    release(&kmem[victim].lock);
    return slow;
  }

  return 0;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  int cpu;
  struct run *r;
  struct run *rest;

  // 固定当前 CPU
  push_off();
  cpu = cpuid();

  // 先取本地页
  acquire(&kmem[cpu].lock);
  r = kmem[cpu].freelist;
  if(r != 0)
    kmem[cpu].freelist = r->next;
  release(&kmem[cpu].lock);

  // 本地空时去偷
  if(r == 0){
    r = steal_freelist(cpu);
    if(r != 0){
      // 第一页直接返回
      rest = r->next;
      r->next = 0;

      // 剩余页挂回本地
      if(rest != 0){
        acquire(&kmem[cpu].lock);
        struct run *tail = rest;
        while(tail->next != 0)
          tail = tail->next;
        tail->next = kmem[cpu].freelist;
        kmem[cpu].freelist = rest;
        release(&kmem[cpu].lock);
      }
    }
  }

  pop_off();

  // 填充调试字节
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

uint64
freemem(void)
{
  struct run *r;
  uint64 free_pages = 0;

  // 汇总所有 CPU 的空闲页
  for(int i = 0; i < NCPU; i++){
    acquire(&kmem[i].lock);
    for(r = kmem[i].freelist; r != 0; r = r->next)
      free_pages++;
    release(&kmem[i].lock);
  }

  return free_pages * PGSIZE;
}
