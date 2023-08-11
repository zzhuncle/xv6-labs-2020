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

// lab6
// 增加引用计数, 确保每个物理页在最后的PTE对它的引用撤销时被释放, 而不是在此之前
struct page_ref {
  struct spinlock lock;
  // 引用计数 最大物理地址除以页面大小, 为每一个物理地址建一个映射
  int cnt[PHYSTOP / PGSIZE];
} ref;

// 获取内存的引用计数
int krefcnt(void* pa) {  
  return ref.cnt[(uint64)pa / PGSIZE];
}

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// lab6
void
kinit()
{
  initlock(&kmem.lock, "kmem");
  // 在kinit中初始化ref的自旋锁
  initlock(&ref.lock, "ref");
  freerange(end, (void*)PHYSTOP);
}

// lab6
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    // 在kfree中会对cnt减1, 所以要先设为1
    ref.cnt[(uint64)p / PGSIZE] = 1;
    kfree(p);
  }
}

// lab6

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // 只有当引用计数为0才回收空间，否则只是将引用计数减1
  acquire(&ref.lock);
  if (--ref.cnt[(uint64)pa / PGSIZE] == 0) {
    release(&ref.lock);

    r = (struct run*)pa;

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);
    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  } else {
    release(&ref.lock);
  }
}

// lab6

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r) {
    kmem.freelist = r->next; // 从空闲链表中删除获取的内存
    acquire(&ref.lock);
    ref.cnt[(uint64)r / PGSIZE] = 1;  // 将引用计数初始化为1
    release(&ref.lock);
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}


// lab6
// uvmcopy调用, 用于增加引用计数
int kaddrefcnt(uint64 pa) { 
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    return -1;
  acquire(&ref.lock);
  ++ref.cnt[(uint64)pa / PGSIZE];
  release(&ref.lock);
  return 0;
}