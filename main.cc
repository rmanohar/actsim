/*************************************************************************
 *
 *  This file is part of the ACT library
 *
 *  Copyright (c) 2020 Rajit Manohar
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
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <act/act.h>
#include <act/passes.h>
#include "config.h"
#include "actsim.h"
#include <lisp.h>
#include <lispCli.h>

static void signal_handler (int sig)
{
  LispInterruptExecution = 1;
}

static void clr_interrupt (void)
{
  LispInterruptExecution = 0;
}

static void usage (char *name)
{
  fprintf (stderr, "Usage: %s <actfile> <process>\n", name);
  exit (1);
}

static ActSim *glob_sim;

int process_cycle (int argc, char **argv)
{
  if (argc != 1) {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return 0;
  }
  glob_sim->runSim (NULL);
  return 1;
}

int process_step (int argc, char **argv)
{
  int nsteps;
  if (argc != 1 && argc != 2) {
    fprintf (stderr, "Usage: %s [num]\n", argv[0]);
    return 0;
  }
  if (argc == 1) {
    nsteps = 1;
  }
  else {
    nsteps = atoi (argv[1]);
    if (nsteps <= 0) {
      fprintf (stderr, "%s: zero/negative steps?\n", argv[0]);
      return 0;
    }
  }
  glob_sim->Step (nsteps);
  return 1;
}

int process_advance (int argc, char **argv)
{
  int nsteps;
  if (argc != 2) {
    fprintf (stderr, "Usage: %s [num]\n", argv[0]);
    return 0;
  }
  nsteps = atoi (argv[1]);
  if (nsteps <= 0) {
    fprintf (stderr, "%s: zero/negative delay?\n", argv[0]);
    return 0;
  }
  glob_sim->Advance (nsteps);
  return 1;
}
		    

struct LispCliCommand Cmds[] = {
  { NULL, "Running a simulation", NULL },
  { "cycle", "cycle - run until simulation stops", process_cycle },
  { "step", "step [n] - run the next [n] events", process_step },
  { "advance", "advance <delay> - run for <delay> time", process_advance }
};

int main (int argc, char **argv)
{
  Act *a;
  char *proc;

  /* initialize ACT library */
  Act::Init (&argc, &argv);

  /* some usage check */
  if (argc != 3) {
    usage (argv[0]);
  }

  /* read in the ACT file */
  a = new Act (argv[1]);

  /* expand it */
  a->Expand ();

  /* map to cells: these get characterized */
  //ActCellPass *cp = new ActCellPass (a);
  //cp->run();
 
  /* find the process specified on the command line */
  Process *p = a->findProcess (argv[2]);

  if (!p) {
    fatal_error ("Could not find process `%s' in file `%s'", argv[2], argv[1]);
  }

  if (!p->isExpanded()) {
    fatal_error ("Process `%s' is not expanded.", argv[2]);
  }

  /* do stuff here */
  ActStatePass *sp = new ActStatePass (a);
  sp->run (p);
  
  ActSim *glob_sim = new ActSim (p);

  signal (SIGINT, signal_handler);

  LispInit ();
  LispCliInit (NULL, ".actsim_history", "actsim> ", Cmds,
	       sizeof (Cmds)/sizeof (Cmds[0]));

  while (!LispCliRun (stdin)) {

  }

  LispCliEnd ();

  return 0;
}
