
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include <criterion/criterion.h>
#include <teavpn2/server/common.h>


void contiguous_block_allocation()
{
  char arena[4096];
  t_ar_init(arena, sizeof(arena));

  char *a = t_ar_alloc(1024);
  char *b = t_ar_alloc(1024);
  char *c = t_ar_alloc(1024);
  char *d = t_ar_alloc(1024);

  memset(a, 'A', 2048);
  cr_assert(!memcmp(a, b, 1024));

  memset(b, 'B', 2048);
  cr_assert(!memcmp(b, c, 1024));

  memset(c, 'D', 2048);
  cr_assert(!memcmp(c, d, 1024));
}


Test(simple, test)
{
  printf(" -- Memory test --\n");

  /* Contiguous block allocation test. */
  contiguous_block_allocation();
}
