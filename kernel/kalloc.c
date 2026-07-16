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
} kmem[NCPU];

static int refcnt[(PHYSTOP - KERNBASE) / PGSIZE];

static int
refindex(uint64 pa)
{
  return (pa - KERNBASE) / PGSIZE;
}

void
kinit()
{
  for (int i = 0; i < NCPU; i++)
    initlock(&kmem[i].lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    __atomic_store_n(&refcnt[refindex((uint64)p)], 1, __ATOMIC_RELAXED);
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
  int old;
  int id;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  index = refindex((uint64)pa);
  old = __atomic_fetch_sub(&refcnt[index], 1, __ATOMIC_ACQ_REL);
  if (old < 1)
    panic("kfree ref");
  if (old > 1)
    return;

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  r = (struct run *)pa;

  push_off();
  id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();
}

static struct run *
steal(int id)
{
  for (int off = 1; off < NCPU; off++) {
    int donor = (id + off) % NCPU;
    struct run *first;

    acquire(&kmem[donor].lock);
    first = kmem[donor].freelist;
    if (first) {
      int count = 0;
      int take;
      struct run *tail;
      struct run *r;
      struct run *rest;

      for (struct run *p = first; p; p = p->next)
        count++;
      take = (count + 1) / 2;
      tail = first;
      for (int i = 1; i < take; i++)
        tail = tail->next;
      kmem[donor].freelist = tail->next;
      tail->next = 0;
      release(&kmem[donor].lock);

      r = first;
      rest = r->next;
      r->next = 0;
      if (rest) {
        acquire(&kmem[id].lock);
        tail->next = kmem[id].freelist;
        kmem[id].freelist = rest;
        release(&kmem[id].lock);
      }
      return r;
    }
    release(&kmem[donor].lock);
  }
  return 0;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int id;

  push_off();
  id = cpuid();
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if (r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  if (r == 0)
    r = steal(id);
  pop_off();

  if (r) {
    __atomic_store_n(&refcnt[refindex((uint64)r)], 1, __ATOMIC_RELEASE);
    memset((char *)r, 5, PGSIZE); // fill with junk
  }
  return (void *)r;
}

void
kaddref(void *pa)
{
  int index;
  int old;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kaddref");

  index = refindex((uint64)pa);
  old = __atomic_fetch_add(&refcnt[index], 1, __ATOMIC_ACQ_REL);
  if (old < 1)
    panic("kaddref ref");
}

int
krefcnt(void *pa)
{
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefcnt");

  return __atomic_load_n(&refcnt[refindex((uint64)pa)], __ATOMIC_ACQUIRE);
}

uint64
kfreemem(void)
{
  uint64 bytes = 0;
  struct run *r;

  for (int i = 0; i < NCPU; i++)
    acquire(&kmem[i].lock);
  for (int i = 0; i < NCPU; i++)
    for (r = kmem[i].freelist; r; r = r->next)
      bytes += PGSIZE;
  for (int i = NCPU - 1; i >= 0; i--)
    release(&kmem[i].lock);

  return bytes;
}
