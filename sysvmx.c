// System calls for the hypervisor in vmx.c.
//
// This file is the thin boundary between user space (runvm.c) and the
// hypervisor proper. Each sys_* function below is called by the
// system call dispatcher in syscall.c (see the table indexed by
// SYS_vmcreate, SYS_vmsetreg, SYS_vmgetreg and SYS_vmrun in syscall.h). It does
// only two things:
//
//   1. fetch the arguments from the user's saved registers and check
//      them, and
//   2. call the real implementation in vmx.c (vmcreate, vmsetreg,
//      vmrun), which does the work and returns the result.
//
// Argument passing. A system call takes no C arguments: the
// dispatcher calls sys_*(void). User space put its arguments in the
// registers rdi, rsi, rdx, rcx, r8, r9 (in this order) and the trap
// handler saved them in the process's trap frame, myproc()->tf. The
// helpers in syscall.c read argument number n from there:
//
//   argint(n, &i)        n-th argument as a 32-bit int
//   argint64(n, &i)      n-th argument as a full 64-bit value
//   argptr(n, &p, size)  n-th argument as a pointer to size bytes
//
// All three return 0 on success and -1 on failure. The wrappers
// below follow the xv6 convention: if any fetch fails, return -1 to
// the user without calling the hypervisor. The return value of a
// system call is placed in the user's rax, so -1 as a uint64 is
// "all ones" and a user program sees it as the int -1.
//
// User pointers. xv6 kernel code runs on the same page table as the
// calling process, so the kernel can dereference a user pointer
// directly, but it must first make sure the pointer lies inside the
// process's address space (below myproc()->sz). Otherwise a
// malicious or buggy program could pass a kernel address and trick
// the kernel into reading or overwriting its own memory. argptr()
// performs that check for a whole block of 'size' bytes: the start
// and the end must both be inside [0, sz), and size must not be
// negative.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "vmxapi.h"

// int vmcreate(void *image, int len, int memsz)
//
// Create a VM with memsz bytes of guest-physical memory and copy the
// len-byte guest image to guest-physical address VM_LOAD_ADDR.
// Returns a VM id (a small non-negative number) or -1.
//
// The image pointer is a user pointer, and the kernel will read len
// bytes from it, so argptr() must check that all len bytes lie in
// the caller's address space. That makes the length (argument 1)
// necessary before the pointer (argument 0) can be validated, which
// is why the arguments are fetched out of order: len first, then
// the pointer using len as the size. A negative len is rejected by
// argptr() itself. memsz is only a number here (the guest's memory
// is kernel memory, not a user buffer); vmcreate() in vmx.c checks
// that it is in range (0 < memsz <= VM_MAXMEM) and large enough
// to hold the image. The '||' chain stops at the first failure.
uint64
sys_vmcreate(void)
{
  char *image;
  int len, memsz;

  if(argint(1, &len) < 0 || argptr(0, &image, len) < 0 || argint(2, &memsz) < 0)
    return -1;
  return vmcreate(image, len, memsz);
}

// int vmsetreg(int vm, int reg, uint64 val)
//
// Set one register of VM number 'vm' before it runs: reg is one of
// VMREG_RAX .. VMREG_R15, VMREG_RSP, VMREG_RIP or VMREG_RFLAGS
// (vmxapi.h). Returns 0, or -1 if the VM does not exist (or is not
// owned by the caller's process) or reg is out of range; those
// checks are made by vmsetreg() in vmx.c, not here.
//
// No user memory is involved: all three arguments are values. The
// register value is a full 64 bits (guest RIP and RSP are 64-bit
// addresses), so it is fetched with argint64(), not argint(), which
// would truncate it to 32 bits. vm and reg are small and fit in an
// int. The value is declared signed (int64) only because argint64()
// takes an int64 pointer; vmsetreg() takes it as a uint64.
uint64
sys_vmsetreg(void)
{
  int vm, reg;
  int64 val;

  if(argint(0, &vm) < 0 || argint(1, &reg) < 0 || argint64(2, &val) < 0)
    return -1;
  return vmsetreg(vm, reg, val);
}

// int vmgetreg(int vm, int reg, uint64 *val)
//
// Read one register of VM number 'vm' into *val, using the same reg
// codes as vmsetreg(). Returns 0, or -1 if the VM or reg is bad or
// val is not a valid user pointer. The value is as of the last exit.
//
// val is an output pointer, so argptr() checks that all 8 bytes lie
// inside the process's address space before vmgetreg() stores to it.
uint64
sys_vmgetreg(void)
{
  int vm, reg;
  char *val;

  if(argint(0, &vm) < 0 || argint(1, &reg) < 0 ||
     argptr(2, &val, sizeof(uint64)) < 0)
    return -1;
  return vmgetreg(vm, reg, (uint64*)val);
}

// int vmrun(int vm, struct vmexit *exit)
//
// Run VM number 'vm' until it exits for a reason the kernel cannot
// handle by itself, and describe that exit in *exit. Returns 0 when
// *exit has been filled in, or -1 if the VM does not exist or the
// calling process was killed.
//
// The pointer argument is an output. On return vmrun() overwrites the
// whole structure with *exit = e, so the kernel writes
// sizeof(struct vmexit) bytes through the user pointer, and argptr()
// checks exactly that range. A pointer that is in the user address
// space but only partly (near the top of sz) is rejected, because the
// check covers the end of the block as well as its start.
//
// The cast below only changes the type: argptr() returns the
// pointer as a char*, which is how it reports a block of memory.
// The variable is named 'exit' to match the user-level prototype; it
// shadows nothing inside this function.
uint64
sys_vmrun(void)
{
  int vm;
  char *exit;

  if(argint(0, &vm) < 0 || argptr(1, &exit, sizeof(struct vmexit)) < 0)
    return -1;
  return vmrun(vm, (struct vmexit*)exit);
}

// int vmmem(int vm, uint64 addr, void *buf, int len, int flags)
//
// HW2. Copy len bytes between buf and the guest's memory at addr: into
// the guest with VMMEM_WRITE, out of it otherwise. addr is guest-physical,
// or guest-virtual with VMMEM_VIRT (vmxapi.h). Returns len, or -1 if the
// VM or an address is bad. argptr() checks that the whole user buffer
// is inside the process, in either direction.
//
// runxv6 uses it to load the guest kernel, to write the BIOS data area
// and the MP tables, and to emulate INS and OUTS for the IDE disk.
// vmmem() copies straight to or from the user's buf: during a system
// call the process's page table is still loaded, so the kernel can use
// a checked user address directly.
uint64
sys_vmmem(void)
{
  int vm, len, flags;
  int64 addr;
  char *buf;

  // addr is a 64-bit guest address, so it needs argint64(); argint()
  // would truncate it. len is fetched before buf because argptr()
  // needs it to check the buffer's extent; || evaluates left to right.
  // argptr() also rejects a negative len.
  if(argint(0, &vm) < 0 || argint64(1, &addr) < 0 || argint(3, &len) < 0 ||
     argptr(2, &buf, len) < 0 || argint(4, &flags) < 0)
    return -1;
  return vmmem(vm, addr, buf, len, flags);
}

// int vminject(int vm, int vector)
//
// HW2. Make external interrupt `vector` (32-255) pending for the guest;
// a later vmrun() injects it when the guest can take it.
//
// This is how runxv6's emulated devices interrupt the guest: the
// timer (vector 32), COM1 (36) and the IDE disk (46). Vectors 0-31 are
// reserved for exceptions, so vminject() rejects them.
// Making a vector pending twice before it is injected delivers it
// once, as with a real interrupt controller.
uint64
sys_vminject(void)
{
  int vm, vector;

  if(argint(0, &vm) < 0 || argint(1, &vector) < 0)
    return -1;
  return vminject(vm, vector);
}
