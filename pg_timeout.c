/* -------------------------------------------------------------------------
 *
 * pg_timeout.c
 * 
 * Background code to handle session timeout.
 *
 * This code is reusing worker_spi.c code from PostgresSQL code.
 *
 * Copyright 2020 Pierre Forstmann
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* these headers are used by this particular worker's code */
#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "tcop/utility.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_timeout_main);

void		_PG_init(void);

/* 
 * flags set by signal handlers 
 * */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* GUC variables */

/*
 * parameter default value in seconds set by _PG_init 
 */
static int	pg_timeout_idle_session_timeout = 0;
static int	pg_timeout_naptime = 0;

#define LOG_MESSAGE "%s: idle session PID=%d user=%s database=%s application=%s hostname=%s"
static char 	*null_value="NULL";
/*
 * Signal handler for SIGTERM
 *		Set a flag to let the main loop to terminate, and set our latch to wake
 *		it up.
 */
static void
pg_timeout_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 *		Set a flag to tell the main loop to reread the config file, and set
 *		our latch to wake it up.
 */
static void
pg_timeout_sighup(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sighup = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

Datum
pg_timeout_main(PG_FUNCTION_ARGS)
{
	StringInfoData 	buf_select;
	StringInfoData 	buf_kill;

	char		*usename_val;
	char		*datname_val;
	char		*application_name_val;
	char		*client_hostname_val;	

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, pg_timeout_sighup);
	pqsignal(SIGTERM, pg_timeout_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
#if PG_VERSION_NUM >=110000
	BackgroundWorkerInitializeConnection("postgres", NULL, 0);
#else
	BackgroundWorkerInitializeConnection("postgres", NULL);
#endif
	elog(LOG, "%s initialized", MyBgworkerEntry->bgw_name);

	/*
	 */

	/* In PG 9.5 and 9.6 only client backend are taken into account.
 	*  In PG 10 and above, background workers are also taken into account 
 	*  but with state and stage_change set to null:
 	*  no need to filter on backend_type.
 	*/
	initStringInfo(&buf_select);
	appendStringInfo(&buf_select,
					"SELECT pid, usename, datname, application_name, client_hostname "
    					"FROM pg_stat_activity "
    					"WHERE pid <> pg_backend_pid() "
      					"AND state = 'idle' "
      					"AND state_change < current_timestamp - INTERVAL '%d' SECOND",
					pg_timeout_idle_session_timeout
					);
	initStringInfo(&buf_kill);
	appendStringInfo(&buf_kill,
					"SELECT pg_terminate_backend(pid) "
    					"FROM pg_stat_activity "
    					"WHERE pid <> pg_backend_pid() "
      					"AND state = 'idle' "
      					"AND state_change < current_timestamp - INTERVAL '%d' SECOND",
					pg_timeout_idle_session_timeout
					);
	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */

	while (!got_sigterm)
	{
		int			ret;
		int			rc;
		int			nr;
		int			i;

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
#if PG_VERSION_NUM >= 100000
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   pg_timeout_naptime * 1000L,
					   PG_WAIT_EXTENSION);
#else
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   pg_timeout_naptime * 1000L);
#endif
		ResetLatch(MyLatch);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		CHECK_FOR_INTERRUPTS();

		/*
		 * In case of a SIGHUP, just reload the configuration.
		 */
		if (got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * Start a transaction on which we can run queries.  Note that each
		 * StartTransactionCommand() call should be preceded by a
		 * SetCurrentStatementStartTimestamp() call, which sets both the time
		 * for the statement we're about the run, and also the transaction
		 * start time.  Also, each other query sent to SPI should probably be
		 * preceded by SetCurrentStatementStartTimestamp(), so that statement
		 * start time is always up to date.
		 *
		 * The SPI_connect() call lets us run queries through the SPI manager,
		 * and the PushActiveSnapshot() call creates an "active" snapshot
		 * which is necessary for queries to have MVCC data to work on.
		 *
		 * The pgstat_report_activity() call makes our activity visible
		 * through the pgstat views.
		 */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());
		pgstat_report_activity(STATE_RUNNING, buf_select.data);

		/* We can now execute queries via SPI */
		ret = SPI_execute(buf_select.data, false, 0);

		if (ret != SPI_OK_SELECT)
			elog(FATAL, "cannot select from pg_stat_activity: error code %d",
				     ret);
		nr = SPI_processed;		

		i = 0;
		for (i = 0; i < nr; i++)
		{
			bool		isnull;
			int32		pid_val;

			pid_val = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[i],
							  SPI_tuptable->tupdesc,
							  1, &isnull));
			if (!isnull)
			{
				usename_val = SPI_getvalue(SPI_tuptable->vals[i],
							    SPI_tuptable->tupdesc,
							    2);
				datname_val = SPI_getvalue(SPI_tuptable->vals[i],
							    SPI_tuptable->tupdesc,
							    3);
				application_name_val = SPI_getvalue(SPI_tuptable->vals[i],
							    SPI_tuptable->tupdesc,
							    4);
				client_hostname_val = SPI_getvalue(SPI_tuptable->vals[i],
							    SPI_tuptable->tupdesc,
							    5);
				/* 
  				* client_hostname_val is NULL pointer because column client_hostname is null.
  				*/
				if (usename_val == NULL)
					usename_val = null_value;
				if (datname_val == NULL)
					datname_val = null_value;
				if (application_name_val == NULL)
					client_hostname_val = null_value;
				if (client_hostname_val == NULL)
					client_hostname_val = null_value;
				
					elog(LOG, LOG_MESSAGE, 
						 MyBgworkerEntry->bgw_name, pid_val, usename_val,
                	                         datname_val, application_name_val, 
                        	                 client_hostname_val);
			}
			else	elog(WARNING, "%s: pid is NULL",
					 MyBgworkerEntry->bgw_name);
		}

		if (nr > 0)
		{
			ret = SPI_execute(buf_kill.data, false, 0);
			if (ret != SPI_OK_SELECT)
				elog(FATAL, "cannot select pg_terminate_backend: error code %d",
					     ret);

			elog(LOG, "%s: idle session(s) since %d seconds terminated", 
				  MyBgworkerEntry->bgw_name,
        	                  pg_timeout_idle_session_timeout);
		}

		/*
		 * And finish our transaction.
		 */
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		pgstat_report_stat(false);
		pgstat_report_activity(STATE_IDLE, NULL);
	}

	proc_exit(1);
}

/*
 * Entrypoint of this module.
 *
 */
void
_PG_init(void)
{
	BackgroundWorker worker;

	/* get the configuration */
	DefineCustomIntVariable("pg_timeout.naptime",
							"Duration between each check (in seconds).",
							NULL,
							&pg_timeout_naptime,
							10,
							1,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomIntVariable("pg_timeout.idle_session_timeout",
							"Maximum idle session time.",
							NULL,
							&pg_timeout_idle_session_timeout,
							60,
							1,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	/* set up common data for all our workers */
	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = pg_timeout_naptime;
	sprintf(worker.bgw_library_name, "pg_timeout");
	sprintf(worker.bgw_function_name, "pg_timeout_main");
	worker.bgw_notify_pid = 0;

	snprintf(worker.bgw_name, BGW_MAXLEN, "pg_timeout_worker");
#if PG_VERSION_NUM >= 110000
	snprintf(worker.bgw_type, BGW_MAXLEN, "pg_timeout");
#endif
	worker.bgw_main_arg = 0;

	RegisterBackgroundWorker(&worker);

	elog(LOG, "%s started with pg_timeout.naptime=%d seconds", 
                  worker.bgw_name,
                  pg_timeout_naptime);

	elog(LOG, "%s started with pg_timeout.idle_session_timeout=%d seconds", 
                  worker.bgw_name,
                  pg_timeout_idle_session_timeout);
}

