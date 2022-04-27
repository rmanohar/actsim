/*************************************************************************
 *
 *  Copyright (c) 2022 Rajit Manohar
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA  02110-1301, USA.
 *
 **************************************************************************
 */
#include <stdio.h>
#include <common/array.h>
#include <common/misc.h>
#include "../actsim_ext.h"

L_A_DECL (FILE *, rom_fp);

expr_res actsim_rom_read (int argc, struct expr_res *args)
{
  expr_res ret;
  ret.width = 64; 
  ret.v = 0;
  if (argc != 1) {
    fprintf (stderr, "actim_rom_read: should have 1 argument only\n"); 
    return ret;
  }

  if (args[0].v > 4000) {
    fprintf (stderr, "actsim_rom_read: more than 4000 ROMSs?!\n");
    return ret;
  }


  while (args[0].v >= A_LEN (rom_fp)) {
     A_NEW (rom_fp, FILE *);
     A_NEXT (rom_fp) = NULL;
     A_INC (rom_fp);
  }
  if (!rom_fp[args[0].v]) {
     char buf[100];
     snprintf (buf, 100, "_rom_file_.%d", (int)args[0].v);
     rom_fp[args[0].v] = fopen (buf, "r");
     if (!rom_fp[args[0].v]) {
        fprintf (stderr, "Could not open file `%s' for ROM contents.\n", buf);
        return ret;
     }
  }
  if (rom_fp[args[0].v]) {
     if (fscanf (rom_fp[args[0].v], "%lx", &ret.v) != 1) {
        ret.v = 0;
        fclose (rom_fp[args[0].v]);
        rom_fp[args[0].v] = NULL;
     }
  }
  return ret; 
}

expr_res actsim_close_rom (int argc, struct expr_res *args)
{
  expr_res ret;
  ret.width = 64; 
  ret.v = 0;
  if (argc != 1) {
    fprintf (stderr, "actim_rom_read: should have 1 argument only\n"); 
    return ret;
  }
  if (args[0].v >= A_LEN (rom_fp)) {
    fprintf (stderr, "actim_rom_read: invalid ID %d!\n", (int)args[0].v);
    return ret;
  }
  if (rom_fp[args[0].v]) {
     fclose (rom_fp[args[0].v]);
     rom_fp[args[0].v] = NULL;
  }
  return ret;
}
