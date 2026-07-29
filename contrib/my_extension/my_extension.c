#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "postmaster/bgworker.h"
#include "miscadmin.h"
#include "endpoint_handlers.h"

PG_MODULE_MAGIC;

static int my_initial_value = 10;

int *shared_value = NULL;

static bool worker_registered = false;

PG_FUNCTION_INFO_V1(print_hello_c);
PG_FUNCTION_INFO_V1(set_value);

Datum
print_hello_c(PG_FUNCTION_ARGS)
{
    char *message = "hello from c";
    text *result = cstring_to_text(message);
    PG_RETURN_TEXT_P(result);
}

Datum
set_value(PG_FUNCTION_ARGS)
{
    int new_value = PG_GETARG_INT32(0);

    if (shared_value == NULL)
    {
        bool found;

        shared_value = (int *) ShmemInitStruct("my_extension_shared", sizeof(int), &found);
        
        if (!found)
        {
            *shared_value = my_initial_value;
            elog(LOG, "my_extension: shared memory initialized with value %d", my_initial_value);
        }
    }
    *shared_value = new_value;
    elog(LOG, "my_extension: value changed to %d", new_value);
    PG_RETURN_VOID();
}

PGDLLEXPORT void
my_bgworker_main(Datum main_arg)
{

    bool found;

    shared_value = (int *) ShmemInitStruct("my_extension_shared", sizeof(int), &found);
        
    if (!found)
    {
        *shared_value = my_initial_value;
        elog(LOG, "my_extension: shared memory initialized with value %d", my_initial_value);
    }

    BackgroundWorkerUnblockSignals();

    elog(LOG, "my_bgworker: started");

    register_endpoint("/value/get", handler_get_value);
    register_endpoint("/value/set", handler_post_value);

    process_rest();
    
    while(1)
    {
        rest_server_poll();
        
        pg_usleep(5000000L);

        if (InterruptPending)
        {
            break;
        }

        if (shared_value != NULL)
        {
            elog(LOG, "my_bgworker: shared_value = %d", *shared_value);
        }
        else
        {
            elog(LOG, "my_bgworker: shared_value not available");
        }
    }

    elog(LOG, "my_bgworker: stopped");
}

static void
register_my_bgworker(void)
{
    BackgroundWorker worker;

    if (worker_registered)
    {
        return;
    }

    memset(&worker, 0, sizeof(BackgroundWorker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 5;
    //worker.bgw_main = my_bgworker_main;
    worker.bgw_main_arg = (Datum) 0;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "my_extension");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "my_bgworker_main");
    snprintf(worker.bgw_name, BGW_MAXLEN, "My BGWorker");

    RegisterBackgroundWorker(&worker);
    worker_registered = true;

    elog(LOG, "my_extension: bgworker registered");
}

static void
my_shmem_request(void)
{
    RequestAddinShmemSpace(sizeof(int));
    elog(LOG, "my_extension: shared_memory requested");
}

void
_PG_init(void)
{
    //guc
    DefineCustomIntVariable("my_extension.initial_value",
                            "Initial value for bgworker",
                            "Sets initial value for the bgworker's variable",
                            &my_initial_value, 10, 0, 100,
                            PGC_SIGHUP, 0, NULL, NULL, NULL);
    
    //RequestAddinShmemSpace(sizeof(int));

    shmem_request_hook = my_shmem_request;
    //shmem_request_hook = my_shmem_startup;

    if (!IsUnderPostmaster)
    {
        register_my_bgworker();
    }

    elog(LOG, "my_extension: initialized");
}

void
_PG_fini(void)
{
    
}