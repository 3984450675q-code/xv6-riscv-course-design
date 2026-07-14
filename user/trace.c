#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int i;
  char *cmdargv[MAXARG];

  if (argc < 3 || argc - 2 >= MAXARG) {
    fprintf(2, "usage: trace mask command [args...]\n");
    exit(1);
  }

  if (trace(atoi(argv[1])) < 0) {
    fprintf(2, "trace: trace failed\n");
    exit(1);
  }

  for (i = 2; i < argc; i++)
    cmdargv[i - 2] = argv[i];
  cmdargv[argc - 2] = 0;

  exec(cmdargv[0], cmdargv);
  fprintf(2, "trace: exec %s failed\n", cmdargv[0]);
  exit(1);
}
