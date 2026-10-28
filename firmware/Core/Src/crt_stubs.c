/* The startup file replaces newlib's crt0 (-nostartfiles), so the two hooks
 * __libc_init_array() expects are provided here. Nothing to do in either. */
void _init(void) {}
void _fini(void) {}
