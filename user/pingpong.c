#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int p2c[2];
  int c2p[2];
  char byte = 'x';

  if (argc != 1) {
    fprintf(2, "usage: pingpong\n");
    exit(1);
  }

  if (pipe(p2c) < 0 || pipe(c2p) < 0) {
    fprintf(2, "pingpong: pipe failed\n");
    exit(1);
  }

  int pid = fork();
  if (pid < 0) {
    fprintf(2, "pingpong: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    close(p2c[1]);
    close(c2p[0]);

    if (read(p2c[0], &byte, 1) == 1) {
      printf("%d: received ping\n", getpid());
      write(c2p[1], &byte, 1);
    }

    close(p2c[0]);
    close(c2p[1]);
    exit(0);
  }

  close(p2c[0]);
  close(c2p[1]);

  write(p2c[1], &byte, 1);
  if (read(c2p[0], &byte, 1) == 1)
    printf("%d: received pong\n", getpid());

  close(p2c[1]);
  close(c2p[0]);
  wait(0);
  exit(0);
}
