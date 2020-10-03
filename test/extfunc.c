#include <stdio.h>
#include <act/actsim_ext.h>

static FILE *fp = NULL;
static int count = 0;

struct expr_res example (int num, struct expr_res *args)
{
  struct expr_res t;
  if (!fp) { 
    fp = fopen ("test.out", "w");
    count = 0;
  }
  fprintf (fp, "%d: %d\n", count++, args[0].v);
  t.v = 4;
  t.width = 32;
  return t;
}
