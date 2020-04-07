# pg_timeout

PostgreSQL extension to manage database idle session timeout.

# Installation 
## Compiling

This module can be built using the standard PGXS infrastructure. For this to work, the pg_config program must be available in your $PATH:

`git clone https://github.com/pierreforstmann/pg_timeout.git` <br>
`cd pg_timeout` <br>
`make` <br>
`make install` <br>

This extension has been validated with PostgresSQL 9.5, 9.6, 10, 11 and 12.

## PostgreSQL setup

Extension can be loaded at server level with `shared_preload_libraries` parameter: <br>
`shared_preload_libraries = 'pg_timeout'`

# Usage

pg_timeout has 2 specific GUC: <br>
- `pg_timeout.naptime`: number of seconds for the dedicated backgroud worker to sleep between idle session checks (default value is 10 seconds)<br>
- `pg_timeout.idle_session_timeout`: database session idle timeout in seconds (default value is 60 seconds)<br>

Note that pg_timeout only takes care of database session with idle status (idle in transaction is not taken into account).

## Example

Add in postgresql.conf: <br>
`shared_preload_libraries = 'pg_timeout'` <br>
`pg_timeout.naptime=30` <br>
`pg_timeout.idle_session_timeout=30` <br>

Any database session with is idle for more than 30 seconds is killed. In database instance log you get messages similar to: <br>
`LOG:  pg_timeout_worker: idle session PID=26546 user=pierre database=pierre application=psql hostname=NULL` <br>
`LOG:  pg_timeout_worker: idle session(s) since 30 seconds terminated` <br>
`FATAL:  terminating connection due to administrator command`

If the database session was started by psql, you get:

`FATAL:  terminating connection due to administrator command` <br>
`server closed the connection unexpectedly` <br>
`This probably means the server terminated abnormally` <br>
`before or while processing the request.` <br>
`The connection to the server was lost. Attempting reset: Succeeded.` <br>

 

