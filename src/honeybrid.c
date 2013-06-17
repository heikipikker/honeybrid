/*
 * This file is part of the honeybrid project.
 *
 * 2007-2009 University of Maryland (http://www.umd.edu)
 * (Written by Robin Berthier <robinb@umd.edu>, Thomas Coquelin <coquelin@umd.edu> and Julien Vehent <julien@linuxwall.info> for the University of Maryland)
 *
 * 2012-2013 University of Connecticut (http://www.uconn.edu)
 * (Extended by Tamas K Lengyel <tamas.k.lengyel@gmail.com>
 *
 * Honeybrid is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/*!	\mainpage Hybrid Honeypot Gateway
 *
 * 	\section Introduction
 *
 * 	This project is a honeypot architecture able to combine low and high interaction honeypots in the same framework.
 * 	The objective is to have a scalable solutions and to collect detailed attack processes.
 * 	The core of the project is a software gateway based on Iptables and built upon two engines: a Redirection Engine and a Decision Engine.
 * 	The concept of the architecture is to use a front end of low interaction honeypot to reply to all incoming traffic.
 * 	Then the goal of the Decision Engine is to filter interesting attacks from the noise of incoming traffic received.
 * 	Filtered attacks are forwarded to the Redirection Engine which is able to actively redirect the destination of the connection, so that it can be further investigated using a high interaction back-end.
 *
 */

/*!	\file honeybrid.c
 \brief Main File

 This is the main program file for Honeybrid. It creates a hook using LibNetfilter Queue
 and, for each connection, maintain a stateful table.
 It forwards a packet to a determined destination and submits this packet to the decision engine.
 When the decision engine decides to redirect a connection, this redirection engine replays the recorded
 connection to its new destination and maintains it until its end.

 Packets needs to be redirected to the QUEUE destination using netfilter, this can be done using:
 # iptables -A INPUT -j QUEUE && iptables -A FORWARD -j QUEUE && iptables -A OUTPUT -j QUEUE

 Other filters can also be set up using the regulars iptables capabilities,
 it is also recommended to limit the redirections to TCP and UDP packets
 (just add the option -p to the iptables commands)

 \Author J. Vehent, 2007
 \Author Thomas Coquelin, 2008
 \Author Robin Berthier, 2007-2009
 \Author Tamas K Lengyel, 2012-2013
 */

/* \todo to include in programmer documentation:
 //What we should use to log messages:
 //For debugging:
 printdbg("%smessage\n", H(30));

 //For processing information:
 syslog(LOG_INFO,"message");

 //For critical warning
 warn("open");
 warnx("%s message", __func__);

 //For fatal error
 err("fopen");
 errx(1,"%s message", __func__);
 */

#include "honeybrid.h"

#include <limits.h>
#include <errno.h>
#include <syslog.h>
#include <signal.h>
#include <linux/netfilter.h>
#include <libnetfilter_queue/libnetfilter_queue.h>
#include <sys/stat.h>
#include <execinfo.h>

#include "constants.h"
#include "structs.h"
#include "globals.h"
#include "convenience.h"
#include "netcode.h"
#include "log.h"
#include "decision_engine.h"
#include "modules.h"
#include "connections.h"

// Forward declare q_cb
int q_cb(struct nfq_q_handle *gh, struct nfgenmsg *nfmsg, struct nfq_data *nfad,
        void *data);

// Get the Queue ID the packet should be assigned to
// based on the last byte of the src and dst IP address
#define PKT2QUEUEID(pkt) \
    ( \
        (uint32_t) \
            ( \
                ( \
                ((pkt->packet.ip->saddr & 0xFF000000) >> 24) \
                + \
                ((pkt->packet.ip->daddr & 0xFF000000) >> 24) \
                ) \
                % decision_threads \
            ) \
    )

#ifdef HAVE_LIBEV
#include <ev.h>
struct ev_loop *loop;
struct ev_signal signals[7];
#endif

static struct nfq_q_handle *qh = NULL;
static struct nfq_handle *h = NULL;

/*! usage function
 \brief print command line informations */
void usage(char **argv) {
    g_printerr(
            "Usage: %s <commands>\n\n"
                    "Where commands include:\n"
                    "  -c <config_file>: start with config file\n"
                    "            For example: honeybrid -c /etc/honeybrid.conf\n"
                    "  -x <pid>: halt a running engine using its PID\n"
                    "            For example: honeybrid -x `cat /var/run/honeybrid.pid`\n"
                    "  -q <queuenum>: select a specific queue number for NF_QUEUE to listen to\n"
                    "  -d: daemonize Honeybrid (send it to the background)\n"
                    //"  -s: show status information\n"
                    "  -h: print this help\n\n", argv[0]);
    exit(1);
}

/*! term_signal_handler
 *
 \brief called when the program receive a signal that should close the program, free memory and delete lock file
 *
 \param[in] signal_nb: number of the signal
 \param[in] siginfo: informations regarding to the signal
 \param[in] context: NULL */
#ifdef HAVE_LIBEV
static void term_signal_handler(struct ev_loop *loop,
        __attribute__((unused))      struct ev_signal *w,
        __attribute__ ((unused)) int revents) {

    running = NOK; /*! this will cause the queue loop to stop */

    ev_unloop(loop, EVUNLOOP_ALL);

    printdbg("%s Signal number:\t%d\n", H(0), w->signum);

    if (fdebug != -1)
        g_printerr("\n");

}
#else
static void term_signal_handler(int signal_nb, siginfo_t * siginfo, __attribute__((unused)) void *unused) {
    printdbg("%s: Signal %d received, halting engine\n", __func__, signal_nb);
    printdbg("* Signal number:\t%d\n", siginfo->si_signo);
    printdbg("* Signal code:  \t%d\n", siginfo->si_code);
    printdbg("* Signal error: \t%d '%s'\n", siginfo->si_errno,
            strerror(siginfo->si_errno));
    printdbg("* Sending pid:  \t%d\n", siginfo->si_pid);
    printdbg("* Sending uid:  \t%d\n", siginfo->si_uid);
    printdbg("* Fault address:\t%p\n", siginfo->si_addr);
    printdbg("* Exit value:   \t%d\n", siginfo->si_status);

    /*! this will cause the queue loop to stop */
    running = NOK;
    g_printerr("\n");
}
#endif //HAVE_LIBEV
/*! init_signal
 \brief installs signal handlers
 \return 0 if exit with success, anything else if not */
void init_signal() {
#ifdef HAVE_LIBEV

    ev_signal_init(&signals[0], term_signal_handler, SIGHUP);
    ev_signal_start(loop, &signals[0]);
    ev_unref(loop);

    ev_signal_init(&signals[1], term_signal_handler, SIGINT);
    ev_signal_start(loop, &signals[1]);
    ev_unref(loop);

    ev_signal_init(&signals[2], term_signal_handler, SIGQUIT);
    ev_signal_start(loop, &signals[2]);
    ev_unref(loop);

    ev_signal_init(&signals[3], term_signal_handler, SIGILL);
    ev_signal_start(loop, &signals[3]);
    ev_unref(loop);

    ev_signal_init(&signals[4], term_signal_handler, SIGSEGV);
    ev_signal_start(loop, &signals[4]);
    ev_unref(loop);

    ev_signal_init(&signals[5], term_signal_handler, SIGTERM);
    ev_signal_start(loop, &signals[5]);
    ev_unref(loop);

    ev_signal_init(&signals[6], term_signal_handler, SIGBUS);
    ev_signal_start(loop, &signals[6]);
    ev_unref(loop);

#else
    /*! Install terminating signal handler: */
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof sa_term);

    sa_term.sa_sigaction = (void *) term_signal_handler;
    sa_term.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigfillset(&sa_term.sa_mask);

    /*! SIGHUP*/
    if (sigaction(SIGHUP, &sa_term, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGHUP", __func__);

    /*! SIGINT*/
    if (sigaction(SIGINT, &sa_term, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGINT", __func__);

    /*! SIGQUIT*/
    if (sigaction(SIGQUIT, &sa_term, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGQUIT", __func__);

    /*! SIGILL*/
    if (sigaction(SIGILL, &sa_term, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGILL", __func__);

    /*! SIGSEGV*/
    if (sigaction(SIGSEGV, &sa_term, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGSEGV", __func__);

    /*! SIGTERM*/
    if (sigaction(SIGTERM, &sa_term, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGTERM", __func__);

    /*! SIGBUS*/
    if (sigaction(SIGBUS, &sa_term, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGBUS", __func__);

    /*! ignore signals: */
    struct sigaction sa_ignore;
    memset(&sa_ignore, 0, sizeof sa_ignore);
    sa_ignore.sa_handler = SIG_IGN;
    sigfillset(&sa_ignore.sa_mask);

    /*! SIGABRT*/
    if (sigaction(SIGABRT, &sa_ignore, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGABRT", __func__);

    /*! SIGALRM*/
    if (sigaction(SIGALRM, &sa_ignore, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGALRM", __func__);

    /*! SIGUSR2*/
    if (sigaction(SIGUSR2, &sa_ignore, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGUSR2", __func__);

    /*! SIGPOLL*/
    if (sigaction(SIGPOLL, &sa_ignore, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGPOLL", __func__);

    /*! rotate logs: */
    struct sigaction sa_rotate_log;
    memset(&sa_rotate_log, 0, sizeof(sa_rotate_log));

    sa_rotate_log.sa_sigaction = (void *) rotate_connection_log;
    //sa_rotate_log.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sa_rotate_log.sa_flags = SA_RESTART;
    sigfillset(&sa_rotate_log.sa_mask);

    /*! SIGUSR1*/
    if (sigaction(SIGUSR1, &sa_rotate_log, NULL) != 0)
    errx(1, "%s: Failed to install sighandler for SIGUSR1", __func__);
#endif
}

/*! init_syslog
 \brief initialize syslog logging */
static void init_syslog(int argc, char *argv[]) {
    int options, i;
    char buf[MAXPATHLEN];

#ifdef LOG_PERROR
    options = LOG_PERROR | LOG_PID | LOG_CONS;
#else
    options = LOG_PID|LOG_CONS;
#endif
    openlog("honeybrid", options, LOG_DAEMON);

    /* Create a string containing all the command line
     * arguments and pass it to syslog:
     */

    buf[0] = '\0';
    for (i = 1; i < argc; i++) {
        if (i > 1 && g_strlcat(buf, " ", sizeof(buf)) >= sizeof(buf))
            break;
        if (g_strlcat(buf, argv[i], sizeof(buf)) >= sizeof(buf))
            break;
    }

    syslog(LOG_NOTICE, "started with %s", buf);
}

/*! parse_config
 \brief Configuration parsing function, read the configuration from a specific file 
 and parse it into a hash table or other tree data structures using Bison/Flex
 */
void init_parser(char *filename) {

    g_printerr("--------------------------\nReading configuration\n");

    FILE *fp = fopen(filename, "r");
    if (!fp)
        err(1, "fopen(%s)", filename);

    //extern int yydebug;
    //yydebug = 1;
    yyin = fp;
    yyparse();

    fclose(fp);

    g_printerr("--------------------------\n");
}

void init_variables() {
    /*! create the hash table to store the config */
    if (NULL
            == (config = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                    g_free)))
        errx(1, "%s: Fatal error while creating config hash table.\n",
                __func__);

    /*! create the array of pointer to store the target information */
    if (NULL == (targets = g_ptr_array_new()))
        errx(1, "%s: Fatal error while creating targets pointer of array.\n",
                __func__);

    /*! create the hash table to store module information */
    if (NULL
            == (module = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                    (GDestroyNotify) g_hash_table_destroy)))
        errx(1, "%s: Fatal error while creating module hash table.\n",
                __func__);

    /*! create the hash table for the log engine */
    if (NULL
            == (uplink = g_hash_table_new_full(g_int_hash, g_int_equal, NULL,
                    (GDestroyNotify) free_interface)))
        errx(1, "%s: Fatal error while creating uplink hash table.\n",
                __func__);

    /* create the main B-Tree to store meta informations of active connections */
    if (NULL == (conn_tree = g_tree_new((GCompareFunc) g_strcmp0))) {
        errx(1, "%s: Fatal error while creating conn_tree.\n", __func__);
    }

    /*! create the hash table for the log engine */
    if (NULL == (module_to_save = g_hash_table_new(g_str_hash, g_str_equal)))
        errx(1, "%s: Fatal error while creating module_to_save hash table.\n",
                __func__);

    /*! create the redirection table */
    if (NULL
            == (high_redirection_table = g_hash_table_new_full(g_str_hash,
                    g_str_equal, g_free, g_free)))
        errx(1,
                "%s: Fatal error while creating high_redirection_table hash table.\n",
                __func__);

    /* set debug file */
    fdebug = -1;

    /*! init the connection id counter */
    c_id = 0;

    /*! Enable threads */
    threading = OK;

    /*! Enable data processing */
    running = OK;

#ifdef HAVE_LIBEV
    loop = ev_default_loop(0);
#endif
}

/*! init_nfqueue
 *
 \brief Function to create the NF_QUEUE loop
 \param[in] queuenum the queue identifier
 \return file descriptor for queue
 */
static int init_nfqueue(unsigned short int queuenum) {

    h = nfq_open();
    if (!h)
        errx(1, "%s Error during nfq_open()", __func__);

    if (nfq_unbind_pf(h, AF_INET) < 0)
        errx(1, "%s Error during nfq_unbind_pf()", __func__);

    if (nfq_bind_pf(h, AF_INET) < 0)
        errx(1, "%s Error during nfq_bind_pf()", __func__);

    syslog(LOG_INFO, "NFQUEUE: binding to queue '%hd'\n", queuenum);

    qh = nfq_create_queue(h, queuenum, &q_cb, NULL);
    if (!qh)
        errx(1, "%s Error during nfq_create_queue()", __func__);

    if (nfq_set_mode(qh, NFQNL_COPY_PACKET, PAYLOADSIZE) < 0)
        errx(1, "%s Can't set packet_copy mode", __func__);

    running = OK;

    return (nfnl_fd(nfq_nfnlh(h)));
}

static void close_nfqueue(unsigned short int queuenum) {
    syslog(LOG_INFO, "NFQUEUE: unbinding from queue '%hd'\n", queuenum);
    nfq_destroy_queue(qh);
    nfq_close(h);
}

/*! close_thread
 \brief Function that waits for thread to close themselves */
int close_thread() {

    /* First, let's make sure all packets already queued get processed */
    uint32_t i;
    struct pkt_struct pkt = { .last = TRUE };
    for (i = 0; i < decision_threads; i++) {
        g_async_queue_push(de_queues[i], &pkt);
        printdbg("%s: Waiting for de_thread %i to terminate\n", H(0), i);
        g_thread_join(de_threads[i]);
        g_async_queue_unref(de_queues[i]);
    }

    /* Shut down other threads */
    threading = NOK;
    g_cond_broadcast(&threading_cond);

    g_thread_join(mod_backup);
    g_thread_join(thread_clean);

    return 0;
}

/*! close_hash function
 \brief Destroy the different hashes used by honeybrid */
int close_hash() {
    /*! Destroy hash tables
     */

    if (high_redirection_table != NULL) {
        printdbg("%s: Destroying table high_redirection_table\n", H(0));
        g_rw_lock_writer_lock(&hihredirlock);
        g_hash_table_destroy(high_redirection_table);
        high_redirection_table = NULL;
    }

    if (config != NULL) {
        printdbg("%s: Destroying table config\n", H(0));
        g_hash_table_destroy(config);
        config = NULL;
    }

    if (module != NULL) {
        printdbg("%s: Destroying table module\n", H(0));
        g_hash_table_destroy(module);
    }

    if (uplink != NULL) {
        printdbg("%s: Destroying table uplink\n", H(0));
        g_hash_table_destroy(uplink);
        uplink = NULL;
    }

    if (module_to_save != NULL) {
        printdbg("%s: Destroying table module_to_save\n", H(0));
        g_hash_table_destroy(module_to_save);
        module_to_save = NULL;
    }

    return 0;
}

/*! close_conn_tree function
 \brief Function to free memory taken by conn_tree */
int close_conn_tree() {

    printdbg("%s: Destroying connection tree\n", H(0));

    /*! clean the memory
     * traverse the B-Tree to remove the singly linked lists and then destroy the B-Tree
     */
    int delay = 0;
    entrytoclean = g_ptr_array_new();

    /*! call the clean function for each value, delete the value if TRUE is returned */
    g_tree_foreach(conn_tree, (GTraverseFunc) expire_conn, &delay);

    /*! remove each key listed from the btree */
    g_ptr_array_foreach(entrytoclean, (GFunc) free_conn, NULL);

    /*! free the array */
    g_ptr_array_free(entrytoclean, TRUE);
    entrytoclean = NULL;

    g_tree_destroy(conn_tree);
    conn_tree = NULL;

    return 0;
}

/*! close_target
 \brief destroy global structure "targets" when the program has to quit */
int close_target(void) {
    printdbg("%s: Destroying targets\n", H(0));
    g_ptr_array_foreach(targets, (GFunc) free_target_gfunc, NULL);
    g_ptr_array_free(targets, TRUE);
    return OK;
}

/*! close_all
 \brief destroy structures and free memory when the program has to quit */
void close_all(void) {
    /*! wait for thread to close */
    if (close_thread() < 0)
        g_printerr("%s: Error when waiting for threads to close\n", H(0));

    /*! delete conn_tree */
    if (close_conn_tree() < 0)
        g_printerr("%s: Error when closing conn_tree\n", H(0));

    /*! delete lock file */
    if (unlink(pidfile) < 0)
        g_printerr("%s: Error when removing lock file\n", H(0));

    /*! close log file */
    if (OUTPUT_LOGFILES == ICONFIG_REQUIRED("output")) {
        close_connection_log();
    }

    /*! close debug log file */
    if (fdebug != -1) {
        close_debug_log();
    }

    /*! delete hashes */
    if (close_hash() < 0)
        printdbg("%s: Error when closing hashes\n", H(0));

    if (close_target() < 0)
        printdbg("%s: Error when closing targets\n", H(0));

}

/*! process_packet
 *
 \brief Function called for each received packet, this is the core of the redirection engine
 \param[in] tb a Netfilter Queue structure that contain both the packet and the metadatas
 \param[in] nfq_packet_id the nfq packet id
 \return statement = 1 if the packet should be accepted or 0 if the packet has to be dropped. Default is to drop. */
struct pkt_struct *
process_packet(struct nfq_data *tb, uint32_t nfq_packet_id) {

    struct pkt_struct * pkt = NULL;
    unsigned char *nf_packet;
    struct in_addr in;

    /*! extract ip header from packet payload */
    int size = nfq_get_payload(tb, &nf_packet);
    if (size < 0) {
        goto done;
    }

    uint32_t mark = nfq_get_nfmark(tb);

    /*! check if protocol is invalid (not TCP or UDP) */
    if ((((struct iphdr*) nf_packet)->protocol != IPPROTO_TCP)
            && (((struct iphdr*) nf_packet)->protocol != IPPROTO_UDP)) {

        printdbg(
                "%s Incorrect protocol: %d, packet dropped\n", H(0), (((struct iphdr*) nf_packet)->protocol));

        goto done;
    }

    in.s_addr = ((struct iphdr*) nf_packet)->saddr;

    /*! Initialize the packet structure (into pkt) and find the origin of the packet */
    if (init_pkt(nf_packet, &pkt, mark, nfq_packet_id) == NOK) {
        printdbg("%s Packet structure couldn't be initialized\n", H(0));

        pkt = NULL;
        goto done;

    }

    printdbg(
            "%s** NEW packet from %s %s, %d bytes. Mark %u, NFQID %u **\n", H(0), inet_ntoa(in), lookup_proto(((struct iphdr*) nf_packet)->protocol), size, mark, nfq_packet_id);

    done: return pkt;
}

void de_thread(gpointer data) {

    uint32_t thread_id = GPOINTER_TO_UINT(data);
    struct pkt_struct *pkt = NULL;

    printdbg("%s: Decision engine thread %i started\n", H(0), thread_id);

    while ((pkt = (struct pkt_struct *) g_async_queue_pop(de_queues[thread_id]))) {

        // Exit the thread
        if (pkt->last) {
            printdbg("%s Shutting down thread %u\n", H(1), thread_id);
            return;
        }

        status_t statement = NOK;
        struct conn_struct *conn = NULL;
        uint32_t nfq_packet_id = pkt->nfq_packet_id;
        uint32_t mark;

        /*! Initialize the connection structure (into conn) and get the state of the connection */
        if (init_conn(pkt, &conn) == NOK) {
            conn = NULL;
            printdbg(
                    "%s Connection structure couldn't be initialized, packet dropped\n", H(0));
            free_pkt(pkt);
            goto done;
        }

        printdbg(
                "%s Origin: %s %s, %i bytes\n", H(conn->id), lookup_origin(pkt->origin), lookup_state(conn->state), pkt->data);

        /*! Check that there was no problem getting the current connection structure
         *  and make sure the STATE is valid */
        if (((conn->state < INIT) && (pkt->origin == EXT))
                || (conn->state == INVALID)) {

            printdbg("%s Packet not from a valid connection\n", H(conn->id));
            if (pkt->packet.ip->protocol == IPPROTO_TCP && reset_ext == 1)
                reply_reset(&(pkt->packet));

            free_pkt(pkt);
            goto done;
        }

        if (conn->state == DROP) {
            printdbg("%s This connection is marked as DROPPED\n", H(conn->id));

            if (pkt->packet.ip->protocol == IPPROTO_TCP && reset_ext == 1)
                reply_reset(&(pkt->packet));

            free_pkt(pkt);
            goto done;
        }

        // Setup iptables mark on the packet based on what's recorded in the conn_struct
        init_mark(pkt, conn);
        mark = pkt->mark;

        switch (pkt->origin) {
            /*! Packet is from the low interaction honeypot */
            case LIH:
                switch (conn->state) {
                    case INIT:
                        if (pkt->packet.ip->protocol == IPPROTO_TCP
                                && pkt->packet.tcp->syn != 0) {
                            conn->hih.lih_syn_seq = ntohl(pkt->packet.tcp->seq);
                        }

                        // Only store packets if there are backends
                        if (conn->target->back_handler_count > 0) {
                            store_pkt(conn, pkt);
                        } else {
                            free_pkt(pkt);
                        }

                        statement = OK;
                        break;
                    case DECISION:
                        if (pkt->packet.ip->protocol == IPPROTO_TCP
                                && pkt->packet.tcp->syn != 0) {
                            conn->hih.lih_syn_seq = ntohl(pkt->packet.tcp->seq);
                        }

                        // Only store packets if there are backends
                        if (conn->target->back_handler_count > 0) {
                            store_pkt(conn, pkt);
                        } else {
                            free_pkt(pkt);
                        }

                        statement = OK;
                        break;
                    case PROXY:
                        printdbg(
                                "%s Packet from LIH proxied directly to its destination\n", H(conn->id));
                        statement = OK;
                        free_pkt(pkt);
                        break;
                    case CONTROL:
                        if (pkt->packet.ip->protocol == IPPROTO_TCP
                                && pkt->packet.tcp->syn != 0) {
                            conn->hih.lih_syn_seq = ntohl(pkt->packet.tcp->seq);
                        }

                        statement = DE_process_packet(pkt);

                        // Only store packets if there are backends
                        if (conn->target->back_handler_count > 0) {
                            store_pkt(conn, pkt);
                        } else {
                            free_pkt(pkt);
                        }
                        break;
                    default:
                        printdbg(
                                "%s Packet from LIH at wrong state => reset\n", H(conn->id));
                        if (pkt->packet.ip->protocol == IPPROTO_TCP)
                            reply_reset(&(pkt->packet));
                        free_pkt(pkt);
                        break;
                }
                break;

            case HIH:
                /*! Packet is from the high interaction honeypot */
                switch (conn->state) {
                    case REPLAY:
                        /*! push the packet to the synchronization list in conn_struct */
                        if (pkt->packet.ip->protocol == IPPROTO_TCP
                                && pkt->packet.tcp->syn == 1) {
                            conn->hih.delta = ~ntohl(pkt->packet.tcp->seq) + 1
                                    + conn->hih.lih_syn_seq;
                        }
                        replay(conn, pkt);
                        free_pkt(pkt);
                        break;
                    case FORWARD:
                        forward(pkt);
                        free_pkt(pkt);
                        break;
                        /*! This one should never occur because PROXY are only between EXT and LIH... but we never know! */
                    case PROXY:
                        printdbg(
                                "%s Packet from HIH proxied directly to its destination\n", H(conn->id));
                        statement = OK;
                        free_pkt(pkt);
                        break;
                    case CONTROL:
                        statement = DE_process_packet(pkt);
                        free_pkt(pkt);
                        break;
                    default:
                        /*! We are surely in the INIT state, so the HIH is initiating a connection to outside. We reset or control it */
                        if (deny_hih_init == 1) {
                            printdbg(
                                    "%s Packet from HIH at wrong state, so we reset\n", H(conn->id));
                            if (pkt->packet.ip->protocol == IPPROTO_TCP) {
                                reply_reset(&(pkt->packet));
                            }
                            statement = OK;
                            switch_state(conn, DROP);
                            free_pkt(pkt);
                        } else {
                            printdbg(
                                    "%s Packet from HIH in a new connection, so we control it\n", H(conn->id));
                            switch_state(conn, CONTROL);
                            statement = DE_process_packet(pkt);
                            free_pkt(pkt);
                        }
                        break;
                }
                break;

            case EXT:
            default:
                /*! Packet is from the external attacker (origin == EXT) */
                switch (conn->state) {
                    case INIT:
                        //g_string_assign(conn->decision_rule, ";");
                        statement = DE_process_packet(pkt);
                        // Only store packets if there are backends
                        if (conn->target->back_handler_count > 0) {
                            store_pkt(conn, pkt);
                        } else {
                            free_pkt(pkt);
                        }
                        break;
                    case DECISION:
                        statement = DE_process_packet(pkt);
                        // Only store packets if there are backends
                        if (conn->target->back_handler_count > 0) {
                            store_pkt(conn, pkt);
                        } else {
                            free_pkt(pkt);
                        }
                        break;
                    case FORWARD:
                        forward(pkt);
                        free_pkt(pkt);
                        break;
                    case PROXY:
                        printdbg(
                                "%s Packet from EXT proxied directly to its destination (PROXY)\n", H(conn->id));
                        statement = OK;
                        free_pkt(pkt);
                        break;
                    case CONTROL:
                        printdbg(
                                "%s Packet from EXT proxied directly to its destination (CONTROL)\n", H(conn->id));
                        statement = OK;
                        free_pkt(pkt);
                        break;
                    default:
                        free_pkt(pkt);
                        break;
                }
                break;
        }

        done:

        if (conn) {
            g_rw_lock_writer_unlock(&conn->lock);
        }

        if (statement == OK) {
            nfq_set_verdict2(qh, nfq_packet_id, NF_ACCEPT, mark, 0, NULL);
        } else {
            nfq_set_verdict(qh, nfq_packet_id, NF_DROP, 0, NULL);
        }
    }
}

/*! q_cb
 *
 \brief Callback function launched by the netfilter queue handler each time a packet is received
 * */
int q_cb(struct nfq_q_handle *gh, __attribute__ ((unused)) struct nfgenmsg *nfmsg,
	       struct nfq_data *nfad, __attribute__((unused)) void *data) {

    /*! get packet id */
    struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr(nfad);
    uint32_t id = ntohl(ph->packet_id);

    struct pkt_struct *pkt = process_packet(nfad, id);
    if(pkt) {
        g_async_queue_push(de_queues[PKT2QUEUEID(pkt)], pkt);
    } else {
        nfq_set_verdict(gh, id, NF_DROP, 0, NULL);
    }

    return running;
}

#ifndef HAVE_LIBEV
/*! netlink loop
 \brief Function to create and maintain the NF_QUEUE loop
 \param[in] queuenum the queue identifier
 \return status
 */
short int netlink_loop(unsigned short int queuenum) {

    int fd = -1, rv = -1, watchdog=0;
    char buf[BUFSIZE];

    fd = init_nfqueue(queuenum);

    while (running == OK) {
        memset(buf, 0, BUFSIZE);
        rv = recv(fd, buf, BUFSIZE, 0);
        if (rv < 0) {
            printdbg("%s Error %d: recv() returned %d '%s'\n", H(0), errno,
                    rv, strerror(errno));
            watchdog++;
            if (watchdog > 100) {
                printdbg(
                        "%s Error: too many consecutive failures, giving up\n",
                        H(0));
                running = NOK;
            }
        } else {
            nfq_handle_packet(h, buf, rv);
            if (watchdog > 0) {
                watchdog = 0;
            }
        }
    }

    close_nfqueue(queuenum);
    return (0);
}

#else

static void nfqueue_ev_cb(__attribute__((unused))       struct ev_loop *loop,
        struct ev_io *w, __attribute__ ((unused)) int revents) {
    int rv;
    char buf[BUFSIZE];

    rv = recv(w->fd, buf, sizeof(buf), 0);
    if (rv >= 0 && running == OK) {
        nfq_handle_packet(h, buf, rv);
    }
}

#endif

/*! main
 \brief process arguments, daemonize, init variables, create QUEUE handler and process each packet
 \param[in] argc, number of arguments
 \param[in] argv, table with arguments
 *
 \return 0 if exit with success, anything else if not */
int main(int argc, char *argv[]) {

    g_printerr("%s  v%s\n\n", banner, PACKAGE_VERSION);

    /*! parsing arguments */
    if (argc < 2) {
        usage(argv);
    }

    int argument;
    char *config_file_name = "";
    unsigned short int nfqueuenum = 0;
    gboolean daemonize = FALSE;
    debug = FALSE;

    while ((argument = getopt(argc, argv, "sc:x:V:q:h:d?")) != -1) {
        switch (argument) {
            case 'c':
                /*! define configuration filename */
                config_file_name = optarg;
                break;
            case 'x':
                /*! send a shutdown request to honeybrid */
                g_printerr("Trying to shutdown honeybrid at pid %s\n", optarg);

                /*! convert argument to int */
                int pid = atoi(optarg);

                /*! check that process exists */
                if (-1 == kill(pid, 0)) {
                    errx(1, "%s: ERROR: Process does not exist", __func__);
                } else {
                    g_printerr("%s: Sending signal to halt engine\n", __func__);
                    /*! send signal USR1 to PID */
                    kill(pid, SIGQUIT);
                    exit(0);
                }
                break;
            case 'V':
                printf("Honeybrid Version %s\n", PACKAGE_VERSION);
                exit(0);
                break;
            case 'q':
                nfqueuenum = (unsigned short int) atoi(optarg);
                break;
            case 's':
                g_printerr("Status informations not yet implemented\n");
                exit(-1);
                break;
                /*! \todo
                 add a signal handler to output statistics (figure out the correct file description for output...)
                 Statistics should include:
                 - version
                 - start time and duration
                 - packet processed:
                 * total
                 * TCP
                 * UDP
                 * Other
                 - connection processed:
                 * total
                 * INIT
                 * DECISION
                 * REPLAY
                 * FORWARD
                 * INVALID
                 * PROXY
                 - decision engine, for each module:
                 * rules loaded
                 * signature loaded
                 * connection redirected (MATCH)
                 * connection left alone (DOES NOT MATCH)
                 - errors
                 * NF_QUEUE restarts
                 * expected data
                 - top ports?
                 - top IP addresses?
                 */
                /*case 'd':
                 g_printerr("Daemonizing honeybrid\n");
                 daemonize = 1;
                 break;*/
            case 'd':
                g_printerr("Daemonizing honeybrid\n");
                daemonize = TRUE;
                break;
            case 'h':
            case '?':
            default:
                usage(argv);
                break;
                /* not reached */
        }
    }

    /*! initialize syslog */
    init_syslog(argc, argv);
    /*! initialize data structures */
    init_variables();
    /*! parse the configuration files and store values in memory */
    init_parser(config_file_name);
    /*! initialize signal handlers */
    init_signal();

    if (ICONFIG("max_packet_buffer") > 0) {
        max_packet_buffer = ICONFIG("max_packet_buffer");
    } else {
        max_packet_buffer = ULLONG_MAX;
    }

    deny_hih_init = ICONFIG("deny_hih_init");
    reset_ext = ICONFIG("reset_ext");

    output_t output = ICONFIG_REQUIRED("output");

    /* Start Honeybrid in the background if necessary */
    if (daemonize) {
        if (output != OUTPUT_STDOUT) {
            g_printerr("Honeybrid starting as background process\n");

            if (daemon(1, 0) < 0) {
                unlink(pidfile);
                err(1, "daemon");
            }
        } else {
            g_printerr("Output is defined as STDOUT, can't daemonize!\n");
        }
    }

    /*! Create PID file */
    pidfile = g_malloc0(
            snprintf(NULL, 0, "%s/honeybrid.pid",
                    CONFIG_REQUIRED("exec_directory")) + 1);
    sprintf((char *) pidfile, "%s/honeybrid.pid",
            CONFIG_REQUIRED("exec_directory"));
    unlink(pidfile);
    FILE *fp;
    if ((fp = fopen(pidfile, "w")) == NULL) {
        err(1, "fopen: %s", pidfile);
    }
    mainpid = getpid();
    fprintf(fp, "%d\n", mainpid);
    fclose(fp);
    chmod(pidfile, 0644);

    setlogmask(LOG_UPTO(LOG_INFO));

    /* Setting debug file */
    if (ICONFIG("debug") > 0) {

        debug = TRUE;

        if (CONFIG("debug_file")) {
            if ((fdebug = open_debug_log()) != -1) {

                if (!daemonize) {
                    g_printerr("Redirecting output to %s/%s.\n",
                            CONFIG_REQUIRED("log_directory"),
                            CONFIG_REQUIRED("debug_file"));
                    g_printerr(
                            "You should start with -d to daemonize Honeybrid!\n");
                }

                (void) dup2(fdebug, STDIN_FILENO);
                (void) dup2(fdebug, STDOUT_FILENO);
                (void) dup2(fdebug, STDERR_FILENO);
                if (fdebug > 2) {
                    close(fdebug);
                }
                syslog(LOG_INFO, "Starting Honeybrid.\n");
            } else {
                syslog(LOG_INFO, "file: %s", strerror(errno));
            }
        }
    }

    if (output == OUTPUT_MYSQL) {
#ifdef HAVE_MYSQL
        init_mysql_log();
#else
        errx(1, "%s: Honeybrid wasn't compiled with MySQL!", __func__);
#endif
    }

    if (output == OUTPUT_LOGFILES) {
        open_connection_log();
    }

    decision_threads = ICONFIG_REQUIRED("decision_threads");
    printdbg("%s Starting with %u decision threads.\n", H(0), decision_threads);

    de_threads = malloc(sizeof(GThread*) * decision_threads);
    de_queues = malloc(sizeof(GAsyncQueue*) * decision_threads);

    uint32_t i;
    for (i = 0; i < decision_threads; i++) {
        de_queues[i] = g_async_queue_new();
    }

    /*! init the Decision Engine threads */
    for (i = 0; i < decision_threads; i++) {
        if ((de_threads[i] = g_thread_new("de_thread", (void *) de_thread,
                GUINT_TO_POINTER(i))) == NULL) {
            errx(1, "%s: Unable to start the decision engine thread %i",
                    __func__, i);
        }
    }

    /*! initiate modules that can have only one instance */
    init_modules();

    /*! create the raw sockets for UDP/IP and TCP/IP */
    if (NOK == init_raw_sockets()) {
        errx(1, "%s: failed to create the raw sockets", __func__);
    }

    /*! create a thread for the management, cleaning stuffs and so on */
    if ((thread_clean = g_thread_new("cleaner", (void *) clean, NULL)) == NULL) {
        errx(1, "%s Unable to start the cleaning thread", __func__);
    } else {
        printdbg("%s Cleaning thread started\n", H(0));
    }

#ifdef HAVE_LIBEV

    int my_nfq_fd;

    my_nfq_fd = init_nfqueue(nfqueuenum);

    /*! Watcher for processing packets received on NF_QUEUE: */
    ev_io queue_watcher;
    ev_io_init(&queue_watcher, nfqueue_ev_cb, my_nfq_fd, EV_READ);
    ev_io_start(loop, &queue_watcher);

    printdbg("%s Starting ev_loop\n", H(0));

    ev_loop(loop, 0);

    close_nfqueue(nfqueuenum);
#else
    /*! Starting the nfqueue loop to start processing packets */
    printdbg("%s Starting netlink_loop\n", H(0));
    netlink_loop(queuenum);
#endif

    close_all();
    g_printerr("Honeybrid exited successfully.\n");
    exit(0);
}
