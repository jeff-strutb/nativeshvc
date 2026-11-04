/*
 * module.c: a test program's module header on the PSP, which its loader
 * needs, and all of user memory but 1 MB, kept for the system, as its
 * heap. Weak: a test written as a game (NSHVC_MAIN, nshvc.h) has its own.
 */
#include <pspmoduleinfo.h>

#pragma weak module_info
#pragma weak sce_newlib_heap_kb_size
PSP_MODULE_INFO("nshvc_test", 0, 1, 0);
PSP_HEAP_SIZE_KB(-1024);
