#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

#define MAXLINE 512

static int
is_space(char c)
{
  return c == ' ' || c == '\t';
}

static void
run(char *argv[])
{
  int pid = fork();

  if (pid < 0) {
    fprintf(2, "xargs: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    exec(argv[0], argv);
    fprintf(2, "xargs: exec %s failed\n", argv[0]);
    exit(1);
  }

  wait(0);
}

int
main(int argc, char *argv[])
{
  char *xargv[MAXARG];
  char line[MAXLINE];
  int base_argc;
  int line_pos = 0;
  char ch;

  if (argc < 2) {
    fprintf(2, "usage: xargs command [args...]\n");
    exit(1);
  }

  if (argc >= MAXARG) {
    fprintf(2, "xargs: too many arguments\n");
    exit(1);
  }

  for (base_argc = 0; base_argc < argc - 1; base_argc++)
    xargv[base_argc] = argv[base_argc + 1];

  while (read(0, &ch, 1) == 1) {
    if (ch == '\n') {
      line[line_pos] = 0;

      int xargc = base_argc;
      char *p = line;
      while (*p) {
        while (is_space(*p))
          *p++ = 0;
        if (*p == 0)
          break;
        if (xargc >= MAXARG - 1) {
          fprintf(2, "xargs: too many arguments\n");
          exit(1);
        }
        xargv[xargc++] = p;
        while (*p && !is_space(*p))
          p++;
      }

      if (xargc > base_argc) {
        xargv[xargc] = 0;
        run(xargv);
      }
      line_pos = 0;
      continue;
    }

    if (line_pos >= MAXLINE - 1) {
      fprintf(2, "xargs: input line too long\n");
      exit(1);
    }
    line[line_pos++] = ch;
  }

  if (line_pos > 0) {
    line[line_pos] = 0;

    int xargc = base_argc;
    char *p = line;
    while (*p) {
      while (is_space(*p))
        *p++ = 0;
      if (*p == 0)
        break;
      if (xargc >= MAXARG - 1) {
        fprintf(2, "xargs: too many arguments\n");
        exit(1);
      }
      xargv[xargc++] = p;
      while (*p && !is_space(*p))
        p++;
    }

    if (xargc > base_argc) {
      xargv[xargc] = 0;
      run(xargv);
    }
  }

  exit(0);
}
