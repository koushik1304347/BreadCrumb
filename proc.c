#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

// ptable.lock also protects this table and every proc.spec field.  Keeping
// membership and context state under one lock makes fork admission, completion,
// cancellation, and exit detachment atomic with respect to one another.
static struct spec_context spectable[NSPEC];
static uint nextspecid = 1;

static void spec_context_free_locked(struct spec_context *sc);

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

static struct spec_context*
spec_lookup_locked(int id)
{
  struct spec_context *sc;
  for(sc = spectable; sc < &spectable[NSPEC]; sc++)
    if(sc->state != SPEC_FREE && sc->id == id)
      return sc;
  return 0;
}

static struct spec_context*
spec_pin_state(int id, int cleanup)
{
  struct spec_context *sc;

  acquire(&ptable.lock);
  sc = spec_lookup_locked(id);
  if(sc == 0 || (sc->state != SPEC_ACTIVE &&
     !(cleanup && sc->state == SPEC_ABORTING)))
    sc = 0;
  else
    sc->refs++;
  release(&ptable.lock);
  return sc;
}

// Pin an active context before using resources outside ptable.lock. The
// returned pointer must be paired with spec_unpin(); the reference prevents
// slot reuse while a caller may sleep or take resource_lock.
struct spec_context*
spec_pin(int id)
{
  return spec_pin_state(id, 0);
}

// Cleanup-only pin for member exit and abort discard after cancellation has
// closed admission to new speculative work.
struct spec_context*
spec_pin_cleanup(int id)
{
  return spec_pin_state(id, 1);
}

void
spec_unpin(struct spec_context *sc)
{
  acquire(&ptable.lock);
  if(sc->state == SPEC_FREE || sc->refs <= (uint)sc->members)
    panic("spec unpin invariant");
  sc->refs--;
  if(sc->members == 0 && sc->refs == 0)
    spec_context_free_locked(sc);
  release(&ptable.lock);
}

static int
spec_id_in_use_locked(int id)
{
  return spec_lookup_locked(id) != 0;
}

// At most NSPEC identities are live, so a bounded scan finds a free positive
// identity without ever reusing one still held by a context/reference.
static int
spec_new_id_locked(void)
{
  uint id;
  int tries;

  for(tries = 0; tries <= NSPEC; tries++){
    id = nextspecid;
    if(id == 0 || id > 0x7fffffffU)
      id = 1;
    nextspecid = id + 1;
    if(!spec_id_in_use_locked(id))
      return id;
  }
  return 0;
}

static void
spec_context_free_locked(struct spec_context *sc)
{
  if(sc->members != 0 || sc->refs != 0)
    panic("spec context still referenced");
  if(sc->resources != 0)
    panic("spec resources still attached");
  sc->id = 0;
  sc->owner_pid = 0;
  sc->state = SPEC_FREE;
  sc->members = 0;
  sc->refs = 0;
  sc->resources = 0;
}

static void
spec_detach_locked(struct proc *p)
{
  struct spec_context *sc;

  if(p->spec == 0)
    return;
  sc = spec_lookup_locked(p->spec);
  if(sc == 0 || sc->members < 1 || sc->refs < 1)
    panic("spec detach invariant");
  p->spec = 0;
  sc->members--;
  sc->refs--;
  if(sc->members == 0 && sc->refs == 0)
    spec_context_free_locked(sc);
}

// Caller holds ptable.lock.  Killing is cooperative: a process in the kernel
// notices killed on syscall/trap return, and sleepers are made runnable here.
static void
spec_cancel_locked(struct spec_context *sc)
{
  struct proc *p;

  if(sc->state == SPEC_FREE || sc->state == SPEC_COMMITTING)
    return;
  sc->state = SPEC_ABORTING;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->spec != sc->id)
      continue;
    p->killed = 1;
    if(p->state == SLEEPING)
      p->state = RUNNABLE;
  }
}

// These locked hooks only reserve the lifecycle transition. They must not sleep
// or acquire resource_lock. Later resource work must use a pinned reference
// outside ptable.lock, then revalidate/finalize under ptable.lock.
static void
spec_commit_context_locked(struct spec_context *sc)
{
  sc->state = SPEC_COMMITTING;
}

static void
spec_abort_context_locked(struct spec_context *sc)
{
  sc->state = SPEC_ABORTING;
}

int
spec_begin(void)
{
  int id;
  struct proc *p = myproc();
  struct spec_context *sc;

  acquire(&ptable.lock);
  if(p->spec != 0 || p->killed){
    release(&ptable.lock);
    return -1;
  }
  for(sc = spectable; sc < &spectable[NSPEC]; sc++)
    if(sc->state == SPEC_FREE)
      break;
  if(sc == &spectable[NSPEC] || (id = spec_new_id_locked()) == 0){
    release(&ptable.lock);
    return -1;
  }
  sc->id = id;
  sc->owner_pid = p->pid;
  sc->state = SPEC_ACTIVE;
  sc->members = 1;
  sc->refs = 1;
  sc->resources = 0;
  p->spec = id;
  release(&ptable.lock);
  return 0;
}

static int
spec_complete(int commit)
{
  struct proc *p = myproc();
  struct spec_context *sc;

  acquire(&ptable.lock);
  if(p->spec == 0 || p->killed){
    release(&ptable.lock);
    return -1;
  }
  sc = spec_lookup_locked(p->spec);
  if(sc == 0 || sc->state != SPEC_ACTIVE || sc->owner_pid != p->pid ||
     sc->members != 1 || sc->refs != 1){
    release(&ptable.lock);
    return -1;
  }
  if(commit)
    spec_commit_context_locked(sc);
  else
    spec_abort_context_locked(sc);
  spec_detach_locked(p);
  release(&ptable.lock);
  return 0;
}

int
spec_commit(void)
{
  return spec_complete(1);
}

int
spec_abort(void)
{
  return spec_complete(0);
}

void
pinit(void)
{
  int i;
  initlock(&ptable.lock, "ptable");
  for(i = 0; i < NSPEC; i++)
    initlock(&spectable[i].resource_lock, "spec-resource");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->parent = 0;
  p->pgdir = 0;
  p->kstack = 0;
  p->tf = 0;
  p->context = 0;
  p->chan = 0;
  p->killed = 0;
  p->spec = 0;
  p->cwd = 0;
  p->name[0] = 0;
  memset(p->ofile, 0, sizeof(p->ofile));

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    acquire(&ptable.lock);
    p->pid = 0;
    p->state = UNUSED;
    release(&ptable.lock);
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

// Free a process that has not yet been admitted to the runnable set.
// The slot stays EMBRYO while cleanup that may sleep or acquire other locks
// runs, and is made reusable only after all resources have been released.
static void
free_embryo(struct proc *p)
{
  int fd;

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      fileclose(p->ofile[fd]);
      p->ofile[fd] = 0;
    }
  }
  if(p->cwd){
    begin_op();
    iput(p->cwd);
    end_op();
    p->cwd = 0;
  }
  if(p->pgdir){
    freevm(p->pgdir);
    p->pgdir = 0;
  }
  if(p->kstack){
    kfree(p->kstack);
    p->kstack = 0;
  }
  acquire(&ptable.lock);
  p->spec = 0;
  p->pid = 0;
  p->parent = 0;
  p->tf = 0;
  p->context = 0;
  p->chan = 0;
  p->killed = 0;
  p->name[0] = 0;
  p->state = UNUSED;
  release(&ptable.lock);
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    free_embryo(np);
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  // Admission and cancellation use the same lock. A child is either counted
  // before it can run, or not admitted at all.
  if(curproc->killed || (curproc->spec != 0 &&
     (spec_lookup_locked(curproc->spec) == 0 ||
      spec_lookup_locked(curproc->spec)->state != SPEC_ACTIVE))){
    release(&ptable.lock);
    free_embryo(np);
    return -1;
  }
  if(curproc->spec != 0){
    struct spec_context *sc = spec_lookup_locked(curproc->spec);
    np->spec = sc->id;
    sc->members++;
    sc->refs++;
  }

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  struct spec_context *sc;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Owner cancellation starts before exit resource cleanup. The association
  // remains attached while descriptors/cwd are released so future private
  // resources can be detached or discarded in the correct context.
  acquire(&ptable.lock);
  if(curproc->spec != 0){
    sc = spec_lookup_locked(curproc->spec);
    if(sc == 0)
      panic("spec owner exit missing context");
    if(sc->owner_pid == curproc->pid && sc->state == SPEC_ACTIVE)
      spec_cancel_locked(sc);
  }
  release(&ptable.lock);

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Membership ends after this process's ordinary exit cleanup, before it
  // becomes a zombie. Thus an unreaped zombie cannot block owner completion.
  spec_detach_locked(curproc);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->spec = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
  struct spec_context *sc;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      if(p->spec != 0){
        sc = spec_lookup_locked(p->spec);
        if(sc && sc->owner_pid == p->pid)
          spec_cancel_locked(sc);
      }
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
