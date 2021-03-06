 /*****************************************************************************\
 *  jobacct_gather_aix.c - slurm job accounting gather plugin for AIX.
 *****************************************************************************
 *
 *  Copyright (C) 2005 Hewlett-Packard Development Company, L.P.
 *  Written by Andy Riebs, <andy.riebs@hp.com>, who borrowed heavily
 *  from other parts of SLURM, and Danny Auble, <da@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.schedmd.com/slurmdocs/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
 *
 *  This file is patterned after jobcomp_linux.c, written by Morris Jette and
 *  Copyright (C) 2002 The Regents of the University of California.
\*****************************************************************************/

#include <signal.h>
#include "src/common/slurm_xlator.h"
#include "src/common/slurm_jobacct_gather.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/slurmd/common/proctrack.h"

#ifdef HAVE_AIX
#include <procinfo.h>
#include <sys/types.h>
#define NPROCS 5000
#endif

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "jobacct" for SLURM job completion logging) and <method>
 * is a description of how this plugin satisfies that application.  SLURM will
 * only load job completion logging plugins if the plugin_type string has a
 * prefix of "jobacct/".
 *
 * plugin_version - an unsigned 32-bit integer giving the version number
 * of the plugin.  If major and minor revisions are desired, the major
 * version number may be multiplied by a suitable magnitude constant such
 * as 100 or 1000.  Various SLURM versions will likely require a certain
 * minimum version for their plugins as the job accounting API
 * matures.
 */
const char plugin_name[] = "Job accounting gather AIX plugin";
const char plugin_type[] = "jobacct_gather/aix";
const uint32_t plugin_version = 200;

/* Other useful declarations */
static int pagesize = 0;

#ifdef HAVE_AIX
typedef struct prec {	/* process record */
	pid_t   pid;
	pid_t   ppid;
	int     usec;   /* user cpu time */
	int     ssec;   /* system cpu time */
	int     pages;  /* pages */
	float	rss;	/* maxrss */
	float	vsize;	/* max virtual size */
} prec_t;

/* Finally, pre-define all the routines. */

static void _get_offspring_data(List prec_list, prec_t *ancestor, pid_t pid);
static void _destroy_prec(void *object);

/* system call to get process table */
extern int getprocs(struct procsinfo *procinfo, int, struct fdsinfo *,
		    int, pid_t *, int);
    /* procinfo:   pointer to array of procinfo struct */
    /* nproc:      number of user procinfo struct */
    /* sizproc:    size of expected procinfo structure */


/*
 * _get_offspring_data() -- collect memory usage data for the offspring
 *
 * For each process that lists <pid> as its parent, add its memory
 * usage data to the ancestor's <prec> record. Recurse to gather data
 * for *all* subsequent generations.
 *
 * IN:	prec_list       list of prec's
 * 	ancestor	The entry in prec_list to which the data
 * 			should be added. Even as we recurse, this will
 * 			always be the prec for the base of the family
 * 			tree.
 * 	pid		The process for which we are currently looking
 * 			for offspring.
 *
 * OUT:	none.
 *
 * RETVAL:	none.
 *
 * THREADSAFE! Only one thread ever gets here.
 */
static void _get_offspring_data(List prec_list, prec_t *ancestor, pid_t pid)
{
	ListIterator itr;
	prec_t *prec = NULL;

	itr = list_iterator_create(prec_list);
	while((prec = list_next(itr))) {
		if (prec->ppid == pid) {
			_get_offspring_data(prec_list, ancestor, prec->pid);
			debug2("adding %d to %d rss = %f vsize = %f",
			      prec->pid, ancestor->pid,
			      prec->rss, prec->vsize);
			ancestor->usec += prec->usec;
			ancestor->ssec += prec->ssec;
			ancestor->pages += prec->pages;
			ancestor->rss += prec->rss;
			ancestor->vsize += prec->vsize;
		}
	}
	list_iterator_destroy(itr);

	return;
}

/*
 * jobacct_gather_p_poll_data() - Build a table of all current processes
 *
 * IN/OUT: task_list - list containing current processes.
 * IN: pgid_plugin - if we are running with the pgid plugin.
 * IN: cont_id - container id of processes if not running with pgid.
 *
 * OUT:	none
 *
 * THREADSAFE! Only one thread ever gets here.  It is locked in
 * slurm_jobacct_gather.
 *
 * Assumption:
 *    Any file with a name of the form "/proc/[0-9]+/stat"
 *    is a Linux-style stat entry. We disregard the data if they look
 *    wrong.
 */
extern void jobacct_gather_p_poll_data(
	List task_list, bool pgid_plugin, uint64_t cont_id)
{
	struct procsinfo proc;
	pid_t *pids = NULL;
	int npids = 0;
	int i;
	uint32_t total_job_mem = 0, total_job_vsize = 0;
	int pid = 0;
	static int processing = 0;
	prec_t *prec = NULL;
	struct jobacctinfo *jobacct = NULL;
	List prec_list = NULL;
	ListIterator itr;
	ListIterator itr2;

	if (!pgid_plugin && (cont_id == (uint64_t)NO_VAL)) {
		debug("cont_id hasn't been set yet not running poll");
		return;
	}

	if (processing) {
		debug("already running, returning");
		return;
	}

	processing = 1;
	prec_list = list_create(_destroy_prec);

	if (!pgid_plugin) {
		/* get only the processes in the proctrack container */
		slurm_container_get_pids(cont_id, &pids, &npids);
		if (!npids) {
			debug4("no pids in this container %"PRIu64"", cont_id);
			goto finished;
		}
		for (i = 0; i < npids; i++) {
			pid = pids[i];
			if (!getprocs(&proc, sizeof(proc), 0, 0, &pid, 1))
				continue; /* Assume the process went away */
			prec = xmalloc(sizeof(prec_t));
			list_append(prec_list, prec);
			prec->pid = proc.pi_pid;
			prec->ppid = proc.pi_ppid;
			prec->usec = proc.pi_ru.ru_utime.tv_sec +
				proc.pi_ru.ru_utime.tv_usec * 1e-6;
			prec->ssec = proc.pi_ru.ru_stime.tv_sec +
				proc.pi_ru.ru_stime.tv_usec * 1e-6;
			prec->pages = proc.pi_majflt;
			prec->rss = (proc.pi_trss + proc.pi_drss) * pagesize;
			//prec->rss *= 1024;
			prec->vsize = (proc.pi_tsize / 1024);
			prec->vsize += (proc.pi_dvm * pagesize);
			//prec->vsize *= 1024;
			/*  debug("vsize = %f = (%d/1024)+(%d*%d)",   */
/*    		      prec->vsize, proc.pi_tsize, proc.pi_dvm, pagesize);  */
		}
	} else {
		while (getprocs(&proc, sizeof(proc), 0, 0, &pid, 1) == 1) {
			prec = xmalloc(sizeof(prec_t));
			list_append(prec_list, prec);
			prec->pid = proc.pi_pid;
			prec->ppid = proc.pi_ppid;
			prec->usec = proc.pi_ru.ru_utime.tv_sec +
				proc.pi_ru.ru_utime.tv_usec * 1e-6;
			prec->ssec = proc.pi_ru.ru_stime.tv_sec +
				proc.pi_ru.ru_stime.tv_usec * 1e-6;
			prec->pages = proc.pi_majflt;
			prec->rss = (proc.pi_trss + proc.pi_drss) * pagesize;
			//prec->rss *= 1024;
			prec->vsize = (proc.pi_tsize / 1024);
			prec->vsize += (proc.pi_dvm * pagesize);
			//prec->vsize *= 1024;
			/*  debug("vsize = %f = (%d/1024)+(%d*%d)",   */
/*    		      prec->vsize, proc.pi_tsize, proc.pi_dvm, pagesize);  */
		}
	}
	if (!list_count(prec_list))
		goto finished;

	slurm_mutex_lock(&jobacct_lock);
	if (!task_list || !list_count(task_list)) {
		slurm_mutex_unlock(&jobacct_lock);
		goto finished;
	}
	itr = list_iterator_create(task_list);
	while ((jobacct = list_next(itr))) {
		itr2 = list_iterator_create(prec_list);
		while ((prec = list_next(itr2))) {
			//debug2("pid %d ? %d", prec->ppid, jobacct->pid);
			if (prec->pid == jobacct->pid) {
				/* find all my descendents */
				_get_offspring_data(prec_list, prec,
						    prec->pid);

				/* tally their usage */
				jobacct->max_rss = jobacct->tot_rss =
					MAX(jobacct->max_rss, (int)prec->rss);
				total_job_mem += jobacct->max_rss;
				jobacct->max_vsize = jobacct->tot_vsize =
					MAX(jobacct->max_vsize,
					    (int)prec->vsize);
				total_job_vsize += prec->vsize;
				jobacct->max_pages = jobacct->tot_pages =
					MAX(jobacct->max_pages, prec->pages);
				jobacct->min_cpu = jobacct->tot_cpu =
					MAX(jobacct->min_cpu,
					    (prec->usec + prec->ssec));
				debug2("%d size now %d %d time %d",
				      jobacct->pid, jobacct->max_rss,
				      jobacct->max_vsize, jobacct->tot_cpu);

				break;
			}
		}
		list_iterator_destroy(itr2);
	}
	list_iterator_destroy(itr);

	jobacct_gather_handle_mem_limit(total_job_mem, total_job_vsize);

finished:
	list_destroy(prec_list);
	processing = 0;

	return;
}

static void _destroy_prec(void *object)
{
	prec_t *prec = (prec_t *)object;
	xfree(prec);
	return;
}

#endif

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init ( void )
{
	pagesize = getpagesize()/1024;

	verbose("%s loaded", plugin_name);
	return SLURM_SUCCESS;
}

extern int fini ( void )
{
	return SLURM_SUCCESS;
}

extern int jobacct_gather_p_endpoll ( void )
{
	return SLURM_SUCCESS;
}


extern int jobacct_gather_p_add_task(pid_t pid, jobacct_id_t *jobacct_id)
{
	return SLURM_SUCCESS;
}
