#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "syscall.h"

// User code makes a system call with INT T_SYSCALL.
// System call number in %rax.
// Arguments on the registers and stack, from the user call to the C
// library system call function. The saved user %rsp points
// to a saved program counter, and then the seventh argument.

// Fetch the int at addr from the current process.
int
fetchint(uint64 addr, int64 *ip)
{
  struct proc *curproc = myproc();

  if(addr >= curproc->sz || addr+8 > curproc->sz)
    return -1;
  *ip = *(int64*)(addr);
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Doesn't actually copy the string - just sets *pp to point at it.
// Returns length of string, not including nul.
int
fetchstr(uint64 addr, char **pp)
{
  char *s, *ep;
  struct proc *curproc = myproc();

  if(addr >= curproc->sz)
    return -1;
  *pp = (char*)addr;
  ep = (char*)curproc->sz;
  for(s = *pp; s < ep; s++){
    if(*s == 0)
      return s - *pp;
  }
  return -1;
}

// Fetch the nth 64-bit system call argument.
int
argint64(int n, int64 *ip)
{
  struct proc *curproc = myproc();

  switch(n){
    case 0:
      *ip = curproc->tf->rdi;
      break;
    case 1:
      *ip = curproc->tf->rsi;
      break;
    case 2:
      *ip = curproc->tf->rdx;
      break;
    case 3:
      *ip = curproc->tf->rcx;
      break;
    case 4:
      *ip = curproc->tf->r8;
      break;
    case 5:
      *ip = curproc->tf->r9;
      break;
    default:
      return fetchint((curproc->tf->rsp) + 8 + 8*(n-6), ip);
  }
  return 0;
}

// Fetch the nth 32-bit system call argument.
int
argint(int n, int *ip)
{
  int64 ip64;
  int status = argint64(n, &ip64);
  *ip = (int)ip64;
  return status;
}

// Fetch the nth word-sized system call argument as a pointer
// to a block of memory of size bytes.  Check that the pointer
// lies within the process address space.
int
argptr(int n, char **pp, int size)
{
  int64 i;
  struct proc *curproc = myproc();
 
  if(argint64(n, &i) < 0)
    return -1;
  if(size < 0 || (uint64)i >= curproc->sz || (uint64)i+size > curproc->sz)
    return -1;
  *pp = (char*)i;
  return 0;
}

// Fetch the nth word-sized system call argument as a string pointer.
// Check that the pointer is valid and the string is nul-terminated.
// (There is no shared writable memory, so the string can't change
// between this check and being used by the kernel.)
int
argstr(int n, char **pp)
{
  int64 addr;
  if(argint64(n, &addr) < 0)
    return -1;
  return fetchstr(addr, pp);
}

extern uint64 sys_chdir(void);
extern uint64 sys_close(void);
extern uint64 sys_dup(void);
extern uint64 sys_exec(void);
extern uint64 sys_exit(void);
extern uint64 sys_fork(void);
extern uint64 sys_fstat(void);
extern uint64 sys_getpid(void);
extern uint64 sys_kill(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_mknod(void);
extern uint64 sys_open(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_sleep(void);
extern uint64 sys_unlink(void);
extern uint64 sys_wait(void);
extern uint64 sys_write(void);
extern uint64 sys_uptime(void);
extern uint64 sys_vmcreate(void);
extern uint64 sys_vmsetreg(void);
extern uint64 sys_vmrun(void);
extern uint64 sys_vmgetreg(void);
extern uint64 sys_vmmem(void);
extern uint64 sys_vminject(void);
extern uint64 sys_fdready(void);
extern uint64 sys_consraw(void);

static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe]    sys_pipe,
[SYS_read]    sys_read,
[SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
[SYS_fstat]   sys_fstat,
[SYS_chdir]   sys_chdir,
[SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
[SYS_sleep]   sys_sleep,
[SYS_uptime]  sys_uptime,
[SYS_open]    sys_open,
[SYS_write]   sys_write,
[SYS_mknod]   sys_mknod,
[SYS_unlink]  sys_unlink,
[SYS_link]    sys_link,
[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
[SYS_vmcreate] sys_vmcreate,
[SYS_vmsetreg] sys_vmsetreg,
[SYS_vmrun]   sys_vmrun,
[SYS_vmgetreg] sys_vmgetreg,
[SYS_vmmem]   sys_vmmem,
[SYS_vminject] sys_vminject,
[SYS_fdready] sys_fdready,
[SYS_consraw] sys_consraw,
};

void
syscall(void)
{
  int num;
  struct proc *curproc = myproc();

  num = curproc->tf->rax;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    curproc->tf->rax = syscalls[num]();
  } else {
    cprintf("%d %s: unknown sys call %d\n",
            curproc->pid, curproc->name, num);
    curproc->tf->rax = -1;
  }
}
