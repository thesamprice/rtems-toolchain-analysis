/* Separate TU so it cannot be inlined into handler().  */
volatile void **sink_slot;
void sink (void **p) { sink_slot = (volatile void **) p; }
