/*
 *  Copyright (c) 1998 by the University of Oregon.
 *  All rights reserved.
 *
 *  Permission to use, copy, modify, and distribute this software and
 *  its documentation in source and binary forms for lawful
 *  purposes and without fee is hereby granted, provided
 *  that the above copyright notice appear in all copies and that both
 *  the copyright notice and this permission notice appear in supporting
 *  documentation, and that any documentation, advertising materials,
 *  and other materials related to such distribution and use acknowledge
 *  that the software was developed by the University of Oregon.
 *  The name of the University of Oregon may not be used to endorse or 
 *  promote products derived from this software without specific prior 
 *  written permission.
 *
 *  THE UNIVERSITY OF OREGON DOES NOT MAKE ANY REPRESENTATIONS
 *  ABOUT THE SUITABILITY OF THIS SOFTWARE FOR ANY PURPOSE.  THIS SOFTWARE IS
 *  PROVIDED "AS IS" AND WITHOUT ANY EXPRESS OR IMPLIED WARRANTIES,
 *  INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, TITLE, AND 
 *  NON-INFRINGEMENT.
 *
 *  IN NO EVENT SHALL UO, OR ANY OTHER CONTRIBUTOR BE LIABLE FOR ANY
 *  SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES, WHETHER IN CONTRACT,
 *  TORT, OR OTHER FORM OF ACTION, ARISING OUT OF OR IN CONNECTION WITH,
 *  THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/*
 * Part of this program has been derived from PIM sparse-mode pimd.
 * The pimd program is covered by the license in the accompanying file
 * named "LICENSE.pimd".
 *  
 * The pimd program is COPYRIGHT 1998 by University of Southern California.
 *
 * Part of this program has been derived from mrouted.
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE.mrouted".
 * 
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */

#include "defs.h"
#include <getopt.h>

char configfilename[256] = _PATH_PIMD_CONF;
char versionstring[100];

static char pidfilename[]  = _PATH_PIMD_PID;
/* TODO: not used
static char genidfilename[] = _PATH_PIMD_GENID;
*/

int   foreground = 0;
char *progname;

static int sighandled = 0;
#define GOT_SIGINT      0x01
#define GOT_SIGHUP      0x02
#define GOT_SIGUSR1     0x04
#define GOT_SIGUSR2     0x08
#define GOT_SIGALRM     0x10

#define NHANDLERS       4

static struct ihandler {
    int fd;			/* File descriptor               */
    ihfunc_t func;		/* Function to call with &fd_set */
} ihandlers[NHANDLERS];
static int nhandlers = 0;

/*
 * Forward declarations.
 */
static void handler (int);
static void timer (void *);
static void cleanup (void);
static void restart (int);
static void cleanup (void);
static void resetlogging (void *);

int
register_input_handler(fd, func)
    int fd;
ihfunc_t func;
{
    if (nhandlers >= NHANDLERS)
	return -1;
    
    ihandlers[nhandlers].fd = fd;
    ihandlers[nhandlers++].func = func;
    
    return 0;
}

static void pidfile_cleanup(void)
{
    remove(pidfilename);
}

int usage(int code)
{
    char buf[768];

    printf("Usage: %s [-hnpqrsv] [-d SYS[,SYS]] [-f FILE] [-l LVL]\n\n", progname);
    printf(" -d SYS    Enable debug of subsystem(s)\n");
    printf(" -f FILE   Configuration file, default: %s\n", _PATH_PIMD_CONF);
    printf(" -h        This help text\n");
    printf(" -l LVL    Set log level: none, err, notice (default), info, debug\n");
    printf(" -n        Run in foreground, do not detach from calling terminal\n");
    printf(" -s        Use syslog, default unless running in foreground, -n\n");
    printf(" -v        Show program version\n");
    
    fprintf(stderr, "\nAvailable subystems for debug:\n");
    if (!debug_list(DEBUG_ALL, buf, sizeof(buf))) {
	char line[82] = "  ";
	char *ptr;

	ptr = strtok(buf, " ");
	while (ptr) {
	    char *sys = ptr;
	    char buf[20];

	    ptr = strtok(NULL, " ");

	    /* Flush line */
	    if (strlen(line) + strlen(sys) + 3 >= sizeof(line)) {
		puts(line);
		strlcpy(line, "  ", sizeof(line));
	    }

	    if (ptr)
		snprintf(buf, sizeof(buf), "%s ", sys);
	    else
		snprintf(buf, sizeof(buf), "%s", sys);

	    strlcat(line, buf, sizeof(line));
	}

	puts(line);
    }

    printf("\nBug report address: %-40s\n", PACKAGE_BUGREPORT);
#ifdef PACKAGE_URL
    printf("Project homepage: %s\n", PACKAGE_URL);
#endif

    return code;
}

int
main(argc, argv)
    int argc;
    char *argv[];
{	
    int dummy, dummysigalrm;
    FILE *fp;
    struct timeval tv, difftime, curtime, lasttime, *timeout;
    fd_set rfds, readers;
    int nfds, n, i, secs;
    struct sigaction sa;
    struct debugname *d;
    char ch;
    int rc;

    setlinebuf(stderr);
    snprintf(versionstring, sizeof(versionstring), "%s version %s", PACKAGE_NAME, PACKAGE_VERSION);

    progname = strrchr(argv[0], '/');
    if (progname)
	progname++;
    else
	progname = argv[0];

    while ((ch = getopt(argc, argv, "d:f:h?l:nsv")) != EOF) {
	switch (ch) {
	case 'd':
	    rc = debug_parse(optarg);
	    if (rc == (int)DEBUG_PARSE_FAIL)
		return usage(1);
	    debug = rc;
	    break;

	case 'f':
	    strcpy(configfilename, optarg);
	    break;

	case '?':
	case 'h':
	    return usage(0);

	case 'l':
	    rc = log_str2lvl(optarg);
	    if (rc == -1)
		return usage(1);
	    log_level = rc;
	    break;

	case 'n':
	    foreground = 1;
	    if (log_syslog > 0)
		log_syslog--;
	    break;

	case 's':
	    if (log_syslog == 0)
		log_syslog++;
	    break;

	case 'v':
	    printf("%s\n", versionstring);
	    return 0;

	default:
	    return usage(1);
	}
    }

    argc -= optind;
    if (argc > 0)
	return usage(1);

    if (debug) {
	char buf[350];

	debug_list(debug, buf, sizeof(buf));
	printf("debug level 0x%lx (%s)\n", debug, buf);
    }

    if (geteuid() != 0) {
	fprintf(stderr, "%s: must be root\n", PACKAGE_NAME);
	exit(1);
    }

    if (!foreground) {
	/* Detach from the terminal */
#ifdef TIOCNOTTY
      int t;
#endif /* TIOCNOTTY */

	if (fork())
	    exit(0);
	close(0);
	close(1);
	close(2);
	(void)open("/", 0);
	dup2(0, 1);
	dup2(0, 2);
#if defined(SYSV) || defined(__linux__)
	setpgrp();
#else
#ifdef TIOCNOTTY
	t = open("/dev/tty", 2);
	if (t >= 0) {
	    (void)ioctl(t, TIOCNOTTY, NULL);
	    close(t);
	}
#else
	if (setsid() < 0)
	    perror("setsid");
#endif /* TIOCNOTTY */
#endif /* SYSV */
    } /* End of child process code */

    if (log_syslog) {
	openlog(progname, LOG_PID, LOG_DAEMON);
	setlogmask(LOG_UPTO(log_level));
    }

    logit(LOG_NOTICE, 0, "%s starting", versionstring);
    
    /* TODO: XXX: use a combination of time and hostid to initialize the
     * random generator.
     */
    srand48(time(NULL));
    
    /* Start up the log rate-limiter */
    resetlogging(NULL);

    callout_init();
    init_igmp();
    init_pim();
    init_routesock(); /* Both for Linux netlink and BSD routing socket */
    init_pim_mrt();
    init_timers();
    ipc_init();

    /* TODO: check the kernel DVMRP/MROUTED/PIM support version */
    
    init_vifs();
    
#ifdef RSRR
    rsrr_init();
#endif /* RSRR */

    sa.sa_handler = handler;
    sa.sa_flags = 0;	/* Interrupt system calls */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    
    FD_ZERO(&readers);
    FD_SET(igmp_socket, &readers);
    nfds = igmp_socket + 1;
    for (i = 0; i < nhandlers; i++) {
	FD_SET(ihandlers[i].fd, &readers);
	if (ihandlers[i].fd >= nfds)
	    nfds = ihandlers[i].fd + 1;
    }
    
    IF_DEBUG(DEBUG_IF)
	dump_vifs(stderr, 0);
    IF_DEBUG(DEBUG_PIM_MRT)
	dump_pim_mrt(stderr, 0);
    
    /* schedule first timer interrupt */
    timer_setTimer(TIMER_INTERVAL, timer, NULL);

    atexit(pidfile_cleanup);
    fp = fopen(pidfilename, "w");
    if (fp) {
	fprintf(fp, "%d\n", getpid());
	fclose(fp);
    }
    
    /*
     * Main receive loop.
     */
    dummy = 0;
    dummysigalrm = SIGALRM;
    difftime.tv_usec = 0;
    gettimeofday(&curtime, NULL);
    lasttime = curtime;
    for(;;) {
	bcopy((char *)&readers, (char *)&rfds, sizeof(rfds));
	secs = timer_nextTimer();
	if (secs == -1)
	    timeout = NULL;
	else {
	   timeout = &tv;
	   timeout->tv_sec = secs;
	   timeout->tv_usec = 0;
        }
	
	if (sighandled) {
	    if (sighandled & GOT_SIGINT) {
		sighandled &= ~GOT_SIGINT;
		break;
	    }
	    if (sighandled & GOT_SIGHUP) {
		sighandled &= ~GOT_SIGHUP;
		restart(SIGHUP);
	    }
	    if (sighandled & GOT_SIGALRM) {
		sighandled &= ~GOT_SIGALRM;
		timer(&dummysigalrm);
	    }
	}
	if ((n = select(nfds, &rfds, NULL, NULL, timeout)) < 0) {
	    if (errno != EINTR) /* SIGALRM is expected */
		logit(LOG_WARNING, errno, "select failed");
	    continue;
	}

	/*
	 * Handle timeout queue.
	 *
	 * If select + packet processing took more than 1 second,
	 * or if there is a timeout pending, age the timeout queue.
	 *
	 * If not, collect usec in difftime to make sure that the
	 * time doesn't drift too badly.
	 *
	 * If the timeout handlers took more than 1 second,
	 * age the timeout queue again.  XXX This introduces the
	 * potential for infinite loops!
	 */
	do {
	    /*
	     * If the select timed out, then there's no other
	     * activity to account for and we don't need to
	     * call gettimeofday.
	     */
	    if (n == 0) {
		curtime.tv_sec = lasttime.tv_sec + secs;
		curtime.tv_usec = lasttime.tv_usec;
		n = -1;	/* don't do this next time through the loop */
	    } else
		gettimeofday(&curtime, NULL);
	    difftime.tv_sec = curtime.tv_sec - lasttime.tv_sec;
	    difftime.tv_usec += curtime.tv_usec - lasttime.tv_usec;
#ifdef TIMERDEBUG
	    IF_DEBUG(DEBUG_TIMEOUT)
		logit(LOG_DEBUG, 0, "TIMEOUT: secs %d, diff secs %d, diff usecs %d", secs, difftime.tv_sec, difftime.tv_usec );
#endif
	    while (difftime.tv_usec > 1000000) {
		difftime.tv_sec++;
		difftime.tv_usec -= 1000000;
	    }
	    if (difftime.tv_usec < 0) {
		difftime.tv_sec--;
		difftime.tv_usec += 1000000;
	    }
	    lasttime = curtime;
	    if (secs == 0 || difftime.tv_sec > 0) {
#ifdef TIMERDEBUG
		IF_DEBUG(DEBUG_TIMEOUT)
		    logit(LOG_DEBUG, 0, "\taging callouts: secs %d, diff secs %d, diff usecs %d", secs, difftime.tv_sec, difftime.tv_usec );
#endif
		age_callout_queue(difftime.tv_sec);
	    }
	    secs = -1;
	} while (difftime.tv_sec > 0);

	/* Handle sockets */
	if (n > 0) {
	    /* TODO: shall check first igmp_socket for better performance? */
	    for (i = 0; i < nhandlers; i++) {
		if (FD_ISSET(ihandlers[i].fd, &rfds)) {
		    (*ihandlers[i].func)(ihandlers[i].fd, &rfds);
		}
	    }
	}
    
    } /* Main loop */

    logit(LOG_NOTICE, 0, "%s exiting", versionstring);
    cleanup();
    exit(0);
}

/*
 * The 'virtual_time' variable is initialized to a value that will cause the
 * first invocation of timer() to send a probe or route report to all vifs
 * and send group membership queries to all subnets for which this router is
 * querier.  This first invocation occurs approximately TIMER_INTERVAL seconds
 * after the router starts up.   Note that probes for neighbors and queries
 * for group memberships are also sent at start-up time, as part of initial-
 * ization.  This repetition after a short interval is desirable for quickly
 * building up topology and membership information in the presence of possible
 * packet loss.
 *
 * 'virtual_time' advances at a rate that is only a crude approximation of
 * real time, because it does not take into account any time spent processing,
 * and because the timer intervals are sometimes shrunk by a random amount to
 * avoid unwanted synchronization with other routers.
 */

u_long virtual_time = 0;

/*
 * Timer routine. Performs all perodic functions:
 * aging interfaces, quering neighbors and members, etc... The granularity
 * is equal to TIMER_INTERVAL.
 */
static void 
timer(i)
    void *i;
{
    age_vifs();	        /* Timeout neighbors and groups         */
    age_routes();  	/* Timeout routing entries              */
    
    virtual_time += TIMER_INTERVAL;
    timer_setTimer(TIMER_INTERVAL, timer, NULL);
}	

/*
 * Performs all necessary functions to quit gracefully
 */
/* TODO: implement all necessary stuff */
static void
cleanup()
{
#ifdef RSRR
    rsrr_clean();
#endif

    free_all_callouts();
    stop_all_vifs();
    k_stop_pim(igmp_socket);

    igmp_clean();
    pim_clean();
    mrt_clean();
    ipc_exit();

    /*
     * When IOCTL_OK_ON_RAW_SOCKET is defined, 'udp_socket' is equal
     * 'to igmp_socket'. Therefore, 'udp_socket' should be closed only
     * if they are different.
     */
#ifndef IOCTL_OK_ON_RAW_SOCKET
    if (udp_socket > 0)
	close(udp_socket);
    udp_socket = 0;
#endif

    /* Both for Linux netlink and BSD routing socket */
    routesock_clean();

    /* No more socket callbacks */
    nhandlers = 0;
}


/*
 * Signal handler.  Take note of the fact that the signal arrived
 * so that the main loop can take care of it.
 */
static void
handler(sig)
    int sig;
{
    switch (sig) {
    case SIGALRM:
	sighandled |= GOT_SIGALRM;
	break;

    case SIGINT:
    case SIGTERM:
	sighandled |= GOT_SIGINT;
	break;
	
    case SIGHUP:
	sighandled |= GOT_SIGHUP;
	break;
    }
}


/* TODO: not verified */
/* PIMDM TODO */
/*
 * Restart the daemon
 */
static void
restart(i)
    int i;
{
    logit(LOG_NOTICE, 0, "%s restart", versionstring);
    
    /*
     * reset all the entries
     */
    cleanup();

    /* TODO: delete?
       free_all_routes();
       */

    /*
     * start processing again
     */
    init_igmp();
    init_pim();
    init_routesock();
    init_pim_mrt();
    init_vifs();
    ipc_init();

#ifdef RSRR
    rsrr_init();
#endif /* RSRR */

    /* schedule timer interrupts */
    timer_setTimer(TIMER_INTERVAL, timer, NULL);
}


int
daemon_restart(void *arg)
{
    (void)arg;
    restart(1);

    return 0;
}

int
daemon_kill(void *arg)
{
    (void)arg;
    handler(SIGTERM);

    return 0;
}


static void
resetlogging(arg)
    void *arg;
{
    int nxttime = 60;
    void *narg = NULL;
    
    if (arg == NULL && log_nmsgs > LOG_MAX_MSGS) {
	nxttime = LOG_SHUT_UP;
	narg = (void *)&log_nmsgs;	/* just need some valid void * */
	syslog(LOG_WARNING, "logging too fast, shutting up for %d minutes",
	       LOG_SHUT_UP / 60);
    } else {
	log_nmsgs = 0;
    }
    
    timer_setTimer(nxttime, resetlogging, narg);
}
