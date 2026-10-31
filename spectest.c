#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "param.h"

static int failures;

static void
check(int ok, char *what)
{
  if(ok)
    printf(1, "PASS: %s\n", what);
  else {
    printf(1, "FAIL: %s\n", what);
    failures++;
  }
}

static int
wait_until_complete(void)
{
  int i;
  for(i = 0; i < 1000; i++){
    if(xcommit() == 0)
      return 0;
    sleep(1);
  }
  return -1;
}

static void
fd_string(int fd, char *s)
{
  int tens = fd / 10;
  if(tens){
    s[0] = '0' + tens;
    s[1] = '0' + fd % 10;
    s[2] = 0;
  } else {
    s[0] = '0' + fd;
    s[1] = 0;
  }
}

static void
test_exec_membership(void)
{
  int result[2], gate[2], pid;
  char resultfd[4], gatefd[4], c = 0;

  pipe(result);
  pipe(gate);
  check(xbegin() == 0, "exec-membership owner begins");
  pid = fork();
  if(pid == 0){
    char *args[5];
    close(result[0]);
    close(gate[1]);
    fd_string(result[1], resultfd);
    fd_string(gate[0], gatefd);
    args[0] = "spectest";
    args[1] = "execprobe";
    args[2] = resultfd;
    args[3] = gatefd;
    args[4] = 0;
    exec("spectest", args);
    c = 'F';
    write(result[1], &c, 1);
    exit();
  }
  close(result[1]);
  close(gate[0]);
  read(result[0], &c, 1);
  check(c == 'P', "exec preserves inherited non-owner membership");
  check(xcommit() == -1, "exec'd member remains counted until exit");
  c = 'G';
  write(gate[1], &c, 1);
  wait();
  check(xcommit() == 0, "owner completes after exec'd member detaches");
  close(result[0]);
  close(gate[1]);
}

static void
test_basic(void)
{
  check(xcommit() == -1 && xabort() == -1,
        "completion outside speculation is rejected");
  check(xbegin() == 0, "begin succeeds");
  check(xbegin() == -1, "nested begin is rejected");
  check(xabort() == 0, "owner abort succeeds");
  check(xabort() == -1, "second completion is rejected");
  check(xbegin() == 0 && xcommit() == 0,
        "a completed process can begin and commit again");
}

static void
test_shared_membership(void)
{
  int ready[2], release[2];
  int pid, status;

  pipe(ready);
  pipe(release);
  check(xbegin() == 0, "shared-membership owner begins");
  pid = fork();
  if(pid == 0){
    char c = 'R';
    close(ready[0]);
    close(release[1]);
    status = (xbegin() == -1 && xcommit() == -1 && xabort() == -1);
    c = status ? 'P' : 'F';
    write(ready[1], &c, 1);
    read(release[0], &c, 1);
    close(ready[1]);
    close(release[0]);
    exit();
  }
  close(ready[1]);
  close(release[0]);
  {
    char c = 0;
    read(ready[0], &c, 1);
    check(c == 'P', "inherited child rejects nested begin and completion");
  }
  check(xabort() == -1, "owner abort is refused while child lives");
  check(xcommit() == -1, "owner cannot complete while child lives");
  {
    char c = 'G';
    write(release[1], &c, 1);
  }
  wait();
  check(xcommit() == 0,
        "fork inheritance shares one context until child detaches");
  close(ready[0]);
  close(release[1]);
}

static void
test_unreaped_zombie(void)
{
  int ready[2], pid;
  char c = 'R';

  pipe(ready);
  check(xbegin() == 0, "unreaped-zombie owner begins");
  pid = fork();
  if(pid == 0){
    close(ready[0]);
    write(ready[1], &c, 1);
    close(ready[1]);
    exit();
  }
  close(ready[1]);
  read(ready[0], &c, 1);
  check(wait_until_complete() == 0,
        "detached zombie no longer counts before wait reaps it");
  check(wait() == pid, "unreaped zombie remains waitable");
  close(ready[0]);
}

static void
test_member_kill(void)
{
  int ready[2], gate[2], pid;
  char c = 'R';

  pipe(ready);
  pipe(gate);
  check(xbegin() == 0, "member-kill owner begins");
  pid = fork();
  if(pid == 0){
    close(ready[0]);
    close(gate[1]);
    write(ready[1], &c, 1);
    read(gate[0], &c, 1);
    exit();
  }
  close(ready[1]);
  close(gate[0]);
  read(ready[0], &c, 1);
  check(kill(pid) == 0, "ordinary member can be killed");
  wait();
  check(xcommit() == 0, "killed non-owner detaches without cancelling owner");
  close(ready[0]);
  close(gate[1]);
}

static void
test_grandchild(void)
{
  int inner[2], info[2], gate[2];
  int child, grandchild;
  char c = 'R';

  pipe(inner);
  pipe(info);
  pipe(gate);
  check(xbegin() == 0, "grandchild owner begins");
  child = fork();
  if(child == 0){
    int g = fork();
    if(g == 0){
      close(inner[0]);
      close(info[0]);
      close(info[1]);
      close(gate[1]);
      write(inner[1], &c, 1);
      read(gate[0], &c, 1);
      close(inner[1]);
      close(gate[0]);
      exit();
    }
    close(inner[1]);
    close(info[0]);
    close(gate[0]);
    close(gate[1]);
    read(inner[0], &c, 1);
    write(info[1], &g, sizeof(g));
    close(inner[0]);
    close(info[1]);
    exit();
  }
  close(inner[0]);
  close(inner[1]);
  close(info[1]);
  close(gate[0]);
  read(info[0], &grandchild, sizeof(grandchild));
  wait();                         // ensure the intermediate child detached
  check(xcommit() == -1,
        "living grandchild still counts after its parent exits");
  check(kill(grandchild) == 0, "grandchild can be cancelled by pid");
  check(wait_until_complete() == 0,
        "owner completes after grandchild detaches");
  close(info[0]);
  close(gate[1]);
}

static void
test_owner_cancellation(int by_kill)
{
  int gate[2], notice[2], control[2], bad[2];
  int owner;
  char c = 'R';

  pipe(gate);
  pipe(notice);
  pipe(control);
  pipe(bad);
  owner = fork();
  if(owner == 0){
    int inner[2], worker;
    close(gate[1]);
    close(notice[0]);
    close(control[1]);
    close(bad[0]);
    if(xbegin() < 0)
      exit();
    pipe(inner);
    worker = fork();
    if(worker == 0){
      close(inner[0]);
      close(notice[1]);
      close(control[0]);
      write(inner[1], &c, 1);
      close(inner[1]);
      read(gate[0], &c, 1);
      write(bad[1], &c, 1);       // must not execute after group cancellation
      close(gate[0]);
      close(bad[1]);
      exit();
    }
    close(inner[1]);
    read(inner[0], &c, 1);        // worker registered and reached its gate
    close(inner[0]);
    write(notice[1], &c, 1);
    close(notice[1]);
    if(by_kill)
      read(control[0], &c, 1);    // root kills this sleeping owner
    close(control[0]);
    exit();                       // owner death cancels its remaining member
  }

  close(gate[0]);
  close(notice[1]);
  close(control[0]);
  close(bad[1]);
  read(notice[0], &c, 1);
  if(by_kill)
    check(kill(owner) == 0, "owner kill request succeeds");
  check(wait() == owner, by_kill ? "killed owner exits" : "owner exits voluntarily");
  close(notice[0]);
  close(control[1]);
  close(gate[1]);                 // release an uncancelled worker on failure
  check(read(bad[0], &c, 1) == 0,
        by_kill ? "owner kill wakes and terminates sleeping member"
                : "owner exit wakes and terminates sleeping member");
  close(bad[0]);
}

static void
test_context_exhaustion(void)
{
  int gate[2], ready[2], pids[NSPEC];
  int i, n = 0;
  char c = 'R';

  pipe(gate);
  pipe(ready);
  for(i = 0; i < NSPEC; i++){
    int pid = fork();
    if(pid == 0){
      close(gate[1]);
      close(ready[0]);
      if(xbegin() < 0)
        exit();
      write(ready[1], &c, 1);
      read(gate[0], &c, 1);
      exit();
    }
    if(pid < 0)
      break;
    pids[n++] = pid;
    read(ready[0], &c, 1);
  }
  check(n == NSPEC, "all bounded context slots can be allocated");
  check(xbegin() == -1, "begin fails cleanly when context table is full");
  for(i = 0; i < n; i++)
    kill(pids[i]);
  for(i = 0; i < n; i++)
    wait();
  check(xbegin() == 0 && xabort() == 0,
        "cancelled contexts are reclaimed and reusable");
  close(gate[0]);
  close(gate[1]);
  close(ready[0]);
  close(ready[1]);
}

static void
test_failed_fork_and_slot_reuse(void)
{
  int gate[2], ready[2], pids[NPROC];
  int i, n = 0, failed = 0;
  char c = 'R';

  pipe(gate);
  pipe(ready);
  check(xbegin() == 0, "fork-failure owner begins");
  while(n < NPROC){
    int pid = fork();
    if(pid == 0){
      close(gate[1]);
      close(ready[0]);
      write(ready[1], &c, 1);
      read(gate[0], &c, 1);
      exit();
    }
    if(pid < 0){
      failed = 1;
      break;
    }
    pids[n++] = pid;
    read(ready[0], &c, 1);
  }
  check(failed, "fork failure is reached under process/resource pressure");
  for(i = 0; i < n; i++)
    kill(pids[i]);
  for(i = 0; i < n; i++)
    wait();
  check(xcommit() == 0,
        "failed fork and child exits leave no leaked membership references");
  close(gate[0]);
  close(gate[1]);
  close(ready[0]);
  close(ready[1]);

  for(i = 0; i < 24; i++){
    int pid;
    if(xbegin() < 0){
      check(0, "repeated begin/complete and process-slot reuse");
      return;
    }
    pid = fork();
    if(pid == 0)
      exit();
    if(pid < 0 || wait() != pid || xcommit() < 0){
      check(0, "repeated begin/complete and process-slot reuse");
      return;
    }
  }
  check(1, "repeated begin/complete and process-slot reuse");
}

static void
test_ordinary_file_effects(void)
{
  int fd;
  char in[8];

  check(xbegin() == 0, "ordinary-effects example begins");
  fd = open("specleak", O_CREATE|O_RDWR);
  if(fd >= 0){
    write(fd, "visible", 7);
    close(fd);
  }
  check(xabort() == 0, "lifecycle abort succeeds");
  fd = open("specleak", O_RDONLY);
  if(fd >= 0){
    int n = read(fd, in, sizeof(in));
    int same = n == 7;
    int i;
    close(fd);
    for(i = 0; i < 7 && same; i++)
      if(in[i] != "visible"[i])
        same = 0;
    check(same,
          "ordinary file write remains visible after xabort (expected Step 2)");
  } else {
    check(0, "ordinary file write remains visible after xabort (expected Step 2)");
  }
  unlink("specleak");
}

int
main(int argc, char **argv)
{
  if(argc == 4 && strcmp(argv[1], "execprobe") == 0){
    int resultfd = atoi(argv[2]);
    int gatefd = atoi(argv[3]);
    char c = 'P';
    if(xbegin() != -1 || xcommit() != -1 || xabort() != -1)
      c = 'F';
    write(resultfd, &c, 1);
    read(gatefd, &c, 1);
    close(resultfd);
    close(gatefd);
    exit();
  }
  test_basic();
  test_shared_membership();
  test_exec_membership();
  test_unreaped_zombie();
  test_member_kill();
  test_grandchild();
  test_owner_cancellation(0);
  test_owner_cancellation(1);
  test_context_exhaustion();
  test_failed_fork_and_slot_reuse();
  test_ordinary_file_effects();
  if(failures)
    printf(1, "SPEC TESTS FAILED: %d\n", failures);
  else
    printf(1, "SPEC TESTS PASSED\n");
  exit();
}
