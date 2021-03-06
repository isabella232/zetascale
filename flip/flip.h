//----------------------------------------------------------------------------
// ZetaScale
// Copyright (c) 2016, SanDisk Corp. and/or all its affiliates.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License version 2.1 as published by the Free
// Software Foundation;
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License v2.1 for more details.
//
// A copy of the GNU Lesser General Public License v2.1 is provided with this package and
// can also be found at: http://opensource.org/licenses/LGPL-2.1
// You should have received a copy of the GNU Lesser General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 59 Temple
// Place, Suite 330, Boston, MA 02111-1307 USA.
//----------------------------------------------------------------------------

#ifndef __FLIP_H
#define __FLIP_H

#ifdef FLIP_ENABLED

#include <stdbool.h>
#include <inttypes.h>
#include "api/zs.h"
#include "api/fdf_internal.h"

#define MAX_FLIP_LINE_LEN   1000
#define MAX_FLIP_CMD_WORDS  100
#define MAX_FLIP_NAME_LEN   256
#define MAX_PARAMS          10
#define FLIP_FILE  "/var/lib/zs/zs.flip"


typedef enum {
	FLIP_BOOL=1,
	FLIP_INT,
	FLIP_UINT32,
	FLIP_UINT64,
	FLIP_STR
} flip_type_t;
	
typedef struct {
	flip_type_t data_type;
	char        *name;
	void        *data;
	bool        any_data;
} flip_param_t;

typedef struct {
	flip_param_t   return_param;
	bool           is_set;
	flip_param_t   param_list[MAX_PARAMS];
	uint32_t       count;
} flip_cond_t;

#define MAX_COND_PER_FLIP            200
#define MAX_FLIP_POINTS              100

typedef struct flip_info {
	char           name[MAX_FLIP_NAME_LEN];
	uint32_t       cond_cnt;
	flip_cond_t    conditions[MAX_COND_PER_FLIP];
	uint32_t       num_params;
} flip_info_t;

#if 0
typedef enum {
	SW_CRASH_ON_MPUT,
	HW_CRASH_ON_MPUT,
	MAX_FLIP_POINTS
} flip_names_t;
#endif

typedef struct {
	uint32_t flip_ioctl_type;
	uint32_t index;
	flip_info_t *flip_instance;
} flip_ioctl_data_t;

enum {
	FLIP_IOCTL_GET_INSTANCE=1
};

/* Internal functions */
bool flip_is_valid_param(flip_info_t *f, flip_cond_t *fc, char *param_str);
bool flip_set_param(flip_info_t *f, flip_cond_t *fc, char *param_str, char *val_str);
bool flip_set_param_ptr(flip_param_t *p, char *val_str);
void flip_print_param(FILE *fp, flip_info_t *f, flip_cond_t *fc, char *param_str);
flip_info_t *lookup_flip_instance(char *name);
flip_info_t *get_flip_instance(int index);
flip_info_t *get_new_flip_instance(void);

void flip_parse_file(char *filename);
flip_type_t flip_str_to_type(char *str_type);

/* Flip external functions */
void process_flip_cmd(FILE *fp, cmd_token_t *tokens, size_t ntokens);
void process_flip_cmd_str(FILE *fp, char *cmd);

void flip_handle_ioctl(void *opaque);
void flip_init(void);
bool flip_get(char *fname, ...);
void flip_dump_file(char *filename);
int flip_set(char *name, ...);
#define flip_is_on flip_get_if_on

#endif
#endif
#ifndef __FLIP_H
#define __FLIP_H

#ifdef FLIP_ENABLED

#include <stdbool.h>
#include <inttypes.h>
#include "api/zs.h"
#include "api/fdf_internal.h"

#define MAX_FLIP_LINE_LEN   1000
#define MAX_FLIP_CMD_WORDS  100
#define MAX_FLIP_NAME_LEN   256
#define MAX_PARAMS          10
#define FLIP_FILE  "/var/lib/zs/btree.flip"


typedef enum {
	FLIP_BOOL=1,
	FLIP_INT,
	FLIP_UINT32,
	FLIP_UINT64,
	FLIP_STR
} flip_type_t;
	
typedef struct {
	flip_type_t data_type;
	char        *name;
	void        *data;
	bool        any_data;
} flip_param_t;

typedef struct flip_info {
	char           name[MAX_FLIP_NAME_LEN];
	flip_param_t   return_param;
	uint32_t       num_params;
	bool           is_set;
	flip_param_t   param_list[MAX_PARAMS];
	uint32_t       count;
} flip_info_t;

typedef enum {
	SW_CRASH_ON_MPUT,
	HW_CRASH_ON_MPUT,
	MAX_FLIP_POINTS
} flip_names_t;

typedef struct {
	uint32_t flip_ioctl_type;
	uint32_t index;
	flip_info_t *flip_instance;
} flip_ioctl_data_t;

enum {
	FLIP_IOCTL_GET_INSTANCE=1
};

/* Internal functions */
bool flip_is_valid_param(flip_info_t *f, char *param_str);
bool flip_set_param(flip_info_t *f, char *param_str, char *val_str);
bool flip_set_param_ptr(flip_param_t *p, char *val_str);
void flip_print_param(FILE *fp, flip_info_t *f, char *param_str);
flip_info_t *lookup_flip_instance(char *name);
flip_info_t *get_flip_instance(int index);
flip_info_t *get_new_flip_instance(void);

void flip_parse_file(char *filename);
flip_type_t flip_str_to_type(char *str_type);

/* Flip external functions */
void process_flip_cmd(FILE *fp, cmd_token_t *tokens, size_t ntokens);

void flip_handle_ioctl(void *opaque);
void flip_init(void);
bool flip_get(char *fname, ...);
void flip_dump_file(char *filename);
int flip_set(char *name, ...);
#define flip_is_on flip_get_if_on

#endif
#endif
