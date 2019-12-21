#ident "$Id: sms.c,v 1.3 2010/09/22 12:06:55 gert Exp $"

/* sms.c - part of mgetty+sendfax
 *
 * this module is used when the modem sends a string that triggers one of 
 * the "A_SMS_*" actions - typically this should be "+CMTI" or "+CDSI".
 *
 * the incoming SMS message or transmission report is retrieved from
 * the modem, and then passed to "sms-handler <script>" (if configured)
 *
 * $Log: sms.c,v $
 * Revision 1.3  2010/09/22 12:06:55  gert
 * change from simple fork()/system() to double-fork()/exec() -> guarantee
 * that there are no zombies, and also no mgetty parent processes hanging around
 *
 * Revision 1.2  2010/09/22 09:06:46  gert
 * add (int) cast to avoid int/long issue on 64bit AIX
 *
 * Revision 1.1  2010/09/14 21:13:44  gert
 * basic handler for "retrieve incoming SMS from modem, hand to script"
 *
 */

#include <stdio.h>
#include "syslibs.h"
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "mgetty.h"
#include "policy.h"
#include "tio.h"

#ifdef SMS

/* TODO */
extern boolean fwf_timeout;
extern RETSIGTYPE fwf_sig_alarm(SIG_HDLR_ARGS);

extern char **environ;

/* helper function: fork() twice, so that grandchild process is 
 * inherited by init and won't cause zombies
 */
pid_t doublefork _P0(void)
{
pid_t p;
    p = fork();

    if ( p == -1 )
	{ lprintf( L_ERROR, "can't fork()" ); return -1; }

    if ( p != 0 )	/* parent */
    {
	int status;
	wait( &status );
	return p;
    }

    /* child -> fork grandchild, exit */
    p = fork();

    if ( p == -1 )
	{ lprintf( L_ERROR, "can't fork() [child]" ); return -1; }

    if ( p != 0 )	/* original child */
	{ exit(0); }

    /* grandchild */
    return 0;
}

int handle_incoming_sms _P5( (is_report, fd, sms_handler, uid, gid ),
			      boolean is_report, int fd, 
			      char * sms_handler, uid_t uid, gid_t gid )
{
char *s, *p;
int memloc, i;
char cmdbuf[50];
char meta_buf[200], text_buf[200];

    /* timeout protect the whole thing */
    signal( SIGALRM, fwf_sig_alarm ); alarm(15); fwf_timeout = FALSE;

    s =  mdm_get_line( fd );
    if ( s == NULL )
    {
        alarm(0); signal( SIGALRM, SIG_DFL );
	errno = EINVAL;
	lprintf( L_ERROR, "handle_incoming_sms: message data missing" );
	return ERROR;
    }

    lprintf( L_MESG, "handle_incoming_sms: '%s'", s );

    /* incoming SMS:      +CMTI:"MT",4
     * SMS status report: +CDSI:"MT",1
     * "MT" = "where this report is stored"
     * "4"  = "the number of the storage"
     *
     * -> retrieve with AT+CMGR=<n>, delete with AT+CMGD=<n>
     */

    p = strchr( s, ',' );	/* Komma in string, and digit next? */
    if ( p == NULL || !isdigit( *(p+1) ) )
    {
        alarm(0); signal( SIGALRM, SIG_DFL );
	errno = EINVAL;
	lprintf( L_ERROR, "handle_incoming_sms: error parsing SMS notification '%s'", s );
	return ERROR;
    }

    memloc = atoi( p+1 );

    /* retrieve SMS meta-data and text */
    sprintf( cmdbuf, "AT+CMGR=%d", memloc );
    if ( mdm_send( cmdbuf, fd ) == ERROR )
    {
        alarm(0); signal( SIGALRM, SIG_DFL ); return ERROR;
    }

    i=0;
    meta_buf[0]=text_buf[0]='\0';

    while(1)
    {
	s = mdm_get_line( fd );
	if ( s == NULL ) break;				/* error */
	if ( strcmp( s, cmdbuf ) == 0 ) continue;	/* command echo */
	if ( strcmp( s, "OK" ) == 0 ||			/* final string */
	     strcmp( s, "ERROR" ) == 0 ) break;

	lprintf( L_NOISE, "%d: %s", i, s );
	if ( i == 0 )					/* meta data */
	    strncpy( meta_buf, s, sizeof(meta_buf)-1 );
	else						/* message */
	    strncpy( text_buf, s, sizeof(text_buf)-1 );
	i++;
    }

    alarm(0); signal( SIGALRM, SIG_DFL );

    /* force zero-termination */
    meta_buf[ sizeof(meta_buf)-1 ]='\0';
    text_buf[ sizeof(text_buf)-1 ]='\0';

    /* delete message (has its own timeout handling) */
    sprintf( cmdbuf, "AT+CMGD=%d", memloc );
    mdm_command( cmdbuf, fd );

    if ( s == NULL )
    {
	lprintf( L_WARN, "unknown failure in incoming SMS" );
	return ERROR;
    }

    lprintf( L_AUDIT, "incoming SMS: '%s', '%s'", meta_buf, text_buf );

    if ( sms_handler == NULL )			/* nothing more to do */
	return NOERROR;

    /* fork child (double-forking, so we don't have zombies)
     *
     * child will detach from tty, attach to console, setuid, exec() handler
     */

    switch( doublefork() )
    {
	case 0:		/* child */
	    /* detach from controlling tty -> no SIGHUP */
	    close( 0 ); close( 1 ); close( 2 );
#if defined(BSD) || defined(sunos4)
	    setpgrp( 0, getpid() );
	    if ( ( i = open( "/dev/tty", O_RDWR ) ) >= 0 )
	    {
		ioctl( i, TIOCNOTTY, NULL );
		close( i );
	    }
#else
	    setpgrp();
#endif

	    /* connect stdin to /dev/null, stdout+stderr to console */
	    if ( ( fd = open( "/dev/null", O_RDWR )) != 0 )
		{ lprintf( L_ERROR, "can't connect /dev/null to stdin" ); }

	    if ( ( fd = open( CONSOLE, O_WRONLY ) ) != 1 )	/* stdout */
		{ lprintf( L_ERROR, "can't connect CONSOLE to stdout" ); }
	    else
		{ dup2( fd, 2 ); }				/* stderr */

	    /* set UIDs (don't run scripts as root) - ignore errors
             */
#if SECUREWARE
	    if ( setluid( uid ) == -1 )
		lprintf( L_ERROR, "cannot set LUID %d", uid );
#endif
	    if ( setgid( gid ) == -1 )
		lprintf( L_ERROR, "cannot set gid %d", gid );
	    if ( setuid( uid ) == -1 )
		lprintf( L_ERROR, "cannot set uid %d", uid );

	    setup_environment();

	    if ( is_report )
	    {
		set_env_var( "SMS_REPORT", meta_buf );
	    }
	    else
	    {
		set_env_var( "SMS_META", meta_buf );
		set_env_var( "SMS_TEXT", text_buf );
	    }

	    lprintf( L_NOISE, "notify: '%.320s', uid=%d, gid=%d", 
				sms_handler, (int) uid, (int) gid );

	    execle( sms_handler, sms_handler, (char*)NULL, environ );

	    lprintf( L_ERROR, "exec() failed" );
	    exit(0);
	case -1:
	    lprintf( L_ERROR, "fork() failed" );
	    break;
    }

    /* parent -> return, back to your regular program */
    sleep(1);
    return NOERROR;
}

#endif	/* SMS */
