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

**Answer:** There are 12 blocks fron inode + 128^2 from doubly-linked indirect blocks with 512/4 32-bit (4-byte) entries each. This totals 16396 blocks for 8394752 bytes (just over 8MB) total. If BS=1024, we get 12+(1024/4)^2 = 65548 blocks = 67121152 bytes > 64MB.

### Question 1.2

> Suppose someone changes `GUESTMEM` in `runxv6.c` to 16 MB and forgets the Makefile. What happens when the guest boots, at about which guest-physical address, and why? What happens in the opposite case (`GUESTMEM` 64 MB, `PHYSTOP` still 32 MB)?

**Answer:** If `GUESTMEM < PHYSTOP`, the EPT would not map the upper pages of guest physical memory, and the guest would error during boot while calling `kfree` for each of it's guest physical pages. Assuming the guest builds the free liost sequentially, the first address to error would be at `GUESTMEM`. Vice versa, if `GUESTMEM > PHYSTOP`, the host gives the guest extra pages but the guest is hard-coded not to use them, so the guest runs correctly and the host wastes some memory.

### Question 1.3

> `runxv6` must pass your keystrokes to the guest while it keeps the guest running. Explain why it needs *both* `fdready()` and `consraw()`: what would go wrong with `fdready()` alone (console still in line mode), and what would go wrong with `consraw()` alone (no `fdready()`)?

**Answer:**
- Withoud `consraw()`: The host would print back charactes as they are typed, block VM entry until a newline, and capture excape sequences meant for the guest.
- Without `fdready()`: The console could not know if a read operation would block and non-blocking input would not be possible, again blocking VM entry on console input and open pipes.

### Question 1.4

> Why did we turn `addrs[12]` into a doubly-indirect block instead of adding a 14th address to the inode? Give one advantage the classic 11 direct + singly-indirect + doubly-indirect layout has over ours, and explain why the difference matters little in practice.

**Answer:** 64-byte inodes fit evenly in a block, but another entry would change the inode size to 68 and either waste space in each inode block or cause alignment issues with inodes spanning multiple blocks. The classic layout keeps a lower level of indirection for small (<=69.5KB) files, which reduces disk reads and improves cache performance, but adds some code complexity to determine the degree of indirection. The difference is not significant for sequential reads, since indirect blocks in the upper level are read 128 times in a row and stay in the cache.

### Question 1.5

> The Makefile builds the guest kernel's objects in `guest/` instead of next to the host kernel's. Describe what could go wrong, step by step, if both kernels were built in the same directory, and which kernel would end up broken.

**Answer:** `make` does not distinguish between files with the same filename but different target. Building the guest and the host would use cached build files (based on their `mtime`) from one target for the other, and could break the guest if a cached file is compiled with the host's `PHYSTOP` that does not match the host's `GUESTMEM` (see 1.2).

## Part 2: Reading and writing guest memory

### Question 2.1

> `gva2gpa()` tests bit 7 (`PS`) of the entry only at the PDPT and PD levels. What does bit 7 mean in a PML4 entry and in a page-table entry (SDM Tables 5-15 and 5-20)? Describe a concrete guest mapping that a walk testing `PS` at every level would translate wrongly, and what it would return.

**Answer:** The bit is reserved (must be 0) for a PDPTE and determines the memory type for a PTE. Testing PS _should_ be safe but unnecessary for a PDPTE, since the branch would never be taken. In my implementation, the test for the PTE would also be safe, since in one case the flag marks the current table as a leaf table and jumps to the physical address calculation and in the other case the bottom-level of the paging structure falls through to the physical address calculation.

### Question 2.2

> `gva2gpa()` reads every page-table entry through `gpa2kva()`, that is, through the EPT. Why can it not use `P2V()` on the table address, as xv6's own `walkpglevel()` (in `vm.c`) does for its page tables? Which memory would `P2V(table)` read? How does the CPU solve the same problem when the guest itself accesses memory (SDM 31.3.1)?

**Answer:** The "physical" addresses are the _guest's_ physical, not the host's. Since the guest's CR3 is no longer active, a memory access using P2V would use the _host's_ paging structure in hardware, so one more level of abstraction is necessary.

VM entry sets the CPU's actual CR3 from the guest's stored value in the VMCS, so the hardware virtual address translation happens appropriately for the guest not the host. 

### Question 2.3

> xv6's IDE driver reads a sector with `insl(0x1f0, b->data, BSIZE/4)`, 512 bytes into `b->data`. Suppose `b->data` starts 256 bytes before the end of a guest page. Describe, iteration by iteration, what `vmmem()` does with this request. Why would one translation followed by one 512-byte `memmove()` be wrong, even though the guest kernel maps its data to consecutive guest-physical pages?

**Answer:** `memmove` must respect the real physical memory layout, which is different from host virtual, guest physical, or guest virtual. The page size is always guaranteed to be some multiple of 4KB regardless of implemetation (and our implementation does uses exactly in the EPT), regardless of how "guest physical" memory is presented, so the buffer is consumed in 4KB-aligned chunks. While the buffer is not empty, the relevant guest address in translated to host virtual, and the number `n` of of bytes to consume from the buffer is calculated to stay within a 4KB page before movin `n` bytes from the buffer to or from the guest page.

### Question 2.4

> `vmrun()` saves `GUEST_CR3` into `vm->cr3` after every exit. Why can `vmmem()` not simply call `vmread(GUEST_CR3)` when it needs the value? And why is the saved value never out of date when `vmmem()` uses it?

**Answer:** The VMCS is not accessible after a VMCLEAR - so the value must be copied _before_ the VM is cleared. Because the only entry/exit to the VM is in the VMENTER/VMCLEAR loop, `vm->cr3` is guaranteed to be up-to-date whenever the VMM resumes and not change until the VM resumes.


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
