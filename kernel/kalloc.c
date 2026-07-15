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

struct {
  struct spinlock lock;
  struct run *freelist;
  int refcnt[(PHYSTOP - KERNBASE) / PGSIZE];
} kmem;

static int
refindex(uint64 pa)
{
  return (pa - KERNBASE) / PGSIZE;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    acquire(&kmem.lock);
    kmem.refcnt[refindex((uint64)p)] = 1;
    release(&kmem.lock);
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int index;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.lock);
  index = refindex((uint64)pa);
  if (kmem.refcnt[index] < 1)
    panic("kfree ref");
  kmem.refcnt[index]--;
  if (kmem.refcnt[index] > 0) {
    release(&kmem.lock);
    return;
  }

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  r = (struct run *)pa;
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
    kmem.refcnt[refindex((uint64)r)] = 1;
  }
  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}

void
kaddref(void *pa)
{
  int index;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kaddref");

  acquire(&kmem.lock);
  index = refindex((uint64)pa);
  if (kmem.refcnt[index] < 1)
    panic("kaddref ref");
  kmem.refcnt[index]++;
  release(&kmem.lock);
}

int
krefcnt(void *pa)
{
  int count;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefcnt");

  acquire(&kmem.lock);
  count = kmem.refcnt[refindex((uint64)pa)];
  release(&kmem.lock);
  return count;
}

uint64
kfreemem(void)
{
  uint64 bytes = 0;
  struct run *r;

  acquire(&kmem.lock);
  for (r = kmem.freelist; r; r = r->next)
    bytes += PGSIZE;
  release(&kmem.lock);

  return bytes;
}
