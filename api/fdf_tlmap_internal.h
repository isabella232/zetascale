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

/*
 * File:   fdf_tlmap_internal.h
 * Author: Brian O'Krafka
 *
 * Created on September 11, 2008
 *
 * SanDisk Proprietary Material, © Copyright 2012 SanDisk, all rights reserved.
 * http://www.sandisk.com
 *
 * $Id: tlmap.h 308 2008-02-20 22:34:58Z tomr $
 */

#ifndef _ZS_TLMAP_INTERNAL_H
#define _ZS_TLMAP_INTERNAL_H

#define N_ENTRIES_TO_MALLOC    100
#define N_ITERATORS_TO_MALLOC  100

struct ZSTLMapBucket;

typedef struct ZSTLMapEntry {
    char                  *contents;
    uint64_t               datalen;
    int32_t                refcnt;
    char                  *key;
    uint32_t               keylen;
    struct ZSTLMapEntry  *next;
    struct ZSTLMapEntry  *next_lru;
    struct ZSTLMapEntry  *prev_lru;
    struct ZSTLMapBucket *bucket;
} ZSTLMapEntry_t;

typedef struct ZSTLMapBucket {
    struct ZSTLMapEntry *entry;
} ZSTLMapBucket_t;

typedef struct ZSTLIterator {
    uint64_t                enum_bucket;
    ZSTLMapEntry_t        *enum_entry;
    struct ZSTLIterator   *next;
} ZSTLIterator_t;

typedef struct ZSTLMap {
    uint64_t          nbuckets;
    uint64_t          max_entries;
    uint64_t          n_entries;
    char              use_locks;
    ZSTLMapBucket_t *buckets;
    pthread_mutex_t   mutex;
    pthread_mutex_t   enum_mutex;
    ZSTLMapEntry_t  *lru_head;
    ZSTLMapEntry_t  *lru_tail;
    void              (*replacement_callback)(void *callback_data, char *key, uint32_t keylen, char *pdata, uint64_t datalen);
    void             *replacement_callback_data;
    uint32_t          NEntries;
    uint32_t          NUsedEntries;
    ZSTLMapEntry_t  *FreeEntries;
    uint32_t          NIterators;
    uint32_t          NUsedIterators;
    struct ZSTLIterator *FreeIterators;
} ZSTLMap_t;

#endif /* _ZS_TLMAP_INTERNAL_H */
