#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "x86.h"
#include "spinlock.h"
#include "proc.h"

struct ptable ptables[NCPU];
static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(struct ptable *pt, void *chan);

void
pinit(void)
{
  int c;
  for (c = 0; c < NCPU; c++) {
    ptables[c].name[0] = (char) c;
    safestrcpy(ptables[c].name+1, "ptable", MAXNAME-1);
    initlock(&ptables[c].lock, ptables[c].name);
  }
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

  acquire(&ptable->lock);
  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  release(&ptable->lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;   // XXX global var!
  release(&ptable->lock);

  // Allocate kernel stack if possible.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
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

static void
addrun1(struct ptable *pt, struct proc *p)
{
  struct proc *q;
  cprintf("%d: add to run %d\n", cpu->id, p->pid);
  for (q = pt->runq; q != 0; q = q->next) {
    if (q == p) {
      cprintf("allready on q\n");
      p->state = RUNNABLE; 
      return;
    }
  }
  p->state = RUNNABLE;   // race?
  p->next = pt->runq;
  pt->runq = p;
}

static void
addrun(struct proc *p)
{
  acquire(&ptable->lock);
  addrun1(ptable, p);
  release(&ptable->lock);
}

static void 
delrun1(struct ptable *pt, struct proc *proc)
{
  struct proc *p = 0;
  struct proc *n;
  n = pt->runq;
  while (n != 0) {
    if (n == proc) {
      if (p == 0) {
	pt->runq = n->next;
      } else {
	p->next = n->next;
      }
      n->next = 0;
      return;
    } else {
      p = n;
      n = n->next;
    }
  }
}

void
delrun(struct proc *proc)
{
  acquire(&ptable->lock);
  delrun1(ptable, proc);
  release(&ptable->lock);
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
  addrun(p);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  
  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
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

  cprintf("%d: fork\n", cpunum());

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);
 
  pid = np->pid;
  addrun(np);
  safestrcpy(np->name, proc->name, sizeof(proc->name));
  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;
  int c;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  iput(proc->cwd);
  proc->cwd = 0;

  cprintf("%d: exit %s\n", cpunum(), proc->name);

  acquire(&ptable->lock);    // XXX sleep/wakeup race?  lock on all ptables?

  delrun1(ptable, proc);

  release(&ptable->lock);    // XXX sleep/wakeup race?  lock on all ptables?

  // Parent might be sleeping in wait().
  wakeup(proc->parent);

  // Pass abandoned children to init.
  for (c = 0; c < NCPU; c++) {
    acquire(&ptable[c].lock);    // XXX sleep/wakeup race?  lock on all ptables?
    for(p = ptables[c].proc; p < &ptables[c].proc[NPROC]; p++){
      if(p->parent == proc){
	p->parent = initproc;
	if(p->state == ZOMBIE)
	  wakeup1(&ptables[c], initproc);   // XXX race
      }
    }
    release(&ptable[c].lock);    // XXX sleep/wakeup race?  lock on all ptables?
  }

  acquire(&ptable->lock);    // XXX sleep/wakeup race?  lock on all ptables?

  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;
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
  int c;

  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for (c = 0; c < NCPU; c++) {
      acquire(&ptables[c].lock);  // XXX race on sleep and wakeup?
      for(p = ptables[c].proc; p < &ptables[c].proc[NPROC]; p++){
	if(p->parent != proc)
	  continue;
	havekids = 1;
	if(p->state == ZOMBIE){
	  // Found one.
	  pid = p->pid;
	  kfree(p->kstack);
	  p->kstack = 0;
	  freevm(p->pgdir);
	  p->state = UNUSED;
	  p->pid = 0;
	  p->parent = 0;
	  p->name[0] = 0;
	  p->killed = 0;
	  release(&ptables[c].lock);
	  return pid;
	}
      }
      release(&ptables[c].lock);  // XXX race on sleep and wakeup?
    }

    acquire(&ptable->lock);  // XXX race on sleep and wakeup?

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable->lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable->lock);  //DOC: wait-sleep

    release(&ptable->lock);  // XXX race on sleep and wakeup?
  }
}

void 
steal(void)
{
  int c;
  struct proc *p;
  int r = 0;

  for (c = 0; c < NCPU; c++) {
    if (c == cpunum())
      continue;
    acquire(&ptables[c].lock);
    for(p = ptables[c].runq; p != 0; p = p->next) {
      if (p->state == RUNNABLE) {
	cprintf("%d: steal %d from %d\n", cpunum(), p->pid, c);
	delrun1(&ptables[c], p);
	addrun(p);
	r = 1;
	break;
      }
    }
    release(&ptables[c].lock);
    if (r) {
      return;
    }
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

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable->lock);
    for(p = ptable->runq; p != 0; p = p->next) {
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable->lock and then reacquire it
      // before jumping back to us.
      proc = p;
      switchuvm(p);
      p->state = RUNNING;
      cprintf("%d: running %d\n", cpu->id, p->pid);
      swtch(&cpu->scheduler, proc->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      proc = 0;
    }
    release(&ptable->lock);
    steal();
  }
}

// Enter scheduler.  Must hold only ptable->lock
// and have changed proc->state.
void
sched(void)
{
  int intena;

  if(!holding(&ptable->lock))
    panic("sched ptable->lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable->lock);  //DOC: yieldlock
  proc->state = RUNNABLE;  // race?  stays in runqueue
  sched();
  release(&ptable->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  // Still holding ptable->lock from scheduler.
  release(&ptable->lock);
  
  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable->lock in order to
  // change p->state and then call sched.
  // Once we hold ptable->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable->lock locked),
  // so it's okay to release lk.
  if(lk != &ptable->lock){  //DOC: sleeplock0
    acquire(&ptable->lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  delrun1(ptable, proc);
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable->lock){  //DOC: sleeplock2
    release(&ptable->lock);
    acquire(lk);
  }
}

// scan a proctable and wakeup any process sleeping on chan
static void
wakeup1(struct ptable *pt, void *chan)
{
  struct proc *p;

  for(p = pt->proc; p < &pt->proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      addrun1(pt, p);
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  int c;

  for (c = 0; c < NCPU; c++) {
    acquire(&ptables[c].lock);
    wakeup1(ptable, chan);
    release(&ptables[c].lock);
  }
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
  int c;

  for (c = 0; c < NCPU; c++) {
    acquire(&ptables[c].lock);
    for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
      if(p->pid == pid){
	p->killed = 1;
	// Wake process from sleep if necessary.
	if(p->state == SLEEPING)
	  addrun1(&ptables[c], p);
	release(&ptables[c].lock);
	return 0;
      }
    }
    release(&ptables[c].lock);
  }
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(int c)
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
  struct proc *q;
  char *state;
  uint pc[10];
  
  cprintf("proc table cpu %d\n", c);
  for(p = ptables[c].proc; p < &ptables[c].proc[NPROC]; p++){
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
  cprintf("runq: ");
  for (q = ptables[c].runq; q != 0; q = q->next) {
    if(q->state >= 0 && q->state < NELEM(states) && states[q->state])
      state = states[q->state];
    else
      state = "???";
    cprintf("%d %s %s, ", q->pid, state, q->name);
  }
  cprintf("\n");
}

void
procdumpall(void)
{
  int c;
  for (c = 0; c < NCPU; c++) {
    procdump(c);
  }
}
