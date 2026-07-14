#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "kernel/stat.h"
#include "user/user.h"

#define TEST_PAGES 32

static void
fail(char *message)
{
  printf("pgtbltest: %s\n", message);
  exit(1);
}

static void
ugetpid_test(void)
{
  for (int i = 0; i < 32; i++) {
    if (ugetpid() != getpid())
      fail("ugetpid returned the wrong process id");
  }
  printf("ugetpid_test: OK\n");
}

static void
pgaccess_test(void)
{
  char *raw;
  char *pages;
  uint64 mask;
  uint64 expected;

  raw = sbrk((TEST_PAGES + 1) * PGSIZE);
  if (raw == SBRK_ERROR)
    fail("sbrk failed");
  pages = (char *)PGROUNDUP((uint64)raw);

  mask = 0;
  if (pgaccess(pages, TEST_PAGES, &mask) < 0)
    fail("initial pgaccess failed");

  pages[1 * PGSIZE] = 1;
  pages[2 * PGSIZE] = 1;
  pages[30 * PGSIZE] = 1;

  mask = 0;
  if (pgaccess(pages, TEST_PAGES, &mask) < 0)
    fail("pgaccess failed");
  expected = (1ULL << 1) | (1ULL << 2) | (1ULL << 30);
  if (mask != expected)
    fail("pgaccess returned the wrong access mask");

  mask = ~0ULL;
  if (pgaccess(pages, TEST_PAGES, &mask) < 0)
    fail("second pgaccess failed");
  if (mask != 0)
    fail("pgaccess did not clear accessed bits");

  pages[2 * PGSIZE]++;
  mask = 0;
  if (pgaccess(pages, TEST_PAGES, &mask) < 0)
    fail("re-access pgaccess failed");
  if (mask != (1ULL << 2))
    fail("page access was not detected after clearing");

  if (pgaccess(pages, 65, &mask) != -1)
    fail("pgaccess accepted too many pages");
  if (pgaccess(pages, 1, (uint64 *)MAXVA) != -1)
    fail("pgaccess accepted a bad result address");

  printf("pgaccess_test: OK\n");
}

int
main(int argc, char *argv[])
{
  ugetpid_test();
  pgaccess_test();
  printf("pgtbltest: OK\n");
  exit(0);
}
