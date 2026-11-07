/*
 * runxv6: run xv6 itself as a guest (HW2).
 *
 *   runxv6 xv6kernel xv6fs.img
 *
 * This is the user-level half of the hypervisor for an xv6 guest, the
 * part that QEMU plays on top of KVM. The kernel (vmx.c) runs the guest
 * and handles the exits it must; everything that looks like hardware to
 * the guest is emulated here:
 *
 *  - "firmware": the guest kernel is an ELF file that we load straight
 *    into guest memory, as a multiboot loader would, and start in 32-bit
 *    protected mode at its entry point. We also write the small tables a
 *    BIOS would leave behind: the BIOS data area and an MP configuration
 *    table that describes one CPU and one I/O APIC.
 *  - the serial port COM1 (the guest's console): output goes to our
 *    standard output, and keys typed on our console go to the guest;
 *  - the IDE disk (the guest's disk 1, its file system), served from
 *    the image file xv6fs.img, which we keep in memory;
 *  - the interrupts these devices raise, and the timer interrupt, which
 *    we ask the kernel to inject with vminject().
 *
 * Type Ctrl-] to stop the guest. Writes to the guest's disk change only
 * the copy in memory, not xv6fs.img.
 *
 * How it fits together. The kernel exits to us on every IN, OUT, INS
 * and OUTS (the VMCS sets "unconditional I/O exiting", SDM 28.1.3), so
 * every device register the guest touches through a port ends up in
 * doio() below. The guest's local APIC and I/O APIC are not emulated:
 * vmcreate() backs their memory-mapped registers with plain zeroed
 * pages, which is enough for xv6 to initialize them. Interrupts bypass
 * them altogether: we name a vector with vminject(), and the kernel
 * delivers it through the guest's IDT on a later VM entry, as soon as
 * the guest has interrupts enabled (vmxinject() in vmx.c, SDM 29.6).
 *
 * The Makefile builds the guest kernel, xv6kernel, from the same sources
 * as the host kernel, but for a 32 MB machine (-DPHYSTOP=0x2000000)
 * with a smaller file system (-DFSSIZE=2000), built by mkfsguest.
 */

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "elf.h"      /* struct elfhdr, struct proghdr */
#include "mp.h"       /* the MP table layouts that the guest's mp.c reads */
#include "vmxapi.h"   /* vmcreate, vmrun, struct vmexit, VMREG_*, ... */

/*
 * Guest memory: must match PHYSTOP of the guest kernel (Makefile).
 * The guest's kinit1() and kinit2() hand every page up to PHYSTOP to
 * its page allocator. If PHYSTOP were larger than the memory we give
 * the VM, the guest would sooner or later touch a guest-physical page
 * the EPT does not map, and we would stop on an EPT violation. It must
 * also be at most VM_MAXMEM (64 MB), or vmcreate() fails.
 */
#define GUESTMEM (32*1024*1024)

/*
 * Interrupt vectors: xv6 programs IRQ n as vector 32 + n. T_IRQ0 is 32
 * in traps.h; lapicinit() puts T_IRQ0 + IRQ_TIMER in the local APIC's
 * timer register, and ioapicenable() routes IRQ n to vector T_IRQ0 + n.
 * Because we inject vectors directly, the APIC programming itself is
 * never consulted, but the vectors must be the ones the guest's trap()
 * dispatches on: timer (IRQ 0), COM1 (IRQ 4), and the primary IDE
 * channel (IRQ 14). When several are pending, the kernel delivers the
 * highest vector first, as a local APIC would (nextpending() in vmx.c).
 */
#define VEC_TIMER (32 + 0)
#define VEC_COM1  (32 + 4)
#define VEC_IDE   (32 + 14)

/*
 * Ctrl-] is ASCII GS (0x1D), familiar as telnet's escape key. The
 * guest has no use for it, so we can keep it for ourselves; every other
 * key, including Ctrl-C and Ctrl-P, reaches the guest.
 */
#define QUITKEY 0x1d   /* Ctrl-] */

/* The VM id that vmcreate() returned; every hypervisor call takes it. */
static int vm;

/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */

/*
 * Print an error and exit. Leave raw mode first: otherwise the shell we
 * return to would read from a console that neither echoes nor collects
 * whole lines.
 */
static void
die(char *msg)
{
  consraw(0);
  printf(2, "runxv6: %s\n", msg);
  exit();
}

/*
 * Read a guest register. The value is the one the kernel saved at the
 * last VM exit (vmgetreg() in vmx.c), which is what we want: we call
 * this only between vmrun() calls, while the guest is stopped.
 */
static uint64
getreg(int reg)
{
  uint64 v;

  vmgetreg(vm, reg, &v);
  return v;
}

/* Read a whole file into memory; return the buffer and its size. */
static char*
readfile(char *name, int *size)
{
  struct stat st;
  char *buf;
  int fd, n, off;

  /* fstat() gives the size, so one buffer can hold the whole file. */
  if((fd = open(name, O_RDONLY)) < 0 || fstat(fd, &st) < 0)
    return 0;
  if((buf = malloc(st.size)) == 0)
    return 0;
  /* read() may return fewer bytes than asked for, so loop. */
  for(off = 0; off < st.size; off += n)
    if((n = read(fd, buf + off, st.size - off)) <= 0)
      return 0;
  close(fd);
  *size = st.size;
  return buf;
}

/*
 * Give the guest the result of an IN of size bytes (as in runvm.c).
 *
 * I/O exits are "fault-like" (SDM 28.1): the IN did not execute, so the
 * guest's RAX still holds its old value, and it is up to us to store
 * what the device would have returned. The kernel has already moved the
 * guest's RIP past the IN (vmxhandle() in vmx.c), so when the guest
 * resumes it sees the IN complete with our value.
 */
static void
completein(int size, uint val)
{
  uint64 rax, mask;

  /*
   * IN EAX writes a 32-bit register, and in 64-bit mode every write to
   * a 32-bit register clears bits 63:32. val is a uint, so storing it
   * in RAX does exactly that.
   */
  if(size == 4){
    vmsetreg(vm, VMREG_RAX, val);
    return;
  }
  /*
   * IN AL and IN AX write only the low 8 or 16 bits and leave the rest
   * of RAX alone, so merge val into the old RAX. Guest code may keep a
   * live value in the upper bits; clobbering them would corrupt it.
   */
  mask = size == 1 ? 0xFF : 0xFFFF;
  rax = getreg(VMREG_RAX);
  vmsetreg(vm, VMREG_RAX, (rax & ~mask) | (val & mask));
}

/* ------------------------------------------------------------------
 * Loading the guest: the ELF kernel, the BIOS data area, the MP tables
 * ------------------------------------------------------------------ */

/*
 * Copy each loadable segment of the kernel to its physical load address
 * (p_paddr) in guest memory, and return the entry point. Guest memory
 * starts out zeroed, so the part of a segment beyond its file contents
 * (the bss) is already zero.
 *
 * An ELF file starts with a header (struct elfhdr) that gives the entry
 * point and the file offset (phoff) and count (phnum) of the program
 * headers. Each program header (struct proghdr) describes one segment:
 *
 *  - type: ELF_PROG_LOAD (PT_LOAD) for a segment to place in memory;
 *    others (notes, the GNU stack marker) only describe the file.
 *  - off, filesz: where the segment's bytes are in the file, and how
 *    many there are.
 *  - memsz: how much memory the segment occupies, at least filesz.
 *    The bytes from filesz to memsz are the bss, and must be zero.
 *  - vaddr: the virtual address the segment is linked to run at.
 *  - paddr: the physical address to load it at.
 *
 * For xv6 the two addresses differ. kernel.ld links the kernel at the
 * high virtual address 0x80000100000 (KERNLINK) but, with AT(0x100000),
 * asks to be loaded at physical 0x100000 (1 MB). The guest starts with
 * paging off, so it is physical addresses that count: entry.S runs at
 * the low address and builds page tables that map the high one onto it.
 */
static uint64
loadkernel(char *image, int size)
{
  struct elfhdr *elf = (struct elfhdr*)image;
  struct proghdr *ph;
  int i;

  /* Check that the header fits in the file before reading it. */
  if(size < sizeof(*elf) || elf->magic != ELF_MAGIC)
    die("the kernel is not an ELF file");
  for(i = 0; i < elf->phnum; i++){
    /* The program headers form an array at file offset phoff. */
    ph = (struct proghdr*)(image + elf->phoff + i * sizeof(*ph));
    /*
     * Skip what is not loadable, and segments with nothing in the file
     * (pure bss): their memory is zero already.
     */
    if(ph->type != ELF_PROG_LOAD || ph->filesz == 0)
      continue;
    /*
     * The file bytes must lie inside the file, and the whole segment,
     * bss included, inside guest memory: the guest would use all memsz
     * bytes of it.
     */
    if (ph->off + ph->filesz > size || ph->paddr + ph->memsz > GUESTMEM)
      die("a kernel segment does not fit");
    /*
     * Without VMMEM_VIRT, the address is guest-physical: copy filesz
     * bytes from the file into guest memory at paddr.
     */
    if(vmmem(vm, ph->paddr, image + ph->off, ph->filesz, VMMEM_WRITE) < 0)
      die("cannot write guest memory");
  }
  /*
   * entry.S sets _start = V2P_WO(entry), the physical address of
   * entry, so the entry point is valid with paging off, as RIP needs.
   */
  return elf->entry;
}

/*
 * The MP specification protects each table with a checksum byte chosen
 * so that all the bytes of the table add up to 0 (modulo 256); the
 * guest's mpsearch1() and mpconfig() check that with sum(). We call
 * this while the checksum field is still 0, so -sum is the right value.
 */
static uchar
checksum(uchar *p, int len)
{
  uchar sum = 0;

  while(len-- > 0)
    sum += *p++;
  return -sum;  /* the bytes, including this one, must add up to 0 */
}

/*
 * The tables a PC BIOS leaves for the operating system, as far as xv6
 * reads them (mp.c):
 *
 *  - the BIOS data area at 0x400: the size of base memory, 640 KB, at
 *    0x413. xv6 looks for the MP floating pointer in the last KB of base
 *    memory, so this must be set.
 *  - an MP floating pointer structure (signature "_MP_") at 0xF0000, in
 *    the BIOS ROM area that xv6 also searches. It points to:
 *  - an MP configuration table ("PCMP") listing one processor (APIC id
 *    0, the boot processor) and one I/O APIC (id 0), with the local APIC
 *    at VM_LAPIC_ADDR.
 *
 * See the Intel MultiProcessor Specification 1.4, chapter 4.
 *
 * The spec lets the floating pointer be in one of three places, and
 * mpsearch() looks in them in order: the first KB of the extended BIOS
 * data area (EBDA), whose segment is the 16-bit word at 0x40E; the last
 * KB of base memory, whose size in KB is the word at 0x413; and the
 * BIOS ROM, 0xF0000 to 0xFFFFF. Our word at 0x40E stays 0 (no EBDA), so
 * xv6 searches 0x9FC00-0x9FFFF, finds only zeros, and moves on to the
 * ROM area. Were 0x413 also 0, xv6 would compute 0 - 1024 as the start
 * of base memory's last KB and search a bogus address.
 *
 * Before any of this, mpinit() calls acpiinit(), which searches the
 * same areas for an ACPI "RSD PTR " signature. There is none, so it
 * returns 0 and xv6 falls back to the MP tables.
 *
 * The guest's memory is plain RAM from 0 to GUESTMEM, with no ROM and
 * no holes, so 0xF0000 is ordinary memory that we can simply write.
 * xv6 never gives memory below its kernel (1 MB) to its page allocator,
 * so the tables stay intact.
 */
static void
mptables(void)
{
  // The first thing to write is the base memory size: 640 (KB), at 0x413
  uint64 size = 640;
  if (vmmem(vm, 0x413, &size, sizeof(uint64), VMMEM_WRITE) < 0)
    die("vmmem failed writing base memory size");

  // The floating pointer (struct mp, 16 bytes, MP spec 4.1) at 0xF0000. Signature _MP_; physaddr,
  // the physical address of the configuration table; length 1 (in 16-byte units); specrev 4
  // (spec 1.4); type 0, meaning “a configuration table is present” (a nonzero value would select
  // one of the spec’s default configurations, which xv6 rejects); and imcrp 0, meaning no IMCR, so
  // mpinit() leaves ports 0x22 and 0x23 alone.
  struct mp pointer = {
      .signature = "_MP_",    // what mpsearch() scans for, 16 bytes at a time
      .physaddr = 0x000F0010, // the physical address of the configuration table;
      .length = 1,            // in 16-byte units;
      .specrev = 4,           // 4 (spec 1.4);
      .checksum = 0,          // xor of all other bytes in the struct
      .type = 0,              // 0, meaning “a configuration table is present”;
      .imcrp = 0,             // 0, meaning no IMCR, so mpinit() leaves ports 0x22 and 0x23 alone.
      .reserved = ""
  };
  pointer.checksum = checksum((uchar *)&pointer, sizeof(pointer));
  if (vmmem(vm, 0x000F0000, &pointer, sizeof(pointer), VMMEM_WRITE) < 0)
    die("vmmem failed writing mp");

  struct {
    // The configuration table header (struct mpconf, 44 bytes, MP spec 4.2) at 0xF0010, right after
    // the floating pointer. Signature PCMP; length, the size in bytes of the header and all
    // entries; version 4 (mpconfig() accepts 1 or 4); entry, the number of entries; lapicaddr, the
    // local APIC address, VM_LAPIC_ADDR (xv6 sets its lapic pointer from it). product is free text.
    struct mpconf header;
    // Directly after the header, the entries: one processor entry (struct mpproc, 20 bytes, type
    // MPPROC, MP spec 4.3.1), enabled (flags bit 0) and the bootstrap processor (MPBOOT); and one
    // I/O APIC entry (struct mpioapic, 8 bytes, type MPIOAPIC, MP spec 4.3.3), enabled, at
    // VM_IOAPIC_ADDR.
    struct mpproc proc;
    struct mpioapic apic;
  } conf = {
      .header =
          {
              .signature = "PCMP",         // "PCMP"
              .version = 4,                // [14]
              .product = "XV6 VMX mpconf", // product id
              .entry = 2,                  // entry count
              .lapicaddr = VM_LAPIC_ADDR,  // address of local APIC
          },
      .proc =
          {
              .type = MPPROC,
              .flags = 0x01 | 0x02, // EN | BP
          },
      .apic = {
          .type = MPIOAPIC,      // entry type (2)
          .addr = VM_IOAPIC_ADDR // I/O APIC address
      }
  };
  conf.header.length = sizeof(conf);
  conf.header.checksum = checksum((uchar *)&conf, sizeof(conf));
  if (vmmem(vm, pointer.physaddr, &conf, conf.header.length, VMMEM_WRITE) < 0)
    die("vmmem failed writing mpconf");
}

/* ------------------------------------------------------------------
 * COM1: a 16550 UART, as far as xv6's uart.c uses it
 * ------------------------------------------------------------------ */

/*
 * COM1's eight registers are at I/O ports 0x3F8-0x3FF; IRQ 4 is its
 * interrupt line. The register layout and bits below follow the
 * National Semiconductor (now TI) PC16550D datasheet.
 */
#define COM1 0x3f8

/*
 * The registers that simply hold what the guest writes: interrupt
 * enable (IER), line control (LCR), modem control (MCR), scratch (SCR),
 * and the two bytes of the baud-rate divisor (DLL low, DLM high).
 */
static uchar ier, lcr, mcr, scr, dll, dlm;
/*
 * The receiver: characters typed on our console, oldest first. A real
 * 16550 holds one character (or 16 with its FIFO on); a larger queue
 * keeps a fast typist or a paste from losing keys. rxr and rxw only
 * grow, and wrap around harmlessly: rxw - rxr is always the number of
 * characters queued, and "% sizeof(rxbuf)" turns either into an index.
 */
static char rxbuf[256];          /* typed characters not yet read */
static uint rxr, rxw;            /* read and write positions */

/* Is the receiver empty? Its opposite is the LSR's data-ready bit. */
static int
rxempty(void)
{
  return rxr == rxw;
}

/*
 * A character typed on our console: queue it and interrupt the guest.
 *
 * When the queue is full the character is dropped; a real UART would
 * also drop it, and set the overrun bit (LSR bit 1), which we do not
 * model.
 *
 * A 16550 raises an interrupt only for the sources enabled in the IER.
 * Bit 0 (ERBFI) enables "received data available", which uartinit()
 * sets. Before that, characters are only queued; the write that sets
 * bit 0 raises the interrupt (see com1()).
 *
 * One interrupt per character is more than xv6 needs: uartintr() calls
 * consoleintr(uartgetc), which keeps reading until the LSR says no data
 * is left. An interrupt that finds the queue already drained is
 * harmless. Interrupts injected before the guest takes the first one
 * merge, since the kernel keeps one pending bit per vector.
 */
static void
rxpush(char c)
{
  if(rxw - rxr < sizeof(rxbuf))
    rxbuf[rxw++ % sizeof(rxbuf)] = c;
  if(ier & 0x01)
    vminject(vm, VEC_COM1);
}

/*
 * Register offsets: 0 data (transmit or receive), 1 interrupt enable,
 * 2 interrupt identification, 3 line control, 4 modem control, 5 line
 * status, 6 modem status, 7 scratch. With bit 7 of the line control
 * register set (DLAB), offsets 0 and 1 are the baud-rate divisor.
 *
 * In more detail, as uart.c uses them:
 *
 *  0  read: receiver buffer (RBR), the next received character.
 *     write: transmitter holding register (THR), a character to send.
 *     DLAB = 1: divisor latch low byte (DLL).
 *  1  interrupt enable (IER): bit 0 received data available, bit 1
 *     transmitter empty, bit 2 line status, bit 3 modem status. xv6
 *     sets only bit 0. DLAB = 1: divisor latch high byte (DLM).
 *  2  read: interrupt identification (IIR). Bit 0 is 0 when an
 *     interrupt is pending (it is active low), and bits 3:1 say which
 *     one; bits 7:6 say whether the FIFOs are on.
 *     write: FIFO control (FCR). uartinit() writes 0 (FIFOs off).
 *  3  line control (LCR): bits 1:0 word length (3 = 8 bits), bit 2
 *     stop bits, bits 5:3 parity, bit 6 break, bit 7 DLAB.
 *  4  modem control (MCR): bit 0 DTR, bit 1 RTS, bit 2 OUT1, bit 3
 *     OUT2, bit 4 loopback. On a PC, OUT2 connects the UART's interrupt
 *     to the interrupt controller; uartinit() sets it.
 *  5  line status (LSR): bit 0 data ready (DR), bits 1-4 overrun,
 *     parity, framing and break errors, bit 5 transmitter holding
 *     register empty (THRE), bit 6 transmitter empty (TEMT).
 *  6  modem status (MSR): the modem input lines.
 *  7  scratch (SCR): a byte of storage, used to probe for a UART.
 *
 * Not modeled: the FIFOs, the modem lines, loopback, line errors, and
 * timing (the divisor is kept but never used: a character goes out the
 * moment it is written). Like QEMU, we do not gate the interrupt on
 * MCR.OUT2.
 */
static void
com1(struct vmexit *e)
{
  /* Which register; and whether offsets 0 and 1 mean the divisor. */
  int off = e->io.port - COM1, dlab = lcr & 0x80;
  uint v = 0;
  char c;

  /* OUT: the guest writes a register. */
  if(!e->io.in){
    switch(off){
    case 0:
      if(dlab){
        /* Divisor low byte: uartinit() writes 115200/9600 = 12. */
        dll = e->io.data;
      } else {
        /*
         * THR: a character to transmit (uartputc()). Send it to our
         * standard output at once, so the transmitter is empty again
         * by the time the guest next looks at the LSR.
         */
        c = e->io.data;
        write(1, &c, 1);
      }
      break;
    case 1:
      if(dlab){
        dlm = e->io.data;
      } else {
        /*
         * IER. If the guest has just enabled receive interrupts while
         * characters are waiting, the interrupt condition already
         * holds and a 16550 would raise the interrupt right away. Do
         * the same, or keys typed early would sit unnoticed until the
         * next key arrives.
         */
        ier = e->io.data;
        if((ier & 0x01) && !rxempty())
          vminject(vm, VEC_COM1);
      }
      break;
    /*
     * LCR: kept only so that it reads back and so that we know DLAB.
     * The word length, parity and stop bits do not matter, since our
     * "line" is a terminal, not a wire.
     */
    case 3: lcr = e->io.data; break;
    /* MCR: kept so that it reads back (see above about OUT2). */
    case 4: mcr = e->io.data; break;
    case 7: scr = e->io.data; break;
    /*
     * Offset 2 (FCR) is ignored: xv6 only turns the FIFOs off, and our
     * UART has none. Offsets 5 and 6 are status registers.
     */
    }
    return;
  }
  /* IN: the guest reads a register. */
  switch(off){
  case 0:
    if(dlab)
      v = dll;
    else if(!rxempty())
      /*
       * RBR: hand over the oldest character and remove it from the
       * queue, as reading RBR does on a 16550 (DR clears when nothing
       * is left). uartgetc() reads it after checking DR; uartinit()
       * reads it once to discard stale input and then gets 0.
       */
      v = (uchar)rxbuf[rxr++ % sizeof(rxbuf)];
    break;
  case 1: v = dlab ? dlm : ier; break;
  /*
   * IIR: 0x04 is bit 0 clear (an interrupt is pending) with bits 3:1 =
   * 010, "received data available", the only source we model. 0x01 is
   * "no interrupt pending". Bits 7:6 are 0: no FIFOs. xv6 reads the IIR
   * only in uartinit(), to clear old conditions, and ignores the value.
   */
  case 2: v = ((ier & 0x01) && !rxempty()) ? 0x04 : 0x01; break;
  case 3: v = lcr; break;
  case 4: v = mcr; break;
  case 5:
    /*
     * Transmitter always empty (0x60); bit 0: a character is waiting.
     *
     * 0x60 is THRE (bit 5) and TEMT (bit 6). uartputc() spins until
     * THRE is set; without it, every character would cost the guest a
     * long busy-wait. uartgetc() reads RBR only when DR (bit 0) is set.
     * uartinit() also reads the LSR to see whether a UART exists: an
     * empty port would read 0xFF, which 0x60 or 0x61 never is.
     */
    v = 0x60 | (rxempty() ? 0 : 0x01);
    break;
  case 7: v = scr; break;
  /* Offset 6 (MSR) reads 0: no modem lines are active. */
  }
  completein(e->io.size, v);
}

/* ------------------------------------------------------------------
 * The IDE disk, as far as xv6's ide.c uses it
 * ------------------------------------------------------------------ */

/*
 * The primary ATA (IDE) channel, IRQ 14. Its command block registers
 * are at ports 0x1F0-0x1F7, by offset:
 *
 *  0  data: the sector contents, moved through this one port a word
 *     at a time (16 bits on the ATA bus; xv6's insl() and outsl() use
 *     32-bit accesses, which PC IDE controllers and QEMU accept).
 *  1  read: error; write: features. xv6 uses neither.
 *  2  sector count: how many sectors to transfer; 0 means 256.
 *  3  LBA bits 7:0   (called "sector number" in the old CHS scheme)
 *  4  LBA bits 15:8  ("cylinder low")
 *  5  LBA bits 23:16 ("cylinder high")
 *  6  drive/head: bits 7 and 5 are obsolete and set to 1, bit 6
 *     selects LBA (rather than CHS) addressing, bit 4 (DEV) selects
 *     drive 0 or 1, and bits 3:0 are LBA bits 27:24. xv6 writes
 *     0xE0 | dev << 4 | LBA bits 27:24.
 *  7  read: status; write: command. Writing the command register
 *     starts the command, so the other registers are written first.
 *
 * The control block register at 0x3F6 reads as the alternate status
 * and is written as the device control register.
 *
 * The PIO protocols for the commands xv6 uses (ATA/ATAPI, "PIO
 * data-in" and "PIO data-out"):
 *
 *  - read: the drive sets BSY, fetches the sector into its buffer,
 *    then clears BSY, sets DRQ and raises its interrupt. The host
 *    reads 512 bytes from the data port; after the last one the drive
 *    clears DRQ. (For several sectors, this repeats per sector, or per
 *    block of sectors with READ MULTIPLE.)
 *  - write: the drive sets DRQ without interrupting. The host writes
 *    512 bytes to the data port; the drive then sets BSY, writes the
 *    sector to the medium, clears BSY and raises its interrupt.
 *
 * Here the medium is memory, so nothing takes time: the drive is never
 * busy, and each step happens in the same exit as the guest's access.
 */

/*
 * Status register bits. BSY: the drive is working and the other bits
 * are not valid. DRDY: the drive is ready to accept a command. DRQ:
 * the drive is ready to move data through the data port. Bit 0 (ERR,
 * written as 0x01 below): the last command failed. Bit 5, device fault
 * (DF), is never set here.
 */
#define IDE_BSY  0x80
#define IDE_DRDY 0x40
#define IDE_DRQ  0x08

static char *disk;               /* the guest's disk 1, in memory */
static int disksize;
/*
 * The task-file registers: sector count, the three LBA bytes, and
 * drive/head, as last written. The status starts out as DRDY: ideinit()
 * first spins in idewait() until the status reads BSY = 0 and DRDY = 1,
 * then decides that disk 1 is present if the status is nonzero after
 * selecting it. The status here does not depend on the drive selected.
 */
static uchar idecount, idelba[3], idehead, idestatus = IDE_DRDY;
/*
 * The drive's sector buffer. idestart() panics if a block spans more
 * than 7 sectors, so 8 are enough for any BSIZE xv6 can use.
 */
static char idebuf[8 * 512];     /* sectors being transferred */
static int idepos, idelen;       /* position and size of the transfer */
static int idewriting;           /* the transfer is a write */
static uint idesector;           /* first sector of a write */

/* The guest wrote the command register. */
static void
idecommand(uint cmd)
{
  uint sector, n;

  /*
   * LBA28: reassemble the 28-bit sector number from the bytes that
   * idestart() split it into: bits 7:0, 15:8 and 23:16 from ports
   * 0x1F3-0x1F5, bits 27:24 from the low four bits of drive/head.
   */
  sector = idelba[0] | idelba[1] << 8 | idelba[2] << 16 |
           (idehead & 0x0F) << 24;
  /* A sector count of 0 means 256 sectors. */
  n = idecount ? idecount : 256;
  /*
   * Fail a command for drive 0 (bit 4 of drive/head clear), for more
   * sectors than our buffer holds, or for sectors past the end of the
   * disk image. We assume LBA addressing and do not check bit 6.
   */
  if(!(idehead & 0x10) || n > sizeof(idebuf) / 512 ||
     (sector + n) * 512 > disksize){
    /* Only disk 1 exists here; xv6 never uses disk 0 after boot. */
    /*
     * DRDY with ERR, and no DRQ: there is no data phase. A real drive
     * would also interrupt; we do not, so a guest that waited for the
     * interrupt would wait forever. xv6 does not get here: its root
     * file system is on disk 1 (ROOTDEV), and idestart() panics on a
     * block number beyond FSSIZE, the size of xv6fs.img.
     */
    idestatus = IDE_DRDY | 0x01;  /* ERR */
    return;
  }
  /* A new transfer of n sectors starts at the beginning of idebuf. */
  idepos = 0;
  idelen = n * 512;
  switch(cmd){
  /*
   * xv6's idestart() sends READ SECTORS when a block is one sector
   * and READ MULTIPLE when it is several; in this tree BSIZE is 512,
   * so it is always READ SECTORS. On a real drive READ SECTORS has
   * one data phase and one interrupt per sector, while READ MULTIPLE
   * has one per group of sectors (the group size is set by the SET
   * MULTIPLE MODE command). xv6's ideintr() reads the whole block with
   * one insl() after one interrupt, so we treat both commands alike:
   * all n sectors are ready at once, behind a single interrupt.
   */
  case 0x20:   /* READ SECTORS */
  case 0xC4:   /* READ MULTIPLE */
    /* "Read the medium" into the drive's buffer. */
    memmove(idebuf, disk + sector * 512, idelen);
    idewriting = 0;
    /*
     * BSY clear, DRDY and DRQ set: the data can be read now.
     * ideintr() calls idewait(1), which wants BSY = 0, DRDY = 1, and
     * neither ERR nor DF, before it reads the data with insl().
     */
    idestatus = IDE_DRDY | IDE_DRQ;
    /*
     * Raise IRQ 14. Without it, ideintr() never runs, and the process
     * that called iderw() sleeps forever. iderw() holds idelock, so the
     * guest has interrupts off right now; the kernel keeps the
     * interrupt pending and delivers it once the guest sets IF again.
     */
    vminject(vm, VEC_IDE);  /* the data is ready */
    break;
  /*
   * The writes, chosen by idestart() like the reads. No interrupt
   * yet: the drive waits for the data (DRQ set). idestart() does not
   * even wait for DRQ; it calls outsl() right after the command, which
   * works because DRQ is set at once here.
   */
  case 0x30:   /* WRITE SECTORS */
  case 0xC5:   /* WRITE MULTIPLE */
    idewriting = 1;
    /* Where the data goes once it has all arrived (idedone()). */
    idesector = sector;
    idestatus = IDE_DRDY | IDE_DRQ;  /* send the data */
    break;
  /*
   * Any other command (for example IDENTIFY DEVICE, 0xEC, which xv6
   * does not send): fail it with ERR, as a drive does with a command
   * it does not implement.
   */
  default:
    idestatus = IDE_DRDY | 0x01;
  }
}

/* n more bytes of the transfer went through the data port. */
static void
idedone(int n)
{
  idepos += n;
  if(idepos < idelen)
    return;
  /* The last byte moved: clear DRQ, as no more data is expected. */
  idestatus = IDE_DRDY;
  /*
   * A read is complete now; its interrupt came at the start. A write
   * has all its data: "write it to the medium" and raise the
   * completion interrupt. ideintr() sees B_DIRTY, skips reading, and
   * wakes the process sleeping in iderw().
   */
  if(idewriting){
    memmove(disk + idesector * 512, idebuf, idelen);
    vminject(vm, VEC_IDE);  /* the write is done */
  }
}

/*
 * The data port with INS or OUTS (xv6's insl and outsl): move all the
 * count * size bytes between the guest's buffer, at guest-virtual
 * address e->io.addr, and idebuf, then advance RDI (INS) or RSI (OUTS)
 * past the buffer and set RCX to 0, as the CPU would have at the end of
 * the REP instruction.
 *
 * insl() and outsl() in x86.h are "cld; rep insl" and "cld; rep outsl"
 * with RCX = BSIZE/4: 128 dwords, 512 bytes. Like other I/O exits, the
 * exit happens before the instruction does anything (SDM 28.1), so RCX
 * still holds the whole count and RDI or RSI the start of the buffer.
 * The kernel records the guest-linear address of the memory operand,
 * ES base + RDI for INS or DS base + RSI for OUTS (SDM 30.2.1); in
 * 64-bit mode those segment bases are 0, so this is just RDI or RSI.
 * The kernel then moves RIP past the instruction as a whole, so we must
 * do all its iterations now, leaving memory and registers as the CPU
 * would. CLD has cleared the direction flag, so addresses go up: that
 * is why we add n.
 */
static void
idestring(struct vmexit *e)
{
  /* HW2 Part 5: write this function; see HW2.md. */
  die("idestring not written");
}

/* Ports 0x1F0-0x1F7, and 0x3F6 (control and alternate status). */
static void
ide(struct vmexit *e)
{
  int off = e->io.port - 0x1F0;
  uint v = 0;

  if(e->io.port == 0x3F6){
    /*
     * Alternate status: the status register's bits. On a real drive,
     * reading the status register (0x1F7) also acknowledges a pending
     * interrupt and reading this one does not; we keep no interrupt
     * line state, so the two read the same.
     */
    if(e->io.in)
      completein(e->io.size, idestatus);
    /*
     * Writes are to device control: bit 1 (nIEN) disables the drive's
     * interrupt, bit 2 (SRST) resets the drive. idestart() writes 0,
     * interrupts on and no reset, which is how we always behave.
     */
    return;
  }
  if(e->io.string){
    /* xv6 moves data only through the data port. */
    if(off != 0)
      die("string I/O to an IDE port other than data");
    idestring(e);
    return;
  }
  /* OUT: the guest writes a register. */
  if(!e->io.in){
    switch(off){
    /*
     * A single OUTW or OUTL to the data port. xv6 uses outsl(), but a
     * driver may move data one word at a time. The data is in the low
     * size bytes of io.data, least significant byte first, which is
     * the order it appears in memory. A drive ignores data writes when
     * it expects none (DRQ clear), and so do we.
     */
    case 0:   /* one word or dword of data */
      if(idewriting && idepos < idelen){
        memmove(idebuf + idepos, &e->io.data, e->io.size);
        idedone(e->io.size);
      }
      break;
    case 2: idecount = e->io.data; break;
    /* 0x1F3-0x1F5: LBA bits 7:0, 15:8, 23:16. */
    case 3: case 4: case 5: idelba[off - 3] = e->io.data; break;
    case 6: idehead = e->io.data; break;
    /* The command register: run the command now. */
    case 7: idecommand(e->io.data); break;
    /* Offset 1 (features) is ignored: xv6 does not write it. */
    }
    return;
  }
  /* IN: the guest reads a register. */
  switch(off){
  case 0:
    /* A single INW or INL: the next word or dword of a read. */
    if(!idewriting && idepos < idelen){
      memmove(&v, idebuf + idepos, e->io.size);
      idedone(e->io.size);
    }
    break;
  /* The task-file registers read back what was written. */
  case 2: v = idecount; break;
  case 3: case 4: case 5: v = idelba[off - 3]; break;
  case 6: v = idehead; break;
  /* Status: what idewait() polls. */
  case 7: v = idestatus; break;
  /* Offset 1 (error) reads 0; xv6 looks only at ERR in the status. */
  }
  completein(e->io.size, v);
}

/* ------------------------------------------------------------------
 * The CGA display's CRT controller (ports 0x3D4, 0x3D5)
 * ------------------------------------------------------------------ */

/*
 * xv6's console also writes to the CGA text screen, which is ordinary
 * memory at 0xB8000, and keeps the cursor position in the CRT
 * controller's registers 14 and 15: it writes a register number to
 * 0x3D4 and reads or writes the register through 0x3D5. If a read
 * returned garbage, cgaputc() would panic with a cursor out of range.
 *
 * consputc() sends every character to both uartputc() and cgaputc().
 * The CRT controller (a Motorola 6845 on the original CGA) keeps the
 * cursor address, 80 * row + column, in register 14 (high byte) and
 * register 15 (low byte). cgaputc() reads both, updates the position,
 * writes them back, and panics ("pos under/overflow") if the position
 * is beyond the 25 x 80 screen; an unanswered read would give 0xFFFF.
 * So we keep the registers as plain storage that reads back. They
 * start at 0, the top-left corner.
 *
 * Nobody displays the screen: 0xB8000 is just guest RAM here, and the
 * guest's console reaches us through COM1.
 */
static uchar crtindex, crtregs[256];  /* any uchar index is in range */

static void
crt(struct vmexit *e)
{
  if(e->io.port == 0x3D4){
    /* The index port: which register 0x3D5 refers to. */
    if(e->io.in)
      completein(e->io.size, crtindex);
    else
      crtindex = e->io.data;
  } else {
    /* The data port: the selected register. */
    if(e->io.in)
      completein(e->io.size, crtregs[crtindex]);
    else
      crtregs[crtindex] = e->io.data;
  }
}

/* ------------------------------------------------------------------
 * Port I/O dispatch
 * ------------------------------------------------------------------ */

/*
 * Called for every EXIT_IO, after the kernel has decoded the exit
 * qualification (SDM 30.2.1, Table 30-5: size, direction, string, REP
 * and port number) into e->io and moved the guest's RIP past the
 * instruction. Choose the device by port number.
 */
static void
doio(struct vmexit *e)
{
  uint p = e->io.port;

  if(p >= COM1 && p < COM1 + 8){
    com1(e);
  } else if((p >= 0x1F0 && p < 0x1F8) || p == 0x3F6){
    ide(e);
  } else if(p == 0x3D4 || p == 0x3D5){
    crt(e);
  } else if(e->io.string){
    /*
     * A string instruction would also need memory, RCX, and RSI or RDI
     * updated; rather than fake that for a device we do not know, stop.
     */
    die("string I/O to an unknown port");
  } else if(e->io.in){
    /*
     * No device: the PICs (0x20, 0x21, 0xA0, 0xA1), which xv6 only
     * masks; the keyboard (0x60, 0x64), which never interrupts here; the
     * CMOS clock (0x70, 0x71). Reads float high.
     *
     * On a PC, a read from a port that no device answers returns all
     * ones; completein() keeps the low size bytes. An OUT to such a
     * port is simply dropped, as on a real bus.
     */
    completein(e->io.size, 0xFFFFFFFF);
  }
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */

/*
 * Read the keys typed on our console, if any, without blocking.
 *
 * xv6 has no non-blocking read, so fdready() asks first whether a
 * read() would return at once. The console is in raw mode (consraw), so
 * read() returns each key as soon as it is typed, without waiting for a
 * whole line.
 */
static void
pollconsole(void)
{
  char buf[64];
  int i, n;

  while(fdready(0) > 0){
    if((n = read(0, buf, sizeof(buf))) <= 0)
      return;
    for(i = 0; i < n; i++){
      /* Our own key: never passed to the guest. */
      if(buf[i] == QUITKEY){
        consraw(0);
        printf(1, "\nrunxv6: stopped (Ctrl-])\n");
        exit();
      }
      /* Everything else arrives at the guest's COM1 receiver. */
      rxpush(buf[i]);
    }
  }
}

int
main(int argc, char *argv[])
{
  struct vmexit e;
  char *kernel;
  int ksize;
  uint64 entry;
  uint lasttick;

  if(argc != 3){
    printf(2, "usage: runxv6 kernel disk-image\n");
    exit();
  }
  if((kernel = readfile(argv[1], &ksize)) == 0)
    die("cannot read the kernel");
  /*
   * The whole disk image stays in memory (2000 blocks of 512 bytes, as
   * built by mkfsguest); the guest's writes change only this copy.
   */
  if((disk = readfile(argv[2], &disksize)) == 0)
    die("cannot read the disk image");

  /* An empty VM: the image is loaded from the ELF file instead. */
  /*
   * With len 0, vmcreate() copies nothing to VM_LOAD_ADDR (that is for
   * flat binaries such as helloguest). It allocates GUESTMEM bytes of
   * zeroed guest RAM plus the local APIC and I/O APIC pages, and sets
   * the guest up in flat 32-bit protected mode with paging off, the
   * state a multiboot loader leaves and entry.S expects.
   */
  if((vm = vmcreate(kernel, 0, GUESTMEM)) < 0)
    die("vmcreate failed");
  entry = loadkernel(kernel, ksize);
  /* The segments are in guest memory now; the file is not needed. */
  free(kernel);
  mptables();
  /*
   * Start at the kernel's entry point. The other registers start at 0;
   * a multiboot loader would put a magic number in EAX and an info
   * pointer in EBX, but xv6's entry.S does not look at them.
   */
  vmsetreg(vm, VMREG_RIP, entry);

  printf(1, "runxv6: booting %s (type Ctrl-] to stop)\n", argv[1]);
  /*
   * Raw mode: every key goes straight to us, and on to the guest,
   * whose own console echoes and edits lines. In normal mode, our
   * console would echo each key a second time and give us only whole
   * lines.
   */
  consraw(1);
  /* uptime() counts host timer ticks; see the end of the loop. */
  lasttick = uptime();
  for(;;){
    /*
     * Run the guest until it does something the kernel sends to user
     * space. The kernel also returns once the host timer has ticked
     * (EXIT_EXTERNAL_INTR), so we get here at least once per tick even
     * if the guest does no I/O.
     */
    if(vmrun(vm, &e) < 0)
      die("vmrun failed");
    /*
     * VM entry failed while loading the guest, most often on invalid
     * guest state (reason 33: a check of SDM 29.3.1 failed). The exit
     * qualification is usually 0, but a few values name the cause
     * (SDM 29.8).
     */
    if(e.entryfail){
      printf(2, "runxv6: VM entry failed, reason %d, qualification 0x%x\n",
             e.reason, e.qual);
      die("stopped");
    }
    switch(e.reason){
    case EXIT_IO:
      /* A device register; RIP already points past the instruction. */
      doio(&e);
      break;
    case EXIT_EXTERNAL_INTR:
      /* The host timer ticked: just a chance to run below. */
      break;
    case EXIT_HLT:
      /* Waiting for an interrupt: wait for the next timer tick. */
      /*
       * xv6's scheduler spins with interrupts on rather than halting,
       * so this is rare. A HLT with interrupts off (RFLAGS.IF, bit 9,
       * clear) would stop a real CPU for good, since only an interrupt
       * can resume it, so stop. Otherwise the kernel has moved RIP past
       * the HLT; sleep(1) waits for the next host tick, and the timer
       * interrupt injected below then resumes the guest after the HLT,
       * as on a real CPU.
       */
      if(!(getreg(VMREG_RFLAGS) & 0x200))
        die("the guest halted with interrupts off");
      sleep(1);
      break;
    case EXIT_VMFAIL:
      /* VMLAUNCH itself failed: a VM-instruction error (SDM 33.4). */
      printf(2, "runxv6: VMLAUNCH failed, error %d\n", e.error);
      die("stopped");
    case EXIT_TRIPLE_FAULT:
      /*
       * The guest faulted while delivering a double fault: the CPU
       * would shut down (a PC then resets). Usually a broken IDT, GDT,
       * or kernel stack in the guest.
       */
      printf(2, "runxv6: guest triple fault at rip 0x%x\n", e.rip);
      die("stopped");
    case EXIT_EPT_VIOLATION:
      /*
       * Outside guest RAM and the two APIC pages, nothing is mapped:
       * the guest followed a wild pointer, or used memory above
       * GUESTMEM (a PHYSTOP mismatch).
       */
      printf(2, "runxv6: guest touched unmapped guest-physical 0x%x, "
             "rip 0x%x\n", e.gpa, e.rip);
      die("stopped");
    default:
      /*
       * Anything else, such as RDMSR or WRMSR of an MSR other than
       * EFER, which the kernel does not emulate.
       */
      printf(2, "runxv6: unhandled exit %d, qualification 0x%x, rip 0x%x\n",
             e.reason, e.qual, e.rip);
      die("stopped");
    }
    /* Once per host tick: the guest's timer interrupt. */
    /*
     * The guest's local APIC timer is not emulated (its registers are a
     * plain page), so we play its part: one vector 32 per host tick.
     * The guest's trap() counts ticks on it and preempts the running
     * process, as it would at each local APIC timer interrupt, and its
     * lapiceoi() just writes the plain page. If the guest has not taken
     * the previous one yet, the two merge into one pending interrupt.
     */
    if(uptime() != lasttick){
      lasttick = uptime();
      vminject(vm, VEC_TIMER);
    }
    /* Keys typed since the last exit go to COM1 (and may interrupt). */
    pollconsole();
  }
}
