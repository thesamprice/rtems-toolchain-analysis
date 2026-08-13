extern void use (int *p);
/* Taking the address of an incoming register parameter forces it to memory. */
void spill_arg (int a)
{
  use (&a);
}
