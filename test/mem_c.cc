#include <mem.h>
#include <misc.h>
#include <act/actsim_ext.h>

#define MEMORY_IMAGE_FILE "mem.image"
#define MEMORY_DUMP_FILE "memout.image"

static Mem *m = NULL;

extern "C" struct expr_res load_memimage (int num, struct expr_res *args)
{
  struct expr_res ret;
  /* num should be 0 */
  if (!m) {
    m = new Mem();
  }
  m->Clear();
  m->ReadImage (MEMORY_IMAGE_FILE);
  ret.v = 1;
  ret.width = 1;
  return ret;
}

extern "C" struct expr_res readword (int num, struct expr_res *args)
{ 
  /* num should be 1 */
  unsigned int addr = args[0].v;
  struct expr_res ret;
  if (!m) {
    m = new Mem();
  }
  Assert (sizeof (Word) == 4, "Hmm");
  ret.width = 32;
  ret.v = m->LEReadWord ((LL)addr); 
  return ret;
}

extern "C" struct expr_res writeword (int num, struct expr_res *args)
{
  unsigned int addr, val;
  struct expr_res ret;
  addr = args[0].v;
  val = args[1].v;
  if (!m) {
    m = new Mem();
  }
  Assert (sizeof (Word) == 4, "Hmm");
  m->LEWriteWord ((LL)addr, val);
  ret.v = 1;
  ret.width = 1;
  return ret;
}

extern "C" struct expr_res save_memimage (int num, struct expr_res *args)
{
  struct expr_res ret;
  FILE *fp;
  if (!m) {
    m = new Mem();
  }
  fp = fopen (MEMORY_DUMP_FILE, "w");
  if (!fp) {
    fatal_error ("Could not create file `%s' for writing\n", MEMORY_DUMP_FILE);
  }
  m->DumpImage (fp);
  fclose (fp);
  ret.v = 1;
  ret.width = 1;
  return ret;
}

