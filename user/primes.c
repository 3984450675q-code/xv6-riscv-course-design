#include "kernel/types.h"
#include "user/user.h"

static void filter(int in) __attribute__((noreturn));

static void
filter(int in)
{
  int prime;

  if (read(in, &prime, sizeof(prime)) != sizeof(prime)) {
    close(in);
    exit(0);
  }

  printf("prime %d\n", prime);

  int p[2];
  if (pipe(p) < 0) {
    fprintf(2, "primes: pipe failed\n");
    close(in);
    exit(1);
  }

  int pid = fork();
  if (pid < 0) {
    fprintf(2, "primes: fork failed\n");
    close(in);
    close(p[0]);
    close(p[1]);
    exit(1);
  }

  if (pid == 0) {
    close(p[1]);
    close(in);
    filter(p[0]);
  }

  close(p[0]);

  int n;
  while (read(in, &n, sizeof(n)) == sizeof(n)) {
    if (n % prime != 0)
      write(p[1], &n, sizeof(n));
  }

  close(in);
  close(p[1]);
  wait(0);
  exit(0);
}

int
main(int argc, char *argv[])
{
  int p[2];

  if (argc != 1) {
    fprintf(2, "usage: primes\n");
    exit(1);
  }

  if (pipe(p) < 0) {
    fprintf(2, "primes: pipe failed\n");
    exit(1);
  }

  int pid = fork();
  if (pid < 0) {
    fprintf(2, "primes: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    close(p[1]);
    filter(p[0]);
  }

  close(p[0]);
  for (int n = 2; n <= 35; n++)
    write(p[1], &n, sizeof(n));
  close(p[1]);

  wait(0);
  exit(0);
}
