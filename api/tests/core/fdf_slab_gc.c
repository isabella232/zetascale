#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include "zs.h"
#include "../test.h"

static ZS_cguid_t  cguid_shared;
static long size = 1024 * 1024 * 1024;
/* Following used to pass parameter to threads */
static int threads = 1, count, start_id, step = 1, obj_size, op;
enum {
	SET = 0,
	GET,
	DEL,
	ENUM
};

int set_objs(ZS_cguid_t cguid, long thr, int size, int start_id, int count, int step)
{
	int i;
	char key_str[24] = "key00";
	char *key_data;

	key_data = malloc(size);
	assert(key_data);
	memset(key_data, 0, size);

	for(i = 0; i < count; i += step)
	{
		sprintf(key_str, "key%04ld-%08d", thr, start_id + i);
		sprintf(key_data, "key%04ld-%08d_data", thr, start_id + i);

		if(zs_set(cguid, key_str, strlen(key_str) + 1, key_data, size) != ZS_SUCCESS)
			break;
    }

	free(key_data);
	fprintf(stderr, "set_objs: count=%d datalen=%d\n", i, size);
	return i;
}

int del_objs(ZS_cguid_t cguid, long thr, int start_id, int count, int step)
{
	int i;
	char key_str[24] = "key00";

	for(i = 0; i < count; i+=step)
	{
		sprintf(key_str, "key%04ld-%08d", thr, start_id + i);

		if(zs_delete(cguid, key_str, strlen(key_str) + 1) != ZS_SUCCESS)
			break;
    }

	fprintf(stderr, "del_objs: count=%d\n", i);
	return i;
}

int get_objs(ZS_cguid_t cguid, long thr, int start_id, int count, int step)
{
	int i;
	char key_str[24] = "key00";
	char *key_data;
    char        				*data;
    uint64_t     				 datalen;

	key_data = malloc(8*1024*1024);
	assert(key_data);
	memset(key_data, 0, 8*1024*1024);

	for(i = 0; i < count; i += step)
	{
		sprintf(key_str, "key%04ld-%08d", thr, start_id + i);
		sprintf(key_data, "key%04ld-%08d_data", thr, start_id + i);

		t(zs_get(cguid_shared, key_str, strlen(key_str) + 1, &data, &datalen), ZS_SUCCESS);

		assert(!memcmp(data, key_data, datalen));	
    }

	free(key_data);
	fprintf(stderr, "get_objs: count=%d datalen=%ld\n", i, datalen);
	return i;
}

int enum_objs(ZS_cguid_t cguid)
{
	int cnt = 0;
	char* key = "key00";
	char     *data;
	uint64_t datalen;
	uint32_t keylen;
	struct ZS_iterator* _zs_iterator;

    t(zs_enumerate(cguid, &_zs_iterator), ZS_SUCCESS);

    while (zs_next_enumeration(cguid, _zs_iterator, &key, &keylen, &data, &datalen) == ZS_SUCCESS) {
		cnt++;
		
		//fprintf(stderr, "%x sdf_enum: key=%s, keylen=%d, data=%s, datalen=%ld\n", (int)pthread_self(), key, keylen, data, datalen);
		//advance_spinner();
    }

    t(zs_finish_enumeration(cguid, _zs_iterator), ZS_SUCCESS);

	fprintf(stderr, "cguid: %ld enumerated count: %d\n", cguid, cnt);
	return cnt;
}

void* worker(void *arg)
{
	t(zs_init_thread(), ZS_SUCCESS);

	switch(op)
	{
		case SET:
			t(set_objs(cguid_shared, (long)arg, obj_size, start_id, count, step), count);
			break;
		case GET:
			t(get_objs(cguid_shared, (long)arg, start_id, count, step), count);
			break;
		case DEL:
			t(del_objs(cguid_shared, (long)arg, start_id, count, step), count);
			break;
		case ENUM:
			t(enum_objs(cguid_shared), count);
			break;
	}

	return 0;
}
void print_stat()
{
	ZS_stats_t stats;

	zs_get_container_stats(cguid_shared, &stats);

#define PRINT_STAT(a) \
	fprintf(stderr, "slab_gc_stat: " #a "(%d) = %ld\n", a, stats.flash_stats[a]);

	PRINT_STAT(ZS_FLASH_STATS_SLAB_GC_SEGMENTS_COMPACTED);
	PRINT_STAT(ZS_FLASH_STATS_SLAB_GC_SEGMENTS_FREED);
	PRINT_STAT(ZS_FLASH_STATS_SLAB_GC_SLABS_RELOCATED);
	PRINT_STAT(ZS_FLASH_STATS_SLAB_GC_BLOCKS_RELOCATED);
	PRINT_STAT(ZS_FLASH_STATS_SLAB_GC_RELOCATE_ERRORS);
	PRINT_STAT(ZS_FLASH_STATS_SLAB_GC_SIGNALLED);
	PRINT_STAT(ZS_FLASH_STATS_SLAB_GC_SIGNALLED_SYNC);
	PRINT_STAT(ZS_FLASH_STATS_SLAB_GC_WAIT_SYNC);
	PRINT_STAT(ZS_FLASH_STATS_SLAB_GC_SEGMENTS_CANCELLED);

	PRINT_STAT(ZS_FLASH_STATS_NUM_READ_OPS);
	PRINT_STAT(ZS_FLASH_STATS_NUM_GET_OPS);
	PRINT_STAT(ZS_FLASH_STATS_NUM_PUT_OPS);
	PRINT_STAT(ZS_FLASH_STATS_NUM_DEL_OPS);
	PRINT_STAT(ZS_FLASH_STATS_NUM_CREATED_OBJS);
	PRINT_STAT(ZS_FLASH_STATS_NUM_FREE_SEGMENTS);
}

void check_stat(int stat, long value)
{
	ZS_stats_t stats;

	zs_get_container_stats(cguid_shared, &stats);

	assert(stats.flash_stats[stat] == value);
}
void wait_free_segments(int num, int timeout)
{
	ZS_stats_t stats;

	zs_get_container_stats(cguid_shared, &stats);

	fprintf(stderr, "wait_free_segments: %d, timeout %d\n", num, timeout);
	while(stats.flash_stats[ZS_FLASH_STATS_NUM_FREE_SEGMENTS] != num &&timeout)
	{
		zs_get_container_stats(cguid_shared, &stats);
		sleep(1);
		timeout--;
	}

	assert(timeout);

	fprintf(stderr, "wait_free_segments(success): %d, timeout %d\n", num, timeout);
}


void start_threads(int count, void* (*worker)(void*))
{
	pthread_t thread_id[threads];

	int i;

	for(i = 0; i < threads; i++)
		pthread_create(&thread_id[i], NULL, worker, (void*)(long)i);

	for(i = 0; i < threads; i++)
		pthread_join(thread_id[i], NULL);
}

int main(int argc, char *argv[])
{
	ZSLoadProperties(getenv("ZS_PROPERTY_FILE"));
	ZSSetProperty("ZS_SLAB_GC", "On");
	ZSSetProperty("ZS_SLAB_GC_THRESHOLD", "70");
	ZSSetProperty("ZS_FLASH_SIZE", "3");
	ZSSetProperty("ZS_COMPRESSION", "0");
	ZSSetProperty("ZS_BLOCK_SIZE", "512");
	ZSSetProperty("ZS_FLASH_FILENAME", "/dev/shm/schooner%d");
	ZSSetProperty("ZS_LOG_FLUSH_DIR", "/dev/shm");
	ZSSetProperty("SYNC_DATA", "0");
	ZSSetProperty("ZS_O_DIRECT", "0");

	unsetenv("ZS_PROPERTY_FILE");

	size = 1 * 1024 * 1024 * 1024;

	t(zs_init(), ZS_SUCCESS);

	t(zs_init_thread(), ZS_SUCCESS);

	t(zs_create_container("container-slab-gc", size, &cguid_shared), ZS_SUCCESS);

//	int max_512 = 2 * 1024 * 1024; /* Maximum number of 512 SLABS in 1G */
	int max_512 = 1900544; /* Maximum number of 512 SLABS in 1G */

	threads = 16; /* Used in threads */

	/* Fill container with 512 SLABs using 16 threads */
	count = max_512 / threads;
	start_id = 0;
	step = 1;
	op = SET;
	obj_size = 400;
	fprintf(stderr, "SET(before): count=%d\n", threads * count / step);
	start_threads(threads, worker);
	fprintf(stderr, "SET(after): count=%d\n", threads * count / step);

	count = max_512 / threads;
	start_id = 0;
	step = 1;
	op = GET;
	fprintf(stderr, "GET(before): count=%d\n", threads * count / step);
	start_threads(threads, worker);
	fprintf(stderr, "GET(after): count=%d\n", threads * count / step);

	/* Both set should fail, there is no space in container */
	t(set_objs(cguid_shared, 1, 900, 10000000, 1, 1), 0);
	t(set_objs(cguid_shared, 1, 400, 10000000, 1, 1), 0);

	/* Delete 50% of SLABS using 16 threads */
	count = max_512 / threads; /* Delete 50% */
	start_id = 0; /* Delete every second object */
	step = 2;
	op = DEL;
	fprintf(stderr, "DEL(before): count=%d\n", threads * count / step);
	start_threads(threads, worker);
	fprintf(stderr, "DEL(after): count=%d\n", threads * count / step);

	zs_flush_container(cguid_shared);

	wait_free_segments(9, 300);

	print_stat();

	/* This should cause synchronous GC */
	fprintf(stderr, "SET(before): slab size=1024\n");
	t(set_objs(cguid_shared, 1, 900, 100000000, 393216, 1),393216);
	fprintf(stderr, "SET(after): slab size=1024\n");

	check_stat(ZS_FLASH_STATS_SLAB_GC_WAIT_SYNC, 3);

	print_stat();

	count = max_512 / threads;
	start_id = 1; 
	step = 2;
	op = GET;
	fprintf(stderr, "GET(before): count=%d\n", threads * count / step);
	start_threads(threads, worker);
	fprintf(stderr, "GET(after): count=%d\n", threads * count / step);

	fprintf(stderr, "GET(before): slab size=1024\n");
	t(get_objs(cguid_shared, 1, 100000000, 393216, 1),393216);
	fprintf(stderr, "GET(after): slab size=1024\n");

	count = max_512 / 2 + 393216;
	fprintf(stderr, "ENUM(before): count=%d\n", count);
    t(enum_objs(cguid_shared), count);
	fprintf(stderr, "ENUM(after): count=%d\n", count);

	count = max_512 / threads; /* Delete remaining 50% */
	start_id = 1; /* Delete every second object */
	step = 2;
	op = DEL;
	fprintf(stderr, "DEL(before): count=%d\n", threads * count / step);
	start_threads(threads, worker);
	fprintf(stderr, "DEL(after): count=%d\n", threads * count / step);

	zs_flush_container(cguid_shared);

	wait_free_segments(17, 300);

	print_stat();

	fprintf(stderr, "DEL(before): slab size=1024\n");
	t(del_objs(cguid_shared, 1, 100000000, 393216, 2),393216);
	fprintf(stderr, "DEL(after): slab size=1024\n");

	count = 393216/2 ;
	fprintf(stderr, "ENUM(before): count=%d\n", count);
    t(enum_objs(cguid_shared), count);
	fprintf(stderr, "ENUM(after): count=%d\n", count);

	print_stat();

	fprintf(stderr, "DEL(before): slab size=1024\n");
	t(del_objs(cguid_shared, 1, 100000001, 393216, 2),393216);
	fprintf(stderr, "DEL(after): slab size=1024\n");

	zs_flush_container(cguid_shared);

	print_stat();

	wait_free_segments(29, 400);

	fprintf(stderr, "All tests passed\n");

	zs_shutdown();

	return(0);
}

