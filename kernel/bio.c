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

// 质数个 bucket
#define NBUCKET 13

// 每个 hash bucket
struct bucket {
  struct spinlock lock;
  struct buf head;
};

// 全局回收锁 + 所有 bucket
struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct bucket bucket[NBUCKET];
} bcache;

static inline int
bhash(uint blockno)
{
  // block 到 bucket 的映射
  return blockno % NBUCKET;
}

static void
bucket_insert(int idx, struct buf *b)
{
  // 插到 bucket 头部
  b->next = bcache.bucket[idx].head.next;
  b->prev = &bcache.bucket[idx].head;
  bcache.bucket[idx].head.next->prev = b;
  bcache.bucket[idx].head.next = b;
}

static void
bucket_remove(struct buf *b)
{
  // 从原 bucket 脱链
  b->next->prev = b->prev;
  b->prev->next = b->next;
}

static struct buf *
bucket_lookup_locked(int idx, uint dev, uint blockno)
{
  struct buf *b;

  // 在 bucket 内查找缓存块
  for(b = bcache.bucket[idx].head.next; b != &bcache.bucket[idx].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno)
      return b;
  }

  return 0;
}

void
binit(void)
{
  struct buf *b;

  // 全局回收锁
  initlock(&bcache.lock, "bcache");

  // 初始化每个 bucket
  for(int i = 0; i < NBUCKET; i++){
    initlock(&bcache.bucket[i].lock, "bcache.bucket");
    bcache.bucket[i].head.prev = &bcache.bucket[i].head;
    bcache.bucket[i].head.next = &bcache.bucket[i].head;
  }

  // 初始 buffer 分散挂入 bucket
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "bcache.buf");
    b->valid = 0;
    b->refcnt = 0;
    b->dev = (uint)-1;
    b->blockno = 0;
    bucket_insert((int)(b - bcache.buf) % NBUCKET, b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int idx = bhash(blockno);

  // 快路径：目标 bucket 命中
  acquire(&bcache.bucket[idx].lock);
  b = bucket_lookup_locked(idx, dev, blockno);
  if(b != 0){
    b->refcnt++;
    release(&bcache.bucket[idx].lock);
    acquiresleep(&b->lock);
    return b;
  }
  release(&bcache.bucket[idx].lock);

  // 慢路径：串行处理 miss
  acquire(&bcache.lock);
  acquire(&bcache.bucket[idx].lock);

  // 双重检查
  b = bucket_lookup_locked(idx, dev, blockno);
  if(b != 0){
    b->refcnt++;
    release(&bcache.bucket[idx].lock);
    release(&bcache.lock);
    acquiresleep(&b->lock);
    return b;
  }

  // 找空闲 buffer
  for(int i = 0; i < NBUCKET; i++){
    if(i != idx)
      acquire(&bcache.bucket[i].lock);

    for(b = bcache.bucket[i].head.next; b != &bcache.bucket[i].head; b = b->next){
      if(b->refcnt == 0){
        // 跨 bucket 迁移
        if(i != idx){
          bucket_remove(b);
          release(&bcache.bucket[i].lock);
          bucket_insert(idx, b);
        }

        // 重置元数据
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        release(&bcache.bucket[idx].lock);
        release(&bcache.lock);
        acquiresleep(&b->lock);
        return b;
      }
    }

    if(i != idx)
      release(&bcache.bucket[i].lock);
  }

  release(&bcache.bucket[idx].lock);
  release(&bcache.lock);
  panic("bget: no buffers");
}

static int
buf_bucketno(struct buf *b)
{
  // 由 blockno 反推 bucket
  return bhash(b->blockno);
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  // 先拿到已锁住的 buf
  b = bget(dev, blockno);

  // miss 时读盘
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

  // 脏数据写回
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
void
brelse(struct buf *b)
{
  int idx;

  if(!holdingsleep(&b->lock))
    panic("brelse");

  // 先放 buf 的睡眠锁
  releasesleep(&b->lock);

  // 再减引用计数
  idx = buf_bucketno(b);
  acquire(&bcache.bucket[idx].lock);
  b->refcnt--;
  release(&bcache.bucket[idx].lock);
}

void
bpin(struct buf *b)
{
  int idx = buf_bucketno(b);

  // 日志层固定引用
  acquire(&bcache.bucket[idx].lock);
  b->refcnt++;
  release(&bcache.bucket[idx].lock);
}

void
bunpin(struct buf *b)
{
  int idx = buf_bucketno(b);

  // 日志层释放引用
  acquire(&bcache.bucket[idx].lock);
  b->refcnt--;
  release(&bcache.bucket[idx].lock);
}
