#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > USYSCALL)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  backtrace();
  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_sigalarm(void)
{
  int interval;
  uint64 handler;
  struct proc *p = myproc();

  argint(0, &interval);
  argaddr(1, &handler);
  if (interval < 0)
    return -1;

  p->alarm_interval = interval;
  p->alarm_handler = handler;
  p->alarm_ticks = 0;
  return 0;
}

uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();

  if (!p->alarm_active)
    return -1;
  uint64 saved_a0 = p->alarm_trapframe.a0;
  memmove(p->trapframe, &p->alarm_trapframe, sizeof(*p->trapframe));
  p->alarm_active = 0;
  return saved_a0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
  int mask;

  argint(0, &mask);
  myproc()->trace_mask = mask;
  return 0;
}

#define MAX_PGACCESS_PAGES 64

uint64
sys_pgaccess(void)
{
  uint64 start;
  uint64 user_mask;
  uint64 mask = 0;
  int npages;
  int cleared = 0;
  struct proc *p = myproc();

  argaddr(0, &start);
  argint(1, &npages);
  argaddr(2, &user_mask);

  if (npages < 0 || npages > MAX_PGACCESS_PAGES)
    return -1;
  if (start >= MAXVA || start + (uint64)npages * PGSIZE < start ||
      start + (uint64)npages * PGSIZE > MAXVA)
    return -1;

  for (int i = 0; i < npages; i++) {
    pte_t *pte = walk(p->pagetable, start + (uint64)i * PGSIZE, 0);
    if (pte == 0 || (*pte & (PTE_V | PTE_U)) != (PTE_V | PTE_U))
      continue;
    if (*pte & PTE_A) {
      mask |= 1ULL << i;
      *pte &= ~PTE_A;
      cleared = 1;
    }
  }

  if (cleared)
    sfence_vma();
  if (copyout(p->pagetable, user_mask, (char *)&mask, sizeof(mask)) < 0)
    return -1;
  return 0;
}

uint64
sys_sysinfo(void)
{
  uint64 addr;
  struct proc *p = myproc();
  struct sysinfo info;

  argaddr(0, &addr);
  info.freemem = kfreemem();
  info.nproc = nproc();
  if (copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}

uint64
sys_statistics(void)
{
  uint64 dst;
  int sz;
  char *buf;
  int n;

  argaddr(0, &dst);
  argint(1, &sz);
  if (sz < 2)
    return -1;
  if (sz > PGSIZE)
    sz = PGSIZE;
  if ((buf = kalloc()) == 0)
    return -1;

  n = statslock(buf, sz);
  if (copyout(myproc()->pagetable, dst, buf, n) < 0) {
    kfree(buf);
    return -1;
  }
  kfree(buf);
  return n;
}
