#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/sysinfo.h"
#include "user/user.h"

static void
fail(char *s)
{
  printf("sysinfotest: %s\n", s);
  exit(1);
}

static void
getsysinfo(struct sysinfo *info)
{
  if (sysinfo(info) < 0)
    fail("sysinfo failed");
}

static void
test_bad_address(void)
{
  if (sysinfo((struct sysinfo *)0xffffffffffffffff) != -1)
    fail("sysinfo succeeded with bad address");
}

static void
test_freemem(void)
{
  struct sysinfo before, after;
  char *p;

  getsysinfo(&before);
  p = sbrk(PGSIZE);
  if (p == SBRK_ERROR)
    fail("sbrk failed");
  getsysinfo(&after);
  if (after.freemem != before.freemem - PGSIZE)
    fail("free memory did not decrease by one page");
  if (sbrk(-PGSIZE) == SBRK_ERROR)
    fail("sbrk shrink failed");
  getsysinfo(&after);
  if (after.freemem != before.freemem)
    fail("free memory was not restored");
}

static void
test_nproc(void)
{
  struct sysinfo before, during, after;
  int pid;

  getsysinfo(&before);
  pid = fork();
  if (pid < 0)
    fail("fork failed");
  if (pid == 0) {
    pause(50);
    exit(0);
  }

  getsysinfo(&during);
  if (during.nproc != before.nproc + 1)
    fail("process count did not increase");
  wait(0);
  getsysinfo(&after);
  if (after.nproc != before.nproc)
    fail("process count was not restored");
}

int
main(int argc, char *argv[])
{
  test_bad_address();
  test_freemem();
  test_nproc();
  printf("sysinfotest: OK\n");
  exit(0);
}
