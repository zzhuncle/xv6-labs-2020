// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// lab8 q2
// 将 buf 分成 13 份（实验指导书上建议使用的质数），同时获取 trap.c 中的 ticks 变量
// 不能像任务一那样直接使用 bcache 数组，这会改变 buf 的大小
// 要求在修改的过程中保持每个块在缓存中最多只有一个副本
#define NBUCKET 13
extern uint ticks;
struct {
  // 先使用全局锁进行序列化然后再处理并发问题
  // Your solution might need to hold two locks in some cases; for example, during eviction you may need to hold the bcache lock and a lock per bucket. Make sure you avoid deadlock.
  struct spinlock biglock;
  struct spinlock locks[NBUCKET];
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head[NBUCKET];
} bcache;

// lab8 q2
// 原本的 LRU 机制是使用双向链表来实现的，在这里由于使用了 ticks，因此可以不用这种方式实现，直接单向链表即可
// 但单向链表不方便删除 block，因此这里依然使用了双向的结构
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.biglock, "bcache_biglock");
  for (int i = 0;i < NBUCKET;i++) {
    initlock(&bcache.locks[i], "bcache");
  }

  // Create linked list of buffers
  int i;
  for (i = 0;i < NBUCKET;i++) {
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }
  // 初始化时插入桶的多个链表
  for(b = bcache.buf, i = 0; b < bcache.buf+NBUF; b++, i = (i + 1) % NBUCKET){
    b->next = bcache.head[i].next;
    b->prev = &bcache.head[i];
    initsleeplock(&b->lock, "buffer");
    bcache.head[i].next->prev = b;
    bcache.head[i].next = b;
  }
}

// lab8 q2
int 
hash(int blockno) 
{
  return blockno % NBUCKET;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b, *b2 = 0;
  int id = hash(blockno), min_ticks = 0;

  // 重复的循环中第一个循环用于加速, 否则测试也不会通过 
  acquire(&bcache.locks[id]);

  // Is the block already cached?
  for(b = bcache.head[id].next; b != &bcache.head[id]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.locks[id]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.locks[id]);

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  acquire(&bcache.biglock);
  acquire(&bcache.locks[id]);
  /*
  其中，重复的循环（你提到的那个）确保在尝试替换之前，块是否确实不在缓存中。考虑这样一种情况：当函数首次检查块是否在缓存中时，块可能不在那里，但当获取全局锁并准备替换一个块时，另一个进程可能已经把块放入缓存中了。所以，函数需要再次检查以确保它不会意外地替换一个已经在缓存中的块。
  */
  // 下面这个for循环, 虽然和上面的重复, 但也是不能删除的
  for (b = bcache.head[id].next; b != &bcache.head[id]; b = b->next) {
    if(b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.locks[id]);
      release(&bcache.biglock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  // 1) find a LRU block from current bucket.
  for(b = bcache.head[id].next; b != &bcache.head[id]; b = b->next){
    if (b->refcnt == 0 && (b2 == 0 || b->lastuse < min_ticks)) {
      min_ticks = b->lastuse;
      b2 = b;
    }
  }
  if (b2) {
    b2->dev = dev;
    b2->blockno = blockno;
    b2->refcnt++;
    b2->valid = 0;
    // acquiresleep(&b2->lock);
    release(&bcache.locks[id]);
    release(&bcache.biglock);
    acquiresleep(&b2->lock);
    return b2;
  }
  // 2) find block from the other buckets.
  for (int j = hash(id + 1); j != id; j = hash(j + 1)) {
    acquire(&bcache.locks[j]);
    for (b = bcache.head[j].next; b != &bcache.head[j]; b = b->next) {
      if (b->refcnt == 0 && (b2 == 0 || b->lastuse < min_ticks)) {
        min_ticks = b->lastuse;
        b2 = b;
      }
    }
    if(b2) {
      b2->dev = dev;
      b2->refcnt++;
      b2->valid = 0;
      b2->blockno = blockno;
      // remove block from its original bucket.
      b2->next->prev = b2->prev;
      b2->prev->next = b2->next;
      release(&bcache.locks[j]);
      // add block
      b2->next = bcache.head[id].next;
      b2->prev = &bcache.head[id];
      bcache.head[id].next->prev = b2;
      bcache.head[id].next = b2;
      release(&bcache.locks[id]);
      release(&bcache.biglock);
      acquiresleep(&b2->lock);
      return b2;
    }
    release(&bcache.locks[j]);
  }
  release(&bcache.locks[id]);
  release(&bcache.biglock);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  // 如果需要从磁盘读取缓冲区，bread会在返回缓冲区之前调用virtio_disk_rw来完成此操作
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// lab8 q2
// brelse 中，由于不使用之前的方式实现 LRU，因此当遇到空闲块的时候，直接设置它的使用时间即可
// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int id = hash(b->blockno);
  acquire(&bcache.locks[id]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    // b->next->prev = b->prev;
    // b->prev->next = b->next;
    // b->next = bcache.head.next;
    // b->prev = &bcache.head;
    // bcache.head.next->prev = b;
    // bcache.head.next = b;
    b->lastuse = ticks;
  }
  
  release(&bcache.locks[id]);
}

void
bpin(struct buf *b) {
  int id = hash(b->blockno);
  acquire(&bcache.locks[id]);
  b->refcnt++;
  release(&bcache.locks[id]);
}

void
bunpin(struct buf *b) {
  int id = hash(b->blockno);
  acquire(&bcache.locks[id]);
  b->refcnt--;
  release(&bcache.locks[id]);
}


