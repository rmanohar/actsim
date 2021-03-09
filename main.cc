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
  SimDES::interrupt();
}

static void clr_interrupt (void)
{
  LispInterruptExecution = 0;
  SimDES::resume();
}

static void usage (char *name)
{
  fprintf (stderr, "Usage: %s <actfile> <process>\n", name);
  exit (1);
}

static ActStatePass *glob_sp;
static ActSim *glob_sim;
static Act *glob_act;

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

int process_initialize (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <process>\n", argv[0]);
    return 0;
  }
  Process *p = glob_act->findProcess (argv[1]);
  if (!p) {
    fprintf (stderr, "%s: could not find process %s\n", argv[0], argv[1]);
    return 0;
  }
  if (glob_sim) {
    delete glob_sim;
    delete glob_sp;
  }
  SimDES::Init ();
  glob_sp = new ActStatePass (glob_act);
  glob_sp->run (p);
  glob_sim = new ActSim (p);
  return 1;
}

static void dump_state (ActInstTable *x)
{
  if (!x) {
    warning ("Didn't find info; is this a valid instance?");
    return;
  }
  
  if (x->obj) {
    x->obj->dumpState (stdout);
  }
  if (x->H) {
    hash_bucket_t *b;
    hash_iter_t i;
    hash_iter_init (x->H, &i);
    while ((b = hash_iter_next (x->H, &i))) {
      ActInstTable *tmp = (ActInstTable *) b->v;
      dump_state (tmp);
    }
  }
}

static unsigned long _get_energy (ActInstTable *x, double *lk, int ts = 0)
{
  unsigned long tot;
  unsigned long sub;
  double totl, subl;

  if (!x) {
    warning ("Didn't find info; is this a valid instance?");
    *lk = 0;
    return 0;
  }

  tot = 0;
  totl = 0;
  
  if (x->obj) {
    tot = x->obj->getEnergy ();
    totl = x->obj->getLeakage ();

    if (tot > 0 || totl > 0) {
      for (int i=0; i < ts; i++) {
	printf ("  ");
      }
      printf (" - ");
      x->obj->getName()->Print (stdout);
      printf (" %lu  (%g W)\n", tot, totl);
    }
  }
  sub = tot;
  subl = totl;
  if (x->H) {
    hash_bucket_t *b;
    hash_iter_t i;
    double tmpl;
    hash_iter_init (x->H, &i);
    while ((b = hash_iter_next (x->H, &i))) {
      ActInstTable *tmp = (ActInstTable *) b->v;
      tot += _get_energy (tmp, &tmpl, ts+1);
      totl += tmpl;
    }
    if ((tot - sub) > 0 || ((totl - subl) > 0)) {
      for (int i=0; i < ts; i++) {
	printf ("  ");
      }
      printf (" ---:subtree %lu (%g W)\n", tot - sub, totl - subl);
    }
  }
  *lk = totl;
  return tot;
}

ActInstTable *find_table (ActId *id, ActInstTable *x)
{
  char buf[1024];
  hash_bucket_t *b;
  
  if (!id) { return x; }

  if (!x->H) { return NULL; }

  ActId *tmp = id->Rest();
  id->prune();
  id->sPrint (buf, 1024);
  id->Append (tmp);

  b = hash_lookup (x->H, buf);
  if (!b) {
    return NULL;
  }
  else {
    return find_table (id->Rest(), (ActInstTable *)b->v);
  }
}

int process_procinfo (int argc, char **argv)
{
  ActId *id;
  if (argc != 2 && argc != 1) {
    fprintf (stderr, "Usage: %s [<instance-name>]\n", argv[0]);
    return 0;
  }
  if (argc == 1) {
    /* all */
    id = NULL;
  }
  else {
    id = ActId::parseId (argv[1]);
    if (id == NULL) {
      fprintf (stderr, "Could not parse `%s' into an instance name\n",
	       argv[1]);
      return 0;
    }
  }

  if (!id) {
    /*-- print state of each process --*/
    dump_state (glob_sim->getInstTable ());
  }
  else {
    /*-- find this process --*/
    ActInstTable *inst = find_table (id, glob_sim->getInstTable());
    dump_state (inst);
    delete id;
  }
  
  return 1;
}

int process_getenergy (int argc, char **argv)
{
  ActId *id;
  double lk;
    
  if (argc != 2 && argc != 1) {
    fprintf (stderr, "Usage: %s [<instance-name>]\n", argv[0]);
    return 0;
  }
  if (argc == 1) {
    /* all */
    id = NULL;
  }
  else {
    id = ActId::parseId (argv[1]);
    if (id == NULL) {
      fprintf (stderr, "Could not parse `%s' into an instance name\n",
	       argv[1]);
      return 0;
    }
  }

  if (!id) {
    /*-- print state of each process --*/
    printf ("Total: %lu", _get_energy (glob_sim->getInstTable (), &lk));
    printf ("  (%g W)\n", lk);
  }
  else {
    /*-- find this process --*/
    ActInstTable *inst = find_table (id, glob_sim->getInstTable());
    printf ("Total: %lu", _get_energy (inst, &lk));
    printf ("  (%g W)\n", lk);
    delete id;
  }
  return 1;
}

struct LispCliCommand Cmds[] = {
  { NULL, "Running a simulation", NULL },
  { "initialize", "initialize <proc> - initialize simulation for <proc>",
    process_initialize },
  { "cycle", "cycle - run until simulation stops", process_cycle },
  { "step", "step [n] - run the next [n] events", process_step },
  { "advance", "advance <delay> - run for <delay> time", process_advance },
  { "procinfo", "procinfo [<inst-name>] - show the program counter for a process", process_procinfo },
  { "energy", "energy [<inst-name>] - show energy usage", process_getenergy }
};

int main (int argc, char **argv)
{
  char *proc;

  /* initialize ACT library */
  Act::Init (&argc, &argv);

  /* some usage check */
  if (argc != 3) {
    usage (argv[0]);
  }

  /* read in the ACT file */
  glob_act = new Act (argv[1]);

  /* expand it */
  glob_act->Expand ();

  /* map to cells: these get characterized */
  //ActCellPass *cp = new ActCellPass (a);
  //cp->run();
 
  /* find the process specified on the command line */
  Process *p = glob_act->findProcess (argv[2]);

  if (!p) {
    fatal_error ("Could not find process `%s' in file `%s'", argv[2], argv[1]);
  }

  if (!p->isExpanded()) {
    fatal_error ("Process `%s' is not expanded.", argv[2]);
  }

  /* do stuff here */
  glob_sp = new ActStatePass (glob_act);
  glob_sp->run (p);
  
  glob_sim = new ActSim (p);

  signal (SIGINT, signal_handler);

  LispInit ();
  LispCliInit (NULL, ".actsim_history", "actsim> ", Cmds,
	       sizeof (Cmds)/sizeof (Cmds[0]));

  while (!LispCliRun (stdin)) {
    if (LispInterruptExecution) {
      fprintf (stderr, " *** interrupted\n");
    }
    clr_interrupt ();
  }

  LispCliEnd ();

  return 0;
}
