extern void callee (int a);
extern int  other (void);
/* Mirrors do_IRQ: takes an arg, calls other functions, needs the arg after. */
void caller (int a)
{
  other ();
  callee (a);
}
