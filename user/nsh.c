#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define MAXLINE 512
#define MAXARGS 32

struct command {
  char *argv[MAXARGS];
  char *input;
  char *output;
};

struct scanner {
  char *next;
  int pending;
};

static void
fail(char *message)
{
  fprintf(2, "nsh: %s failed\n", message);
  exit(1);
}

static void
closefd(int fd)
{
  if (close(fd) < 0)
    fail("close");
}

// Read exactly one line so a child can still read the remaining standard input.
static int
readline(char *line, int size)
{
  int len = 0, overflow = 0;
  char c;

  for (;;) {
    int n = read(0, &c, 1);
    if (n < 0)
      fail("read");
    if (n == 0 || c == '\n') {
      line[len] = 0;
      if (overflow) {
        fprintf(2, "nsh: command too long\n");
        return -1;
      }
      return n != 0 || len != 0;
    }
    if (len < size - 1)
      line[len++] = c;
    else
      overflow = 1;
  }
}

// Terminate words in place, saving an adjacent operator for the next call.
static int
gettoken(struct scanner *scan, char **word)
{
  int token = scan->pending;
  scan->pending = 0;
  if (token)
    return token;

  char *s = scan->next;
  while (*s && strchr(" \t\r\v\f", *s))
    s++;
  if (*s == 0) {
    scan->next = s;
    return 0;
  }
  if (strchr("<>|", *s)) {
    scan->next = s + 1;
    return *s;
  }
  *word = s;
  while (*s && !strchr(" \t\r\v\f<>|", *s))
    s++;
  if (*s) {
    if (strchr("<>|", *s))
      scan->pending = *s;
    *s++ = 0;
  }
  scan->next = s;
  return 'a';
}

// Return the number of commands, or -1 for invalid input.
static int
parse(char *line, struct command cmds[2])
{
  struct scanner scan = {line, 0};
  int side = 0, argc = 0, token;
  char *word;

  memset(cmds, 0, 2 * sizeof(*cmds));
  while ((token = gettoken(&scan, &word)) != 0) {
    if (token == 'a') {
      if (argc == MAXARGS - 1)
        return -1;
      cmds[side].argv[argc++] = word;
    } else if (token == '|') {
      if (side != 0 || argc == 0)
        return -1;
      side = 1;
      argc = 0;
    } else {
      if (gettoken(&scan, &word) != 'a')
        return -1;
      if (token == '<')
        cmds[side].input = word;
      else
        cmds[side].output = word;
    }
  }
  if (argc == 0)
    return side || cmds[0].input || cmds[0].output ? -1 : 0;
  return side + 1;
}

static void
redirect(char *path, int target, int mode)
{
  if (path) {
    closefd(target);
    if (open(path, mode) != target)
      fail(path);
  }
}

static void
run(struct command cmds[2], int count)
{
  int p[2], children = 0;

  if (count == 2 && pipe(p) < 0)
    fail("pipe");
  for (int i = 0; i < count; i++) {
    int pid = fork();
    if (pid < 0) {
      fprintf(2, "nsh: fork failed\n");
      break;
    }
    if (pid == 0) {
      if (count == 2) {
        int target = 1 - i;
        closefd(target);
        if (dup(p[target]) != target)
          fail("dup");
        closefd(p[0]);
        closefd(p[1]);
      }
      redirect(cmds[i].input, 0, O_RDONLY);
      redirect(cmds[i].output, 1, O_WRONLY | O_CREATE);
      exec(cmds[i].argv[0], cmds[i].argv);
      fail(cmds[i].argv[0]);
    }
    children++;
  }
  // Both children must run before waiting, and neither pipe end stays here.
  if (count == 2) {
    closefd(p[0]);
    closefd(p[1]);
  }
  while (children-- > 0)
    if (wait(0) < 0)
      fail("wait");
}

int
main(void)
{
  char line[MAXLINE];
  struct command cmds[2];

  for (;;) {
    fprintf(2, "@ ");
    int result = readline(line, sizeof(line));
    if (result == 0)
      exit(0);
    if (result < 0)
      continue;
    int count = parse(line, cmds);
    if (count < 0)
      fprintf(2, "nsh: invalid command or too many arguments\n");
    else if (count > 0)
      run(cmds, count);
  }
}
