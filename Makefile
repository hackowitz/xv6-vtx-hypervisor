OBJS = \
	bio.o\
	console.o\
	exec.o\
	file.o\
	fs.o\
	ide.o\
	ioapic.o\
	kalloc.o\
	kbd.o\
	lapic.o\
	log.o\
	main.o\
	mp.o\
	picirq.o\
	pipe.o\
	proc.o\
	sleeplock.o\
	spinlock.o\
	string.o\
	swtch.o\
	syscall.o\
	sysfile.o\
	sysproc.o\
	trapasm.o\
	trap.o\
	uart.o\
	vectors.o\
	vm.o\
	acpi.o\
	vmx.o\
	vmxasm.o\
	sysvmx.o\

# Cross-compiling (e.g., on Mac OS X)
# TOOLPREFIX = x86_64-elf-

# Using native tools (e.g., on X86-64 Linux)
#TOOLPREFIX = 

# Try to infer the correct TOOLPREFIX if not set
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if x86_64-elf-objdump -i 2>&1 | grep '^elf64-x86-64' >/dev/null 2>&1; \
	then echo 'x86_64-elf-'; \
	elif objdump -i 2>&1 | grep 'elf64-x86-64' >/dev/null 2>&1; \
	then echo ''; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find an x86_64-elf version of GCC/binutils." 1>&2; \
	echo "*** Is the directory with x86_64-elf-gcc in your PATH?" 1>&2; \
	echo "*** If your x86_64-elf toolchain is installed with a command" 1>&2; \
	echo "*** prefix other than 'x86_64-elf-', set your TOOLPREFIX" 1>&2; \
	echo "*** environment variable to that prefix and run 'make' again." 1>&2; \
	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

# If the makefile can't find QEMU, specify its path here
# QEMU = qemu-system-x86_64

# Try to infer the correct QEMU
ifndef QEMU
QEMU := $(shell if which qemu > /dev/null; \
	then echo qemu; \
	elif which qemu-system-x86_64 > /dev/null; \
	then echo qemu-system-x86_64; \
	else \
	echo "***" 1>&2; \
	echo "*** Error: Couldn't find a working QEMU executable." 1>&2; \
	echo "*** Is the directory containing the qemu binary in your PATH" 1>&2; \
	echo "*** or have you tried setting the QEMU variable in Makefile?" 1>&2; \
	echo "***" 1>&2; fi)
endif

CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump
# HW1: the stubbed functions leave variables and helper functions unused
# until you fill them in, so unused-variable and unused-function
# warnings are not errors here. Everything else is still -Werror.
CFLAGS = -fno-pic -static -fno-builtin -fno-strict-aliasing -Og -Wno-infinite-recursion -Wall -MD -ggdb -Werror -fno-omit-frame-pointer -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function 
# This flag disables default optimizations or features that are enabled by default.
# In particular, it disables SIMD optimizations. Cleaner than:
# CFLAGS += -mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-ssse3 -mno-sse4 -mno-sse4a -mno-sse4.1 -mno-sse4.2 -mfpmath=387
CFLAGS += -mno-default
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)
ASFLAGS = -gdwarf-2 -Wa,-divide
# LDFLAGS := --no-warn-rwx-segments
LDFLAGS := -z max-page-size=0x1000

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

$(OBJS): CFLAGS += -m64 -mcmodel=large -DX86_64

xv6.img: bootblock kernel
	dd if=/dev/zero of=xv6.img count=10000
	dd if=bootblock of=xv6.img conv=notrunc
	dd if=kernel of=xv6.img seek=1 conv=notrunc

xv6memfs.img: bootblock kernelmemfs
	dd if=/dev/zero of=xv6memfs.img count=10000
	dd if=bootblock of=xv6memfs.img conv=notrunc
	dd if=kernelmemfs of=xv6memfs.img seek=1 conv=notrunc

bootblock: bootasm.S bootmain.c
	$(CC) $(CFLAGS) -m32 -fno-pic -O -nostdinc -I. -c bootmain.c
	$(CC) $(CFLAGS) -m32 -fno-pic -nostdinc -I. -c bootasm.S
	$(LD) $(LDFLAGS) -m elf_i386 -N -e start -Ttext 0x7C00 -o bootblock.o bootasm.o bootmain.o
	$(OBJDUMP) -S bootblock.o > bootblock.asm
	$(OBJCOPY) -S -O binary -j .text bootblock.o bootblock
	./sign.pl bootblock

entryother: entryother.S
	$(CC) $(CFLAGS) -fno-pic -nostdinc -I. -c entryother.S
	$(LD) $(LDFLAGS) -N -e start -Ttext 0x7000 -o bootblockother.o entryother.o
	$(OBJCOPY) -S -O binary -j .text bootblockother.o entryother
	$(OBJDUMP) -S bootblockother.o > entryother.asm

initcode: initcode.S
	$(CC) $(CFLAGS) -nostdinc -I. -c initcode.S
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o initcode.out initcode.o
	$(OBJCOPY) -S -O binary initcode.out initcode
	$(OBJDUMP) -S initcode.o > initcode.asm

kernel: $(OBJS) entry.o entryother initcode kernel.ld
	$(LD) $(LDFLAGS) -T kernel.ld -o kernel entry.o $(OBJS) -b binary initcode entryother
	$(OBJDUMP) -S kernel > kernel.asm
	$(OBJDUMP) -t kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > kernel.sym

# kernelmemfs is a copy of kernel that maintains the
# disk image in memory instead of writing to a disk.
# This is not so useful for testing persistent storage or
# exploring disk buffering implementations, but it is
# great for testing the kernel on real hardware without
# needing a scratch disk.
MEMFSOBJS = $(filter-out ide.o,$(OBJS)) memide.o
kernelmemfs: $(MEMFSOBJS) entry.o entryother initcode kernel.ld fs.img
	$(LD) $(LDFLAGS) -T kernel.ld -o kernelmemfs entry.o  $(MEMFSOBJS) -b binary initcode entryother fs.img
	$(OBJDUMP) -S kernelmemfs > kernelmemfs.asm
	$(OBJDUMP) -t kernelmemfs | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > kernelmemfs.sym

tags: $(OBJS) entryother.S _init
	etags *.S *.c

vectors.S: vectors.pl
	./vectors.pl > vectors.S

ULIB = ulib.o usys.o printf.o umalloc.o

_%: %.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

_forktest: forktest.o $(ULIB)
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o _forktest forktest.o ulib.o usys.o
	$(OBJDUMP) -S _forktest > forktest.asm

# mkfs takes FSSIZE from param.h (HW2 changed it), so it depends on it.
mkfs: mkfs.c fs.h param.h
	gcc -Werror -Wall -o mkfs mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.o

# HW2 added timertest (a test to run inside the guest) and runxv6 (the
# monitor that runs xv6 as a guest).
UPROGS=\
	_cat\
	_echo\
	_forktest\
	_grep\
	_init\
	_kill\
	_ln\
	_ls\
	_mkdir\
	_rm\
	_sh\
	_stressfs\
	_usertests1\
	_usertests2\
	_wc\
	_zombie\
	_timertest\
	_runvm\
	_runxv6\

# Hello-world VT-x guest: a flat binary loaded by runvm, not an xv6
# user program.
GUESTCFLAGS = -m64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
	-mno-red-zone -mgeneral-regs-only -nostdinc -O2 -Wall -Werror -I.

helloguest: helloguestasm.S helloguest.c helloguest.ld
	$(CC) $(GUESTCFLAGS) -c helloguestasm.S -o helloguestasm.o
	$(CC) $(GUESTCFLAGS) -c helloguest.c -o helloguest.o
	$(LD) -T helloguest.ld -z max-page-size=0x1000 -o helloguest.elf helloguestasm.o helloguest.o
	$(OBJDUMP) -S helloguest.elf > helloguest.asm
	$(OBJCOPY) -O binary helloguest.elf helloguest

# The xv6 guest (HW2): the same kernel, built a second time for a 32 MB
# machine whose disk 1 holds a GUESTFSSIZE-block file system. runxv6
# loads xv6kernel into a VM and serves xv6fs.img as the guest's disk.
#
# Size of the guest's disk in blocks. The guest kernel (ide.c) and
# mkfsguest are both compiled with it. 2000 leaves room to run stressfs
# and usertests2 in the guest; 1000 started almost full.
GUESTFSSIZE = 2000
# PHYSTOP 0x2000000 is 32 MB, the guest memory runxv6 asks vmcreate()
# for (GUESTMEM in runxv6.c); the two must agree. memlayout.h and
# param.h use these values instead of their defaults.
GUESTDEFS = -DPHYSTOP=0x2000000 -DFSSIZE=$(GUESTFSSIZE)
# The guest kernel's objects go in their own directory, guest/. They
# come from the same sources as the host kernel's but are compiled with
# other -D values, so they must not share file names: in one directory,
# building one kernel would overwrite the other's bio.o and so on, and
# make, which judges only by timestamps, would link a kernel from
# objects built for the other one.
GUESTOBJS = $(addprefix guest/,$(OBJS))
# The guest's programs: the host's, minus the two VT-x monitors. The
# guest has no VMX (vmxcpuid() hides it), so they could only fail.
GUESTUPROGS = $(filter-out _runvm _runxv6,$(UPROGS))

# The guest kernel's C files. "$(OBJS): CFLAGS += ..." above applies
# only to the targets in OBJS (bio.o, ...), not to guest/bio.o, so the
# kernel flags are repeated here. CFLAGS has -MD, so gcc also writes
# guest/%.d, the headers each object depends on. make does not create
# directories itself, hence mkdir -p (@ keeps make from echoing it).
guest/%.o: %.c
	@mkdir -p guest
	$(CC) $(CFLAGS) -m64 -mcmodel=large -DX86_64 $(GUESTDEFS) -c -o $@ $<

# The guest kernel's assembly files (entry.S includes memlayout.h and
# param.h), with the same -D values as the C files.
guest/%.o: %.S
	@mkdir -p guest
	$(CC) $(ASFLAGS) $(GUESTDEFS) -c -o $@ $<

# Linked like kernel, with the same linker script, from the guest
# objects. initcode and entryother come from the host build: they do
# not use PHYSTOP or FSSIZE, so the same binaries serve both kernels.
# runxv6 loads the ELF file itself, so no boot block or xv6.img-style
# disk is needed.
xv6kernel: $(GUESTOBJS) guest/entry.o entryother initcode kernel.ld
	$(LD) $(LDFLAGS) -T kernel.ld -o xv6kernel guest/entry.o $(GUESTOBJS) -b binary initcode entryother

# mkfs for the guest's disk. mkfs takes the image size from FSSIZE at
# compile time, so the guest's needs its own binary, built with
# GUESTFSSIZE. It depends on the Makefile because GUESTFSSIZE is set
# here: changing it must rebuild mkfsguest.
mkfsguest: mkfs.c fs.h param.h Makefile
	gcc -Werror -Wall -DFSSIZE=$(GUESTFSSIZE) -o mkfsguest mkfs.c

# The guest's disk image: GUESTFSSIZE blocks with the guest's programs.
xv6fs.img: mkfsguest README $(GUESTUPROGS)
	./mkfsguest xv6fs.img README $(GUESTUPROGS)

# The host's disk. HW2 adds xv6kernel and xv6fs.img as ordinary files,
# so that, inside xv6, "runxv6 xv6kernel xv6fs.img" can read them.
# Both are larger than stock xv6's 70 KB file limit, hence the
# doubly-indirect block in fs.h.
fs.img: mkfs README $(UPROGS) helloguest xv6kernel xv6fs.img
	./mkfs fs.img README $(UPROGS) helloguest xv6kernel xv6fs.img

# Header dependencies of the guest objects (from -MD above), so that,
# for example, changing param.h rebuilds them.
-include guest/*.d

-include *.d

# HW2 added xv6kernel, xv6fs.img and mkfsguest, and the guest/
# directory with the guest kernel's objects and dependency files.
clean: 
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*.o *.d *.asm *.sym vectors.S bootblock entryother \
	initcode initcode.out kernel xv6.img fs.img kernelmemfs \
	xv6memfs.img mkfs .gdbinit helloguest helloguest.elf \
	xv6kernel xv6fs.img mkfsguest \
	.bochsrc bochsout.txt serial.out \
	$(UPROGS) \
	.index.html
	rm -rf guest

# make a printout
FILES = $(shell grep -v '^\#' runoff.list)
PRINT = runoff.list runoff.spec README toc.hdr toc.ftr $(FILES)

xv6.pdf: $(PRINT)
	./runoff
	ls -l xv6.pdf

print: xv6.pdf

clean_print:
	rm -rf fmt xv6.pdf

# run in emulators

bochs : fs.img xv6.img
	if [ ! -e .bochsrc ]; then ln -s dot-bochsrc .bochsrc; fi
	bochs -q

# Headless bochs (no GUI window): useful over SSH or when running under nix
# without a display. The xv6 console is the serial port, connected to this
# terminal by bochs-console.py; type Ctrl-A x to quit. A killed Bochs leaves
# its disk-image lock files behind, which make the next run fail.
bochs-nox : fs.img xv6.img
	rm -f xv6.img.lock fs.img.lock
	-python3 bochs-console.py bochs -q -f dot-bochsrc-nox
	rm -f xv6.img.lock fs.img.lock

# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 2
endif
# The hypervisor needs VT-x, which QEMU provides only through KVM
# (with nested virtualization enabled on the host). Run without it
# with "make qemu QEMUKVM=".
QEMUKVM ?= -enable-kvm -cpu host
QEMUOPTS = -drive file=fs.img,index=1,media=disk,format=raw -drive file=xv6.img,index=0,media=disk,format=raw -smp $(CPUS) -m 512 $(QEMUKVM) $(QEMUEXTRA)

qemu: fs.img xv6.img
	$(QEMU) -serial mon:stdio $(QEMUOPTS)

qemu-memfs: xv6memfs.img
	$(QEMU) -drive file=xv6memfs.img,index=0,media=disk,format=raw -smp $(CPUS) -m 256

qemu-nox: fs.img xv6.img
	$(QEMU) -nographic $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl
	cp .gdbinit.tmpl .gdbinit
	echo "echo + target remote localhost:$(GDBPORT)" >> .gdbinit
	echo "target remote localhost:$(GDBPORT)" >>.gdbinit

launch.json: launch.json.tmpl
	mkdir -p .vscode
	cp .gdbinit.tmpl .gdbinit.vscode
	sed "s/localhost:1234/localhost:$(GDBPORT)/" < $< > .vscode/$@

qemu-gdb: fs.img xv6.img .gdbinit
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -serial mon:stdio $(QEMUOPTS) -S $(QEMUGDB)

qemu-nox-gdb: fs.img xv6.img .gdbinit
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -nographic $(QEMUOPTS) -S $(QEMUGDB)

# CUT HERE
# prepare dist for students
# after running make dist, probably want to
# rename it to rev0 or rev1 or so on and then
# check in that version.

EXTRA=\
	mkfs.c ulib.c user.h cat.c echo.c forktest.c grep.c kill.c\
	ln.c ls.c mkdir.c rm.c stressfs.c usertests.c wc.c zombie.c\
	printf.c umalloc.c\
	README dot-bochsrc *.pl toc.* runoff runoff1 runoff.list\
	.gdbinit.tmpl gdbutil\

dist:
	rm -rf dist
	mkdir dist
	for i in $(FILES); \
	do \
		grep -v PAGEBREAK $$i >dist/$$i; \
	done
	sed '/CUT HERE/,$$d' Makefile >dist/Makefile
	echo >dist/runoff.spec
	cp $(EXTRA) dist

dist-test:
	rm -rf dist
	make dist
	rm -rf dist-test
	mkdir dist-test
	cp dist/* dist-test
	cd dist-test; $(MAKE) print
	cd dist-test; $(MAKE) bochs || true
	cd dist-test; $(MAKE) qemu

# update this rule (change rev#) when it is time to
# make a new revision.
tar:
	rm -rf /tmp/xv6
	mkdir -p /tmp/xv6
	cp dist/* dist/.gdbinit.tmpl /tmp/xv6
	(cd /tmp; tar cf - xv6) | gzip >xv6-rev10.tar.gz  # the next one will be 10 (9/17)

.PHONY: dist-test dist

.index.html: .index.md
	pandoc \
	    --standalone \
	    --metadata='pagetitle:Xv6-64 Source Code' \
	    --variable='keywords:operating system' \
	    --variable='keywords:unix' \
	    --variable='highlighting-css: *{font-family:Arial,sans-serif;}' \
	    -o .index.html \
	    .index.md
