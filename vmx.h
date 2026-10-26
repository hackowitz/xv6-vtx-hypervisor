/*
 * Intel VT-x (VMX) definitions.
 * See the Intel SDM, Volume 3 (325384-092), chapters 26-33 and Appendix A-C.
 *
 * This header is the "vocabulary" of the hypervisor: the MSRs that
 * report what the CPU supports, the bits of the VMCS control fields
 * that select which guest actions cause a VM exit, and the numeric
 * encodings used to name each VMCS field in VMREAD/VMWRITE.
 *
 * The VMCS (Virtual-Machine Control Structure) is a 4 KB, CPU-defined,
 * opaque memory region. Software never reads or writes it with normal
 * loads and stores; it uses VMREAD/VMWRITE with a 32-bit "field
 * encoding" that names the field. The VMCS has six groups of fields
 * (SDM 27.4-27.9):
 *
 *   guest-state area   loaded on VM entry, saved on VM exit
 *   host-state area    loaded on VM exit (where the hypervisor resumes)
 *   VM-execution       what the guest may do without causing an exit
 *     control fields
 *   VM-exit control    how an exit behaves
 *     fields
 *   VM-entry control   how an entry behaves
 *     fields
 *   VM-exit            read-only: why the last exit happened
 *     information
 *
 * Which header defines what: this file is only used by the kernel
 * (vmx.c); vmxapi.h is shared with user programs.
 */

/*
 * ------------------------------------------------------------------
 * Model-specific registers (MSRs)
 * ------------------------------------------------------------------
 * VMX reports its capabilities through a family of read-only MSRs
 * (SDM Appendix A). They exist only if CPUID.1:ECX.VMX is set;
 * reading them on a CPU without VMX raises #GP and panics the kernel,
 * so vmx.c reads them only after checking for VMX (see vmx_ok in
 * struct cpu, proc.h).
 */

/*
 * Firmware/BIOS control over VMX. Must have the lock bit and the
 * "VMXON outside SMX" bit set before VMXON works (SDM 26.7).
 */
#define MSR_IA32_FEATURE_CONTROL      0x03A

/*
 * Basic VMX information (A.1): bits 30:0 are the VMCS revision
 * identifier, which software must store in the first 4 bytes of the
 * VMXON region and of every VMCS; bit 55 says the "TRUE" control MSRs
 * below exist.
 */
#define MSR_IA32_VMX_BASIC            0x480

/*
 * Capability MSRs for the control fields (A.3-A.5). Each is 64 bits:
 * the low 32 bits are the "allowed-0" settings (a 1 bit there means
 * the control MUST be 1), the high 32 bits are the "allowed-1"
 * settings (a 0 bit there means the control MUST be 0). vmx.c's
 * adjustctl() uses them to turn a wish-list of controls into a legal
 * value.
 */
#define MSR_IA32_VMX_PINBASED_CTLS    0x481
#define MSR_IA32_VMX_PROCBASED_CTLS   0x482
#define MSR_IA32_VMX_EXIT_CTLS        0x483
#define MSR_IA32_VMX_ENTRY_CTLS       0x484

/*
 * Fixed bits of CR0 and CR4 while in VMX operation (A.7-A.8). A bit
 * that is 1 in FIXED0 must be 1 in the register; a bit that is 0 in
 * FIXED1 must be 0. The host must obey them to execute VMXON; the
 * guest must obey them too, except that "unrestricted guest" relaxes
 * CR0.PE and CR0.PG.
 */
#define MSR_IA32_VMX_CR0_FIXED0       0x486
#define MSR_IA32_VMX_CR0_FIXED1       0x487
#define MSR_IA32_VMX_CR4_FIXED0       0x488
#define MSR_IA32_VMX_CR4_FIXED1       0x489

/* Capabilities of the secondary processor-based controls (A.3.3). */
#define MSR_IA32_VMX_PROCBASED_CTLS2  0x48B

/*
 * EPT and VPID capabilities (A.10): supported page-walk lengths,
 * memory types, and which INVEPT/INVVPID types exist.
 */
#define MSR_IA32_VMX_EPT_VPID_CAP     0x48C

/*
 * "TRUE" variants of the pin/proc/exit/entry capability MSRs (A.2).
 * On older CPUs some controls are forced to 1 in the plain MSRs for
 * legacy reasons ("default1" bits); the TRUE MSRs report the real
 * constraints. They exist only if VMX_BASIC bit 55 is set.
 */
#define MSR_IA32_VMX_TRUE_PINBASED_CTLS  0x48D
#define MSR_IA32_VMX_TRUE_PROCBASED_CTLS 0x48E
#define MSR_IA32_VMX_TRUE_EXIT_CTLS      0x48F
#define MSR_IA32_VMX_TRUE_ENTRY_CTLS     0x490

/*
 * Base addresses of FS and GS in 64-bit mode. They are not part of a
 * segment descriptor, so the host's values are copied into the VMCS
 * host-state area (HOST_FS_BASE/HOST_GS_BASE) by vmxhoststate().
 */
#define MSR_IA32_FS_BASE              0xC0000100
#define MSR_IA32_GS_BASE              0xC0000101

/* Bits of IA32_FEATURE_CONTROL (SDM 26.7). */
/* Once set, the MSR is read-only until reset; VMXON requires this. */
#define FEATURE_CONTROL_LOCKED        (1 << 0)
/* Allow VMXON in normal (non-SMX, non-TXT) operation. */
#define FEATURE_CONTROL_VMXON_OUTSIDE_SMX (1 << 2)

/* IA32_VMX_BASIC bit 55: the TRUE_* capability MSRs are supported. */
#define VMX_BASIC_TRUE_CTLS           (1ULL << 55)

/* CPUID.1:ECX bit 5: the CPU supports VMX at all. */
#define CPUID1_ECX_VMX                (1 << 5)

/*
 * ------------------------------------------------------------------
 * Control register and EFER bits
 * ------------------------------------------------------------------
 */

/*
 * CR0.NE (numeric error): must be 1 in VMX operation (it is a fixed-1
 * bit). CR0.ET (bit 4) is hardwired to 1 on modern CPUs; the guest's
 * initial CR0 sets it for a clean value.
 */
#define CR0_NE                        0x00000020
#define CR0_ET                        0x00000010

/*
 * CR4.VMXE: set to enable VMX; VMXON faults without it. vmxinit()
 * sets it on the host, and vmcsinit() makes the guest see it as 0
 * through the CR4 guest/host mask, so the guest cannot clear it.
 */
#define CR4_VMXE                      0x00002000

/*
 * IA32_EFER.LMA (bit 10, "long mode active"). The processor, not
 * software, sets it when paging is enabled with EFER.LME = 1. The
 * hypervisor mirrors it into the "IA-32e mode guest" entry control
 * before every VM entry (see vmrun()).
 */
#define EFER_LMA                      0x00000400

/*
 * ------------------------------------------------------------------
 * VM-execution controls: what makes the guest exit
 * ------------------------------------------------------------------
 * The fields PIN_BASED_VM_EXEC_CONTROL, CPU_BASED_VM_EXEC_CONTROL and
 * SECONDARY_VM_EXEC_CONTROL (defined below) are bit vectors that select
 * which guest events cause a VM exit (SDM 27.6). A control that is
 * not set means the guest performs that operation natively.
 */

/* Pin-based VM-execution controls (27.6.1): asynchronous events. */

/*
 * External-interrupt exiting: a host device interrupt arriving while
 * the guest runs causes a VM exit, so the guest cannot starve the
 * host of interrupts. Without this, the interrupt would be delivered
 * through the GUEST's IDT, which is empty here.
 */
#define PIN_EXTINT_EXITING            (1 << 0)

/* NMI exiting: a non-maskable interrupt causes a VM exit as well. */
#define PIN_NMI_EXITING               (1 << 3)

/*
 * Primary processor-based VM-execution controls (27.6.2): exits on
 * individual instructions and on CR accesses.
 */

/* HLT exits. runvm uses HLT as the guest's "I am done" signal. */
#define CPU_HLT_EXITING               (1 << 7)
/*
 * Interrupt-window exiting (HW2): exit as soon as the guest can take an
 * external interrupt (RFLAGS.IF = 1 and no STI/MOV SS blocking). Set it
 * while an interrupt is waiting to be injected (SDM 27.6.2, 28.2).
 *
 * Bit 2 of the primary processor-based controls (SDM Table 27-6). The
 * exit comes before the guest executes any instruction in an open
 * window, so if the window is already open at VM entry the guest exits
 * at once (SDM 29.7.5); the exit reason is 7, "interrupt window" (SDM
 * Appendix C, Table C-1; EXIT_INTR_WINDOW in vmxapi.h). vmxinject()
 * turns it on only while an interrupt waits, since otherwise the guest
 * would exit before every instruction it runs with interrupts enabled.
 */
#define CPU_INTR_WINDOW_EXITING       (1 << 2)

/* INVLPG exits (not used by vmx.c). */
#define CPU_INVLPG_EXITING            (1 << 9)

/*
 * MOV to CR3 / MOV from CR3 exit (not used by vmx.c: with EPT the
 * guest manages its own page tables without hypervisor help).
 * vmcsinit() rejects a CPU that forces these on.
 */
#define CPU_CR3_LOAD_EXITING          (1 << 15)
#define CPU_CR3_STORE_EXITING         (1 << 16)

/*
 * Every IN/OUT/INS/OUTS exits, regardless of port. This is how the
 * emulated serial port reaches runvm. It overrides the I/O bitmaps.
 */
#define CPU_UNCOND_IO_EXITING         (1 << 24)

/*
 * Use I/O bitmaps to select which ports exit (not used here; the
 * unconditional control above is simpler).
 */
#define CPU_USE_IO_BITMAPS            (1 << 25)

/*
 * Use an MSR bitmap to select which RDMSR/WRMSR exit. Left clear, so
 * every RDMSR and WRMSR exits (vmxmsr() then emulates IA32_EFER only).
 */
#define CPU_USE_MSR_BITMAPS           (1 << 28)

/*
 * Activate secondary controls: SECONDARY_VM_EXEC_CONTROL is honored
 * only if this bit is 1. Needed for EPT and unrestricted guest.
 */
#define CPU_ACTIVATE_SECONDARY        (1U << 31)

/* Secondary processor-based VM-execution controls (27.6.2). */

/*
 * Enable EPT (Extended Page Tables, SDM ch. 31): guest-physical
 * addresses (what the guest thinks are physical addresses) are
 * translated to host-physical addresses by a second set of page
 * tables, rooted at the EPT pointer. This is how a VM gets its
 * private memory, with no shadow page tables.
 */
#define CPU2_ENABLE_EPT               (1 << 1)

/*
 * Unrestricted guest: lets the guest run with CR0.PE = 0 or CR0.PG = 0
 * (real mode, or protected mode without paging). Requires EPT. The
 * guest here starts in 32-bit protected mode with paging off, and its
 * own code then switches to 64-bit mode.
 */
#define CPU2_UNRESTRICTED_GUEST       (1 << 7)

/*
 * ------------------------------------------------------------------
 * VM-exit controls (27.7): what the processor does on a VM exit
 * ------------------------------------------------------------------
 */

/*
 * "Host address-space size": the host resumes in 64-bit mode (loads
 * CS.L = 1 and sets EFER.LMA/LME). Required because xv6-64 runs in
 * long mode.
 */
#define EXIT_HOST_ADDR_SPACE_SIZE     (1 << 9)

/*
 * Acknowledge interrupt on exit: the processor acknowledges the
 * pending external interrupt at the APIC and records its vector.
 * Not used by vmx.c: the interrupt is left pending, and the host
 * takes it through its own IDT when vmrun() re-enables interrupts.
 */
#define EXIT_ACK_INTR_ON_EXIT         (1 << 15)

/* Save the guest's IA32_EFER into GUEST_IA32_EFER at each exit. */
#define EXIT_SAVE_IA32_EFER           (1 << 20)

/* Load HOST_IA32_EFER into IA32_EFER at each exit. */
#define EXIT_LOAD_IA32_EFER           (1 << 21)

/*
 * ------------------------------------------------------------------
 * VM-entry controls (27.8): what the processor does on VM entry
 * ------------------------------------------------------------------
 */

/*
 * "IA-32e mode guest": the guest runs in 64-bit mode. Must be
 * consistent with EFER.LMA in the guest state (the entry checks
 * fail otherwise). The guest starts in 32-bit mode with this bit
 * clear; vmrun() sets it once the guest's EFER shows LMA.
 */
#define ENTRY_IA32E_MODE_GUEST        (1 << 9)

/* Load GUEST_IA32_EFER into IA32_EFER on entry. */
#define ENTRY_LOAD_IA32_EFER          (1 << 15)

/*
 * ------------------------------------------------------------------
 * EPT (Extended Page Tables), SDM chapter 31
 * ------------------------------------------------------------------
 * EPT is a 4-level tree, like x86-64 paging: PML4 -> PDPT -> PD -> PT,
 * each table a 4 KB page of 512 8-byte entries, indexed by 9 bits of
 * the guest-physical address. eptwalk() in vmx.c walks it.
 */

/*
 * Capability bits of IA32_VMX_EPT_VPID_CAP (A.10). vmcreate()
 * requires all of those it uses.
 */
/* 4-level page walk is supported. */
#define EPT_CAP_WALK_4                (1ULL << 6)
/* Write-back is a supported EPT memory type. */
#define EPT_CAP_MEMTYPE_WB            (1ULL << 14)
/* The INVEPT instruction is supported... */
#define EPT_CAP_INVEPT                (1ULL << 20)
/* ...with the single-context type (flush one EPTP)... */
#define EPT_CAP_INVEPT_SINGLE         (1ULL << 25)
/* ...and with the all-context type (flush every EPTP; unused). */
#define EPT_CAP_INVEPT_ALL            (1ULL << 26)

/*
 * EPT entry permission bits (31.3.2). An entry with none of R/W/X
 * set is "not present"; the code tests EPT_RWX to mean present.
 * An access without permission causes an EPT violation exit.
 */
#define EPT_R                         (1ULL << 0)
#define EPT_W                         (1ULL << 1)
#define EPT_X                         (1ULL << 2)
#define EPT_RWX                       (EPT_R | EPT_W | EPT_X)

/*
 * EPT memory type, bits 5:3 of a leaf entry: 6 is write-back
 * (cacheable). Meaningful only in leaf entries; it is ignored (or
 * must be zero) in entries that point to a lower-level table.
 */
#define EPT_MEMTYPE_WB                (6ULL << 3)   /* leaf entries only */

/*
 * Extract the physical address of the next-level table (or of the
 * mapped 4 KB page) from an entry: bits 51:12.
 */
#define EPT_ADDR(e)                   ((e) & 0x000FFFFFFFFFF000ULL)

/*
 * EPT pointer (EPTP) format (27.6.11). It holds the physical address
 * of the PML4 table in bits 51:12, ORed with:
 *   bits 2:0   memory type used to access the EPT tables (6 = WB)
 *   bits 5:3   page-walk length minus 1 (3 means 4 levels)
 * (bit 6, accessed/dirty flags, stays 0.) vmcreate() builds the value.
 */
#define EPTP_MEMTYPE_WB               6ULL
#define EPTP_WALK_4                   (3ULL << 3)

/*
 * INVEPT types (SDM 33.3). The CPU caches guest-physical to
 * host-physical translations; INVEPT discards them. Single-context
 * discards those tagged with one EPTP; all-context discards all.
 */
#define INVEPT_SINGLE_CONTEXT         1
#define INVEPT_ALL_CONTEXT            2

/*
 * ------------------------------------------------------------------
 * VMCS field encodings (SDM Appendix B)
 * ------------------------------------------------------------------
 * The 32-bit encoding passed to VMREAD/VMWRITE has this layout:
 *   bit 0       access type (0 = full field, 1 = high half of a
 *               64-bit field; not used here)
 *   bits 9:1    index within the group
 *   bits 11:10  type: 0 control, 1 read-only exit info, 2 guest
 *               state, 3 host state
 *   bits 14:13  width: 0 = 16-bit, 1 = 64-bit, 2 = 32-bit,
 *               3 = natural width (64 bits in long mode)
 * So the top hex digit reads as width and type: 0x0800 is a 16-bit
 * guest-state field, 0x4000 a 32-bit control field, 0x6C00 a
 * natural-width host-state field, and so on.
 *
 * The fields below are grouped by width, then by type. Only the
 * fields this hypervisor touches are defined; the SDM lists more.
 */

/* ---- 16-bit guest-state fields: segment selectors (B.1.2) ---- */
#define GUEST_ES_SELECTOR             0x0800
#define GUEST_CS_SELECTOR             0x0802
#define GUEST_SS_SELECTOR             0x0804
#define GUEST_DS_SELECTOR             0x0806
#define GUEST_FS_SELECTOR             0x0808
#define GUEST_GS_SELECTOR             0x080A
#define GUEST_LDTR_SELECTOR           0x080C
#define GUEST_TR_SELECTOR             0x080E

/*
 * ---- 16-bit host-state fields: selectors loaded on VM exit (B.1.3)
 * (No LDTR: it is set to "unusable" on exit.) The RPL and TI bits of
 * each selector must be 0, and CS and TR must be nonzero. ----
 */
#define HOST_ES_SELECTOR              0x0C00
#define HOST_CS_SELECTOR              0x0C02
#define HOST_SS_SELECTOR              0x0C04
#define HOST_DS_SELECTOR              0x0C06
#define HOST_FS_SELECTOR              0x0C08
#define HOST_GS_SELECTOR              0x0C0A
#define HOST_TR_SELECTOR              0x0C0C

/* ---- 64-bit control fields (B.2.1) ---- */

/* EPT pointer: root of this VM's EPT (format described above). */
#define EPT_POINTER                   0x201A

/* ---- 64-bit read-only data fields (B.2.2) ---- */

/*
 * Guest-physical address that caused an EPT violation or
 * misconfiguration exit. Reported to runvm as vmexit.gpa.
 */
#define GUEST_PHYSICAL_ADDRESS        0x2400

/* ---- 64-bit guest-state fields (B.2.3) ---- */

/*
 * VMCS link pointer: used for VMCS shadowing. Must be all ones
 * (~0) when shadowing is not in use, or VM entry fails.
 */
#define VMCS_LINK_POINTER             0x2800

/* Guest IA32_DEBUGCTL MSR (loaded on entry; set to 0). */
#define GUEST_IA32_DEBUGCTL           0x2802

/*
 * Guest IA32_EFER. Loaded on entry (ENTRY_LOAD_IA32_EFER) and saved
 * on exit (EXIT_SAVE_IA32_EFER). Since the guest's EFER accesses
 * exit, the hypervisor keeps this value in struct vm.efer.
 */
#define GUEST_IA32_EFER               0x2806

/* ---- 64-bit host-state fields (B.2.4) ---- */

/* Host IA32_EFER, loaded on exit (EXIT_LOAD_IA32_EFER). */
#define HOST_IA32_EFER                0x2C02

/* ---- 32-bit control fields (B.3.1) ---- */

/* Pin-based VM-execution controls (PIN_* bits above). */
#define PIN_BASED_VM_EXEC_CONTROL     0x4000

/* Primary processor-based VM-execution controls (CPU_* bits above). */
#define CPU_BASED_VM_EXEC_CONTROL     0x4002

/*
 * Exception bitmap: bit n set means guest exception vector n causes a
 * VM exit. vmcsinit() sets all 32 bits, because the guest has no IDT
 * and a fault would otherwise end in a triple fault with no
 * explanation.
 */
#define EXCEPTION_BITMAP              0x4004

/*
 * Page-fault error-code mask and match: together with bit 14 of the
 * exception bitmap they filter which guest page faults exit. Both 0
 * here, which (with the exception bitmap bit set) means all of them.
 */
#define PAGE_FAULT_ERROR_CODE_MASK    0x4006
#define PAGE_FAULT_ERROR_CODE_MATCH   0x4008

/*
 * Number of CR3-target values (0..4): MOV to CR3 of one of these
 * values does not exit. 0 means none.
 */
#define CR3_TARGET_COUNT              0x400A

/* VM-exit controls (EXIT_* bits above). */
#define VM_EXIT_CONTROLS              0x400C

/*
 * Number of MSRs the processor saves to / loads from memory areas on
 * VM exit. Zero here: EFER is handled by the dedicated controls.
 */
#define VM_EXIT_MSR_STORE_COUNT       0x400E
#define VM_EXIT_MSR_LOAD_COUNT        0x4010

/* VM-entry controls (ENTRY_* bits above). */
#define VM_ENTRY_CONTROLS             0x4012

/* Number of MSRs the processor loads from memory on VM entry. */
#define VM_ENTRY_MSR_LOAD_COUNT       0x4014

/*
 * VM-entry interruption information: lets the hypervisor inject an
 * interrupt or exception into the guest on entry. Bit 31 is "valid";
 * 0 means inject nothing.
 */
#define VM_ENTRY_INTR_INFO_FIELD      0x4016
/*
 * Error code and instruction length that go with an injected event
 * (HW2). The newest SDM calls these fields the "injected-event error
 * code" and the "VM-entry instruction length" (SDM 27.8.3, 29.6).
 * Encodings: SDM Appendix B.3.1, Table B-8, which notes the error code
 * field's old name, "VM-entry error code".
 *
 * The error code is pushed on the guest's stack, as a real exception
 * would push it, only if bit 11 (deliver error code) is set in
 * VM_ENTRY_INTR_INFO_FIELD; its bits 31:16 must then be 0 (SDM
 * 29.2.1.3, 29.6.1.1).
 *
 * The instruction length is used only for software events (types 4, 5
 * and 6 below): the guest RIP pushed for them is the RIP from the
 * guest-state area plus this length, the address after the INT n, just
 * as when the guest executes it (SDM 29.6.1.1). It must be 0-15 (SDM
 * 29.2.1.3). Re-injected events take it from VM_EXIT_INSTRUCTION_LEN,
 * which a VM exit during delivery of a software event sets to the
 * length of the instruction that raised it (SDM 30.2.5).
 */
#define VM_ENTRY_EXCEPTION_ERROR_CODE 0x4018
#define VM_ENTRY_INSTRUCTION_LEN      0x401A

/* Secondary processor-based controls (CPU2_* bits above). */
#define SECONDARY_VM_EXEC_CONTROL     0x401E

/* ---- 32-bit read-only data fields: exit information (B.3.2) ---- */

/*
 * Why a VMX instruction failed (SDM 33.4). Valid after VMfailValid,
 * for instance when VMLAUNCH is rejected or VMWRITE gets a bad field.
 */
#define VM_INSTRUCTION_ERROR          0x4400

/*
 * Exit reason: bits 15:0 are the basic exit reason (EXIT_* in
 * vmxapi.h); bit 31 (EXIT_REASON_ENTRY_FAILURE) means the VM entry
 * itself failed.
 */
#define VM_EXIT_REASON                0x4402

/*
 * Interruption information for exits caused by an exception or NMI:
 * vector, type, error-code-valid (see INTR_INFO_* below).
 */
#define VM_EXIT_INTR_INFO             0x4404

/* Error code of the exception, if INTR_INFO_ERROR_VALID is set. */
#define VM_EXIT_INTR_ERROR_CODE       0x4406
/*
 * IDT-vectoring information (HW2): if the exit happened while the CPU
 * was delivering an event to the guest (an injected interrupt, an
 * exception, an INT n), this field describes that event, and the
 * hypervisor must inject it again or it is lost. The newest SDM calls it
 * the "original-event identification" (SDM 27.9.3, 30.2.4).
 *
 * Encodings: SDM Appendix B.3.2, Table B-9 ("original-event
 * identification" and "original-event error code"). The format is that
 * of the injection field (SDM Table 27-21 against Table 27-18): vector
 * in bits 7:0, type in 10:8, error code valid in bit 11, valid in bit
 * 31. So it can be written back to VM_ENTRY_INTR_INFO_FIELD almost as
 * is, but bit 12 is undefined here (SDM 30.2.4) and must be 0 for
 * injection (SDM 29.2.1.3), so vmxinject() clears it.
 *
 * Example: delivering an interrupt pushes onto the guest's stack, and
 * that write causes an EPT violation; the guest never saw the
 * interrupt. An event injected by VM entry is reported here too if its
 * delivery causes an exit (SDM 29.6.1.2). The error code field is
 * valid only if bit 11 is set (SDM 30.2.4).
 */
#define IDT_VECTORING_INFO            0x4408
#define IDT_VECTORING_ERROR_CODE      0x440A

/*
 * Length in bytes of the instruction that caused the exit. After
 * emulating the instruction, the hypervisor adds this to the guest
 * RIP to step over it.
 */
#define VM_EXIT_INSTRUCTION_LEN       0x440C

/* ---- 32-bit guest-state fields (B.3.3) ---- */

/*
 * Each guest segment register has four VMCS fields: selector, base,
 * limit, and access rights. The limit field of segment i is at
 * GUEST_ES_LIMIT + 2*i and its access rights at GUEST_ES_AR_BYTES +
 * 2*i (see SEG_* below).
 */
#define GUEST_ES_LIMIT                0x4800

/* GDTR and IDTR have only a base and a limit. */
#define GUEST_GDTR_LIMIT              0x4810
#define GUEST_IDTR_LIMIT              0x4812

/* Segment access rights; format described at AR_* below. */
#define GUEST_ES_AR_BYTES             0x4814

/*
 * Interruptibility state: blocking by STI, by MOV SS, by SMI, by NMI.
 * 0 means the guest can take interrupts immediately.
 */
#define GUEST_INTERRUPTIBILITY_INFO   0x4824

/* Activity state: 0 means active (1 = HLT, 2 = shutdown, 3 = wait). */
#define GUEST_ACTIVITY_STATE          0x4826

/* Guest IA32_SYSENTER_CS MSR. */
#define GUEST_SYSENTER_CS             0x482A

/* ---- 32-bit host-state fields (B.3.4) ---- */

/* Host IA32_SYSENTER_CS MSR, loaded on exit. */
#define HOST_IA32_SYSENTER_CS         0x4C00

/* ---- Natural-width control fields (B.4.1) ---- */

/*
 * CR0/CR4 guest/host masks: a bit set in the mask is "owned by the
 * host". If the guest tries to change an owned bit to a value
 * different from the read shadow, the guest causes a CR-access exit;
 * guest reads of an owned bit return the read-shadow value.
 * CR0 mask 0: the guest controls CR0 (it must be able to enable
 * paging itself). CR4 mask CR4_VMXE: the host owns VMXE.
 */
#define CR0_GUEST_HOST_MASK           0x6000
#define CR4_GUEST_HOST_MASK           0x6002

/* Values the guest sees when it reads host-owned bits. */
#define CR0_READ_SHADOW               0x6004
#define CR4_READ_SHADOW               0x6006

/* ---- Natural-width read-only data fields (B.4.2) ---- */

/*
 * Exit qualification: extra, exit-reason-specific detail (30.2.1).
 * For I/O exits: bits 2:0 are the access size minus 1, bit 3 is the
 * direction (1 = IN), bit 4 is set for string instructions, bits
 * 31:16 are the port number. For EPT violations it holds the type
 * of access that failed (read/write/fetch).
 */
#define EXIT_QUALIFICATION            0x6400
/*
 * Guest-linear address (HW2): for INS/OUTS exits, the guest-virtual
 * address of the memory operand (SDM 27.9.1, 30.2.1).
 *
 * Encoding: SDM Appendix B.4.2, Table B-13. For INS it is the ES base
 * plus (E)DI, for OUTS the DS base (or an override segment's) plus
 * (E)SI, so the hypervisor need not compute it from the segment
 * registers (SDM 30.2.1). In 64-bit mode those segment bases are 0, so
 * it is just RDI or RSI, a virtual address that the guest's page
 * tables translate; it is not guest-physical, and vmmem() translates
 * it with VMMEM_VIRT. Bit 5 of the I/O exit qualification says whether
 * the instruction has a REP prefix (SDM Table 30-5).
 */
#define GUEST_LINEAR_ADDRESS          0x640A

/* ---- Natural-width guest-state fields (B.4.3) ---- */

/*
 * Guest control registers. The guest's registers are not those of
 * the CPU while the guest runs; they live here, and are swapped in
 * on entry. GUEST_CR3 is the guest's page-table root (a
 * guest-physical address, translated through EPT).
 */
#define GUEST_CR0                     0x6800
#define GUEST_CR3                     0x6802
#define GUEST_CR4                     0x6804

/* Segment base; base of segment i is at GUEST_ES_BASE + 2*i. */
#define GUEST_ES_BASE                 0x6806

#define GUEST_GDTR_BASE               0x6816
#define GUEST_IDTR_BASE               0x6818

/* Guest debug register 7 (bit 10 is reserved and reads as 1). */
#define GUEST_DR7                     0x681A

/*
 * Guest stack pointer, instruction pointer, and flags. These three
 * general registers are kept in the VMCS, not in struct guestregs,
 * because the processor saves and loads them itself on exit and
 * entry.
 */
#define GUEST_RSP                     0x681C
#define GUEST_RIP                     0x681E
#define GUEST_RFLAGS                  0x6820

/* Debug exceptions pending at entry (e.g. single-step); 0 here. */
#define GUEST_PENDING_DBG_EXCEPTIONS  0x6822

/* Guest SYSENTER stack and entry point MSRs. */
#define GUEST_SYSENTER_ESP            0x6824
#define GUEST_SYSENTER_EIP            0x6826

/* ---- Natural-width host-state fields (B.4.4) ---- */

/*
 * Host control registers, loaded on VM exit. CR3 and CR4 are
 * per-process and per-CPU, so vmxhoststate() rewrites them before
 * each entry.
 */
#define HOST_CR0                      0x6C00
#define HOST_CR3                      0x6C02
#define HOST_CR4                      0x6C04

/*
 * Bases that the host selectors above cannot supply in 64-bit mode:
 * FS and GS (from MSRs), TR, GDTR and IDTR. TR and GDTR depend on
 * the CPU the process is on.
 */
#define HOST_FS_BASE                  0x6C06
#define HOST_GS_BASE                  0x6C08
#define HOST_TR_BASE                  0x6C0A
#define HOST_GDTR_BASE                0x6C0C
#define HOST_IDTR_BASE                0x6C0E

/* Host SYSENTER MSRs, loaded on exit. */
#define HOST_IA32_SYSENTER_ESP        0x6C10
#define HOST_IA32_SYSENTER_EIP        0x6C12

/*
 * Where the host resumes after a VM exit. HOST_RSP is the stack
 * pointer (written by vmx_enter in vmxasm.S, which has its own copy of
 * this constant: keep the two in sync), and HOST_RIP is the address of
 * vmx_exit.
 */
#define HOST_RSP                      0x6C14
#define HOST_RIP                      0x6C16

/*
 * ------------------------------------------------------------------
 * Guest segment register helpers
 * ------------------------------------------------------------------
 */

/*
 * Guest segment registers, in VMCS encoding order: the selector,
 * limit, access rights and base of segment i are at
 * GUEST_ES_SELECTOR + 2*i, GUEST_ES_LIMIT + 2*i, and so on. The
 * order is ES, CS, SS, DS, FS, GS, LDTR, TR (not the order of the
 * x86 segment-register encoding). guestseg() in vmx.c relies on it.
 */
#define SEG_ES   0
#define SEG_CS   1
#define SEG_SS   2
#define SEG_DS   3
#define SEG_FS   4
#define SEG_GS   5
#define SEG_LDTR 6
#define SEG_TR   7

/*
 * Segment access rights, in VMCS format (SDM 27.4.1). Bits 3:0 are
 * the descriptor type, bit 4 is S (1 = code/data segment, 0 =
 * system), bits 6:5 DPL, bit 7 P (present), bit 14 D/B (32-bit
 * segment), bit 15 G (limit in 4 KB units), bit 16 "unusable".
 */
/* 32-bit flat code segment: G, D/B, P, S, execute/read, accessed. */
#define AR_CODE32     0xC09B
/* 32-bit flat data segment: G, D/B, P, S, read/write, accessed. */
#define AR_DATA32     0xC093
/* Busy 32-bit TSS: P, system type 0xB. TR must be a valid TSS. */
#define AR_TSS32_BUSY 0x008B
/* Segment is not usable (a null selector); used for the LDT. */
#define AR_UNUSABLE   0x10000

/*
 * ------------------------------------------------------------------
 * VM-exit interruption information (SDM 30.2.2)
 * ------------------------------------------------------------------
 * Layout of VM_EXIT_INTR_INFO for exits with reason
 * EXIT_EXCEPTION_NMI: bits 7:0 vector, bits 10:8 type, bit 11
 * error code valid, bit 31 valid. Type 2 is NMI; 3 is a hardware
 * exception.
 */
#define INTR_INFO_VECTOR(x)       ((x) & 0xff)
#define INTR_INFO_TYPE(x)         (((x) >> 8) & 0x7)
#define INTR_INFO_ERROR_VALID     (1 << 11)
#define INTR_TYPE_NMI             2
/*
 * More event types, for injecting events (HW2; SDM 27.8.3).
 *
 * The same layout serves VM_ENTRY_INTR_INFO_FIELD (SDM Table 27-18) and
 * IDT_VECTORING_INFO (SDM Table 27-21). Bit 31 is "valid": VM entry
 * injects an event only if it is set, and every VM exit clears it in
 * the entry field (SDM 27.8.3).
 */
#define INTR_INFO_VALID           (1U << 31)
/* An interrupt from a device: what vminject() injects (bits 10:8 = 0). */
#define INTR_TYPE_EXT_INTR        0
/* An exception raised by the CPU, such as #PF or #GP. */
#define INTR_TYPE_HW_EXCEPTION    3
/* INT n, such as xv6's system-call trap. */
#define INTR_TYPE_SOFT_INTR       4
/* INT1 (opcode F1). */
#define INTR_TYPE_PRIV_SW_EXC     5
/* INT3 or INTO. */
#define INTR_TYPE_SOFT_EXC        6
/*
 * Types 4-6 are raised by an instruction, so injecting one needs
 * VM_ENTRY_INSTRUCTION_LEN; types 0 and 3 do not (SDM 29.6.1.1). Type 1
 * is reserved and 7 is "other event" (SDM Table 27-18); neither is used.
 */
/*
 * Guest interruptibility state: blocking by STI and by MOV SS.
 *
 * Bits of GUEST_INTERRUPTIBILITY_INFO (SDM 27.4.2, Table 27-3). STI
 * with RFLAGS.IF = 0, and MOV SS or POP SS, block interrupts until the
 * next instruction boundary, even though RFLAGS.IF may be 1. VM entry
 * fails if it injects an external interrupt while either bit is set
 * (SDM 29.3.1.5) or while RFLAGS.IF is 0 (SDM 29.3.1.4), so
 * vmxinject() tests all three before injecting.
 */
#define INTERRUPTIBILITY_STI      (1 << 0)
#define INTERRUPTIBILITY_MOVSS    (1 << 1)

/*
 * Bit 31 of VM_EXIT_REASON: the "exit" is a failed VM entry (invalid
 * guest state, bad MSR load, ...). The guest never ran; the basic
 * reason and the exit qualification say what was wrong (SDM 29.8).
 */
#define EXIT_REASON_ENTRY_FAILURE (1U << 31)

/*
 * ------------------------------------------------------------------
 * Guest general-purpose registers
 * ------------------------------------------------------------------
 * The processor saves and restores RSP, RIP, and RFLAGS (through the
 * VMCS), but NOT the other general-purpose registers: on VM exit
 * they still hold the guest's values, and on entry they must hold
 * the guest's values. vmx_enter and vmx_exit in vmxasm.S copy them
 * to and from this structure.
 *
 * The layout must match the offsets used there: vmxasm.S addresses
 * each field by a hard-coded byte offset (rax at 0, rbx at 8, ...,
 * r15 at 112), so reordering or inserting a field breaks it silently.
 * The order of the first fifteen fields also matches the VMREG_*
 * enum in vmxapi.h, because vmsetreg() indexes the structure as an
 * array of uint64.
 */
struct guestregs {
  uint64 rax;   /* 0 */
  uint64 rbx;   /* 8 */
  uint64 rcx;   /* 16 */
  uint64 rdx;   /* 24 */
  uint64 rsi;   /* 32 */
  uint64 rdi;   /* 40: vmx_exit saves it last, through the stack */
  uint64 rbp;   /* 48 */
  uint64 r8;    /* 56 */
  uint64 r9;    /* 64 */
  uint64 r10;   /* 72 */
  uint64 r11;   /* 80 */
  uint64 r12;   /* 88 */
  uint64 r13;   /* 96 */
  uint64 r14;   /* 104 */
  uint64 r15;   /* 112 */
};
