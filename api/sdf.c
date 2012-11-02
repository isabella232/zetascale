//  sdf.cc

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include "sdf.h"
#include "sdf_internal.h"
#include "utils/properties.h"
#include "protocol/protocol_utils.h"
#include "protocol/protocol_common.h"
#include "protocol/action/action_thread.h"
#include "protocol/action/async_puts.h"
#include "protocol/home/home_flash.h"
#include "protocol/action/async_puts.h"
#include "protocol/replication/copy_replicator.h"
#include "protocol/replication/replicator.h"
#include "protocol/replication/replicator_adapter.h"
#include "ssd/fifo/mcd_ipf.h"
#include "ssd/fifo/mcd_osd.h"
#include "ssd/fifo/mcd_bak.h"
#include "shared/init_sdf.h"
#include "shared/private.h"
#include "shared/open_container_mgr.h"
#include "shared/container_meta.h"
#include "shared/name_service.h"
#include "shared/shard_compute.h"
#include "shared/internal_blk_obj_api.h"
#include "agent/agent_common.h"
#include "agent/agent_helper.h"

#define LOG_ID PLAT_LOG_ID_INITIAL
#define LOG_CAT PLAT_LOG_CAT_SDF_NAMING
#define LOG_DBG PLAT_LOG_LEVEL_DEBUG
#define LOG_INFO PLAT_LOG_LEVEL_INFO
#define LOG_ERR PLAT_LOG_LEVEL_ERROR
#define LOG_WARN PLAT_LOG_LEVEL_WARN
#define LOG_FATAL PLAT_LOG_LEVEL_FATAL
#define BUF_LEN 2048

time_t current_time = 0;

/*
** Externals
*/
extern void *cmc_settings;

extern 
SDF_cguid_t generate_cguid(
	SDF_internal_ctxt_t	*pai, 
	const char		*path, 
	uint32_t 		 node, 
	int64_t 		 cntr_id
	);

extern 
SDF_container_meta_t * build_meta(
	const char 		*path, 
	SDF_container_props_t 	 props, 
	SDF_cguid_t 		 cguid, 
	SDF_shardid_t 		 shard
	);

extern 
SDF_shardid_t build_shard(
	struct SDF_shared_state *state, 
	SDF_internal_ctxt_t 	*pai,
        const char 		*path, 
	int 			 num_objs,  
	uint32_t 		 in_shard_count,
        SDF_container_props_t 	 props,  
	SDF_cguid_t 		 cguid,
        enum build_shard_type 	 build_shard_type, 
	const char 		*cname
	);

typedef enum {
    apgrx = 0,
    apgrd,
    apcoe,
    ahgtr,
    ahcob,
    ahcwd,
    hacrc,
    hacsc,
    hagrf,
    hagrc,
    owrites_s,
    owrites_m,
    ipowr_s,
    ipowr_m,
    newents,
    MCD_NUM_SDF_COUNTS,
}SDF_cache_counts_t;

static char * SDFCacheCountStrings[] = {
    "APGRX",
    "APGRD",
    "APCOE",
    "AHGTR",
    "AHCOB",
    "AHCWD",
    "HACRC",
    "HACSC",
    "HAGRF",
    "HAGRC",
    "overwrites_s",
    "overwrites_m",
    "inplaceowr_s",
    "inplaceowr_m",
    "new_entries",
};

typedef enum {
    MCD_STAT_GET_KEY = 0,
    MCD_STAT_GET_DATA,
    MCD_STAT_SET_KEY,
    MCD_STAT_SET_DATA,
    MCD_STAT_GET_TIME,
    MCD_STAT_SET_TIME,
    MCD_STAT_MAX_COUNT,
} mcd_fth_stat_t;

typedef enum {
    SDF_DRAM_CACHE_HITS = 0,
    SDF_DRAM_CACHE_MISSES,
    SDF_FLASH_CACHE_HITS,
    SDF_FLASH_CACHE_MISSES,
    SDF_DRAM_CACHE_CASTOUTS,
    SDF_DRAM_N_OVERWRITES,
    SDF_DRAM_N_IN_PLACE_OVERWRITES,
    SDF_DRAM_N_NEW_ENTRY,
    MCD_NUM_SDF_STATS,
} SDF_cache_stats_t;

extern
struct shard *
container_to_shard( 
	SDF_internal_ctxt_t * pai, 
	local_SDF_CONTAINER lc );

extern
void
shard_recover_phase2( 
	mcd_osd_shard_t * shard );

extern 
SDF_status_t create_put_meta(
	SDF_internal_ctxt_t 	*pai, 
	const char 		*path, 
	SDF_container_meta_t 	*meta,
        SDF_cguid_t 		 cguid
	);

extern 
SDF_boolean_t agent_engine_pre_init(
	struct sdf_agent_state 	*state, 
	int 			 argc, 
	char 			*argv[]
	);

extern 
SDF_boolean_t agent_engine_post_init(
	struct sdf_agent_state 	*state
	);

extern 
SDF_status_t delete_container_internal_low(
	SDF_internal_ctxt_t 	*pai, 
	const char 		*path, 
	SDF_boolean_t 		 serialize,
	int *deleted
	);

SDF_status_t SDFGetStatsStr (
    struct SDF_thread_state *sdf_thread_state,
        SDF_cguid_t cguid, char *stats_str );
void action_stats_new_cguid(SDF_internal_ctxt_t *pac, char *str, int size, SDF_cguid_t cguid);
void action_stats(SDF_internal_ctxt_t *pac, char *str, int size);
#if 0
static void set_log_level( unsigned int log_level )
{
    char                buf[80];
    char              * levels[] = { "devel", "trace", "debug", "diagnostic",
                                     "info", "warn", "error", "fatal" };

    sprintf( buf, "apps/memcached/server=%s", levels[log_level] );
    plat_log_parse_arg( buf );
}
#endif

static int sdf_check_delete_in_future(void *data)
{
    return(0);
}

static void load_settings(flash_settings_t *osd_settings)
{
    (void) strcpy(osd_settings->aio_base, getProperty_String("AIO_BASE_FILENAME", "/schooner/data/schooner%d")); // base filename of flash files
//    (void) strcpy(osd_settings->aio_base, getProperty_String("AIO_BASE_FILENAME", "/schooner/backup/schooner%d")); // base filename of flash files
    osd_settings->aio_create          = 1;// use O_CREAT - membrain sets this to 0
    osd_settings->aio_total_size      = getProperty_Int("AIO_FLASH_SIZE_TOTAL", 0); // this flash size counts!
    osd_settings->aio_sync_enabled    = getProperty_Int("AIO_SYNC_ENABLED", 0); // AIO_SYNC_ENABLED
    osd_settings->rec_log_verify      = 0;
    osd_settings->enable_fifo         = 1;
    osd_settings->bypass_aio_check    = 0;
    osd_settings->chksum_data         = 1; // membrain sets this to 0
    osd_settings->chksum_metadata     = 1; // membrain sets this to 0
    osd_settings->sb_data_copies      = 0; // use default
    osd_settings->multi_fifo_writers  = getProperty_Int("SDF_MULTI_FIFO_WRITERS", 1);
    osd_settings->aio_wc              = false;
    osd_settings->aio_error_injection = false;
    osd_settings->aio_queue_len       = MCD_MAX_NUM_FTHREADS;

    // num_threads // legacy--not used

    osd_settings->num_cores        = getProperty_Int("SDF_FTHREAD_SCHEDULERS", 1); // "-N" 
    osd_settings->num_sdf_threads  = getProperty_Int("SDF_THREADS_PER_SCHEDULER", 1); // "-T"

    osd_settings->sdf_log_level    = getProperty_Int("SDF_LOG_LEVEL", 4); 
    osd_settings->aio_num_files    = getProperty_Int("AIO_NUM_FILES", 1); // "-Z"
    osd_settings->aio_sub_files    = 0; // what are these? ignore?
    osd_settings->aio_first_file   = 0; // "-z" index of first file! - membrain sets this to -1
    osd_settings->mq_ssd_balance   = 0;  // what does this do?
    osd_settings->no_direct_io     = 0; // do NOT use O_DIRECT
    osd_settings->sdf_persistence  = 0; // "-V" force containers to be persistent!
    osd_settings->max_aio_errors   = getProperty_Int("MEMCACHED_MAX_AIO_ERRORS", 1000 );
    osd_settings->check_delete_in_future = sdf_check_delete_in_future;
    osd_settings->pcurrent_time	    = &current_time;
    osd_settings->is_node_independent = 1;
    osd_settings->ips_per_cntr	    = 1;
    osd_settings->rec_log_size_factor = 0;
}

/*
** Globals
*/
static struct sdf_agent_state agent_state;
static sem_t Mcd_fsched_sem;
static sem_t Mcd_initer_sem;

ctnr_map_t CtnrMap[MCD_MAX_NUM_CNTRS];

int get_ctnr_from_cguid(SDF_cguid_t cguid)
{
    int i;
    int i_ctnr = -1;

    for (i=0; i<MCD_MAX_NUM_CNTRS; i++) {
        if (CtnrMap[i].cguid == cguid) {
	    i_ctnr = i;
	    break;
	}
    }
    return(i_ctnr);
}

int get_ctnr_from_cname(char *cname)
{
    int i;
    int i_ctnr = -1;

    for (i=0; i<MCD_MAX_NUM_CNTRS; i++) {
	if ((NULL != CtnrMap[i].cname) && (0 == strcmp(CtnrMap[i].cname, cname))) {
            i_ctnr = i;
            break;
        }
    }
    return(i_ctnr);
}



SDF_status_t SDFGetContainers(
	struct SDF_thread_state	*sdf_thread_state,
	SDF_cguid_t             *cguids,
	uint32_t                *n_cguids
	)
{
    int   i;
    int   n_containers;
    n_containers = 0;
    for (i=0; i<MCD_MAX_NUM_CNTRS; i++) {
        if( CtnrMap[i].cguid != 0 ) {
            cguids[n_containers] = Mcd_containers[i].cguid;
            n_containers++;
        }
    }
    *n_cguids = n_containers;
    return SDF_SUCCESS;
}

#define MCD_FTH_STACKSIZE       81920

static void mcd_fth_initer(uint64_t arg)
{
    /*
     *  Final SDF Initialization
     */
    struct sdf_agent_state    *state = (struct sdf_agent_state *) arg;

    if ( SDF_TRUE != agent_engine_post_init( state ) ) {
        mcd_log_msg( 20147, PLAT_LOG_LEVEL_FATAL,
                     "agent_engine_post_init() failed" );
        plat_assert_always( 0 == 1 );
    }

    mcd_fth_start_osd_writers();

    /*
     * signal the parent thread
     */
    sem_post( &Mcd_initer_sem );
}

#define STAT_BUFFER_SIZE 16384

static void *stats_thread(void *arg) {
    SDF_cguid_t cguids[128];
    uint32_t n_cguids;
    struct SDF_thread_state *sdf_thd_state;
    sdf_thd_state = (struct SDF_thread_state *)arg;
    char stats_str[STAT_BUFFER_SIZE];
    FILE *stats_log;
    int i;
    struct SDF_thread_state *thd_state;

    thd_state = SDFInitPerThreadState((struct SDF_state *)arg);
    if( thd_state == NULL ) {
         fprintf(stderr,"Stats Thread:Unable to open the log file /tmp/fdf_stats.log. Exiting\n");
        return NULL;
    }

    stats_log = fopen("/var/log/fdfstats.log","a+");
    if( stats_log == NULL ) {
        fprintf(stderr,"Stats Thread:Unable to open the log file /tmp/fdf_stats.log. Exiting\n");
        return NULL;
    }

    while(1) {
        SDFGetContainers(thd_state,cguids,&n_cguids);
        if( n_cguids <= 0 ) {
             fprintf(stderr,"Stats Thread:No container exists\n");    
             sleep(10);
             continue;
        }
        for ( i = 0; i < n_cguids; i++ ) {
            memset(stats_str,0,STAT_BUFFER_SIZE);
            SDFGetStatsStr(thd_state,cguids[i],stats_str);
            fputs(stats_str,stats_log);
            //fprintf(stderr,"%s",stats_str);
            sleep(1);
        }
        sleep(10);
    }
    fclose(stats_log);
}

static void *scheduler_thread(void *arg)
{
    // mcd_osd_assign_pthread_id();

    /*
     * signal the parent thread
     */
    sem_post( &Mcd_fsched_sem );

    fthSchedulerPthread( 0 );

    return NULL;
}


static int mcd_fth_cleanup( void )
{
    return 0;   /* SUCCESS */
}

void start_stats_thread(struct SDF_state *sdf_state) {
    pthread_t thd;
    int rc;
    fprintf(stderr,"starting stats thread\n");
    rc = pthread_create(&thd,NULL,stats_thread,(void *)sdf_state);
    if( rc != 0 ) {
        fprintf(stderr,"Unable to start the stats thread\n");
    }
}


static void *run_schedulers(void *arg)
{
    int                 rc;
    int                 i;
    pthread_t           fth_pthreads[MCD_MAX_NUM_SCHED];
    pthread_attr_t      attr;
    int                 num_sched;

    num_sched = (uint64_t) arg;

    /*
     * create the fthread schedulers
     */
    sem_init( &Mcd_fsched_sem, 0, 0 );

    pthread_attr_init( &attr );
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_JOINABLE );

    for ( i = 1; i < num_sched; i++ ) {
        rc = pthread_create( &fth_pthreads[i], &attr, scheduler_thread,
                             NULL);
        if ( 0 != rc ) {
            mcd_log_msg( 20163, PLAT_LOG_LEVEL_FATAL,
                         "pthread_create() failed, rc=%d", rc );
	    plat_assert(0);
        }
        mcd_log_msg( 20164, PLAT_LOG_LEVEL_DEBUG, "scheduler %d created",
                     (int)fth_pthreads[i] );
    }

    /*
     * wait for fthreads initialization
     */
    for ( i = 1; i < num_sched; i++ ) {
        do {
            rc = sem_wait( &Mcd_fsched_sem );
        } while (rc == -1 && errno == EINTR);
        plat_assert( 0 == rc );
    }

    /*
     * Turn the main thread into a fthread scheduler directly as a
     * workaround for ensuring all the fthread stacks are registered
     * from the correct pthread in the N1 case.
     */

    fthSchedulerPthread( 0 );

    for ( i = 1; i < num_sched; i++ ) {
        pthread_join( fth_pthreads[i], NULL );
    }

    mcd_fth_cleanup();

    return(NULL);
}

/*
** API
*/
SDF_status_t SDFInit(
	struct SDF_state 	**sdf_state, 
	int 			  argc, 
	char 			**argv
	)
{
    int                 rc;
    pthread_t           run_sched_pthread;
    pthread_attr_t      attr;
    uint64_t            num_sched;
    struct timeval 	timer;

    sem_init( &Mcd_initer_sem, 0, 0 );

    gettimeofday( &timer, NULL );
    current_time = timer.tv_sec;

    *sdf_state = (struct SDF_state *) &agent_state;

    mcd_aio_register_ops();
    mcd_osd_register_ops();


    //  Initialize a crap-load of settings
    load_settings(&(agent_state.flash_settings));

    //  Set the logging level
    //set_log_level(agent_state.flash_settings.sdf_log_level);

    if (!agent_engine_pre_init(&agent_state, argc, argv)) {
        return(SDF_FAILURE); 
    }

    // spawn initer thread (like mcd_fth_initer)
    fthResume( fthSpawn( &mcd_fth_initer, MCD_FTH_STACKSIZE ),
               (uint64_t) &agent_state);

    ipf_set_active(1);

    // spawn scheduler startup process
    pthread_attr_init( &attr );
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_JOINABLE );

    num_sched = agent_state.flash_settings.num_cores;

    rc = pthread_create( &run_sched_pthread, &attr, run_schedulers,
			 (void *) num_sched);
    if ( 0 != rc ) {
	mcd_log_msg( 20163, PLAT_LOG_LEVEL_FATAL,
		     "pthread_create() failed, rc=%d", rc );
	return rc;
    }
    mcd_log_msg(150022, PLAT_LOG_LEVEL_DEBUG, "scheduler startup process created");

    // Wait until mcd_fth_initer is done
    do {
	rc = sem_wait( &Mcd_initer_sem );
    } while (rc == -1 && errno == EINTR);
    plat_assert( 0 == rc );
    if ( getProperty_Int("SDF_STATS_THREAD", 0) == 1 ) {
        fprintf(stderr,"Starting the stats thread. Check the stats at /var/log/fdfstats.log\n");
        start_stats_thread(*sdf_state);
    }

    return(SDF_SUCCESS);
}

void SDFShutdown() {
    // Stop the Messaging sub system
    fprintf(stderr,"Shutting down the FDF\n");
    sdf_msg_stop();
}

struct SDF_thread_state * SDFInitPerThreadState(
	struct SDF_state *sdf_state
	)
{
    struct SDF_thread_state *pts_out;
    SDF_action_init_t       *pai;
    SDF_action_init_t       *pai_new;
    SDF_action_thrd_state_t *pts;

    struct sdf_agent_state    *state = &agent_state;

    fthSpawnPthread();

    pai = &state->ActionInitState;

    pai_new = (SDF_action_init_t *) plat_alloc( sizeof(SDF_action_init_t) );
    plat_assert_always( NULL != pai_new );
    memcpy( pai_new, pai, (size_t) sizeof(SDF_action_init_t));

    //==================================================


    pts = (SDF_action_thrd_state_t *)
        plat_alloc( sizeof(SDF_action_thrd_state_t) );
    plat_assert_always( NULL != pts );
    pts->phs = state->ActionInitState.pcs;

    pai_new->pts = (void *) pts;
    InitActionAgentPerThreadState(pai_new->pcs, pts, pai_new);
    pai_new->paio_ctxt = pts->pai->paio_ctxt;

    pai_new->ctxt = ActionGetContext(pts);

    pts_out = (struct SDF_thread_state *) pai_new;

    return(pts_out);
}

#ifdef SDFAPI      
SDF_status_t SDFGetContainerProps(
	struct SDF_thread_state	*sdf_thread_state, 
	SDF_cguid_t 		 cguid, 
	SDF_container_props_t 	*pprops
	)
{   
    SDF_status_t             status = SDF_SUCCESS;
    SDF_container_meta_t     meta;
    SDF_internal_ctxt_t     *pai = (SDF_internal_ctxt_t *) sdf_thread_state;

    SDFStartSerializeContainerOp(pai);  
    if ((status = name_service_get_meta(pai, cguid, &meta)) == SDF_SUCCESS) {
        *pprops = meta.properties;      
    }              
    SDFEndSerializeContainerOp(pai);   
                   
    return(status);
}
    
SDF_status_t SDFSetContainerProps(
	struct SDF_thread_state	*sdf_thread_state, 
	SDF_cguid_t 	 	 cguid,
	SDF_container_props_t 	*pprops
	)
{
    SDF_status_t             status = SDF_SUCCESS;
    SDF_container_meta_t     meta;
    SDF_internal_ctxt_t     *pai = (SDF_internal_ctxt_t *) sdf_thread_state;

    SDFStartSerializeContainerOp(pai);
    if ((status = name_service_get_meta(pai, cguid, &meta)) == SDF_SUCCESS) {
        meta.properties = *pprops;
        status = name_service_put_meta(pai, cguid, &meta);
    }
    SDFEndSerializeContainerOp(pai);

    return(status);
}

#define CONTAINER_PENDING

static uint64_t cid_counter = 0;

SDF_status_t SDFCreateContainer(
	struct SDF_thread_state	*sdf_thread_state, 
	char 		        *cname,
	SDF_container_props_t 	*properties, 
	SDF_cguid_t 		*cguid 
	)
{
    int					 i				= 0;
    uint64_t  				 cid				= 0;
    struct SDF_shared_state 		*state 				= &sdf_shared_state;
    SDF_status_t 			 status 			= SDF_FAILURE;
    SDF_shardid_t 			 shardid 			= SDF_SHARDID_INVALID;
    SDF_container_meta_t 		*meta 				= NULL;
    SDF_CONTAINER_PARENT 		 parent 			= containerParentNull;
    local_SDF_CONTAINER_PARENT 		 lparent 			= NULL;
    SDF_boolean_t 			 isCMC				= SDF_FALSE;
    uint32_t 				 in_shard_count			= 0;
    SDF_vnode_t 			 home_node			= 0;
    uint32_t 				 num_objs 			= 0;
    const char 				*writeback_enabled_string	= NULL;
    SDF_internal_ctxt_t			*pai 				= (SDF_internal_ctxt_t *) sdf_thread_state;

    plat_log_msg(20819, LOG_CAT, LOG_INFO, "%s", cname);

    if ((!properties->container_type.caching_container) && (!properties->cache.writethru)) {
        plat_log_msg(30572, LOG_CAT, LOG_ERR,
                     "Writeback caching can only be enabled for eviction mode containers");
        return(SDF_FAILURE_INVALID_CONTAINER_TYPE);
    }
    if (!properties->cache.writethru) {
        if (!properties->container_type.caching_container) {
            plat_log_msg(30572, LOG_CAT, LOG_ERR,
                         "Writeback caching can only be enabled for eviction mode containers");
            return(SDF_FAILURE_INVALID_CONTAINER_TYPE);
        } else {
            writeback_enabled_string = getProperty_String("SDF_WRITEBACK_CACHE_SUPPORT", "On");
            if (strcmp(writeback_enabled_string, "On") != 0) {
                plat_log_msg(30575, LOG_CAT, LOG_ERR,
                             "Cannot enable writeback caching for container '%s' because writeback caching is disabled.",
                             cname);

                properties->cache.writethru = SDF_TRUE;
            }
        }
    }

    SDFStartSerializeContainerOp(pai);

    cid = cid_counter++;

    if (strcmp(cname, CMC_PATH) == 0) {
        *cguid = CMC_CGUID;
        isCMC = SDF_TRUE;
        home_node = CMC_HOME;
    } else {
        *cguid = generate_cguid(pai, cname, init_get_my_node_id(), cid); // Generate the cguid
#if 0
		for (i=0; i<MCD_MAX_NUM_CNTRS; i++) {
	    	if (CtnrMap[i].cguid == 0) {
	        	// this is an unused map entry
				CtnrMap[i].cname = plat_alloc(strlen(cname)+1);
				if (CtnrMap[i].cname == NULL) {
		    		status = SDF_FAILURE_MEMORY_ALLOC;
		    		SDFEndSerializeContainerOp(pai);
		    		return(status);
				}
				strcpy(CtnrMap[i].cname, cname);
				CtnrMap[i].cguid         = *cguid;
				CtnrMap[i].sdf_container = containerNull;
				break;
	    	}
		}
#endif
		if (i == MCD_MAX_NUM_CNTRS) {
	    	plat_log_msg(150023, LOG_CAT,LOG_ERR, "SDFCreateContainer failed for container %s because 128 containers have already been created.", cname);
	    	status = SDF_TOO_MANY_CONTAINERS;
	    	SDFEndSerializeContainerOp(pai);
	    	return(status);
		}
        isCMC = SDF_FALSE;
        home_node = init_get_my_node_id();
    }

    /*
     *  Save the cguid in a useful place so that the replication code in
     *  mcd_ipf.c can find it.
     */
#if 0
    if (cmc_settings != NULL) {
        struct settings *settings = cmc_settings;
        int  i;

        for (i = 0; i < sizeof(settings->vips) / sizeof(settings->vips[0]); i++) {
            if (properties->container_id.container_id ==
                   settings->vips[i].container_id) {
                   settings->vips[i].cguid = *cguid;
            }
        }
    }
#endif

    num_objs = properties->container_id.num_objs;

    /*
     * XXX: Provide default replication parameters for non-CMC containers.
     * It would be better for this to be a test program runtime option for
     * default container properties.
     *
     * 1/6/09: Enabled CMC replication.
     *
     * XXX: Disabling CMC replication doesn't seem to work in the current
     * code, perhaps because it's getting to the replication code with
     * a type of SDF_REPLICATION_NONE?  Try to make the CMC available
     * everywhere since it should be write once.
     */
    if (/*!isCMC &&*/ state->config.always_replicate) {
        if (isCMC && state->config.replication_type != SDF_REPLICATION_SIMPLE) {
            properties->replication.num_replicas = state->config.nnodes;
            properties->replication.num_meta_replicas = 0;
            properties->replication.type = SDF_REPLICATION_SIMPLE;
        } else {
            properties->replication.num_replicas = state->config.always_replicate;
            properties->replication.num_meta_replicas = 1;
            properties->replication.type = state->config.replication_type;
        }

        properties->replication.enabled = 1;
        properties->replication.synchronous = 1;
        if( properties->replication.type == SDF_REPLICATION_V1_2_WAY ) {
            properties->replication.num_replicas = 2;
        }
    }

    /*
       How do we set shard_count :
     1) check if shard_count in the incoming Container properties is non-zero
     2) else use the shard_count from the properties file (by incredibly
        complicated maze of initialization ending up in state->config)
     3) else  use the hard-coded SDF_SHARD_DEFAULT_SHARD_COUNT macro
    */

    in_shard_count = properties->shard.num_shards?properties->shard.num_shards:
        (state->config.shard_count?
         state->config.shard_count:SDF_SHARD_DEFAULT_SHARD_COUNT);

     /* XXX: If we reached here without having set the shard_count in
        container properties, set the property here. In the future we
        might want to assert on this condition.
     */
    if (properties->shard.num_shards == 0) {
        properties->shard.num_shards = in_shard_count;
    }

#ifdef MULTIPLE_FLASH_DEV_ENABLED
    plat_log_msg(21527, LOG_CAT, LOG_INFO, "Container: %s - Multi Devs: %d",
                 path, state->config.flash_dev_count);
#else
    plat_log_msg(21528, LOG_CAT, LOG_INFO, "Container: %s - Single Dev",
                 cname);
#endif
    plat_log_msg(21529, LOG_CAT, LOG_INFO, "Container: %s - Num Shards: %d",
                 cname, properties->shard.num_shards);

    plat_log_msg(21530, LOG_CAT, LOG_INFO, "Container: %s - Num Objs: %d",
                 cname, state->config.num_objs);

    plat_log_msg(21531, LOG_CAT, LOG_INFO, "Container: %s - DEBUG_MULTI_SHARD_INDEX: %d",
                 cname, getProperty_Int("DEBUG_MULTISHARD_INDEX", -1));


    if (ISEMPTY(cname)) {
        status = SDF_INVALID_PARAMETER;
    } else if (doesContainerExistInBackend(pai, cname)) {
        #ifdef CONTAINER_PENDING
        // Unset parent delete flag if with deleted flag
        if (!isContainerParentNull(parent = isParentContainerOpened(cname))) {
                local_SDF_CONTAINER_PARENT lparent = getLocalContainerParent(&lparent, parent);

             if (lparent->delete_pending == SDF_TRUE) {
                    if (!isCMC && (status = name_service_lock_meta(pai, cname)) != SDF_SUCCESS) {
                            plat_log_msg(21532, LOG_CAT, LOG_ERR, "failed to lock %s", cname);
                }

                lparent->delete_pending = SDF_FALSE;

                if (!isCMC && (status = name_service_unlock_meta(pai, cname)) != SDF_SUCCESS) {
                            plat_log_msg(21533, LOG_CAT, LOG_ERR, "failed to unlock %s", cname);
                }

             }

            releaseLocalContainerParent(&lparent); // TODO C++ please!
        }
        #endif
       status = SDF_CONTAINER_EXISTS;
    } else {
	SDF_container_props_t 	 properties2 = *properties;
        if ((shardid = build_shard(state, pai, cname, num_objs,
                                   in_shard_count, properties2, *cguid,
                                   isCMC ? BUILD_SHARD_CMC : BUILD_SHARD_OTHER, cname)) <= SDF_SHARDID_LIMIT) {
            properties2.cguid = *cguid;
            if ((meta = build_meta(cname, properties2, *cguid, shardid)) != NULL) {
#ifdef STATE_MACHINE_SUPPORT
                SDFUpdateMetaClusterGroupInfo(pai,meta,properties->container_id.container_id);
#endif
                if (create_put_meta(pai, cname, meta, *cguid) == SDF_SUCCESS) {

                    // For non-CMC, map the cguid and cname
                    if (!isCMC && (name_service_create_cguid_map(pai, cname, *cguid)) != SDF_SUCCESS) {
                        plat_log_msg(21534, LOG_CAT, LOG_ERR,
                                     "failed to map cguid: %s", cname);
                    }
                    if (!isCMC && (status = name_service_lock_meta(pai, cname)) != SDF_SUCCESS) {
                        plat_log_msg(21532, LOG_CAT, LOG_ERR, "failed to lock %s", cname);
                    } else if (!isContainerParentNull(parent = createParentContainer(pai, cname, meta))) {
                        lparent = getLocalContainerParent(&lparent, parent); // TODO C++ please!
                        lparent->container_type = properties->container_type.type;
                        if (lparent->container_type == SDF_BLOCK_CONTAINER) {
                            lparent->blockSize = properties->specific.block_props.blockSize;
                        }
                        releaseLocalContainerParent(&lparent); // TODO C++ please!

                        status = SDF_SUCCESS;

                        if (!isCMC && (status = name_service_unlock_meta(pai, cname)) != SDF_SUCCESS) {
                            plat_log_msg(21533, LOG_CAT, LOG_ERR, "failed to unlock %s", cname);
                        }
                    } else {
                        plat_log_msg(21535, LOG_CAT, LOG_ERR, "cname=%s, build_shard failed", cname);
                    }
                } else {
                    plat_log_msg(21536, LOG_CAT, LOG_ERR, "cname=%s, createParentContainer() failed", cname);
                }

                container_meta_destroy(meta);

            } else {
                plat_log_msg(21537, LOG_CAT, LOG_ERR, "cname=%s, build_meta failed", cname);
            }
        } else {
            plat_log_msg(21535, LOG_CAT, LOG_ERR, "cname=%s, build_shard failed", cname);
        }
    }

    plat_log_msg(21511, LOG_CAT, LOG_DBG, "%s - %s", cname, SDF_Status_Strings[status]);

    if (status == SDF_SUCCESS) {
    } else if (status != SDF_SUCCESS && status != SDF_CONTAINER_EXISTS) {
        plat_log_msg(21538, LOG_CAT, LOG_ERR, "cname=%s, function returned status = %u", cname, status);
        name_service_remove_meta(pai, cname);
#if 0
        /*
         * XXX We're leaking the rest of the shard anyways, and this could
         * cause dangling pointer problems to manifest from the coalesce, etc.
         * internal flash threads so skip it.
         */
        shardDelete(shard);
        xxxzzz continue from here
#endif
    }
#ifdef SIMPLE_REPLICATION
/*
    else if (status == SDF_SUCCESS) {
        SDFRepDataStructAddContainer(pai, properties, *cguid);
    }
*/
#endif

    if (SDF_SUCCESS == status && CMC_CGUID != *cguid) {
        for (i=0; i<MCD_MAX_NUM_CNTRS; i++) {
            if (CtnrMap[i].cguid == 0) {
                // this is an unused map entry
                CtnrMap[i].cname = plat_alloc(strlen(cname)+1);
                if (CtnrMap[i].cname == NULL) {
                    status = SDF_FAILURE_MEMORY_ALLOC;
                    SDFEndSerializeContainerOp(pai);
                    return(status);
                }
                strcpy(CtnrMap[i].cname, cname);
                CtnrMap[i].cguid         = *cguid;
                CtnrMap[i].sdf_container = containerNull;
#ifdef SDFAPIONLY
                Mcd_containers[i].cguid = *cguid;
                strcpy(Mcd_containers[i].cname, cname);
#endif /* SDfAPIONLY */
                break;
            }           
	}
    }
    SDFEndSerializeContainerOp(pai);
    plat_log_msg(21511, LOG_CAT, LOG_INFO, "%s - %s", cname, SDF_Status_Strings[status]);
    return (status);
}

SDF_status_t SDFOpenContainer(
	struct SDF_thread_state	*sdf_thread_state, 
	SDF_cguid_t 	 	 cguid,
	SDF_container_mode_t 	 mode
	) 
{               
    SDF_status_t 		status 		= SDF_SUCCESS;
    local_SDF_CONTAINER 	lc 		= NULL;
    SDF_CONTAINER_PARENT 	parent;
    local_SDF_CONTAINER_PARENT 	lparent 	= NULL;
    int 			log_level 	= LOG_ERR;
    char 			*path 		= CMC_PATH;
    int  			i_ctnr 		= 0;
    SDF_CONTAINER 		container 	= containerNull;
    SDF_internal_ctxt_t     	*pai 		= (SDF_internal_ctxt_t *) sdf_thread_state;
#ifdef SDFAPIONLY
    //mcd_container_t 		cntr;
    mcd_osd_shard_t 		*mcd_shard	= NULL;
    struct shard		*shard		= NULL;
#endif /* SDFAPIONLY */
                        
    plat_log_msg(21630, LOG_CAT, LOG_INFO, "%lu", cguid);

    SDFStartSerializeContainerOp(pai);

    if (CMC_CGUID != cguid) {
	
		i_ctnr = get_ctnr_from_cguid(cguid);

    	if (i_ctnr != -1)
			path = CtnrMap[i_ctnr].cname;
		else
        	status = SDF_INVALID_PARAMETER;
    }

    if (ISEMPTY(path))
        status = SDF_INVALID_PARAMETER;

    if ( (mode < SDF_READ_MODE) || (mode >= SDF_CNTR_MODE_MAX) ) {
        status = SDF_INVALID_PARAMETER;
    }

	if(status != SDF_SUCCESS || !isContainerNull(CtnrMap[i_ctnr].sdf_container))
	{
	    SDFEndSerializeContainerOp(pai);
		plat_log_msg(160032, LOG_CAT, log_level, "Already opened or error: %s - %s", path, SDF_Status_Strings[status]);
		return status;
	}

    if (!isContainerParentNull(parent = isParentContainerOpened(path))) {

        plat_log_msg(20819, LOG_CAT, LOG_INFO, "%s", path);

        // Test for pending delete
        lparent = getLocalContainerParent(&lparent, parent);
        if (lparent->delete_pending == SDF_TRUE) {
            // Need a different error?
            status = SDF_CONTAINER_UNKNOWN;
            plat_log_msg(21552, LOG_CAT,LOG_ERR, "Delete pending for %s", path);
        } 
        releaseLocalContainerParent(&lparent);
    }

    if (status == SDF_SUCCESS) {

        // Ok to open
        container = openParentContainer(pai, path);

        if (isContainerNull(container)) {
	    fprintf(stderr, "SDFOpenContainer: failed to open parent container for %s\n", path);
	}

	CtnrMap[i_ctnr].sdf_container = container;

        if (!isContainerNull(container)) {
            lc = getLocalContainer(&lc, container);
            lc->mode = mode; // (container)->mode = mode;
            _sdf_print_container_descriptor(container);
            log_level = LOG_INFO;
#if 0
            if (cmc_settings != NULL) {
                 struct settings *settings = cmc_settings;
                 int  i;

                 for (i = 0;
                      i < sizeof(settings->vips) / sizeof(settings->vips[0]);
                      i++)
                 {
                     if (lc->container_id == settings->vips[i].container_id) {
                         settings->vips[i].cguid = lc->cguid;

                         plat_log_msg(21553, LOG_CAT, LOG_DBG, "set cid %d (%d) to cguid %d\n",
                         (int) settings->vips[i].container_id,
                         (int) i,
                         (int) lc->cguid);
                     }
                 }
            }
#endif
            // FIXME: This is where the call to shardOpen goes.
            #define MAX_SHARDIDS 32 // Not sure what max is today
            SDF_shardid_t shardids[MAX_SHARDIDS];
            uint32_t shard_count;
            get_container_shards(pai, lc->cguid, shardids, MAX_SHARDIDS, &shard_count);
            for (int i = 0; i < shard_count; i++) {
                struct SDF_shared_state *state = &sdf_shared_state;
                shardOpen(state->config.flash_dev, shardids[i]);
            }

            status = SDFActionOpenContainer(pai, lc->cguid);
            if (status != SDF_SUCCESS) {
                plat_log_msg(21554, LOG_CAT,LOG_ERR, "SDFActionOpenContainer failed for container %s", path);
            }

#ifdef SDFAPIONLY
            if (CMC_CGUID != cguid) {
                shard = container_to_shard(pai, lc);
                if (NULL != shard) {
                    mcd_shard = (mcd_osd_shard_t *)shard;
                    mcd_shard->cntr = &Mcd_containers[i_ctnr];
                    if( 1 == mcd_shard->persistent ) {
                        shard_recover_phase2( mcd_shard );
                    }
                } else {
                    plat_log_msg(150026, LOG_CAT,LOG_ERR, "Failed to find shard for %s", path);
                }
            }
#endif /* SDFAPIONLY */

            releaseLocalContainer(&lc);
            plat_log_msg(21555, LOG_CAT, LOG_DBG, "Opened %s", path);
        } else {
            status = SDF_CONTAINER_UNKNOWN;
            plat_log_msg(21556, LOG_CAT,LOG_ERR, "Failed to find %s", path);
        }
    }

    if (path) {
	plat_log_msg(21511, LOG_CAT, log_level, "%s - %s", path, SDF_Status_Strings[status]);
    } else {
	plat_log_msg(150024, LOG_CAT, log_level, "NULL - %s", SDF_Status_Strings[status]);
    }

    SDFEndSerializeContainerOp(pai);

    return (status);
}

extern int Mcd_osd_max_nclasses;

static void get_fth_stats(SDF_internal_ctxt_t *pai, char ** ppos, int * lenp,
                                 SDF_CONTAINER sdf_container)
{
    uint64_t            old_value;
    static uint64_t     idle_time = 0;
    static uint64_t     dispatch_time = 0;
    static uint64_t     low_prio_dispatch_time = 0;
    static uint64_t     num_dispatches = 0;
    static uint64_t     num_low_prio_dispatches = 0;
    static uint64_t     avg_dispatch = 0;
    static uint64_t     thread_time = 0;
    static uint64_t     ticks = 0;

    extern uint64_t Mcd_num_pending_ios;
    plat_snprintfcat( ppos, lenp, "STAT pending_ios %lu\r\n",
                      Mcd_num_pending_ios );

    extern uint64_t Mcd_fth_waiting_io;
    plat_snprintfcat( ppos, lenp, "STAT fth_waiting_io %lu\r\n",
                      Mcd_fth_waiting_io );

    plat_snprintfcat( ppos, lenp, "STAT fth_reverses %lu\r\n",
                      (unsigned long) fthReverses );

    plat_snprintfcat( ppos, lenp, "STAT fth_float_stats" );
    for ( int i = 0; i <= fthFloatMax; i++ ) {
        plat_snprintfcat( ppos, lenp, " %lu",
                          (unsigned long) fthFloatStats[i] );
    }
    plat_snprintfcat( ppos, lenp, "\r\n");

    old_value = idle_time;
    SDFContainerStat( pai, sdf_container,
                             FLASH_FTH_SCHEDULER_IDLE_TIME,
                             &idle_time );
    plat_snprintfcat( ppos, lenp, "STAT fth_idle_time %lu %lu\r\n",
                      (unsigned long)idle_time,
                      (unsigned long)(idle_time - old_value) );

    old_value = num_dispatches;
    SDFContainerStat( pai, sdf_container,
                             FLASH_FTH_NUM_DISPATCHES,
                             &num_dispatches );
    plat_snprintfcat( ppos, lenp,
                      "STAT fth_num_dispatches %lu %lu\r\n",
                      num_dispatches, num_dispatches - old_value );

    old_value = dispatch_time;
    SDFContainerStat( pai, sdf_container,
                             FLASH_FTH_SCHEDULER_DISPATCH_TIME,
                             &dispatch_time );
    plat_snprintfcat( ppos, lenp, "STAT fth_dispatch_time %lu %lu\r\n",
                      dispatch_time, dispatch_time - old_value );

    old_value = num_low_prio_dispatches;
    SDFContainerStat( pai, sdf_container,
                             FLASH_FTH_NUM_LOW_PRIO_DISPATCHES,
                             &num_low_prio_dispatches );
    plat_snprintfcat( ppos, lenp,
                      "STAT fth_num_low_prio_dispatches %lu %lu\r\n",
                      num_low_prio_dispatches,
                      num_low_prio_dispatches - old_value );

    old_value = low_prio_dispatch_time;
    SDFContainerStat( pai, sdf_container,
                             FLASH_FTH_SCHEDULER_LOW_PRIO_DISPATCH_TIME,
                             &low_prio_dispatch_time );
    plat_snprintfcat( ppos, lenp,
                      "STAT fth_low_prio_dispatch_time %lu %lu\r\n",
                      low_prio_dispatch_time,
                      low_prio_dispatch_time - old_value );

    old_value = thread_time;
    SDFContainerStat( pai, sdf_container,
                             FLASH_FTH_TOTAL_THREAD_RUN_TIME,
                             &thread_time );
    plat_snprintfcat( ppos, lenp,
                      "STAT fth_thread_run_time %lu %lu\r\n",
                      thread_time, thread_time - old_value );

    old_value = ticks;
    SDFContainerStat( pai, sdf_container,
                             FLASH_TSC_TICKS_PER_MICROSECOND, &ticks);
    plat_snprintfcat( ppos, lenp,
                      "STAT fth_tsc_ticks_per_usec %lu %lu\r\n",
                      ticks, old_value );

    SDFContainerStat( pai, sdf_container,
                             FLASH_FTH_AVG_DISPATCH_NANOSEC,
                             &avg_dispatch );
    plat_snprintfcat( ppos, lenp,
                      "STAT fth_avg_dispatch_nanosec %lu\r\n",
                      avg_dispatch );
}

static void get_flash_stats( SDF_internal_ctxt_t *pai, char ** ppos, int * lenp,
                                 SDF_CONTAINER sdf_container)
{
    uint64_t            space_allocated = 0;
    uint64_t            space_consumed = 0;
    uint64_t            num_objects = 0;
    uint64_t            num_created_objects = 0;
    uint64_t            num_evictions = 0;
    uint64_t            num_hash_evictions = 0;
    uint64_t            num_inval_evictions = 0;
    uint64_t            num_hash_overflows = 0;
    uint64_t            get_hash_collisions = 0;
    uint64_t            set_hash_collisions = 0;
    uint64_t            num_overwrites = 0;
    uint64_t            num_ops = 0;
    uint64_t            num_read_ops = 0;
    uint64_t            num_get_ops = 0;
    uint64_t            num_put_ops = 0;
    uint64_t            num_del_ops = 0;
    uint64_t            num_ext_checks = 0;
    uint64_t            num_full_buckets = 0;
    uint64_t          * stats_ptr;

    SDFContainerStat( pai, sdf_container,
                             FLASH_SLAB_CLASS_SEGS,
                             (uint64_t *)&stats_ptr );
    plat_snprintfcat( ppos, lenp, "STAT flash_class_map" );
    for ( int i = 0; i < Mcd_osd_max_nclasses; i++ ) {
        plat_snprintfcat( ppos, lenp, " %lu", stats_ptr[i] );
    }
    plat_snprintfcat( ppos, lenp, "\r\n" );

    SDFContainerStat( pai, sdf_container,
                             FLASH_SLAB_CLASS_SLABS,
                             (uint64_t *)&stats_ptr );
    plat_snprintfcat( ppos, lenp, "STAT flash_slab_map" );
    for ( int i = 0; i < Mcd_osd_max_nclasses; i++ ) {
        plat_snprintfcat( ppos, lenp, " %lu", stats_ptr[i] );
    }
    plat_snprintfcat( ppos, lenp, "\r\n" );

    SDFContainerStat( pai, sdf_container,
                             FLASH_SPACE_ALLOCATED,
                             &space_allocated );
    plat_snprintfcat( ppos, lenp,
                      "STAT flash_space_allocated %lu\r\n",
                      space_allocated );

    SDFContainerStat( pai, sdf_container,
                             FLASH_SPACE_CONSUMED,
                             &space_consumed );
    plat_snprintfcat( ppos, lenp, "STAT flash_space_consumed %lu\r\n",
                      space_consumed );

    SDFContainerStat( pai, sdf_container,
                             FLASH_NUM_OBJECTS,
                             &num_objects );
    plat_snprintfcat( ppos, lenp, "STAT flash_num_objects %lu\r\n",
                      num_objects );

    SDFContainerStat( pai, sdf_container,
                             FLASH_NUM_CREATED_OBJECTS,
                             &num_created_objects );
    plat_snprintfcat( ppos, lenp,
                      "STAT flash_num_created_objects %lu\r\n",
                      num_created_objects );

    SDFContainerStat( pai, sdf_container,
                             FLASH_NUM_EVICTIONS,
                             &num_evictions );
    plat_snprintfcat( ppos, lenp, "STAT flash_num_evictions %lu\r\n",
                      num_evictions );

    SDFContainerStat( pai, sdf_container,
                                 FLASH_NUM_HASH_EVICTIONS,
                                 &num_hash_evictions );
    plat_snprintfcat( ppos, lenp,
                          "STAT flash_num_hash_evictions %lu\r\n",
                          num_hash_evictions );

    SDFContainerStat( pai, sdf_container,
                                 FLASH_NUM_INVAL_EVICTIONS,
                                 &num_inval_evictions );
    plat_snprintfcat( ppos, lenp,
                          "STAT flash_num_inval_evictions %lu\r\n",
                          num_inval_evictions );

    SDFContainerStat( pai, sdf_container,
                                 FLASH_NUM_SOFT_OVERFLOWS,
                                 &num_hash_overflows );
    plat_snprintfcat( ppos, lenp,
                          "STAT flash_num_soft_overflows %lu\r\n",
                          num_hash_overflows );

    SDFContainerStat( pai, sdf_container,
                                 FLASH_NUM_HARD_OVERFLOWS,
                                 &num_hash_overflows );
    plat_snprintfcat( ppos, lenp,
                          "STAT flash_num_hard_overflows %lu\r\n",
                          num_hash_overflows );

    SDFContainerStat( pai, sdf_container,
                             FLASH_GET_HASH_COLLISIONS,
                                 &get_hash_collisions );
    plat_snprintfcat( ppos, lenp,
                          "STAT flash_get_hash_collisions %lu\r\n",
                          get_hash_collisions );

    SDFContainerStat( pai, sdf_container,
                                 FLASH_SET_HASH_COLLISIONS,
                                 &set_hash_collisions );
    plat_snprintfcat( ppos, lenp,
                          "STAT flash_set_hash_collisions %lu\r\n",
                          set_hash_collisions );

    SDFContainerStat( pai, sdf_container,
                             FLASH_NUM_OVERWRITES,
                             &num_overwrites );
    plat_snprintfcat( ppos, lenp, "STAT flash_num_overwrites %lu\r\n",
                      num_overwrites );

    SDFContainerStat( pai, sdf_container,
                             FLASH_OPS,
                             &num_ops );
    plat_snprintfcat( ppos, lenp, "STAT flash_num_ops %lu\r\n",
                      num_ops );

    SDFContainerStat( pai, sdf_container,
                             FLASH_READ_OPS,
                             &num_read_ops );
    plat_snprintfcat( ppos, lenp, "STAT flash_num_read_ops %lu\r\n",
                      num_read_ops );

    SDFContainerStat( pai, sdf_container,
                             FLASH_NUM_GET_OPS,
                             &num_get_ops );
    plat_snprintfcat( ppos, lenp, "STAT flash_num_get_ops %lu\r\n",
                      num_get_ops );

    SDFContainerStat( pai, sdf_container,
                             FLASH_NUM_PUT_OPS,
                             &num_put_ops );
    plat_snprintfcat( ppos, lenp, "STAT flash_num_put_ops %lu\r\n",
                      num_put_ops );

    SDFContainerStat( pai, sdf_container,
                             FLASH_NUM_DELETE_OPS,
                             &num_del_ops );
    plat_snprintfcat( ppos, lenp, "STAT flash_num_del_ops %lu\r\n",
                      num_del_ops );

    SDFContainerStat( pai, sdf_container,
                                 FLASH_NUM_EXIST_CHECKS,
                                 &num_ext_checks );
    plat_snprintfcat( ppos, lenp,
                          "STAT flash_get_exist_checks %lu\r\n",
                          num_ext_checks );

    SDFContainerStat( pai, sdf_container,
                                 FLASH_NUM_FULL_BUCKETS,
                                 &num_full_buckets );
    plat_snprintfcat( ppos, lenp,
                          "STAT flash_num_full_buckets %lu\r\n",
                          num_full_buckets );

    //mcd_osd_recovery_stats( mcd_osd_container_shard(c->mcd_container), ppos, lenp );

    //mcd_osd_auto_del_stats( mcd_osd_container_shard(c->mcd_container), ppos, lenp );

    //mcd_osd_eviction_age_stats( mcd_osd_container_shard(c->mcd_container), ppos, lenp );
}


static uint64_t parse_count( char * buf, char * name )
{
    char      * s;
    uint64_t    x;

    if ( NULL == ( s = strstr( buf, name ) ) ) {
        // this is ok--stats don't show up if they are zero
        return 0;
    }

    s += strlen( name );
    s++; /* skip '=' */

    if ( 1 != sscanf(s, "%"PRIu64"", &x) ) {
        plat_log_msg( 20648,
                      PLAT_LOG_CAT_SDF_APP_MEMCACHED,
                      PLAT_LOG_LEVEL_ERROR,
                      "sdf stat %s parsing failure", name );
        return 0;
    }

    return x;
}
SDF_status_t
sdf_parse_stats( char * stat_buf, int stat_key, uint64_t * pstat )
{
    int         i;
    uint64_t    stats[MCD_NUM_SDF_STATS];
    uint64_t    counts[MCD_NUM_SDF_COUNTS];

    if ( MCD_NUM_SDF_STATS <= stat_key ) {
#ifdef NOT_NEEDED
        plat_log_msg( 20649,
                      PLAT_LOG_CAT_SDF_APP_MEMCACHED,
                      PLAT_LOG_LEVEL_ERROR,
                      "invalid stat key %d", stat_key );
#endif
        return SDF_FAILURE;
    }

    for ( i = 0; i < MCD_NUM_SDF_COUNTS; i++ ) {
        counts[i] = parse_count( stat_buf, SDFCacheCountStrings[i] );
    }

    if ( ( counts[apgrd] + counts[apgrx] ) >= counts[ahgtr] ) {
        stats[SDF_DRAM_CACHE_HITS] =
            counts[apgrd] + counts[apgrx] - counts[ahgtr];
    }
    else {
        plat_log_msg( 20650,
                      PLAT_LOG_CAT_SDF_APP_MEMCACHED,
                      PLAT_LOG_LEVEL_ERROR,
                      "invalid sdf cache stats, rd=%lu rx=%lu tr=%lu",
                      counts[apgrd], counts[apgrx], counts[ahgtr] );
        stats[SDF_DRAM_CACHE_HITS] = 0;
    }
    stats[SDF_DRAM_CACHE_MISSES]   = counts[ahgtr];
    stats[SDF_FLASH_CACHE_HITS]    = counts[hagrc];
    stats[SDF_FLASH_CACHE_MISSES]  = counts[hagrf];
    stats[SDF_DRAM_CACHE_CASTOUTS] = counts[ahcwd];
    stats[SDF_DRAM_N_OVERWRITES]   = counts[owrites_s] + counts[owrites_m];
    stats[SDF_DRAM_N_IN_PLACE_OVERWRITES] = counts[ipowr_s] + counts[ipowr_m];
    stats[SDF_DRAM_N_NEW_ENTRY]    = counts[newents];

    *pstat = stats[stat_key];

    return SDF_SUCCESS;
}


static void get_sdf_stats( SDF_internal_ctxt_t *pai, char ** ppos, int * lenp,
                               SDF_CONTAINER sdf_container)
{
    char                buf[BUF_LEN];
    uint64_t            n;
    uint64_t            sdf_cache_hits = 0;
    uint64_t            sdf_cache_misses = 0;
    uint64_t            sdf_flash_hits = 0;
    uint64_t            sdf_flash_misses = 0;
    uint64_t            sdf_cache_evictions = 0;
    uint64_t            sdf_n_overwrites = 0;
    uint64_t            sdf_n_in_place_overwrites = 0;
    uint64_t            sdf_n_new_entry = 0;
    //SDF_CONTAINER       container;
    local_SDF_CONTAINER lc;

    /*
     * get stats for this specific container (cguid)
     */
    //container = internal_clientToServerContainer( sdf_container );
    //lc = getLocalContainer( &lc, container );
    lc = getLocalContainer( &lc, sdf_container );
	plat_assert( NULL != lc );

    memset( buf, 0, BUF_LEN);
    action_stats_new_cguid(pai, buf, BUF_LEN, lc->cguid );

    sdf_parse_stats( buf, SDF_DRAM_CACHE_HITS, &sdf_cache_hits );
    sdf_parse_stats( buf, SDF_DRAM_CACHE_MISSES, &sdf_cache_misses );
    sdf_parse_stats( buf, SDF_FLASH_CACHE_HITS, &sdf_flash_hits );
    sdf_parse_stats( buf, SDF_FLASH_CACHE_MISSES, &sdf_flash_misses );
    sdf_parse_stats( buf, SDF_DRAM_CACHE_CASTOUTS, &sdf_cache_evictions );
    sdf_parse_stats( buf, SDF_DRAM_N_OVERWRITES, &sdf_n_overwrites);
    sdf_parse_stats( buf, SDF_DRAM_N_IN_PLACE_OVERWRITES, &sdf_n_in_place_overwrites);
    sdf_parse_stats( buf, SDF_DRAM_N_NEW_ENTRY, &sdf_n_new_entry);

    plat_snprintfcat( ppos, lenp, "STAT sdf_cache_hits %lu\r\n",
                      sdf_cache_hits );

    plat_snprintfcat( ppos, lenp, "STAT sdf_cache_misses %lu\r\n",
                      sdf_cache_misses );

    plat_snprintfcat( ppos, lenp, "STAT sdf_flash_hits %lu\r\n",
                      sdf_flash_hits );

    plat_snprintfcat( ppos, lenp, "STAT sdf_flash_misses %lu\r\n",
                      sdf_flash_misses );

    plat_snprintfcat( ppos, lenp, "STAT sdf_cache_evictions %lu\r\n",
                      sdf_cache_evictions );

    plat_snprintfcat( ppos, lenp, "STAT sdf_n_overwrites %lu\r\n",
                      sdf_n_overwrites);

    plat_snprintfcat( ppos, lenp, "STAT sdf_n_in_place_overwrites %lu\r\n",
                      sdf_n_in_place_overwrites);

    plat_snprintfcat( ppos, lenp, "STAT sdf_n_new_entries %lu\r\n",
                      sdf_n_new_entry);

    SDFContainerStat( pai, sdf_container,
                             SDF_N_ONLY_IN_CACHE, &n);
    plat_snprintfcat( ppos, lenp, "STAT sdf_cache_only_items %lu\r\n", n);

    plat_snprintfcat( ppos, lenp, "STAT %s", buf );

	/*
     * get stats for entire cache (for all cguid's)
     */
    memset( buf, 0, BUF_LEN );
    action_stats(pai, buf, BUF_LEN );
    plat_snprintfcat( ppos, lenp, "STAT %s\r\n", buf );

    // memset( buf, 0, 1024 );
    // home_stats( buf, 1024 );
    // plat_snprintfcat( ppos, lenp, "STAT %s\r\n", buf );
}
static void get_proc_stats( char ** ppos, int * lenp )
{
    int             rc;
    int             proc_fd = 0;
    char            proc_buf[128];
    char          * ptr = NULL;

    proc_fd = open( "/proc/stat", O_RDONLY );
    if ( 0 > proc_fd ) {
        mcd_log_msg ( PLAT_LOG_ID_INITIAL, PLAT_LOG_LEVEL_ERROR,
                      "cannot open /proc/stat" );
        return;
    }
    mcd_log_msg( 20694, PLAT_LOG_LEVEL_DEBUG,
                 "/proc/stat opened, fd=%d", proc_fd );

    memset( proc_buf, 0, 128 );
    if ( 0 < ( rc = read( proc_fd, proc_buf, 127 ) ) ) {
        if ( NULL != ( ptr = strchr( proc_buf, '\n' ) ) ) {
            *ptr = '\0';
            plat_snprintfcat( ppos, lenp, "STAT %s\r\n", proc_buf );
        }
    }
    else {
        mcd_log_msg( 20695, PLAT_LOG_LEVEL_ERROR,
                     "failed to read from proc, rc=%d errno=%d", rc, errno );
    }

    close( proc_fd );
}

SDF_status_t SDFGetStatsStr (
    struct SDF_thread_state *sdf_thread_state,
    SDF_cguid_t cguid, char *stats_str ) 
{
    SDF_status_t status = SDF_FAILURE;
    SDF_CONTAINER sdf_container = containerNull;
    SDF_internal_ctxt_t *pai = (SDF_internal_ctxt_t *) sdf_thread_state;
    //SDF_status_t lock_status = SDF_SUCCESS;
    //SDF_cguid_t parent_cguid = SDF_NULL_CGUID;
    //int log_level = LOG_ERR;
    int  i_ctnr;
    uint64_t space_used = 0;
    uint64_t maxbytes = 0;
    uint64_t num_evictions = 0;
    uint64_t      n = 0;
    //static struct timeval reset_time = {0, 0} ;
    int buf_len;
    char *temp;
    char * pos;
    SDF_container_props_t       dummy_prop;
    memset( (void *)&dummy_prop, 0, sizeof(dummy_prop) );

    //SDFStartSerializeContainerOp(pai);
    //fprintf(stderr,"Container CGID:%u\n",cguid);
    i_ctnr = get_ctnr_from_cguid(cguid);
    if (i_ctnr == -1) {
        status = SDF_INVALID_PARAMETER;
        goto out;
    }
    else {
        sdf_container = CtnrMap[i_ctnr].sdf_container;
    }

    buf_len = STAT_BUFFER_SIZE;
    temp = stats_str;
    memset( temp, 0, buf_len );
    pos = temp;
    buf_len -= strlen( "\r\nEND\r\n" ) + 1;

    SDFContainerStat( pai, sdf_container, SDF_N_CURR_ITEMS, &n );
    plat_snprintfcat( &pos, &buf_len, "STAT curr_items %lu\r\n", n );
    SDFContainerStat( pai, sdf_container,
                             FLASH_SPACE_USED, &space_used );
    plat_snprintfcat( &pos, &buf_len, "STAT bytes %lu\r\n", space_used );
    SDFContainerStat( pai, sdf_container,
                                         FLASH_NUM_EVICTIONS, &num_evictions );
    plat_snprintfcat( &pos, &buf_len, "STAT evictions %lu\r\n",
                              num_evictions );
    SDFContainerStat( pai, sdf_container,
                                         FLASH_SHARD_MAXBYTES, &maxbytes );
    plat_snprintfcat( &pos, &buf_len, "STAT limit_maxbytes %lu\r\n",
                              maxbytes );
    get_flash_stats( pai, &pos, &buf_len, sdf_container );
    get_fth_stats( pai, &pos, &buf_len, sdf_container );
    get_sdf_stats(pai, &pos, &buf_len, sdf_container );
    get_proc_stats(&pos, &buf_len);
out:
    //plat_log_msg(20819, LOG_CAT, log_level, "%s", SDF_Status_Strings[status]);
    //SDFEndSerializeContainerOp(pai);
    return (status);
}

SDF_status_t SDFCloseContainer(
        struct SDF_thread_state      *sdf_thread_state,
	SDF_cguid_t  		      cguid
	)
{
#ifdef SDFAPIONLY
    //mcd_container_t 		cntr;
    struct shard		*shard		= NULL;
    flashDev_t              *flash_dev;
    SDF_container_meta_t     meta;
    SDF_status_t tmp_status;
    struct SDF_shared_state *state = &sdf_shared_state;
#endif
    SDF_status_t status = SDF_FAILURE;
    unsigned     n_descriptors;
    int  i_ctnr;
    SDF_CONTAINER container = containerNull;
    SDF_internal_ctxt_t     *pai = (SDF_internal_ctxt_t *) sdf_thread_state;
    int log_level = LOG_ERR;
    int ok_to_delete = 0;
    SDF_cguid_t parent_cguid = SDF_NULL_CGUID;

    plat_log_msg(21630, LOG_CAT, LOG_INFO, "%lu", cguid);

    SDFStartSerializeContainerOp(pai);

    i_ctnr = get_ctnr_from_cguid(cguid);

    if (i_ctnr == -1) {
        status = SDF_INVALID_PARAMETER;
	goto out;
    } else {
	container = CtnrMap[i_ctnr].sdf_container;
    }

    if (isContainerNull(container)) {
        status = SDF_FAILURE_CONTAINER_GENERIC;
    } else {

        // Delete the container if there are no outstanding opens and a delete is pending
        local_SDF_CONTAINER lcontainer = getLocalContainer(&lcontainer, container);
        SDF_CONTAINER_PARENT parent = lcontainer->parent;
        local_SDF_CONTAINER_PARENT lparent = getLocalContainerParent(&lparent, parent);
        char path[MAX_OBJECT_ID_SIZE] = "";

        if (lparent->name) {
            memcpy(&path, lparent->name, strlen(lparent->name));
        }

        if (lparent->num_open_descriptors == 1 && lparent->delete_pending == SDF_TRUE) {
            ok_to_delete = 1;
        }
        // copy these from lparent before I nuke it!
        n_descriptors = lparent->num_open_descriptors;
        parent_cguid  = lparent->cguid;
        releaseLocalContainerParent(&lparent);

        if (closeParentContainer(container)) {
            status = SDF_SUCCESS;
            log_level = LOG_INFO;
        }

#ifdef SDFAPIONLY
    	if ((status = name_service_get_meta(pai, cguid, &meta)) == SDF_SUCCESS) {

			#ifdef MULTIPLE_FLASH_DEV_ENABLED
				flash_dev = get_flashdev_from_shardid(state->config.flash_dev,
											  meta.shard, state->config.flash_dev_count);
			#else
				flash_dev = state->config.flash_dev;
			#endif
			shard = shardFind(flash_dev, meta.shard);

			if(shard)
			{
				((mcd_osd_shard_t*)shard)->open = 0;

				shardSync(shard);
			}
		}

	    // Invalidate all of the container's cached objects
	    if ((status = name_service_inval_object_container(pai, path)) != SDF_SUCCESS) {
		plat_log_msg(21540, LOG_CAT, LOG_ERR,
			     "%s - failed to flush and invalidate container", path);
		log_level = LOG_ERR;
	    } else {
		plat_log_msg(21541, LOG_CAT, LOG_DBG,
			     "%s - flush and invalidate container succeed", path);
	    }

		if ((tmp_status = name_service_get_meta_from_cname(pai, path, &meta)) == SDF_SUCCESS) {
		    tmp_status = SDFActionDeleteContainer(pai, &meta);
		    if (tmp_status != SDF_SUCCESS) {
			// xxxzzz container will be left in a weird state!
			plat_log_msg(21542, LOG_CAT, LOG_ERR,
				"%s - failed to delete action thread container state", path);
			log_level = LOG_ERR;
		    } else {
			plat_log_msg(21543, LOG_CAT, LOG_DBG,
				"%s - action thread delete container state succeeded", path);
		    }
		}
#endif

#if 0 // not used
        // FIXME: This is where shardClose call goes.
        if (n_descriptors == 1) {
            #define MAX_SHARDIDS 32 // Not sure what max is today
            SDF_shardid_t shardids[MAX_SHARDIDS];
            uint32_t shard_count;
            get_container_shards(pai, parent_cguid, shardids, MAX_SHARDIDS, &shard_count);
            for (int i = 0; i < shard_count; i++) {
                //shardClose(shardids[i]);
            }
        }
#endif

        if (status == SDF_SUCCESS && ok_to_delete) {

		    plat_log_msg(160031, LOG_CAT, LOG_INFO, "Delete request pending. Deleting... cguid=%lu", cguid);

	    	status = delete_container_internal_low(pai, path, SDF_FALSE /* serialize */, NULL);

    		CtnrMap[i_ctnr].cguid         = 0;
        }
    }

    CtnrMap[i_ctnr].sdf_container = containerNull;

out:

    plat_log_msg(20819, LOG_CAT, log_level, "%s", SDF_Status_Strings[status]);

    SDFEndSerializeContainerOp(pai);

    return (status);
}

SDF_status_t SDFDeleteContainer(
	struct SDF_thread_state	*sdf_thread_state,
	SDF_cguid_t		 cguid
	)
{
    SDF_internal_ctxt_t *pai = (SDF_internal_ctxt_t *) sdf_thread_state;
    SDF_status_t status = SDF_INVALID_PARAMETER;
    int  i_ctnr, ok_to_delete = 0;

    plat_log_msg(21630, LOG_CAT, LOG_INFO, "%lu", cguid);

    i_ctnr = get_ctnr_from_cguid(cguid);

    if (i_ctnr >=0 && !ISEMPTY(CtnrMap[i_ctnr].cname)) {
	    status = delete_container_internal_low(pai, CtnrMap[i_ctnr].cname, SDF_TRUE /* serialize */, &ok_to_delete);

		plat_free(CtnrMap[i_ctnr].cname);
		CtnrMap[i_ctnr].cname = NULL;

		if(ok_to_delete)
		{
    		CtnrMap[i_ctnr].cguid         = 0;
		    CtnrMap[i_ctnr].sdf_container = containerNull;
		}
		else
		{
//			status = SDF_FAILURE;
	    	plat_log_msg(160030, LOG_CAT, LOG_INFO, "Container is not deleted (busy or error): cguid=%lu(%d), status=%s", cguid, i_ctnr, SDF_Status_Strings[status]);
		}
    }

    plat_log_msg(20819, LOG_CAT, LOG_INFO, "%s", SDF_Status_Strings[status]);

    return status;
}

SDF_status_t SDFStartContainer(
	struct SDF_thread_state *sdf_thread_state, 
	SDF_cguid_t cguid
	)
{
    SDF_status_t             status = SDF_SUCCESS;
    SDF_container_meta_t     meta;
    struct shard            *shard = NULL;
    struct SDF_shared_state *state = &sdf_shared_state;
    flashDev_t              *flash_dev;
    SDF_internal_ctxt_t     *pai = (SDF_internal_ctxt_t *) sdf_thread_state;

    plat_log_msg(21630, LOG_CAT, LOG_INFO, "%lu", cguid);

    SDFStartSerializeContainerOp(pai);

    if ((status = name_service_get_meta(pai, cguid, &meta)) == SDF_SUCCESS) {

        meta.stopflag = SDF_FALSE;
        if ((status = name_service_put_meta(pai, cguid, &meta)) == SDF_SUCCESS) {

            #ifdef MULTIPLE_FLASH_DEV_ENABLED
                flash_dev = get_flashdev_from_shardid(state->config.flash_dev,
                                                      meta.shard, state->config.flash_dev_count);
            #else
                flash_dev = state->config.flash_dev;
            #endif

            shard = shardFind(flash_dev, meta.shard);

			if(shard)
			{
	            shardStart(shard);

	            /* Clean up additional action node state for the container.
	             */
	            status = SDFActionStartContainer(pai, &meta);
			}
			else
        	{
            	status = SDF_CONTAINER_UNKNOWN;
	            plat_log_msg(160029, LOG_CAT,LOG_ERR, "Failed to find shard for cguid %"PRIu64"", cguid);
	        }
        } else {
            plat_log_msg(21539, LOG_CAT, LOG_ERR,
                         "name_service_put_meta failed for cguid %"PRIu64"", cguid);
        }
    }

    SDFEndSerializeContainerOp(pai);

    plat_log_msg(20819, LOG_CAT, LOG_INFO, "%s", SDF_Status_Strings[status]);

    return(status);
}

SDF_status_t SDFStopContainer(
	struct SDF_thread_state *sdf_thread_state, 
	SDF_cguid_t cguid
	)
{
    SDF_status_t             status = SDF_SUCCESS;
    SDF_container_meta_t     meta;
    struct shard            *shard = NULL;
    struct SDF_shared_state *state = &sdf_shared_state;
    flashDev_t              *flash_dev;
    SDF_internal_ctxt_t     *pai = (SDF_internal_ctxt_t *) sdf_thread_state;

    plat_log_msg(21630, LOG_CAT, LOG_INFO, "%lu", cguid);

    SDFStartSerializeContainerOp(pai);

    if ((status = name_service_get_meta(pai, cguid, &meta)) == SDF_SUCCESS) {

        meta.stopflag = SDF_TRUE;

        if ((status = name_service_put_meta(pai, cguid, &meta)) == SDF_SUCCESS) {

            #ifdef MULTIPLE_FLASH_DEV_ENABLED
                flash_dev = get_flashdev_from_shardid(state->config.flash_dev,
                                                      meta.shard, state->config.flash_dev_count);
            #else
                flash_dev = state->config.flash_dev;
            #endif
            shard = shardFind(flash_dev, meta.shard);

			if(shard)
			{
		    	((mcd_osd_shard_t*)shard)->cntr->state = cntr_stopping;

	            shardStop(shard);

	            /* Clean up additional action node state for the container.
	             */
	            status = SDFActionStopContainer(pai, &meta);

		    	((mcd_osd_shard_t*)shard)->cntr->state = cntr_stopped;
			}
			else
        	{
            	status = SDF_CONTAINER_UNKNOWN;
	            plat_log_msg(160029, LOG_CAT,LOG_ERR, "Failed to find shard for cguid %"PRIu64"", cguid);
	        }
        } else {
            plat_log_msg(21539, LOG_CAT, LOG_ERR,
                         "name_service_put_meta failed for cguid %"PRIu64"", cguid);
        }
    }

    SDFEndSerializeContainerOp(pai);

    plat_log_msg(20819, LOG_CAT, LOG_INFO, "%s", SDF_Status_Strings[status]);

    return(status);
}

SDF_status_t SDFFlushContainer(
	struct SDF_thread_state  *sdf_thread_state,
	SDF_cguid_t               cguid,
	SDF_time_t                current_time
	)
{
    SDF_appreq_t        ar;
    SDF_action_init_t  *pac;

    pac = (SDF_action_init_t *) sdf_thread_state;

    ar.reqtype = APFCO;
    ar.curtime = current_time;
    ar.ctxt = pac->ctxt;
    ar.ctnr = cguid;
    ar.ctnr_type = SDF_OBJECT_CONTAINER;
    ar.internal_request = SDF_TRUE;
    ar.internal_thread = fthSelf();

    ActionProtocolAgentNew(pac, &ar);

    return(ar.respStatus);
}

SDF_status_t SDFGetForReadBufferedObject(
	struct SDF_thread_state   *sdf_thread_state,
	SDF_cguid_t                cguid,
	char                      *key,
	uint32_t                   keylen,
	char                     **data,
	uint64_t                  *datalen,
	SDF_time_t                 current_time,
	SDF_time_t                *expiry_time
	)
{
    SDF_appreq_t        ar;
    SDF_action_init_t  *pac;
    SDF_status_t        status;

    pac = (SDF_action_init_t *) sdf_thread_state;
   
    ar.reqtype = APGRX;
    ar.curtime = current_time;
    ar.ctxt = pac->ctxt;
    ar.ctnr = cguid;
    ar.ctnr_type = SDF_OBJECT_CONTAINER;
    ar.internal_request = SDF_TRUE;
    ar.internal_thread = fthSelf();
    if ((status=SDFObjnameToKey(&(ar.key), (char *) key, keylen)) != SDF_SUCCESS) {
        return(status);
    }
    if (data == NULL) {
        return(SDF_BAD_PBUF_POINTER);
    }
    ar.ppbuf_in = (void **)data;

    ActionProtocolAgentNew(pac, &ar);

    if (datalen == NULL) {
        return(SDF_BAD_SIZE_POINTER);
    }
    *datalen = ar.destLen;

    if (expiry_time) {
    	*expiry_time     = ar.exptime;
	}

    return(ar.respStatus);
}

SDF_status_t SDFGetBuffer(
                   struct SDF_thread_state  *sdf_thread_state,
		   char                     **data,
		   uint64_t                   datalen
               )
{
    char *p;
    p = (char *) plat_alloc(datalen);
    if (p) {
        *data = p;
	return(SDF_SUCCESS);
    } else {
	return(SDF_FAILURE);
    }
}

SDF_status_t SDFCreateBufferedObject(
                   struct SDF_thread_state  *sdf_thread_state,
		   SDF_cguid_t          cguid,
		   char                *key,
		   uint32_t             keylen,
		   char                *data,
		   uint64_t             datalen,
		   SDF_time_t           current_time,
		   SDF_time_t           expiry_time
	       )
{
    SDF_appreq_t        ar;
    SDF_action_init_t  *pac;
    SDF_status_t        status;

    pac = (SDF_action_init_t *) sdf_thread_state;

#if 0
    struct SDF_shared_state    *state = &sdf_shared_state;
    flashDev_t                 *flash_dev;
    struct SDF_iterator        *iterator;
    SDF_container_meta_t meta;
    if ((status = name_service_get_meta(pac, cguid, &meta)) != SDF_SUCCESS) {
        fprintf(stderr, "SDFCreateBufferedObject: failed to get meta for cguid: %lu\n", cguid);
        return SDF_FAILURE; // xxxzzz TODO: better return code?
    }

    iterator = plat_alloc(sizeof(SDF_iterator_t));
    if (iterator == NULL) {
        return(SDF_FAILURE_MEMORY_ALLOC);
    }
    iterator->cguid = cguid;

    #ifdef MULTIPLE_FLASH_DEV_ENABLED
        flash_dev = get_flashdev_from_shardid(state->config.flash_dev,
                                              meta.shard, state->config.flash_dev_count);
    #else
        flash_dev = state->config.flash_dev;
    #endif
    iterator->shard = shardFind(flash_dev, meta.shard);
#endif

    ar.reqtype = APCOE;
    ar.curtime = current_time;
    ar.ctxt = pac->ctxt;
    ar.ctnr = cguid;
    ar.ctnr_type = SDF_OBJECT_CONTAINER;
    ar.internal_request = SDF_TRUE;
    ar.internal_thread = fthSelf();
    if ((status=SDFObjnameToKey(&(ar.key), (char *) key, keylen)) != SDF_SUCCESS) {
        return(status);
    }
    ar.sze = datalen;
    ar.pbuf_out = (void *) data;
    ar.exptime = expiry_time;
    if (data == NULL) {
        return(SDF_BAD_PBUF_POINTER);
    }

    ActionProtocolAgentNew(pac, &ar);

    return(ar.respStatus);
}

SDF_status_t SDFSetBufferedObject(
	struct SDF_thread_state  *sdf_thread_state,
	SDF_cguid_t          	  cguid,
	char                	 *key,
	uint32_t             	  keylen,
	char                	 *data,
	uint64_t             	  datalen,
	SDF_time_t           	  current_time,
	SDF_time_t           	  expiry_time
	)
{
    SDF_appreq_t        ar;
    SDF_action_init_t  *pac;
    SDF_status_t        status;

    pac = (SDF_action_init_t *) sdf_thread_state;

    ar.reqtype = APSOE;
    ar.curtime = current_time;
    ar.ctxt = pac->ctxt;
    ar.ctnr = cguid;
    ar.ctnr_type = SDF_OBJECT_CONTAINER;
    ar.internal_request = SDF_TRUE;
    ar.internal_thread = fthSelf();
    if ((status=SDFObjnameToKey(&(ar.key), (char *) key, keylen)) != SDF_SUCCESS) {
        return(status);
    }
    ar.sze = datalen;
    ar.pbuf_out = (void *) data;
    ar.exptime = expiry_time;
    if (data == NULL) {
        return(SDF_BAD_PBUF_POINTER);
    }

    ActionProtocolAgentNew(pac, &ar);

    return(ar.respStatus);
}

SDF_status_t SDFPutBufferedObject(
	struct SDF_thread_state  *sdf_thread_state,
	SDF_cguid_t          	  cguid,
	char                	 *key,
	uint32_t             	  keylen,
	char                	 *data,
	uint64_t             	  datalen,
	SDF_time_t           	  current_time,
	SDF_time_t           	  expiry_time
	)
{
    SDF_appreq_t        ar;
    SDF_action_init_t  *pac;
    SDF_status_t        status;

    pac = (SDF_action_init_t *) sdf_thread_state;
   
    ar.reqtype = APPAE;
    ar.curtime = current_time;
    ar.ctxt = pac->ctxt;
    ar.ctnr = cguid;
    ar.ctnr_type = SDF_OBJECT_CONTAINER;
    ar.internal_request = SDF_TRUE;
    ar.internal_thread = fthSelf();
    if ((status=SDFObjnameToKey(&(ar.key), (char *) key, keylen)) != SDF_SUCCESS) {
        return(status);
    }
    ar.sze = datalen;
    ar.pbuf_out = (void *) data;
    ar.exptime = expiry_time;
    if (data == NULL) {
        return(SDF_BAD_PBUF_POINTER);
    }

    ActionProtocolAgentNew(pac, &ar);

    return(ar.respStatus);
}

SDF_status_t SDFRemoveObjectWithExpiry(
	struct SDF_thread_state  *sdf_thread_state,
	SDF_cguid_t          	  cguid,
	char                	 *key,
	uint32_t             	  keylen,
	SDF_time_t           	  current_time
	)
{
    SDF_appreq_t        ar;
    SDF_action_init_t  *pac;
    SDF_status_t        status;
    
    pac = (SDF_action_init_t *) sdf_thread_state;
    
    ar.reqtype = APDBE;
    //ar.prefix_delete = pref_del;
    ar.curtime = current_time;
    ar.ctxt = pac->ctxt;
    ar.ctnr = cguid;
    ar.ctnr_type = SDF_OBJECT_CONTAINER;
    ar.internal_request = SDF_TRUE;    ar.internal_thread = fthSelf();
    if ((status=SDFObjnameToKey(&(ar.key), (char *) key, keylen)) != SDF_SUCCESS) {
        return(status); 
    }
    
    ActionProtocolAgentNew(pac, &ar);
    
    return(ar.respStatus);
}

SDF_status_t SDFFlushObject(
	struct SDF_thread_state  *sdf_thread_state,
	SDF_cguid_t          	  cguid,
	char                	 *key,
	uint32_t             	  keylen,
	SDF_time_t           	  current_time
	)
{
    SDF_appreq_t        ar;
    SDF_action_init_t  *pac;
    SDF_status_t        status;

    pac = (SDF_action_init_t *) sdf_thread_state;
   
    ar.reqtype = APFLS;
    ar.curtime = current_time;
    ar.ctxt = pac->ctxt;
    ar.ctnr = cguid;
    ar.ctnr_type = SDF_OBJECT_CONTAINER;
    ar.internal_request = SDF_TRUE;
    ar.internal_thread = fthSelf();
    if ((status=SDFObjnameToKey(&(ar.key), (char *) key, keylen)) != SDF_SUCCESS) {
        return(status);
    }

    ActionProtocolAgentNew(pac, &ar);

    return(ar.respStatus);
}

SDF_cguid_t SDFGenerateCguid(
	struct SDF_thread_state *sdf_thread_state, 
	int64_t 	    cntr_id64
	)
{
    SDF_cguid_t cguid;

    /*   xxxzzz briano
     *   cguid's must be global across all nodes.
     *   Eg: a given container must have the same cguid at any node
     *   on which it has a replica.  We also ensure that all of its
     *   shard id's are the same at any node on which it has a
     *   replica.
     *   cguid = <port_number> | <container_id>
     *
     *   This MUST be kept in sync with the code in memcached that
     *   generates the path as "%s%5d%.8x" for <name>|<port>|<ctnr_id>.
     */

    /*  xxxzzz briano
     *  For now I am NOT including the port number in the cguid.
     *  This is so that we can run 2 or more instances of memcached on
     *  the same physical node without port conflicts.  It decouples
     *  the port number from the global cguid.  Consequently, a container
     *  can have different port numbers on different nodes for the same
     *  container (but this will require different property files per
     *  node).  Alas, this is necessary to support multiple memcached
     *  processes per system.
     */
    cguid = cntr_id64 + NODE_MULTIPLIER; // so they don't collide with CMC_CGUID=1

    plat_log_msg(150019, LOG_CAT, LOG_DBG,
                 "SDFGenerateCguid: %llu", (unsigned long long) cguid);

    return (cguid);
}

SDF_status_t SDFFlushCache(
	struct SDF_thread_state  *sdf_thread_state,
	SDF_time_t           	  current_time
	)
{
    return(SDF_SUCCESS);
}

static SDF_status_t backup_container_prepare( 
                                  void * shard, int full_backup,
                                  uint32_t client_version,
                                  uint32_t * server_version )
{
    int                         rc;
    SDF_status_t                status = SDF_SUCCESS;

    rc = mcd_osd_shard_backup_prepare( (struct shard *)shard, full_backup,
                                       client_version, server_version );

    switch ( rc ) {
    case FLASH_EINVAL:
        status = SDF_FLASH_EINVAL;
        break;
    case FLASH_EBUSY:
        status = SDF_FLASH_EBUSY;
        break;
    case FLASH_EPERM:
        status = SDF_FLASH_EPERM;
        break;
    case FLASH_EINCONS:
        status = SDF_FLASH_EINCONS;
        break;
    }

    return status;
}


static SDF_status_t backup_container( 
                          void * shard, int full_backup,
                          int cancel, int complete, uint32_t client_version,
                          uint32_t * server_version, uint64_t * prev_seqno,
                          uint64_t * backup_seqno, time_t * backup_time )
{
    int                         rc;
    SDF_status_t                status = SDF_SUCCESS;

    rc = mcd_osd_shard_backup( (struct shard *)shard, full_backup, cancel,
                               complete, client_version, server_version,
                               prev_seqno, backup_seqno, backup_time );

    switch ( rc ) {
    case FLASH_EINVAL:
        status = SDF_FLASH_EINVAL;
        break;
    case FLASH_EBUSY:
        status = SDF_FLASH_EBUSY;
        break;
    case FLASH_EPERM:
        status = SDF_FLASH_EPERM;
        break;
    case FLASH_EINCONS:
        status = SDF_FLASH_EINCONS;
        break;
    }

    return status;
}


SDF_status_t SDFEnumerateContainerObjects(
	struct SDF_thread_state  *sdf_thread_state,
	SDF_cguid_t               cguid,
	struct SDF_iterator     **iterator_out
	)
{
    SDF_action_init_t          *pai = (SDF_action_init_t *) sdf_thread_state;
    SDF_status_t                status;
    time_t                      backup_time;
    uint64_t                    prev_seqno;
    uint64_t                    curr_seqno;
    uint32_t                    version;
    SDF_container_meta_t        meta;
    struct SDF_shared_state    *state = &sdf_shared_state;
    flashDev_t                 *flash_dev;
    struct SDF_iterator        *iterator;

    if ((status = name_service_get_meta(pai, cguid, &meta)) != SDF_SUCCESS) {
        fprintf(stderr, "sdf_enumerate: failed to get meta for cguid: %lu\n", cguid);
	return SDF_FAILURE; // xxxzzz TODO: better return code?
    }

    *iterator_out = NULL;
    iterator = plat_alloc(sizeof(SDF_iterator_t));
    if (iterator == NULL) {
        return(SDF_FAILURE_MEMORY_ALLOC);
    }
    iterator->cguid = cguid;

    #ifdef MULTIPLE_FLASH_DEV_ENABLED
	flash_dev = get_flashdev_from_shardid(state->config.flash_dev,
					      meta.shard, state->config.flash_dev_count);
    #else
	flash_dev = state->config.flash_dev;
    #endif
    iterator->shard = shardFind(flash_dev, meta.shard);
	if(!iterator->shard)
    {
	    plat_log_msg(160029, LOG_CAT,LOG_ERR, "Failed to find shard for cguid %"PRIu64"", cguid);
		return SDF_FAILURE;
	}
#if 0
    fprintf(stderr, "SDFEnumerateContainerObjects: iterator->shard->shardID: %lu\n", iterator->shard->shardID);
    fprintf(stderr, "SDFEnumerateContainerObjects: iterator->shard->quota: %lu\n", iterator->shard->quota);
    fprintf(stderr, "SDFEnumerateContainerObjects: iterator->shard->numObjects: %lu\n", iterator->shard->numObjects);
#endif

    status = backup_container_prepare( iterator->shard,
					  1, // full
					  MCD_BAK_BACKUP_PROTOCOL_VERSION, // client version
					  &version );
    if ( SDF_SUCCESS != status ) {
	goto backup_result;
    }

    // must sync data in writeback cache mode
    if (meta.properties.cache.writethru == SDF_FALSE) {

	status = SDF_I_FlushContainer(pai, cguid);

	if (status == SDF_SUCCESS) {
	    status = SDF_I_SyncContainer(pai, cguid);
	}

	if ( SDF_SUCCESS != status ) {
	    plat_log_msg(30664,
			  PLAT_LOG_CAT_SDF_APP_MEMCACHED_BACKUP,
			  PLAT_LOG_LEVEL_ERROR,
			  "container backup sync failed for enum, status=%s",
			  SDF_Status_Strings[status]);
	    // backup has been semi-started, must be cancelled
	    status = backup_container( iterator->shard,
					  1, // full
					  1, // cancel
					  0, // complete
					  MCD_BAK_BACKUP_PROTOCOL_VERSION,
					  &version,
					  &prev_seqno,
					  &curr_seqno,
					  &backup_time );
	    plat_assert( SDF_SUCCESS == status );
	    plat_free(iterator);
	    return SDF_FAILURE; // xxxzzz TODO: better return code?
	}
    }

    // start the backup
    status = backup_container( iterator->shard,
				       1, // full
				       0, // cancel
				       0, // complete
                                       MCD_BAK_BACKUP_PROTOCOL_VERSION,
                                       &version,
                                       &prev_seqno,
                                       &curr_seqno,
                                       &backup_time );

 backup_result:

    if ( SDF_SUCCESS == status ) {
        iterator->addr     = 0;
        iterator->prev_seq = prev_seqno;
        iterator->curr_seq = curr_seqno;
		*iterator_out = iterator;
    } else {
	plat_free(iterator);
    }

    return(status);
}

SDF_status_t SDFFinishEnumeration(
                   struct SDF_thread_state *sdf_thread_state,
		   struct SDF_iterator     *iterator
	       )
{
    SDF_status_t                status;
    time_t                      backup_time;
    uint64_t                    prev_seqno;
    uint64_t                    curr_seqno;
    uint32_t                    version;

	plat_assert(iterator);

    // stop the backup
    status = backup_container( iterator->shard,
                                       1, // full
                                       0, // cancel
                                       1, // complete
                                       MCD_BAK_BACKUP_PROTOCOL_VERSION,  // client version
                                       &version,
                                       &prev_seqno,
                                       &curr_seqno,
                                       &backup_time );

	plat_free(iterator);

    return(status);
}

  /*   From mcd_osd.c:
   *   Enumerate the next object for this container.
   *   Very similar to process_raw_get_command().
   */
extern SDF_status_t process_raw_get_command_enum( 
                                           mcd_osd_shard_t  *shard,
					   osd_state_t      *osd_state,
					   uint64_t          addr, 
					   uint64_t          prev_seq,
					   uint64_t          curr_seq, 
					   int               num_sessions, 
					   int               session_id,
					   int               max_objs,
					   char            **key_out,
					   uint32_t         *keylen_out,
					   char            **data_out,
					   uint64_t         *datalen_out,
					   uint64_t         *addr_out
				       );

SDF_status_t SDFNextEnumeratedObject(
	struct SDF_thread_state *sdf_thread_state,
	struct SDF_iterator     *iterator,
	char                    **key,
	uint32_t                *keylen,
	char                    **data,
	uint64_t                *datalen
	)
{
    SDF_status_t             ret;
    SDF_action_init_t       *pai = (SDF_action_init_t *) sdf_thread_state;

	plat_assert(iterator);

    ret = process_raw_get_command_enum(
				     (mcd_osd_shard_t *) iterator->shard,
				     (osd_state_t *) pai->paio_ctxt,
				     iterator->addr,
				     iterator->prev_seq,
				     iterator->curr_seq,
				     1, // num_sessions 
				     0, // session_id
				     1, // max_objs
				     key,
				     keylen,
				     data,
				     datalen,
				     &(iterator->addr)
                                );
    return(ret);
}

SDF_status_t SDFGetStats(
	struct SDF_thread_state *sdf_thread_state,
	SDF_stats_t             *stats
	)
{
    //  no-op in this simple implementation
    return(SDF_SUCCESS);
}

SDF_status_t SDFGetContainerStats(
	struct SDF_thread_state   *sdf_thread_state,
	SDF_cguid_t                cguid,
	SDF_container_stats_t     *stats
	)
{
    //  no-op in this simple implementation
    return(SDF_SUCCESS);
}

SDF_status_t SDFBackupContainer(
	struct SDF_thread_state   *sdf_thread_state,
	SDF_cguid_t                cguid,
	char                      *backup_directory
	)
{
    //  no-op in this simple implementation
    return(SDF_SUCCESS);
}

SDF_status_t SDFRestoreContainer(
	struct SDF_thread_state   *sdf_thread_state,
	SDF_cguid_t                cguid,
	char                      *backup_directory
	)
{
    //  no-op in this simple implementation
    return(SDF_SUCCESS);
}

SDF_status_t SDFFreeBuffer(
	struct SDF_thread_state  *sdf_thread_state,
	char                     *data
	)
{
    free(data);
    return(SDF_SUCCESS);
}

    /******************************************************
     *
     *  Callback functions for simple_replication.
     *
     ******************************************************/

/*
 * For simple replication: if simple replication is enabled,
 * persistent container will start in stopped mode and requires to
 * be explicitly turned on. If only one node failed, then the
 * recovery module should call this function at the beginning of
 * recovery to wipe the old content out. If both nodes failed, then
 * users need to select a node and manually start it.
 *
 * Return codes:
 *   -EBUSY:    the container is busy and needs to be stopped first
 *   -ENOENT:   container not found
 */
int mcd_format_container_byname_internal( void * pai, char * cname )
{
    #ifdef notdef
    mcd_log_msg(50006, PLAT_LOG_LEVEL_INFO,
                 "formatting container, name=%s", cname );

    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
	if (pMcd_containers[i] == NULL) { continue; }
        if ( 0 == strncmp( cname, mcd_osd_container_cname(pMcd_containers[i]),
                           MCD_CNTR_NAME_MAXLEN ) ) {
            if ( cntr_stopped != mcd_osd_container_state(pMcd_containers[i]) ) {
                return -EBUSY;
            }
            return mcd_do_format_container( pai, pMcd_containers[i], true );
        }
    }
    #endif

    return -ENOENT;
}


/*
 * Return codes:
 *   1: container is active and running
 *   0: container is not running
 *   -ENOENT: container not found
 */
int mcd_is_container_running_byname( char * cname )
{
    #ifdef notdef
    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
	if (pMcd_containers[i] == NULL) { continue; }
        if ( 0 == strncmp( cname, mcd_osd_container_cname(pMcd_containers[i]),
                           MCD_CNTR_NAME_MAXLEN ) ) {
            return ( cntr_running == mcd_osd_container_state(pMcd_containers[i]) );
        }
    }
    #endif

    return -ENOENT;
}


/*
 * returns the TCP port number for the container with the specified cguid
 *
 * Return codes:
 *   0: success
 *   -ENOENT: specified container not found
 */
int mcd_get_tcp_port_by_cguid( SDF_cguid_t cguid, int * tcp_port )
{
    #ifdef notdef
    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
	if (pMcd_containers[i] == NULL) { continue; }
        if ( cguid == mcd_osd_container_cguid(pMcd_containers[i]) ) {
            *tcp_port = mcd_osd_container_tcp_port(pMcd_containers[i]);
            return 0;
        }
    }
    #endif

    return -ENOENT;
}


/*
 * returns the UDP port number for the container with the specified cguid
 *
 * Return codes:
 *   0: success
 *   -ENOENT: specified container not found
 */
int mcd_get_udp_port_by_cguid( SDF_cguid_t cguid, int * udp_port )
{
    #ifdef notdef
    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
	if (pMcd_containers[i] == NULL) { continue; }
        if ( cguid == mcd_osd_container_cguid(pMcd_containers[i]) ) {
            *udp_port = mcd_osd_container_udp_port(pMcd_containers[i]);
            return 0;
        }
    }
    #endif

    return -ENOENT;
}

/*
 * return the container name for the container with the specified cguid
 *
 * @param cname <OUT> buffer for storing container name (must >= 64 bytes)
 *
 * Return codes:
 *   0: success
 *   -ENOENT: specified container not found
 */
int mcd_get_cname_by_cguid( SDF_cguid_t cguid, char * cname )
{
    #ifdef notdef
    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
	if (pMcd_containers[i] == NULL) { continue; }
        if ( cguid == mcd_osd_container_cguid(pMcd_containers[i]) ) {
            snprintf( cname, 64, "%s", mcd_osd_container_cname(pMcd_containers[i]) );
            return 0;
        }
    }
    #endif

    return -ENOENT;
}


/*
 * check whether there is any container update activities going on
 * Return codes:
 *   1: there are ongoing container update operations
 *   0: no ongoing container update operations
 */
int mcd_processing_container_cmds( void )
{
    #ifdef notdef
    return __sync_fetch_and_or( &Mcd_cntr_cmd_count, 0 );
    #endif

    return -ENOENT;
}

int mcd_vip_server_socket(struct vip *vip, Network_Port_Type port_type)
{
    /*
     * FIXME_BINARY: time to remove this function
     */
    plat_abort();
}


/*
 * FIXME: obsolete now and to be removed
 */
int mcd_format_container_internal( void * pai, int tcp_port )
{
    #ifdef notdef
    mcd_log_msg(20728, PLAT_LOG_LEVEL_INFO, "formatting container, port=%d",
                 tcp_port );
    if ( settings.ips_per_cntr ) {
        plat_abort();
    }

    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
	if (pMcd_containers[i] == NULL) { continue; }
        if ( tcp_port == mcd_osd_container_tcp_port(pMcd_containers[i]) ) {
            if ( cntr_stopped != mcd_osd_container_state(pMcd_containers[i]) ) {
                return -EBUSY;
            }
            return mcd_do_format_container( pai, pMcd_containers[i], true );
        }
    }
    #endif

    return -ENOENT;
}


/*
 * FIXME: obsolete now and to be removed
 */
int mcd_is_container_running( int tcp_port )
{
    #ifdef notdef
    if ( settings.ips_per_cntr ) {
        plat_abort();
    }

    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
	if (pMcd_containers[i] == NULL) { continue; }
        if ( tcp_port == mcd_osd_container_tcp_port(pMcd_containers[i]) ) {
            return ( cntr_running == mcd_osd_container_state(pMcd_containers[i]) );
        }
    }
    #endif

    return -ENOENT;
}

#endif /* SDFAPI */

