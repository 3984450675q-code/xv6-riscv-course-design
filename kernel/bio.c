// Buffer cache.
//
// Cached blocks are distributed across hash buckets so that lookups and
// releases for unrelated blocks do not contend on one global lock. Cache
// misses are serialized to preserve the one-buffer-per-block invariant.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 2003

struct bucket {
  struct spinlock lock;
  struct buf *head;
};

struct {
  struct spinlock evict_lock;
  struct bucket bucket[NBUCKET];
  struct buf buf[NBUF];
  uint hand;
} bcache;

static int
bhash(uint dev, uint blockno)
{
  return (blockno + dev * FSSIZE) % NBUCKET;
}

static void
bremove(struct bucket *bucket, struct buf *b)
{
  if (b->prev)
    b->prev->next = b->next;
  else
    bucket->head = b->next;
  if (b->next)
    b->next->prev = b->prev;
}

static void
binsert(struct bucket *bucket, struct buf *b)
{
  b->prev = 0;
  b->next = bucket->head;
  if (bucket->head)
    bucket->head->prev = b;
  bucket->head = b;
}

void
binit(void)
{
  initlock(&bcache.evict_lock, "bcache.evict");
  bcache.hand = 0;

  for (int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.bucket[i].lock, "bcache.bucket");
    bcache.bucket[i].head = 0;
  }

  for (int i = 0; i < NBUF; i++) {
    struct buf *b = &bcache.buf[i];
    struct bucket *bucket;

    b->dev = 0;
    b->blockno = i;
    bucket = &bcache.bucket[bhash(b->dev, b->blockno)];
    b->valid = 0;
    b->refcnt = 0;
    initsleeplock(&b->lock, "buffer");
    binsert(bucket, b);
  }
}

// Look through the buffer cache for block on device dev. If it is absent,
// recycle an unreferenced buffer using a rotating selection hand.
static struct buf *
bget(uint dev, uint blockno)
{
  int h = bhash(dev, blockno);
  struct buf *b;

  acquire(&bcache.bucket[h].lock);
  for (b = bcache.bucket[h].head; b != 0; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.bucket[h].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket[h].lock);

  // Serialize misses so the same disk block can never be installed twice.
  acquire(&bcache.evict_lock);
  acquire(&bcache.bucket[h].lock);
  for (b = bcache.bucket[h].head; b != 0; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.bucket[h].lock);
      release(&bcache.evict_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket[h].lock);

  struct buf *victim = 0;
  int victim_bucket = -1;
  for (int n = 0; n < NBUF; n++) {
    int index = (bcache.hand + n) % NBUF;

    b = &bcache.buf[index];
    victim_bucket = bhash(b->dev, b->blockno);
    acquire(&bcache.bucket[victim_bucket].lock);
    if (b->refcnt == 0) {
      victim = b;
      bcache.hand = (index + 1) % NBUF;
      break;
    }
    release(&bcache.bucket[victim_bucket].lock);
  }

  if (victim == 0) {
    release(&bcache.evict_lock);
    panic("bget: no buffers");
  }

  if (victim_bucket != h)
    acquire(&bcache.bucket[h].lock);
  bremove(&bcache.bucket[victim_bucket], victim);
  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;
  victim->refcnt = 1;
  binsert(&bcache.bucket[h], victim);
  if (victim_bucket != h)
    release(&bcache.bucket[h].lock);
  release(&bcache.bucket[victim_bucket].lock);
  release(&bcache.evict_lock);

  acquiresleep(&victim->lock);
  return victim;
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b = bget(dev, blockno);

  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk. Must be locked.
void
bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

void
brelse(struct buf *b)
{
  int h;

  if (!holdingsleep(&b->lock))
    panic("brelse");
  releasesleep(&b->lock);

  h = bhash(b->dev, b->blockno);
  acquire(&bcache.bucket[h].lock);
  if (b->refcnt < 1)
    panic("brelse ref");
  b->refcnt--;
  release(&bcache.bucket[h].lock);
}

void
bpin(struct buf *b)
{
  int h = bhash(b->dev, b->blockno);

  acquire(&bcache.bucket[h].lock);
  b->refcnt++;
  release(&bcache.bucket[h].lock);
}

void
bunpin(struct buf *b)
{
  int h = bhash(b->dev, b->blockno);

  acquire(&bcache.bucket[h].lock);
  if (b->refcnt < 1)
    panic("bunpin ref");
  b->refcnt--;
  release(&bcache.bucket[h].lock);
}
