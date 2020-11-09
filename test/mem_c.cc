#include <mem.h>
#include <misc.h>

static Mem *m = NULL;

extern "C" void load_memimage (const char *s)
{
  if (!m) {
    m = new Mem();
  }
  m->Clear();
  m->ReadImage (s);
}

extern "C" unsigned int readword (unsigned int addr)
{
  if (!m) {
    m = new Mem();
  }
  Assert (sizeof (Word) == 4, "Hmm");
  return m->LEReadWord ((LL)addr);
}

extern "C" void writeword (unsigned int addr, unsigned int val)
{
  if (!m) {
    m = new Mem();
  }
  Assert (sizeof (Word) == 4, "Hmm");
  m->LEWriteWord ((LL)addr, val);
}

extern "C" void save_memimage (const char *s)
{
  FILE *fp;
  if (!m) {
    m = new Mem();
  }
  fp = fopen (s, "w");
  if (!fp) {
    fatal_error ("Could not create file `%s' for writing\n", s);
  }
  m->DumpImage (fp);
  fclose (fp);
}

