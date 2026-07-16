// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

#define NLOCK 4096

static struct spinlock *locks[NLOCK];
static struct spinlock lock_locks;

static void
findslot(struct spinlock *lk)
{
  acquire(&lock_locks);
  for (int i = 0; i < NLOCK; i++) {
    if (locks[i] == 0) {
      locks[i] = lk;
      release(&lock_locks);
      return;
    }
  }
  release(&lock_locks);
  panic("findslot");
}

void
freelock(struct spinlock *lk)
{
  acquire(&lock_locks);
  for (int i = 0; i < NLOCK; i++) {
    if (locks[i] == lk) {
      locks[i] = 0;
      break;
    }
  }
  release(&lock_locks);
}

void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
  lk->n = 0;
  lk->nts = 0;
  findslot(lk);
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void
acquire(struct spinlock *lk)
{
  push_off(); // disable interrupts to avoid deadlock.
  if (holding(lk))
    panic("acquire");

  __atomic_fetch_add(&lk->n, 1, __ATOMIC_RELAXED);

  // On RISC-V, __atomic_exchange_n turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  //
  // Passing __ATOMIC_ACQUIRE to __atomic_exchange_n tells
  // the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) != 0) {
    __atomic_fetch_add(&lk->nts, 1, __ATOMIC_RELAXED);
  }

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu();
}

// Release the lock.
void
release(struct spinlock *lk)
{
  if (!holding(lk))
    panic("release");

  lk->cpu = 0;

  // Release the lock, equivalent to lk->locked = 0.
  //
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  //
  // On RISC-V, __atomic_store_n turns into a single atomic store:
  //   s1 = &lk->locked
  //   sw zero,0(s1)
  //
  // The __ATOMIC_RELEASE argument to __atomic_store_n tells the
  // the C compiler and the CPU to not move loads or stores past
  // this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  //
  // On RISC-V, this generates a fence instruction before the store:
  //   fence rw,w
  __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);

  pop_off();
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int
holding(struct spinlock *lk)
{
  int r;
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void
push_off(void)
{
  // disable interrupts to prevent an involuntary context
  // switch while using mycpu().
  uint64 flags = rc_sstatus(SSTATUS_SIE);
  int old = !!(flags & SSTATUS_SIE);

  if (mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}

void
pop_off(void)
{
  struct cpu *c = mycpu();
  if (intr_get())
    panic("pop_off - interruptible");
  if (c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if (c->noff == 0 && c->intena)
    intr_on();
}

static int
snprint_lock(char *buf, int sz, struct spinlock *lk)
{
  uint n = __atomic_load_n(&lk->n, __ATOMIC_RELAXED);
  uint nts = __atomic_load_n(&lk->nts, __ATOMIC_RELAXED);

  if (n == 0)
    return 0;
  return snprintf(buf, sz,
                  "lock: %s: #test-and-set %d #acquire() %d\n",
                  lk->name, nts, n);
}

int
statslock(char *buf, int sz)
{
  struct spinlock *top[5] = {0};
  int n = 0;
  int total = 0;

  int bcache_details = 0;

  acquire(&lock_locks);
  n += snprintf(buf + n, sz - n, "--- lock kmem/bcache stats\n");
  for (int i = 0; i < NLOCK; i++) {
    struct spinlock *lk = locks[i];
    int is_kmem;
    int is_bcache;

    if (lk == 0 || lk->name == 0)
      continue;
    is_kmem = strncmp(lk->name, "kmem", 4) == 0;
    is_bcache = strncmp(lk->name, "bcache", 6) == 0;
    if (!is_kmem && !is_bcache)
      continue;

    total += __atomic_load_n(&lk->nts, __ATOMIC_RELAXED);
    uint acquisitions = __atomic_load_n(&lk->n, __ATOMIC_RELAXED);
    if (n < sz - 128 &&
        (is_kmem || (acquisitions > 0 && bcache_details < 24)))
      n += snprint_lock(buf + n, sz - n, lk);
    if (is_bcache && acquisitions > 0)
      bcache_details++;
  }

  n += snprintf(buf + n, sz - n, "--- top 5 contended locks:\n");
  for (int rank = 0; rank < 5 && n < sz; rank++) {
    struct spinlock *best = 0;
    for (int i = 0; i < NLOCK; i++) {
      struct spinlock *lk = locks[i];
      int used = 0;
      if (lk == 0 || lk->name == 0)
        continue;
      for (int j = 0; j < rank; j++)
        if (top[j] == lk)
          used = 1;
      if (!used && (best == 0 || lk->nts > best->nts))
        best = lk;
    }
    if (best == 0)
      break;
    top[rank] = best;
    n += snprint_lock(buf + n, sz - n, best);
  }
  n += snprintf(buf + n, sz - n, "tot= %d\n", total);
  release(&lock_locks);

  if (n >= sz)
    n = sz - 1;
  buf[n] = 0;
  return n;
}
