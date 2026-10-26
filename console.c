// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "stdarg.h"

static void consputc(int);

static int panicked = 0;

static struct {
  struct spinlock lock;
  int locking;
} cons;

static void
printint(int64 xx, int base, int sign)
{
  static char digits[] = "0123456789abcdef";
  char buf[16];
  int i;
  uint64 x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}
//PAGEBREAK: 50

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
  int i, c, locking;
  va_list argp;
  char *s;

  va_start(argp, fmt);

  locking = cons.locking;
  if(locking)
    acquire(&cons.lock);

  if (fmt == 0)
    panic("null fmt");

  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(va_arg(argp, int), 10, 1);
      break;
    case 'x':
    case 'p':
      printint(va_arg(argp, uint64), 16, 0);
      break;
    case 's':
      if((s = va_arg(argp, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }

  if(locking)
    release(&cons.lock);

  va_end(argp);
}

void
panic(char *s)
{
  int i;
  uint64 pcs[10];

  cli();
  cons.locking = 0;
  // use lapiccpunum so that we can call panic from mycpu()
  cprintf("lapicid %d: panic: ", lapicid());
  cprintf(s);
  cprintf("\n");
  getcallerpcs((uint64*) rrbp(), pcs);
  for(i=0; i<10; i++)
    cprintf(" %p", pcs[i]);
  panicked = 1; // freeze other CPU
  for(;;)
    ;
}

//PAGEBREAK: 50
#define BACKSPACE 0x100
#define CRTPORT 0x3d4
static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory

static void
cgaputc(int c)
{
  int pos;

  // Cursor position: col + 80*row.
  outb(CRTPORT, 14);
  pos = inb(CRTPORT+1) << 8;
  outb(CRTPORT, 15);
  pos |= inb(CRTPORT+1);

  if(c == '\n')
    pos += 80 - pos%80;
  else if(c == BACKSPACE){
    if(pos > 0) --pos;
  } else
    crt[pos++] = (c&0xff) | 0x0700;  // black on white

  if(pos < 0 || pos > 25*80)
    panic("pos under/overflow");

  if((pos/80) >= 24){  // Scroll up.
    memmove(crt, crt+80, sizeof(crt[0])*23*80);
    pos -= 80;
    memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
  }

  outb(CRTPORT, 14);
  outb(CRTPORT+1, pos>>8);
  outb(CRTPORT, 15);
  outb(CRTPORT+1, pos);
  crt[pos] = ' ' | 0x0700;
}

void
consputc(int c)
{
  if(panicked){
    cli();
    for(;;)
      ;
  }

  if(c == BACKSPACE){
    uartputc('\b'); uartputc(' '); uartputc('\b');
  } else
    uartputc(c);
  cgaputc(c);
}

#define INPUT_BUF 128
struct {
  char buf[INPUT_BUF];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} input;

// HW2: raw mode (consraw), for runxv6. Characters are not echoed or
// edited, and read() returns each one as soon as it arrives.
//
// runxv6 passes every key to the guest's serial port, and the guest
// xv6 runs its own console code on it: the guest echoes, handles
// backspace and Ctrl-U, and buffers lines. If the host did that too,
// every key would be echoed twice, and the guest would get keys only
// after Enter. Unix terminals have the same mode (stty raw).
static int rawmode;

// Set by the consraw() system call (sysfile.c). Taking cons.lock
// orders the change with consoleintr() and consoleread(), which test
// rawmode while holding it.
void
consraw(int on)
{
  acquire(&cons.lock);
  rawmode = on;
  release(&cons.lock);
}

// HW2: would a read of the console return without blocking?
// consoleread() sleeps while input.r == input.w, so a read returns at
// once if and only if some committed input is waiting. Characters of a
// line still being edited (between input.w and input.e) do not count.
// Used by the fdready() system call.
int
consready(void)
{
  int r;

  acquire(&cons.lock);
  r = input.r != input.w;
  release(&cons.lock);
  return r;
}

#define C(x)  ((x)-'@')  // Control-x

void
consoleintr(int (*getc)(void))
{
  int c, doprocdump = 0;

  acquire(&cons.lock);
  while((c = getc()) >= 0){
    if(rawmode){
      // HW2 raw mode: no echo, no editing; every character is
      // available to read() at once.
      //
      // No consputc(): the guest echoes what it wants shown. Control
      // characters are not interpreted: Ctrl-P, Ctrl-U, backspace and
      // Ctrl-D reach the guest like any other byte. '\r' is not turned
      // into '\n', as on a real serial line; the guest's own console
      // code does that conversion.
      //
      // Keep the character only if the buffer has room; otherwise drop
      // it, as line mode does.
      if(input.e-input.r < INPUT_BUF){
        input.buf[input.e++ % INPUT_BUF] = c;
        // Commit it at once (line mode waits for the end of the line);
        // this also commits any half-edited line from before raw mode.
        input.w = input.e;
        // Wake a reader sleeping in consoleread().
        wakeup(&input.r);
      }
      continue;
    }
    switch(c){
    case C('P'):  // Process listing.
      // procdump() locks cons.lock indirectly; invoke later
      doprocdump = 1;
      break;
    case C('U'):  // Kill line.
      while(input.e != input.w &&
            input.buf[(input.e-1) % INPUT_BUF] != '\n'){
        input.e--;
        consputc(BACKSPACE);
      }
      break;
    case C('H'): case '\x7f':  // Backspace
      if(input.e != input.w){
        input.e--;
        consputc(BACKSPACE);
      }
      break;
    default:
      if(c != 0 && input.e-input.r < INPUT_BUF){
        c = (c == '\r') ? '\n' : c;
        input.buf[input.e++ % INPUT_BUF] = c;
        consputc(c);
        if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
          input.w = input.e;
          wakeup(&input.r);
        }
      }
      break;
    }
  }
  release(&cons.lock);
  if(doprocdump) {
    procdump();  // now call procdump() wo. cons.lock held
  }
}

int
consoleread(struct inode *ip, char *dst, int n)
{
  uint target;
  int c;

  iunlock(ip);
  target = n;
  acquire(&cons.lock);
  while(n > 0){
    while(input.r == input.w){
      if(myproc()->killed){
        release(&cons.lock);
        ilock(ip);
        return -1;
      }
      sleep(&input.r, &cons.lock);
    }
    c = input.buf[input.r++ % INPUT_BUF];
    // In raw mode Ctrl-D is data for the guest, not end of file.
    if(c == C('D') && !rawmode){  // EOF
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        input.r--;
      }
      break;
    }
    *dst++ = c;
    --n;
    if(c == '\n')
      break;
    // HW2: in raw mode, return as soon as the committed input runs out,
    // even if fewer than n bytes were read. A raw-mode reader wants
    // keys as they are typed, and a single key (with no '\n' to end
    // the line) would otherwise leave read() asleep until n arrived.
    // Since the loop only gets here after taking at least one byte, a
    // raw read still blocks until there is something to return.
    if(rawmode && input.r == input.w)
      break;  // raw mode: return what has arrived, not a whole line
  }
  release(&cons.lock);
  ilock(ip);

  return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
  int i;

  iunlock(ip);
  acquire(&cons.lock);
  for(i = 0; i < n; i++)
    consputc(buf[i] & 0xff);
  release(&cons.lock);
  ilock(ip);

  return n;
}

void
consoleinit(void)
{
  initlock(&cons.lock, "console");

  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].read = consoleread;
  cons.locking = 1;

  ioapicenable(IRQ_KBD, 0);
}

