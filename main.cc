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
#include <common/config.h>
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
    return LISP_RET_ERROR;
  }
  glob_sim->runSim (NULL);
  return LISP_RET_TRUE;
}

int process_step (int argc, char **argv)
{
  long nsteps;
  if (argc != 1 && argc != 2) {
    fprintf (stderr, "Usage: %s [num]\n", argv[0]);
    return LISP_RET_ERROR;
  }
  if (argc == 1) {
    nsteps = 1;
  }
  else {
    sscanf (argv[1], "%ld", &nsteps);
    if (nsteps <= 0) {
      fprintf (stderr, "%s: zero/negative steps?\n", argv[0]);
      return LISP_RET_ERROR;
    }
  }
  glob_sim->Step (nsteps);
  if (SimDES::hasPendingEvent()) {
    return LISP_RET_TRUE;
  }
  else {
    return LISP_RET_FALSE;
  }
}

int process_advance (int argc, char **argv)
{
  long nsteps;
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <delay>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  sscanf (argv[1], "%ld", &nsteps);
  if (nsteps <= 0) {
    fprintf (stderr, "%s: zero/negative delay?\n", argv[0]);
    return LISP_RET_ERROR;
  }
  glob_sim->Advance (nsteps);
  if (SimDES::hasPendingEvent()) {
    return LISP_RET_TRUE;
  }
  else {
    return LISP_RET_FALSE;
  }
}

int process_initialize (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <process>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  Process *p = glob_act->findProcess (argv[1]);
  if (!p) {
    fprintf (stderr, "%s: could not find process %s\n", argv[0], argv[1]);
    return LISP_RET_ERROR;
  }
  if (glob_sim) {
    delete glob_sim;
    delete glob_sp;
  }
  SimDES::Init ();
  glob_sp = new ActStatePass (glob_act);
  glob_sp->run (p);
  glob_sim = new ActSim (p);
  glob_sim->runInit ();
  return LISP_RET_TRUE;
}

static void dump_state (FILE *fp, ActInstTable *x)
{
  if (!x) {
    warning ("Didn't find info; is this a valid instance?");
    return;
  }
  
  if (x->obj) {
    x->obj->dumpState (fp);
  }
  if (x->H) {
    hash_bucket_t *b;
    hash_iter_t i;
    hash_iter_init (x->H, &i);
    while ((b = hash_iter_next (x->H, &i))) {
      ActInstTable *tmp = (ActInstTable *) b->v;
      dump_state (fp, tmp);
    }
  }
}

static unsigned long _get_energy (FILE *fp,
				  ActInstTable *x, double *lk,
				  unsigned long *area,
				  int ts = 0)
{
  unsigned long tot;
  unsigned long sub;
  double totl, subl;
  unsigned long suba, tota;

  if (!x) {
    warning ("Didn't find info; is this a valid instance?");
    *lk = 0;
    return 0;
  }

  tot = 0;
  totl = 0;
  tota = 0;
  
  if (x->obj) {
    tot = x->obj->getEnergy ();
    totl = x->obj->getLeakage ();
    tota = x->obj->getArea ();

    if (tot > 0 || totl > 0 || tota > 0) {
      for (int i=0; i < ts; i++) {
	fprintf (fp, "  ");
      }
      fprintf (fp, " - ");
      x->obj->getName()->Print (fp);
      fprintf (fp, " %lu  (%g W); area: %lu\n", tot, totl, tota);
    }
  }
  sub = tot;
  subl = totl;
  suba = tota;
  if (x->H) {
    hash_bucket_t *b;
    hash_iter_t i;
    double tmpl;
    unsigned long tmpa;
    hash_iter_init (x->H, &i);
    while ((b = hash_iter_next (x->H, &i))) {
      ActInstTable *tmp = (ActInstTable *) b->v;
      tot += _get_energy (fp, tmp, &tmpl, &tmpa, ts+1);
      totl += tmpl;
      tota += tmpa;
    }
    if ((tot - sub) > 0 || ((totl - subl) > 0) || ((tota - suba) > 0)) {
      for (int i=0; i < ts; i++) {
	fprintf (fp, "  ");
      }
      fprintf (fp, " ---:subtree %lu (%g W); area: %lu\n",
	       tot - sub, totl - subl, tota - suba);
    }
  }
  *lk = totl;
  *area = tota;
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

ActSimObj *find_object (ActId **id, ActInstTable *x)
{
  char buf[1024];
  hash_bucket_t *b;
  
  if (!(*id)) { return x->obj; }
  if (!x->H) { return x->obj; }

  ActId *tmp = (*id)->Rest();
  (*id)->prune();
  (*id)->sPrint (buf, 1024);
  (*id)->Append (tmp);

  b = hash_lookup (x->H, buf);
  if (!b) {
    return x->obj;
  }
  else {
    (*id) = (*id)->Rest();
    return find_object (id, (ActInstTable *)b->v);
  }
}



int process_procinfo (int argc, char **argv)
{
  ActId *id;
  FILE *fp;
  if (argc != 2 && argc != 3) {
    fprintf (stderr, "Usage: %s <filename> [<instance-name>]\n", argv[0]);
    return LISP_RET_ERROR;
  }

  if (strcmp (argv[1], "-") == 0) {
    fp = stdout;
  }
  else {
    fp = fopen (argv[1], "w");
    if (!fp) {
      fprintf (stderr, "%s: could not open file `%s' for writing\n",
	       argv[0], argv[1]);
      return LISP_RET_ERROR;
    }
  }

  if (argc == 2) {
    /* all */
    id = NULL;
  }
  else {
    id = ActId::parseId (argv[1]);
    if (id == NULL) {
      fprintf (stderr, "Could not parse `%s' into an instance name\n",
	       argv[1]);
      return LISP_RET_ERROR;
    }
  }

  if (!id) {
    /*-- print state of each process --*/
    dump_state (fp, glob_sim->getInstTable ());
  }
  else {
    /*-- find this process --*/
    ActInstTable *inst = find_table (id, glob_sim->getInstTable());
    dump_state (fp, inst);
    delete id;
  }

  if (fp != stdout) {
    fclose (fp);
  }
  
  return LISP_RET_TRUE;
}

int process_getenergy (int argc, char **argv)
{
  ActId *id;
  double lk;
  unsigned long area;
  FILE *fp;
    
  if (argc != 2 && argc != 3) {
    fprintf (stderr, "Usage: %s <filename> [<instance-name>]\n", argv[0]);
    return LISP_RET_ERROR;
  }

  if (strcmp (argv[1], "-") == 0) {
    fp = stdout;
  }
  else {
    fp = fopen (argv[1], "w");
    if (!fp) {
      fprintf (stderr, "%s: could not open file `%s' for writing\n",
	       argv[0], argv[1]);
      return LISP_RET_ERROR;
    }
  }
  
  if (argc == 2) {
    /* all */
    id = NULL;
  }
  else {
    id = ActId::parseId (argv[2]);
    if (id == NULL) {
      fprintf (stderr, "Could not parse `%s' into an instance name\n",
	       argv[2]);
      return LISP_RET_ERROR;
    }
  }

  if (!id) {
    /*-- print state of each process --*/
    fprintf (fp, "Total: %lu", _get_energy (fp,
				       glob_sim->getInstTable (), &lk, &area));
    fprintf (fp, "  (%g W); area: %lu\n", lk, area);
  }
  else {
    /*-- find this process --*/
    ActInstTable *inst = find_table (id, glob_sim->getInstTable());
    fprintf (fp, "Total: %lu", _get_energy (fp, inst, &lk, &area));
    fprintf (fp, "  (%g W); area: %lu\n", lk, area);
    delete id;
  }
  if (fp != stdout) {
    fclose (fp);
  }
  return LISP_RET_TRUE;
}

static int id_to_siminfo (char *s, int *ptype, int *poffset, ActSimObj **pobj)
{
  ActId *id = ActId::parseId (s);
  if (!id) {
    fprintf (stderr, "Could not parse `%s' into an identifier\n", s);
    return 0;
  }

  /* -- find object / id combo -- */
  ActId *tmp = id;
  ActSimObj *obj = find_object (&tmp, glob_sim->getInstTable());
  stateinfo_t *si;
  int offset, type;
  int res;
  act_connection *c;
  
  if (!obj) {
    fprintf (stderr, "Could not find `%s' in simulation\n", s);
    delete id;
    return 0;
  }

  /* -- now convert tmp into a local offset -- */

  si = glob_sp->getStateInfo (obj->getProc ());
  if (!si) {
    fprintf (stderr, "Could not find info for process `%s'\n", obj->getProc()->getName());
    delete id;
    return 0;
  }

  if (!si->bnl->cur->FullLookup (tmp, NULL)) {
    fprintf (stderr, "Could not find identifier `%s' within process `%s'\n",
	     s, obj->getProc()->getName());
    delete id;
    return 0;
  }

  if (!tmp->validateDeref (si->bnl->cur)) {
    fprintf (stderr, "Array index is out of bounds!\n");
    return 0;
  }

  c = tmp->Canonical (si->bnl->cur);
  Assert (c, "What?");

  res = glob_sp->getTypeOffset (si, c, &offset, &type, NULL);
  if (!res) {
    /* it is possible that it is an array reference */
    Array *ta = NULL;
    ActId *tid;
    tid = tmp;
    while (tmp->Rest()) {
      tmp = tmp->Rest();
    }
    ta = tmp->arrayInfo();
    if (ta) {
      tmp->setArray (NULL);
      c = tmp->Canonical (si->bnl->cur);
      Assert (c, "Hmm...");
      res = glob_sp->getTypeOffset (si, c, &offset, &type, NULL);
      if (res) {
	InstType *it = si->bnl->cur->FullLookup (tmp, NULL);
	Assert (it->arrayInfo(), "What?");
	offset += it->arrayInfo()->Offset (ta);
      }
      tmp->setArray (ta);
    }
    if (!res) {
      fprintf (stderr, "Could not find identifier `%s' within process `%s'\n",
	       s, obj->getProc()->getName());
      delete id;
      return 0;
    }
  }

  if (type == 3) {
    type = 2;
  }

  offset = obj->getGlobalOffset (offset, type);

  *ptype = type;
  *poffset = offset;
  if (pobj) {
    *pobj = obj;
  }
  delete id;
  return 1;
}


int process_set (int argc, char **argv)
{
  if (argc != 3) {
    fprintf (stderr, "Usage: %s <name> <val>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;

  if (!id_to_siminfo (argv[1], &type, &offset, NULL)) {
    return LISP_RET_ERROR;
  }

  if (type == 2 || type == 3) {
    printf ("'%s' is a channel; not currently supported!\n", argv[1]);
    return LISP_RET_ERROR;
  }

  int val;
  
  if (type == 0) {
    if (strcmp (argv[2], "0") == 0 || strcmp (argv[2], "#f") == 0) {
      val = 0;
    }
    else if (strcmp (argv[2], "1") == 0 || strcmp (argv[2], "#t") == 0) {
      val = 1;
    }
    else if (strcmp (argv[2], "X") == 0) {
      val = 2;
    }
    else {
      fprintf (stderr, "Boolean must be set to either 0, 1, or X\n");
      return LISP_RET_ERROR;
    }
    glob_sim->setBool (offset, val);
  }
  else if (type == 1) {
    val = atoi (argv[2]);
    if (val < 0) {
      fprintf (stderr, "Integers are unsigned.\n");
      return LISP_RET_ERROR;
    }
    glob_sim->setInt (offset, val);
  }
  else {
    fatal_error ("Should not be here");
  }

  SimDES **arr;
  arr = glob_sim->getFO (offset, type);
  for (int i=0; i < glob_sim->numFanout (offset, type); i++) {
    ActSimDES *p = dynamic_cast <ActSimDES *> (arr[i]);
    Assert (p, "Hmm?");
    p->propagate ();
  }
  return LISP_RET_TRUE;
}

int process_get (int argc, char **argv)
{
  if (argc != 2 && argc != 3) {
    fprintf (stderr, "Usage: %s <name> [#f]\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;

  if (!id_to_siminfo (argv[1], &type, &offset, NULL)) {
    return LISP_RET_ERROR;
  }

  if (type == 2 || type == 3) {
    printf ("'%s' is a channel; not currently supported!\n", argv[1]);
    return LISP_RET_ERROR;
  }


  unsigned long val;
  if (type == 0) {
    val = glob_sim->getBool (offset);
    LispSetReturnInt (val);
    if (argc == 2) {
      if (val == 0) {
	printf ("%s: 0\n", argv[1]);
      }
      else if (val == 1) {
	printf ("%s: 1\n", argv[1]);
      }
      else {
	printf ("%s: X\n", argv[1]);
      }
    }
  }
  else if (type == 1) {
    val = glob_sim->getInt (offset);
    LispSetReturnInt (val);
    if (argc == 2) {
      printf ("%s: %lu  (0x%lx)\n", argv[1], val, val);
    }
  }
  else {
    fatal_error ("Should not be here");
    return LISP_RET_TRUE;
  }
  return LISP_RET_INT;
}

int process_watch (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <name>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;
  ActSimObj *obj;

  if (!id_to_siminfo (argv[1], &type, &offset, &obj)) {
    return LISP_RET_ERROR;
  }

  obj->addWatchPoint (type, offset, argv[1]);

  return LISP_RET_TRUE;
}

int process_breakpt (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <name>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;
  ActSimObj *obj;

  if (!id_to_siminfo (argv[1], &type, &offset, &obj)) {
    return LISP_RET_ERROR;
  }

  obj->toggleBreakPt (type, offset, argv[1]);

  return LISP_RET_TRUE;
}


int process_unwatch (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <name>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  int type, offset;
  ActSimObj *obj;

  if (!id_to_siminfo (argv[1], &type, &offset, &obj)) {
    return LISP_RET_ERROR;
  }

  obj->delWatchPoint (type, offset);

  return LISP_RET_TRUE;
}


int process_logfile (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <file>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  actsim_close_log ();
  
  FILE *fp = fopen (argv[1], "w");
  if (!fp) {
    fprintf (stderr, "%s: could not open file `%s'\n", argv[0], argv[1]);
    return LISP_RET_ERROR;
  }
  actsim_set_log (fp);
  return LISP_RET_TRUE;
}

int process_filter (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <regexp>\n", argv[0]);
    return LISP_RET_ERROR;
  }

  glob_sim->logFilter (argv[1]);

  return LISP_RET_TRUE;
}

int process_error (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <str>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  fprintf (stderr, "ERROR: %s\n", argv[1]);

  return LISP_RET_ERROR;
}

int process_echo (int argc, char **argv)
{
  int nl = 1;
  if (argc > 1 && strcmp (argv[1], "-n") == 0) {
      nl = 0;
  }
  for (int i=2-nl; i < argc; i++) {
    printf ("%s", argv[i]);
    if (i != argc-1) {
      printf (" ");
    }
  }
  if (nl) {
    printf ("\n");
  }
  return LISP_RET_TRUE;
}

int process_mode (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s reset|run\n", argv[0]);
    return LISP_RET_ERROR;
  }
  if (strcmp (argv[1], "reset") == 0) {
    glob_sim->setMode (1);
  }
  else if (strcmp (argv[1], "run") == 0) {
    glob_sim->setMode (0);
  }
  else {
    fprintf (stderr, "%s: unknown mode\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return LISP_RET_TRUE;
}

int process_random (int argc, char **argv)
{
  if (argc == 1) {
    glob_sim->setRandom();
  }
  else if (argc == 3) {
    glob_sim->setRandom (atoi (argv[1]), atoi(argv[2]));
  }
  else {
    fprintf (stderr, "Usage: %s [min max]\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return LISP_RET_TRUE;
}

int process_norandom (int argc, char **argv)
{
  if (argc == 1) {
    glob_sim->setNoRandom();
  }
  else {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return LISP_RET_TRUE;
}

int process_random_seed (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s <val>\n", argv[0]);
    return LISP_RET_ERROR;
  }
  glob_sim->setRandomSeed (atoi (argv[1]));
  return LISP_RET_TRUE;
}

int process_random_choice (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "Usage: %s on|off\n", argv[0]);
    return LISP_RET_ERROR;
  }
  if (strcmp (argv[1], "on") == 0) {
    glob_sim->setRandomChoice (1);
  }
  else if (strcmp (argv[1], "off") == 0) {
    glob_sim->setRandomChoice (0);
  }
  else {
    fprintf (stderr, "Usage: %s on|off\n", argv[0]);
    return LISP_RET_ERROR;
  }
  return LISP_RET_TRUE;
}

int process_break_on_warn (int argc, char **argv)
{
  if (argc != 1) {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return LISP_RET_ERROR;
  }
  glob_sim->setWarning (1);
  return LISP_RET_TRUE;
}

int process_exit_on_warn (int argc, char **argv)
{
  if (argc != 1) {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return LISP_RET_ERROR;
  }
  glob_sim->setWarning (2);
  return LISP_RET_TRUE;
}

int process_resume_on_warn (int argc, char **argv)
{
  if (argc != 1) {
    fprintf (stderr, "Usage: %s\n", argv[0]);
    return LISP_RET_ERROR;
  }
  glob_sim->setWarning (0);
  return LISP_RET_TRUE;
}

static void _compute_status (ActInstTable *tab, int val)
{
  if (tab->obj) {
    tab->obj->printStatus (val);
  }
  if (tab->H) {
    hash_iter_t it;
    hash_bucket_t *b;
    hash_iter_init (tab->H, &it);
    while ((b = hash_iter_next (tab->H, &it))) {
      _compute_status ((ActInstTable *)b->v, val);
    }
  }
}

int process_status (int argc, char **argv)
{
  int val;
  
  if (argc != 2) {
    fprintf (stderr, "Usage: %s 0|1|X\n", argv[0]);
    return LISP_RET_ERROR;
  }
  if (strcmp (argv[1], "0") == 0) {
    val = 0;
  }
  else if (strcmp (argv[1], "1") == 0) {
    val = 1;
  }
  else if (strcmp (argv[1], "X") == 0 || strcmp (argv[1], "U") == 0) {
    val = 2;
  }
  else {
    fprintf (stderr, "Usage: %s 0|1|X\n", argv[0]);
    return LISP_RET_ERROR;
  }
  _compute_status (glob_sim->getInstTable(), val);
  return LISP_RET_TRUE;
}
  


struct LispCliCommand Cmds[] = {
  { NULL, "Initialization and setup", NULL },

  { "echo", "[-n] args - display to screen", process_echo },
  { "error", "<str> - report error and abort execution", process_error },
  
  { "initialize", "<proc> - initialize simulation for <proc>",
    process_initialize },

  { "mode", "reset|run - set running mode", process_mode },
  
  { "random", "[min max] - randomize timings", process_random },
  { "random_seed", "<val> - set random number seed", process_random_seed },
  { "norandom", "- deterministic timing", process_norandom },
  { "random_choice", "on|off - randomize non-deterministic choices", process_random_choice },

#if 0
  { "rand_init", "- randomly set signals that are X for rand_init signals",
    process_rand_init },
  { "dumptc", "<file> - dump transition counts to a file", process_dumptc },
#endif

  { NULL, "Running simulation", NULL },

  { "step", "[n] - run the next [n] events", process_step },
  { "advance", "<delay> - run for <delay> time", process_advance },
  { "cycle", "- run until simulation stops", process_cycle },

  { "set", "<name> <val> - set a variable to a value", process_set },
  { "get", "<name> [#f] - get value of a variable; optional arg turns off display", process_get },

  { "watch", "<n> - add watchpoint for <n>", process_watch },
  { "unwatch", "<n> - delete watchpoint for <n>", process_unwatch },
  { "breakpt", "<n> - add breakpoint for <n>", process_breakpt },

  { "break-on-warn", "- stop simulation on warning", process_break_on_warn },
  { "exit-on-warn", "- like break-on-warn, but exit", process_exit_on_warn },
  { "resume-on-warn", "- like break-on-warn, but exit", process_resume_on_warn },

  { "status", "0|1|X - list all nodes with specified value", process_status },
  
#if 0  
  { "pending", "- dump pending events", process_pending },

  { NULL, "Production rule tracing", NULL },

  { "fanin", "<n> - list fanin for <n>", process_fanin },
  { "fanin-get", "<n> - list fanin with values for <n>", process_fanin_get },
  { "fanout", "<n> - list fanout for <n>", process_fanout },
  
#endif  

  { NULL, "Process and CHP commands", NULL },

#if 0
  { "send", "<chan> <val> - send a value on a channel", process_send },
  { "recv", "<chan> [#f] - receive a value from a channel; optional arg turns off display", process_recv },
#endif

  { "filter", "<regexp> - only show log messages that match regexp", process_filter },
  { "logfile", "<file> - dump actsim log output to a log file <file>", process_logfile },
  
  { "procinfo", "<filename> [<inst-name>] - save the program counter for a process to file (- for stdout)", process_procinfo },
  { "energy", "<filename> [<inst-name>] - save energy usage to file (- for stdout)", process_getenergy }
};


int debug_metrics;

int main (int argc, char **argv)
{
  char *proc;

  config_set_default_int ("sim.chp.default_delay", 10);
  config_set_default_int ("sim.chp.default_energy", 0);
  config_set_default_real ("sim.chp.default_leakage", 0);
  config_set_default_int ("sim.chp.default_area", 0);
  config_set_default_int ("sim.chp.debug_metrics", 0);

  /* initialize ACT library */
  Act::Init (&argc, &argv);

  debug_metrics = config_get_int ("sim.chp.debug_metrics");

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
    p = p->Expand (ActNamespace::Global(), p->CurScope(), 0, NULL);
  }

  if (!p->isExpanded()) {
    fatal_error ("Process `%s' is not expanded.", argv[2]);
  }

  /* do stuff here */
  glob_sp = new ActStatePass (glob_act);
  glob_sp->run (p);
  
  glob_sim = new ActSim (p);
  glob_sim->runInit ();

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
  
  delete glob_sim;

  return 0;
}
