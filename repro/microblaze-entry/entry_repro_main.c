/* Driver for entry_repro.S -- see that file for what is being shown.  */

#define UARTLITE_BASE 0x84000000u
#define UART_TX       (UARTLITE_BASE + 0x04u)
#define UART_STATUS   (UARTLITE_BASE + 0x08u)
#define STATUS_TXFULL 0x08u

static void putc_ (char c)
{
  volatile unsigned *st = (volatile unsigned *) UART_STATUS;
  volatile unsigned *tx = (volatile unsigned *) UART_TX;
  while (*st & STATUS_TXFULL)
    ;
  *tx = (unsigned char) c;
}

static void puts_ (const char *s)
{
  while (*s)
    putc_ (*s++);
}

static void puthex_ (unsigned v)
{
  const char *d = "0123456789abcdef";
  int i;
  puts_ ("0x");
  for (i = 28; i >= 0; i -= 4)
    putc_ (d[(v >> i) & 0xf]);
}

/* Stands in for do_IRQ: takes a pointer argument and forces it to memory,
   which makes GCC spill the incoming register into the caller's argument
   save area.  */
extern void sink (void **p);
void
handler (void *regs)
{
  sink (&regs);
}

extern unsigned fake_interrupt (unsigned sentinel);

#define SENTINEL 0xfeedface

void
cmain (void)
{
  unsigned got = fake_interrupt (SENTINEL);

  puts_ ("PT_R1 expected ");
  puthex_ (SENTINEL);
  puts_ (", got ");
  puthex_ (got);
  puts_ (got == SENTINEL ? "  -> INTACT\n" : "  -> CORRUPTED\n");

  for (;;)
    ;
}
