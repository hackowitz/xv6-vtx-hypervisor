# HW2 answers

Fill in this file and hand it in with your code (your HW1 answers stay
in answers.md). Replace each
`(your answer)` with two or three sentences. Keep the question numbers
(part.question), and do not delete the questions.

- **Name:**
- **Machine and environment you tested on** (CADE lab machine with QEMU, or Bochs):
- **How far did you get?** (for example "the guest boots to a shell", or "stuck in part 4"):


## Part 1: The host side: what changes in xv6 itself

### Question 1.1

> What is the largest file, in blocks and in bytes, that xv6 can hold with the layout of Step 4? How many disk blocks does `xv6fs.img` (1,024,000 bytes) occupy as a file in `fs.img`, counting its pointer blocks? How large could a file be if `BSIZE` were 1024 bytes instead of 512?

**Answer:** (your answer)

### Question 1.2

> Suppose someone changes `GUESTMEM` in `runxv6.c` to 16 MB and forgets the Makefile. What happens when the guest boots, at about which guest-physical address, and why? What happens in the opposite case (`GUESTMEM` 64 MB, `PHYSTOP` still 32 MB)?

**Answer:** (your answer)

### Question 1.3

> `runxv6` must pass your keystrokes to the guest while it keeps the guest running. Explain why it needs *both* `fdready()` and `consraw()`: what would go wrong with `fdready()` alone (console still in line mode), and what would go wrong with `consraw()` alone (no `fdready()`)?

**Answer:** (your answer)

### Question 1.4

> Why did we turn `addrs[12]` into a doubly-indirect block instead of adding a 14th address to the inode? Give one advantage the classic 11 direct + singly-indirect + doubly-indirect layout has over ours, and explain why the difference matters little in practice.

**Answer:** (your answer)

### Question 1.5

> The Makefile builds the guest kernel's objects in `guest/` instead of next to the host kernel's. Describe what could go wrong, step by step, if both kernels were built in the same directory, and which kernel would end up broken.

**Answer:** (your answer)


## Part 2: Reading and writing guest memory

### Question 2.1

> `gva2gpa()` tests bit 7 (`PS`) of the entry only at the PDPT and PD levels. What does bit 7 mean in a PML4 entry and in a page-table entry (SDM Tables 5-15 and 5-20)? Describe a concrete guest mapping that a walk testing `PS` at every level would translate wrongly, and what it would return.

**Answer:** (your answer)

### Question 2.2

> `gva2gpa()` reads every page-table entry through `gpa2kva()`, that is, through the EPT. Why can it not use `P2V()` on the table address, as xv6's own `walkpglevel()` (in `vm.c`) does for its page tables? Which memory would `P2V(table)` read? How does the CPU solve the same problem when the guest itself accesses memory (SDM 31.3.1)?

**Answer:** (your answer)

### Question 2.3

> xv6's IDE driver reads a sector with `insl(0x1f0, b->data, BSIZE/4)`, 512 bytes into `b->data`. Suppose `b->data` starts 256 bytes before the end of a guest page. Describe, iteration by iteration, what `vmmem()` does with this request. Why would one translation followed by one 512-byte `memmove()` be wrong, even though the guest kernel maps its data to consecutive guest-physical pages?

**Answer:** (your answer)

### Question 2.4

> `vmrun()` saves `GUEST_CR3` into `vm->cr3` after every exit. Why can `vmmem()` not simply call `vmread(GUEST_CR3)` when it needs the value? And why is the saved value never out of date when `vmmem()` uses it?

**Answer:** (your answer)


## Part 3: Booting xv6 in a VM

### Question 3.1

> Before Step 1, the exception bitmap was all ones. Suppose you had
> kept it that way for the xv6 guest. What would happen the first
> time a user program in the guest dereferences a null pointer, and
> what would the hypervisor have to do to make the guest behave
> exactly as on a real machine? Why does `INT $64` not raise the
> same issue?

**Answer:** (your answer)

### Question 3.2

> `CPUID` causes a VM exit unconditionally, while instructions such
> as `HLT` exit only if the VMCS asks for it (SDM 28.1.2 and 28.1.3).
> Why would a CPU designer make `CPUID` always exit? Use what you
> did in Step 3 as an example, and give one more reason a
> hypervisor might want to change what the guest sees.

**Answer:** (your answer)

### Question 3.3

> `loadkernel()` copies each segment to `p_paddr`. What would happen,
> exactly, if it used `p_vaddr` instead? And what would go wrong if
> `vmcreate()` did not zero guest memory, given that `loadkernel()`
> copies only `filesz` bytes of each segment?

**Answer:** (your answer)

### Question 3.4

> Your MP table describes one processor with APIC id 0. (a) If its
> APIC id were 1, where exactly would the guest stop, and why would
> you see no message? (b) Suppose instead you listed two processors,
> with ids 0 and 1. Read `startothers()` in `main.c` and
> `lapicstartap()` in `lapic.c`: what would the guest do, and why
> can our zeroed APIC page never make it work?

**Answer:** (your answer)


## Part 4: Interrupts

### Question 4.1

> `nextpending()` delivers the highest pending vector first. Which
> rule of a local APIC does this imitate (SDM 13.8.3), and what does a
> real local APIC also take into account that we ignore? With the timer
> (32) and the disk (46) both pending, which goes first, and what happens
> to a second timer tick that arrives before the guest has taken the
> first?

**Answer:** (your answer)

### Question 4.2

> Rule 1 of `vmxinject()` re-injects an event whose delivery was
> interrupted by a VM exit. For each of (a) a timer interrupt we injected,
> (b) a page fault raised by a guest instruction, (c) an `int $64` system
> call: is the event lost if we do not re-inject it, and why? For (c),
> what goes wrong if we re-inject it but forget the instruction length?

**Answer:** (your answer)

### Question 4.3

> Why must `vmrun()` return to user space once per host tick?
> Describe what the guest would do, and what you would see at the
> terminal, if it returned only on I/O and HLT exits while the guest sits
> at an idle shell prompt and you type a key. Why not return on every
> external-interrupt exit instead?

**Answer:** (your answer)

### Question 4.4

> Why does `vmxinject()` not inject an external interrupt when
> the guest has `RFLAGS.IF = 0`? Give two reasons: one from the VM-entry
> checks, one from what xv6 is doing when IF is 0. Why is waiting for the
> next exit of any kind not good enough, so that we need interrupt-window
> exiting?

**Answer:** (your answer)

### Question 4.5

> Our guest has one CPU. List what the hypervisor and `runxv6`
> would need in order to give it two. Think about the local APIC ID each
> CPU reads from the same guest-physical page, how xv6 starts the second
> CPU (`lapicstartap()`), where each interrupt should go, the VMCS, and
> the fact that an xv6 process has a single thread.

**Answer:** (your answer)


## Part 5: Devices and string I/O

### Question 5.1

> With our design, reading one 512-byte sector costs one VM exit for
> the data instead of 128. Explain why the kernel can advance RIP past a
> `rep insl` that has not executed a single iteration. What would the kernel
> and `runxv6` have to do differently if `runxv6` moved only one dword per
> exit?

**Answer:** (your answer)

### Question 5.2

> Suppose a guest executed `std` before `rep insl` (DF = 1). What
> would the CPU do with RDI, and which bytes of memory would it fill? What
> would our `idestring()` do instead? Sketch the change that would handle
> it (which register tells you DF, and how would the address and the RDI
> update change?).

**Answer:** (your answer)

### Question 5.3

> The guest's keys arrive as serial input, never as PS/2 keyboard
> interrupts. Why is the serial port the natural choice here? What would
> `runxv6` need in order to emulate the keyboard instead (which ports,
> which vector, and what would it have to do to each character)?

**Answer:** (your answer)

### Question 5.4

> Nobody displays the guest's VGA memory at 0xB8000, and yet the
> guest's output is complete. Why? And why, even so, do the CRT controller's
> two ports have to be emulated?

**Answer:** (your answer)

### Question 5.5

> `struct vmexit` has no field that names a device: there is nothing
> like "disk read" or "serial write". Where does the knowledge that port
> 0x1F0 is a disk live, and why is it better there than in the kernel? Name
> the matching pieces in KVM and QEMU.

**Answer:** (your answer)
