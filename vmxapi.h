/*
 * Hypervisor interface shared by the kernel (vmx.c, sysvmx.c)
 * and user programs (runvm.c).
 *
 * The design follows KVM. The kernel runs the guest and handles only
 * the exits it must; everything else is reported to a user-level
 * "virtual machine monitor" (runvm) through the three system calls
 *
 *   int vmcreate(char *image, int len, int memsz);
 *   int vmsetreg(int vm, int reg, uint64 val);
 *   int vmrun(int vm, struct vmexit *e);
 *
 * vmrun() enters the guest and returns when the guest does something
 * the kernel cannot or should not handle. The reason is described in
 * struct vmexit, defined below. The monitor emulates the event (for
 * example, a serial-port write), then calls vmrun() again, which
 * resumes the guest.
 *
 * This file is included on both sides of the system-call boundary, so
 * it must stay free of kernel-only definitions (those are in vmx.h).
 */

/*
 * Guest-physical address where vmcreate() loads the guest image, and
 * where runvm points the guest's RIP. 1 MB is the conventional start
 * of "extended memory" on a PC, which keeps the low 1 MB free.
 * helloguest.ld links the guest at this address; the two must agree.
 */
#define VM_LOAD_ADDR  0x100000

/*
 * Largest guest memory size vmcreate() accepts. Each 4 KB page is
 * allocated from the kernel's own memory and mapped through EPT.
 */
#define VM_MAXMEM     (64*1024*1024)

/*
 * Registers for vmsetreg(). The first fifteen follow the field order
 * of struct guestregs in vmx.h (which is not the hardware register
 * numbering: RBX comes second, RSI and RDI before RBP), because the
 * kernel stores the value at that index of the structure. RSP, RIP,
 * and RFLAGS are kept in the VMCS instead, so they get separate
 * cases. VMREG_NREGS is the count, used for range checking.
 */
enum {
  VMREG_RAX, VMREG_RBX, VMREG_RCX, VMREG_RDX, VMREG_RSI, VMREG_RDI,
  VMREG_RBP, VMREG_R8, VMREG_R9, VMREG_R10, VMREG_R11, VMREG_R12,
  VMREG_R13, VMREG_R14, VMREG_R15,
  VMREG_RSP, VMREG_RIP, VMREG_RFLAGS,
  VMREG_NREGS
};

/*
 * Basic VM exit reasons (SDM Appendix C) that vmrun() reports. They
 * are the low 16 bits of the VMCS exit-reason field. Exits that the
 * kernel handles itself (host interrupts, NMIs, RDMSR/WRMSR of EFER)
 * are normally not seen by user space. Only a subset of reasons is
 * named here; runvm treats any other as an error.
 */

/*
 * An exception or NMI. In HW1 a guest exception arrived here because
 * the exception bitmap was all ones; the vector is in vmexit.intrinfo.
 * Since HW2 the bitmap is 0 and the guest handles its own exceptions,
 * so only a host NMI exits with this reason, and the kernel resumes the
 * guest after it.
 */
#define EXIT_EXCEPTION_NMI      0

/* A host external interrupt arrived while the guest was running. */
#define EXIT_EXTERNAL_INTR      1

/* The guest triple-faulted (a fault while handling a double fault). */
#define EXIT_TRIPLE_FAULT       2

/*
 * Interrupt window (HW2): the guest can now take the interrupt that
 * the hypervisor is waiting to inject. Handled in the kernel.
 *
 * It happens only while vmxinject() has turned on interrupt-window
 * exiting (CPU_INTR_WINDOW_EXITING in vmx.h): RFLAGS.IF is 1 and no
 * STI or MOV SS blocking is in effect (SDM Appendix C, Table C-1).
 * vmxhandle() just resumes the guest, and the next VM entry injects
 * the interrupt, so user space never sees this exit.
 */
#define EXIT_INTR_WINDOW        7

/*
 * The guest executed CPUID. Always causes an exit (SDM 28.1.2). Since
 * HW2 the kernel emulates it (vmxcpuid() in vmx.c, which hides VMX
 * from the guest), so user space does not see it either.
 */
#define EXIT_CPUID              10

/* The guest executed HLT. runvm treats it as the guest finishing. */
#define EXIT_HLT                12

/* The guest executed VMCALL (a hypercall). Not handled yet. */
#define EXIT_VMCALL             18

/* A control-register access that the CR guest/host masks intercept. */
#define EXIT_CR_ACCESS          28

/*
 * IN/OUT/INS/OUTS. The kernel decodes the exit qualification into
 * vmexit.io; user space emulates the device.
 */
#define EXIT_IO                 30

/* RDMSR and WRMSR. The kernel emulates IA32_EFER only. */
#define EXIT_RDMSR              31
#define EXIT_WRMSR              32

/*
 * VM entry failed because of invalid guest state. It is reported with
 * vmexit.entryfail set, and the exit qualification says which check
 * failed (SDM 29.8).
 */
#define EXIT_INVALID_GUEST      33

/*
 * EPT violation: the guest accessed a guest-physical address that
 * the EPT does not map, or mapped without the needed permission.
 * vmexit.gpa holds the address and vmexit.qual the access type.
 * Here all guest memory is mapped, so this signals a wild access.
 */
#define EXIT_EPT_VIOLATION      48

/* EPT misconfiguration: an EPT entry has an invalid format. */
#define EXIT_EPT_MISCONFIG      49

/*
 * Not a hardware exit reason: VMLAUNCH itself failed; see 'error'.
 * 0xFFFF is outside the 16-bit basic reasons the CPU can produce, so
 * it cannot be confused with a real one.
 */
#define EXIT_VMFAIL             0xFFFF

/*
 * Filled in by vmrun() each time the guest exits to user space.
 * The kernel never reads it back: to complete an IN, user space
 * writes the result into the guest's RAX with vmsetreg().
 */
struct vmexit {
  uint reason;       /* basic exit reason (EXIT_*) */
  uint entryfail;    /* 1 if the exit is a failed VM entry */
  uint64 qual;       /* exit qualification: exit-specific detail */
  uint64 rip;        /* guest RIP of the exiting instruction */
  uint error;        /* VM-instruction error, for EXIT_VMFAIL */
  uint intrinfo;     /* exception exits: interruption information */
  uint intrerr;      /* exception exits: error code */
  uint64 gpa;        /* EPT exits: guest-physical address */
  struct {           /* EXIT_IO */
    ushort port;     /* I/O port number */
    uchar size;      /* 1, 2 or 4 bytes */
    uchar in;        /* 1 for IN, 0 for OUT */
    uint data;       /* OUT: value written (unused for IN) */
    /*
     * String I/O (INS/OUTS, HW2): the kernel decodes, runxv6 emulates.
     *
     * xv6's IDE driver moves each sector with REP INSL and REP OUTSL.
     * The data is in guest memory, not in RAX, so io.data is unused;
     * the kernel reports where the data is, and runxv6 copies it with
     * vmmem(VMMEM_VIRT). The kernel still advances RIP past the whole
     * instruction, so runxv6 must do every iteration at once and leave
     * RCX, RDI (INS) or RSI (OUTS) as the CPU would have: RCX 0, and
     * RDI or RSI moved by count * size (forward: xv6 never sets
     * RFLAGS.DF).
     */
    uchar string;    /* 1 for INS/OUTS (exit qualification bit 4) */
    uchar rep;       /* 1 if it has a REP prefix (qualification bit 5) */
    uint64 count;    /* units to transfer: RCX with REP, else 1 */
    /*
     * The VMCS guest-linear address (GUEST_LINEAR_ADDRESS in vmx.h):
     * RDI for INS, RSI for OUTS, since 64-bit mode ignores the segment
     * bases. It goes through the guest's page tables, not the EPT.
     */
    uint64 addr;     /* guest-virtual address of the memory operand */
  } io;
};

/*
 * Flags for vmmem() (HW2). Without VMMEM_VIRT, addr is guest-physical
 * and is translated only through the EPT. With it, addr is first
 * translated as the guest's own MMU would, through the guest's page
 * tables, using its CR0, CR3 and EFER as of the last exit; this is
 * what string I/O needs, since the guest's buffer is a kernel virtual
 * address. The flags can be combined (VMMEM_VIRT | VMMEM_WRITE).
 */
#define VMMEM_WRITE   1  /* copy from the user buffer into the guest */
/* addr is guest-virtual: walk the guest's page tables */
#define VMMEM_VIRT    2

/*
 * Guest-physical addresses of the local APIC and the I/O APIC. vmcreate()
 * backs each with a zeroed page (HW2): enough for xv6 to boot, though not
 * a real emulation of either device.
 *
 * These are the standard PC addresses. xv6 finds the local APIC at the
 * address the MP table gives (runxv6 writes VM_LAPIC_ADDR there) and
 * uses 0xFEC00000 for the I/O APIC (IOAPIC in ioapic.c). With plain
 * memory behind them, xv6's register writes are kept and read back,
 * reads of untouched registers return 0, and no interrupt ever comes
 * through them: runxv6 injects every interrupt with vminject().
 */
#define VM_LAPIC_ADDR  0xFEE00000
#define VM_IOAPIC_ADDR 0xFEC00000
