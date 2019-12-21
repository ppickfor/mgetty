#ident "$Id: clean_line.c,v 4.1 2014/01/31 13:02:08 gert Exp $ Copyright (c) Gert Doering"

/* clean_line.c
 *
 * auxiliary function that doesn't properly fit anywhere else
 * "wait until no more data is available on a tty line"
 *
 * used from mgetty and sendfax (but not from tap, microcom, etc.)
 */

#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#include "mgetty.h"
#include "policy.h"
#include "tio.h"


/* clean_line()
 *
 * wait for the line "fd" to be silent for "waittime" tenths of a second
 * if more than 500 bytes are read, stop logging them. We don't want to
 * have the log files fill up all of the hard disk.
 */

int clean_line _P2 ((fd, waittime), int fd, int waittime )
{
    char buffer[2];
    int	 bytes = 0;				/* bytes read */

#if defined(MEIBE) || defined(NEXTSGTTY) || defined(BROKEN_VTIME) ||\
	defined(FAX_SEND_SOCKETS)
    /* on some systems, the VMIN/VTIME mechanism is obviously totally
     * broken. So, use a select()/flush queue loop instead.
     */
    lprintf( L_NOISE, "waiting for line to clear (select/%d ms), read: ", waittime * 100 );

    while( wait_for_input( fd, waittime * 100 ) &&
	   read( fd, buffer, 1 ) > 0 &&
	   bytes < 10000 )
    {
	if ( ++bytes < 500 ) lputc( L_NOISE, buffer[0] );
    }
#else
TIO	tio, save_tio;

    lprintf( L_NOISE, "waiting for line to clear (VTIME=%d), read: ", waittime);

    /* set terminal timeout to "waittime" tenth of a second */
    tio_get( fd, &tio );
    save_tio = tio;				/*!! FIXME - sgtty?! */
    tio.c_lflag &= ~ICANON;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = waittime;
    tio_set( fd, &tio );

    /* read everything that comes from modem until a timeout occurs */
    while ( read( fd, buffer, 1 ) > 0 &&
	    bytes < 10000 )
    {
        if ( ++bytes < 500 ) lputc( L_NOISE, buffer[0] );
    }

    /* reset terminal settings */
    tio_set( fd, &save_tio );
    
#endif

    if ( bytes > 500 )
        lprintf( L_WARN, "clean_line: only 500 of %d bytes logged", bytes );
    if ( bytes >= 10000 )
    {
	extern char * Device;
        lprintf( L_FATAL, "clean_line: got too much junk (dev=%s).", Device );
    }
    
    return 0;
}
