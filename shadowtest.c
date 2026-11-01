// Built only with SHADOW_TEST=1. No normal filesystem write is shadowed yet.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"
#include "shadowtest.h"

static int failures;

static void
check(int ok, char *what)
{
  printf(1, "%s: %s\n", ok ? "PASS" : "FAIL", what);
  if(!ok)
    failures++;
}

static int
test(int op, int slot, int value)
{
  return shadowtest(op, slot, value);
}

static void
send(int fd, int ok)
{
  char c = ok ? 'P' : 'F';
  write(fd, &c, 1);
}

static int
receive(int fd)
{
  char c = 0;
  return read(fd, &c, 1) == 1 && c == 'P';
}

static int
until(int op, int value)
{
  int i;
  for(i = 0; i < 500; i++){
    if(test(op, 0, 0) == value)
      return 1;
    sleep(1);
  }
  return 0;
}

static void
basic(void)
{
  int i, original[NSHADOW+1], value, ok = 1;
  for(i = 0; i <= NSHADOW; i++)
    original[i] = test(ST_ORIGINAL, i, 0);
  check(test(ST_READ, 0, 0) == -1, "ordinary process cannot access private storage");
  check(xbegin() == 0, "begin for private block tests");
  check(test(ST_READ, 0, 0) == ST_ABSENT, "missing shadow is distinct from error");
  check(test(ST_COPY, 0, 0) == 1 && test(ST_READ, 0, 0) == original[0],
        "first copy preserves original bytes");
  value = original[0] ^ 255;
  check(test(ST_WRITE, 0, value) == 1 && test(ST_READ, 0, 0) == value,
        "private update can be read back");
  check(test(ST_ORIGINAL, 0, 0) == original[0], "original cache block unchanged");
  check(xcommit() == -1 && test(ST_READ, 0, 0) == value,
        "nonempty commit rejected without losing private work");
  for(i = 1; i < NSHADOW; i++)
    if(test(ST_WRITE, i, i) != 1)
      ok = 0;
  check(ok && test(ST_COUNT, 0, 0) == NSHADOW,
        "eight distinct blocks fit, including first-copy entry");
  check(test(ST_WRITE, NSHADOW, 99) == -1 &&
        test(ST_COUNT, 0, 0) == NSHADOW && test(ST_READ, 0, 0) == value,
        "ninth block fails without corrupting existing entries");
  check(test(ST_WRITE, 0, 33) == 1 && test(ST_COUNT, 0, 0) == NSHADOW,
        "repeated block update reuses entry at full capacity");
  check(test(ST_WRITE, -1, 1) == -1 && test(ST_WRITE, NSHADOW+1, 1) == -1,
        "test interface rejects invalid block slots");
  check(xabort() == 0 && test(ST_PAGES, 0, 0) == 0, "abort frees all shadow pages");
  for(i = 0; i <= NSHADOW; i++)
    if(test(ST_ORIGINAL, i, 0) != original[i])
      ok = 0;
  check(ok, "all original blocks unchanged after abort");
  check(xbegin() == 0, "begin allocation-failure test");
  test(ST_FAIL, 0, 0);
  check(test(ST_WRITE, 0, 55) == -1 && test(ST_COUNT, 0, 0) == 0 &&
        test(ST_PAGES, 0, 0) == 0, "allocation failure leaves no reservation or page leak");
  check(test(ST_WRITE, 0, 55) == 1 && test(ST_READ, 0, 0) == 55,
        "allocation succeeds on retry after injected failure");
  check(xabort() == 0, "abort after allocation retry");
}

static void
shared(void)
{
  int result[2], gate[2], pid;
  pipe(result);
  pipe(gate);
  check(xbegin() == 0, "begin shared-context test");
  pid = fork();
  if(pid == 0){
    close(result[0]); close(gate[1]);
    send(result[1], test(ST_WRITE, 0, 71) == 1);
    receive(gate[0]);
    send(result[1], test(ST_READ, 0, 0) == 72);
    exit();
  }
  close(result[1]); close(gate[0]);
  check(receive(result[0]) && test(ST_READ, 0, 0) == 71,
        "parent reads child's private update");
  check(xabort() == -1, "cannot discard storage while a child is alive");
  test(ST_WRITE, 0, 72);
  send(gate[1], 1);
  check(receive(result[0]), "child reads parent's private update");
  wait();
  check(test(ST_READ, 0, 0) == 72, "member exit retains shared shadow contents");
  check(xabort() == 0 && test(ST_PAGES, 0, 0) == 0, "shared storage reclaimed once");
  close(result[0]); close(gate[1]);
}

static void
separate(void)
{
  int result[2], gate[2], pid;
  pipe(result); pipe(gate);
  pid = fork();
  if(pid == 0){
    close(result[0]); close(gate[1]);
    send(result[1], xbegin() == 0 && test(ST_WRITE, 0, 81) == 1);
    receive(gate[0]);
    send(result[1], test(ST_READ, 0, 0) == 81 && xabort() == 0);
    exit();
  }
  close(result[1]); close(gate[0]);
  check(receive(result[0]) && xbegin() == 0 && test(ST_WRITE, 0, 82) == 1,
        "independent contexts shadow the same original block");
  send(gate[1], 1);
  check(receive(result[0]) && test(ST_READ, 0, 0) == 82,
        "one context abort does not affect another context");
  wait();
  check(xabort() == 0 && test(ST_PAGES, 0, 0) == 0, "independent storage reclaimed");
  close(result[0]); close(gate[1]);
}

static void
loading_race(int mode)
{
  int result[2], loader, reader;
  pipe(result);
  check(xbegin() == 0, "begin LOADING reservation test");
  test(ST_HOLD, 0, 1);
  loader = fork();
  if(loader == 0){
    close(result[0]);
    send(result[1], test(ST_WRITE, 0, 91) == 1);
    exit();
  }
  check(until(ST_COUNT, 1), "loader reserves entry before reading disk");
  reader = fork();
  if(reader == 0){
    close(result[0]);
    send(result[1], mode == 2 ? test(ST_WRITE, 0, 92) == 1 :
                               test(ST_READ, 0, 0) == 91);
    exit();
  }
  close(result[1]);
  check(until(ST_WAITERS, 1), "second member waits for LOADING entry");
  check(test(ST_COUNT, 0, 0) == 1 && test(ST_PAGES, 0, 0) == 1,
        "concurrent lookup has one reservation and one allocated page");
  if(mode == 1){
    kill(reader);
    check(wait() == reader && until(ST_WAITERS, 0), "killed LOADING waiter exits without hanging");
  }
  test(ST_HOLD, 0, 0);
  check(receive(result[0]), "loader publishes initialized private contents");
  if(mode != 1)
    check(receive(result[0]), "waiting member uses READY contents");
  wait();
  if(mode != 1)
    wait();
  if(mode == 2)
    check(test(ST_READ, 0, 0) == 92, "competing writer reuses the published entry");
  check(test(ST_COUNT, 0, 0) == 1 && xabort() == 0 && test(ST_PAGES, 0, 0) == 0,
        "reservation race leaves no duplicate entries or leaked pages");
  close(result[0]);
}

static void
owner_death(int killed)
{
  int result[2], gate[2], owner;
  pipe(result); pipe(gate);
  test(ST_HOLD, 0, 1);
  owner = fork();
  if(owner == 0){
    close(result[0]); close(gate[1]);
    if(xbegin() < 0){ send(result[1], 0); exit(); }
    if(fork() == 0){
      test(ST_WRITE, 0, 101);
      exit();
    }
    if(!until(ST_COUNT, 1)){ send(result[1], 0); exit(); }
    if(fork() == 0){
      test(ST_READ, 0, 0);
      exit();
    }
    send(result[1], until(ST_WAITERS, 1));
    if(killed)
      receive(gate[0]);
    exit();
  }
  close(result[1]); close(gate[0]);
  check(receive(result[0]), "owner-death setup has an in-flight loader and waiter");
  if(killed)
    kill(owner);
  wait();
  check(until(ST_PAGES, 0) && until(ST_WAITERS, 0),
        killed ? "killed owner cancels loader/waiter and reclaims pages" :
                 "owner exit cancels loader/waiter and reclaims pages");
  test(ST_HOLD, 0, 0);
  check(xbegin() == 0 && test(ST_READ, 0, 0) == ST_ABSENT && xabort() == 0,
        "new speculation sees no stale private contents");
  close(result[0]); close(gate[1]);
}

int
main(void)
{
  int i, ok = 1;
  basic();
  shared();
  separate();
  loading_race(0);
  loading_race(1);
  loading_race(2);
  owner_death(0);
  owner_death(1);
  for(i = 0; i < 100; i++){
    if(xbegin() < 0 || test(ST_WRITE, 0, i) != 1 || xabort() < 0 ||
       test(ST_PAGES, 0, 0) != 0){
      ok = 0;
      break;
    }
  }
  check(ok, "100 allocate/abort cycles reuse contexts without page leaks");
  printf(1, failures ? "SHADOW TESTS FAILED: %d\n" : "SHADOW TESTS PASSED\n", failures);
  exit();
}
