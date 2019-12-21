#ident "$Id: socket.c,v 4.2 2014/01/28 13:44:14 gert Exp $"

/* socket.c - part of mgetty+sendfax
 *
 * open TCP/IP sockets to talk to remote fax modems (sendfax)
 *
 * $Log: socket.c,v $
 * Revision 4.2  2014/01/28 13:44:14  gert
 * log_init_path() with infix => last 4 digits of "remote tty" (R104, etc.)
 *
 * set SIGPIPE to SIG_IGN - if the remote end of the socket is closed
 * unexpectedly, write() might return an error, or raise SIGPIPE -> code
 * handles errors, so ignore SIGPIPE.
 *
 * Revision 4.1  2014/01/28 13:15:50  gert
 * Basic "open TCP/IP socket to remote machine" implementation
 * (name of remote + tcp portnumber are magick'ed out of ttyRI<n><mm>)
 *
 */

#include "policy.h"
#ifdef FAX_SEND_SOCKETS

#include <stdio.h>
#include "syslibs.h"
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <netdb.h>
#include <netinet/in.h>

#include "mgetty.h"

extern boolean verbose;		/* sendfax.c */
extern char * Device;		/* sendfax.c */

/* sendfax' logic for "what is a remote tty" is currently hardwired
 * (to be able to actually get something in place, without rewriting 
 * most of the config file infrastructure) - if the tty name matches
 * "ttyRInmm", it will be treated as "connect to 'modemserver<n>' on
 * port 50000+<mm>, raw socket, no modem control / telnet escapes"
 *
 * This might get enhanced later on with modem control (RFC 2217) or 
 * configfile-settable ports.  For now, we start with *this*.
 *
 * return value: -1 -> fail, >= 0 -> socket FD
 */

int connect_to_remote_tty _P1( (fax_tty), char * fax_tty )
{
    int line, r;
    int sock = -1;
    char hostname[20];
    char servname[10];
    struct addrinfo hints;
    struct addrinfo * res0, *res;

    /* ttyRI<n> */
    if ( !isdigit( (int) fax_tty[5] ) )
    {
	lprintf( L_ERROR, "invalid tty format: '%s' - 6th char must be 0..9", fax_tty );
	return -1;
    }
    sprintf( hostname, "modemserver%c", fax_tty[5] );

    line = atoi( &fax_tty[6] );
    if ( line < 1 || line > 999 )
    {
	lprintf( L_ERROR, "invalid tty format: '%s' - port number must be 01..999", fax_tty );
	return -1;
    }
    sprintf( servname, "%d", line + 50000 );

    lprintf( L_MESG, "trying '%s' on port %s...", hostname, servname );
    if (verbose) putchar( '\n' );

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV;
    r = getaddrinfo( hostname, servname, &hints, &res0 );

    if ( r != 0 )
    {
	errno = 0;
	lprintf( L_ERROR, "getaddrinfo(%s) failed: %s", hostname, gai_strerror(r));
	return -1;
    }

    /* connect loop from "man getaddrinfo"... */
    for ( res = res0; res != NULL; res = res->ai_next )
    {
	char nbuf[NI_MAXHOST];
	r = getnameinfo( res->ai_addr, res->ai_addrlen, nbuf, sizeof(nbuf)-1, 
		         NULL, 0, NI_NUMERICHOST );
	if ( r != 0 )
	    { lprintf( L_ERROR, "error in getnameinfo (canthappen!)" ); continue; }

        if ( verbose ) 
	    { printf( "  connecting to %s... ", nbuf ); fflush(stdout); }

	sock = socket( res->ai_family, res->ai_socktype, res->ai_protocol );
	if ( sock < 0 )
		{ lprintf( L_ERROR, "error in socket()" ); continue; }

	if ( connect( sock, res->ai_addr, res->ai_addrlen ) < 0 )
	{
	    int save_errno = errno;
	    lprintf( L_ERROR, "error in connect(%s:%s)", nbuf, servname );
	    if ( verbose )
		printf( "connect failed: %s\n", strerror(save_errno) );
	    close(sock);
	    sock = -1;
	    continue;
	}

	if ( verbose ) printf( "OK\n" );
	lprintf( L_MESG, "connection to '%s':'%s' succeeded, sock=%d",
			nbuf, servname, sock );
	break;
    }

    freeaddrinfo(res0);

    if ( sock >= 0 )		/* success? */
    {
	/* make device name externally visible (faxrec())
	 */
	Device = safe_strdup( fax_tty );

	log_init_paths( NULL, NULL, &fax_tty[ strlen(fax_tty)-4 ] );

	/* The rest of the code assumes that a tty fd might return an 
         * error write()ing to it, but not that it might go away completely 
         * and give us an SIGPIPE.  Ignore.  Sue me.
         */
	signal( SIGPIPE, SIG_IGN );
    }

    /* sock is either a connected socket now, or "-1" -> pass up */
    return sock;
}


#endif	/* FAX_SEND_SOCKETS */
