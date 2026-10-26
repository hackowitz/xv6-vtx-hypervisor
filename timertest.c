// timertest: checks that timer interrupts reach this xv6, by sleeping
// and by preempting a spinning child. Used to test the xv6 guest (HW2).
//
// In the guest, every timer tick is an interrupt that runxv6 injects
// with vminject() once per host tick. If injection is broken, the
// first test hangs (ticks never advance) and the second hangs too (the
// child is never preempted), so a hang points at interrupt delivery.
#include "types.h"
#include "user.h"

int
main(void)
{
  int t0, t1, pid;
  // volatile forces a load of spin on every iteration. With a plain
  // int the loop has no side effects, and C11 (6.8.5p6) lets a compiler
  // assume such a loop ends; volatile makes it a real endless loop.
  volatile int spin = 1;

  // sleep() returns only after 50 timer interrupts have advanced
  // ticks (sys_sleep() waits on ticks), so this checks that timer
  // interrupts arrive at all; uptime() shows how many ticks passed.
  t0 = uptime();
  sleep(50);
  t1 = uptime();
  printf(1, "timertest: slept %d ticks\n", t1 - t0);
  // The child spins in user space and never makes a system call, so
  // only a timer interrupt can take the CPU away from it.
  if((pid = fork()) == 0){
    while(spin)
      ;
  }
  sleep(10);   /* runs only if the timer preempts the spinning child */
  // kill() only sets the child's killed flag; the child exits the next
  // time it enters the kernel, which for a spinning child means an
  // interrupt, normally the timer's (trap() checks killed before it
  // returns to user space).
  // wait() returns once it has. With one CPU, as in the guest, both
  // sleep(10) and wait() depend on preemption.
  kill(pid);
  wait();
  printf(1, "timertest: preemption ok\n");
  exit();
}
