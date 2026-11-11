// Buffer cache.
//
// The buffer cache is a hash table of buf structures holding
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

#define NBUCKET 13

struct bucket {
  struct spinlock lock;       // protects the chain and buffer metadata
  struct buf *head;
};

struct {
  struct spinlock lock;      // serializes cache misses and buffer moves
  struct bucket bucket[NBUCKET];
  struct buf buf[NBUF];
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  for(int i = 0; i < NBUCKET; i++)
    initlock(&bcache.bucket[i].lock, "bcache.bucket");

  // Unassigned buffers have no valid device and start in bucket zero.
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->dev = -1;
    b->next = bcache.bucket[0].head;
    bcache.bucket[0].head = b;
    initsleeplock(&b->lock, "buffer");
  }
}

// Caller holds the block's bucket lock.
static struct buf*
blookup(uint dev, uint blockno)
{
  for(struct buf *b = bcache.bucket[blockno % NBUCKET].head; b; b = b->next)
    if(b->dev == dev && b->blockno == blockno)
      return b;
  return 0;
}

// Caller holds bcache.lock and has checked that the block is not cached.
static struct buf*
brecycle(uint dev, uint blockno)
{
  struct buf *victim = 0;
  struct bucket *src = 0;
  uint oldest = 0;

  // Retain the best candidate's lock so it cannot be claimed during the scan.
  // Only this serialized path holds two bucket locks at once.
  for(int i = 0; i < NBUCKET; i++){
    struct bucket *bucket = &bcache.bucket[i];
    int selected = 0;
    acquire(&bucket->lock);
    for(struct buf *b = bucket->head; b; b = b->next){
      if(b->refcnt == 0){
        uint age = __atomic_load_n(&ticks, __ATOMIC_RELAXED) - b->lastuse;
        if(b->dev == (uint)-1)
          age = ~0U; // use unassigned buffers before evicting cached blocks
        if(victim == 0 || age > oldest){
          victim = b;
          oldest = age;
          selected = 1;
        }
      }
    }
    if(selected){
      if(src)
        release(&src->lock);
      src = bucket;
    } else {
      release(&bucket->lock);
    }
  }

  if(victim == 0)
    panic("bget: no buffers");

  struct bucket *dst = &bcache.bucket[blockno % NBUCKET];
  if(src != dst){
    acquire(&dst->lock);
    struct buf **link = &src->head;
    while(*link != victim)
      link = &(*link)->next;
    *link = victim->next;
    victim->next = dst->head;
    dst->head = victim;
  }
  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;
  victim->refcnt = 1;
  if(src != dst)
    release(&dst->lock);
  release(&src->lock);
  return victim;
}

// Return a referenced, sleep-locked buffer for this device and block.
static struct buf*
bget(uint dev, uint blockno)
{
  struct bucket *bucket = &bcache.bucket[blockno % NBUCKET];
  struct buf *b;

  acquire(&bucket->lock);
  b = blookup(dev, blockno);
  if(b)
    b->refcnt++;
  release(&bucket->lock);

  if(b == 0){
    acquire(&bcache.lock);
    // Another miss may have installed the block while we waited.
    acquire(&bucket->lock);
    b = blookup(dev, blockno);
    if(b)
      b->refcnt++;
    release(&bucket->lock);
    if(b == 0)
      b = brecycle(dev, blockno);
    release(&bcache.lock);
  }

  acquiresleep(&b->lock);
  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b->dev, b, 0);
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
  virtio_disk_rw(b->dev, b, 1);
}

// Release a locked buffer.
// Record the last use when the buffer becomes available for recycling.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  struct bucket *bucket = &bcache.bucket[b->blockno % NBUCKET];
  acquire(&bucket->lock);
  b->refcnt--;
  if(b->refcnt == 0)
    b->lastuse = __atomic_load_n(&ticks, __ATOMIC_RELAXED);
  release(&bucket->lock);
}

void
bpin(struct buf *b) {
  struct bucket *bucket = &bcache.bucket[b->blockno % NBUCKET];
  acquire(&bucket->lock);
  b->refcnt++;
  release(&bucket->lock);
}

void
bunpin(struct buf *b) {
  struct bucket *bucket = &bcache.bucket[b->blockno % NBUCKET];
  acquire(&bucket->lock);
  b->refcnt--;
  if(b->refcnt == 0)
    b->lastuse = __atomic_load_n(&ticks, __ATOMIC_RELAXED);
  release(&bucket->lock);
}


