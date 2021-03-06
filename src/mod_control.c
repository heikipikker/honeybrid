/*
 * This file is part of the honeybrid project.
 *
 * 2007-2009 University of Maryland (http://www.umd.edu)
 * Robin Berthier <robinb@umd.edu>, Thomas Coquelin <coquelin@umd.edu>
 * and Julien Vehent <julien@linuxwall.info>
 *
 * 2012-2014 University of Connecticut (http://www.uconn.edu)
 * Tamas K Lengyel <tamas.k.lengyel@gmail.com>
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

/*! \file mod_control.c
 * \brief Source IP based control engine to rate limit high interaction honeypot
 *
 \author Robin Berthier 2009
 */

#include "modules.h"

/*! control
 \brief calculate the number of packets sent by a same source over a given period of time. If too many packets are sent, following packets are rejected
 Parameters required:
 function = hash;
 backup   = /etc/honeybrid/control.tb
 expiration = 600
 max_packet = 1000
 \param[in] pkts, struct that contain the packet to control
 \param[out] set result to 1 if rate limit reached, 0 otherwise
 */
mod_result_t mod_control(struct mod_args *args) {
	gchar *backup_file;

	if (args->pkt == NULL) {
		printdbg("%s Error, NULL packet\n", H(6));
		return REJECT;
	}

	printdbg("%s Module called\n", H(args->pkt->conn->id));

	mod_result_t result = DEFER;
	int expiration;
	int max_packet;
	gchar *param;
	gchar **info;
	GKeyFile *backup;

	GTimeVal t;
	g_get_current_time(&t);
	gint now = (t.tv_sec);

	char src[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &(args->pkt->packet.ip->saddr), src, INET_ADDRSTRLEN);

	/*! get the backup file for this module */
	if (NULL
			== (backup = (GKeyFile *) g_hash_table_lookup(args->node->config,
					"backup"))) {
		/*! We can't decide */
		printdbg("%s mandatory argument 'backup' undefined!\n",
				H(args->pkt->conn->id));
		return result;
	}
	/*! get the backup file path for this module */
	if (NULL
			== (backup_file = (gchar *) g_hash_table_lookup(args->node->config,
					"backup_file"))) {
		/*! We can't decide */
		printdbg("%s error, backup file path missing\n",
				H(args->pkt->conn->id));
		return result;
	}

	/*! get control parameters */
	if (NULL
			== (param = (gchar *) g_hash_table_lookup(args->node->config,
					"expiration"))) {
		/*! no value set for expiration, we go with the default one */
		expiration = 600;
	} else {
		expiration = atoi(param);
	}
	if (NULL
			== (param = (gchar *) g_hash_table_lookup(args->node->config,
					"max_packet"))) {
		/*! no value set for expiration, we go with the default one */
		max_packet = 1000;
	} else {
		max_packet = atoi(param);
	}

	if (NULL == (info = g_key_file_get_string_list(backup, "source", /* generic group name \todo: group by port number? */
	src, NULL, NULL))) {
		printdbg("%s IP not found... new entry created\n",
				H(args->pkt->conn->id));

		info = malloc(3 * sizeof(char *));

		/*! 20 characters should be enough to hold even very large numbers */
		info[0] = malloc(20 * sizeof(gchar));
		info[1] = malloc(20 * sizeof(gchar));
		info[2] = malloc(20 * sizeof(gchar));
		g_snprintf(info[0], 20, "1"); /*! counter */
		g_snprintf(info[1], 20, "%d", now); /*! first seen */
		g_snprintf(info[2], 20, "0"); /*! duration */

	} else {
		/*! We check if we need to expire this entry */
		int age = atoi(info[2]);
		if (age > expiration) {
			printdbg("%s IP found but expired... entry renewed\n",
					H(args->pkt->conn->id));

			g_snprintf(info[0], 20, "1"); /*! counter */
			g_snprintf(info[1], 20, "%d", now); /*! first seen */
			g_snprintf(info[2], 20, "0"); /*! duration */
		} else {
			printdbg("%s IP found... entry updated\n", H(args->pkt->conn->id));

			g_snprintf(info[0], 20, "%d", atoi(info[0]) + 1); /*! counter */
			g_snprintf(info[2], 20, "%d", now - atoi(info[1])); /*! duration */
		}

	}

	if (atoi(info[0]) > max_packet) {
		printdbg("%s Rate limit reached! Packet rejected\n",
				H(args->pkt->conn->id));
		result = REJECT;
	} else {
		printdbg("%s Rate limit not reached. Packet accepted\n",
				H(args->pkt->conn->id));
		result = ACCEPT;
	}

	g_key_file_set_string_list(backup, "source", src,
			(const gchar * const *) info, 3);

	save_backup(backup, backup_file);

	return result;
}

