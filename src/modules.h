/*
 * This file is part of the honeybrid project.
 *
 * Copyright (C) 2007-2009 University of Maryland (http://www.umd.edu)
 * (Written by Robin Berthier <robinb@umd.edu>, Thomas Coquelin <coquelin@umd.edu> and Julien Vehent <julien@linuxwall.info> for the University of Maryland)
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

#ifndef _MODULES_H_
#define _MODULES_H_

#include <glib.h>

//#ifndef _NO_SSL_
#include <openssl/evp.h>
//#endif

#include<ctype.h>

#include "log.h"
#include "tables.h"
#include "types.h"

void init_modules();

void run_module(char *module_name, struct mod_args *args);

void (*get_module(char *modname))(struct mod_args *);

void save_backup_handler();

int save_backup(GKeyFile *data, char *filename);

int write_backup(char *filename, GKeyFile *data, void *userdata);

/*!************ [Basic Modules] **************/

/*!** MODULE YESNO **/
void mod_yesno(struct mod_args *args);

/*!** MODULE COUNTER **/
void mod_counter(struct mod_args *args);

/*!** MODULE RANDOM **/
void mod_random(struct mod_args *args);

/*!*********** [Advanced Modules] ************/

/*!** MODULE HASH **/
const EVP_MD *md;
int init_mod_hash();
void mod_hash(struct mod_args *args);

/*!** MODULE SOURCE **/
void mod_source(struct mod_args *args);

/*!** MODULE CONTROL **/
void mod_control(struct mod_args *args);
//int control(struct pkt_struct *pkt);

#ifdef HAVE_XMPP
/*!** MODULE DIONAEA **/
int init_mod_dionaea();
void mod_dionaea(struct mod_args *args);
#endif

/*!** MODULE TIMED SOURCE **/
void mod_source_time(struct mod_args *args);

/*!** MODULE BACKPICK RANDOM **/
void mod_backpick_random(struct mod_args *args);

/*!** MODULE VMI **/
int init_mod_vmi();
void mod_vmi(struct mod_args *args);

#endif //_MODULES_H_
