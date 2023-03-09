#define IS_POSIX __unix__ || __APPLE__ || !_WIN32

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#if IS_POSIX
#include <pthread.h>
#include <sys/time.h>
#else
#include <windows.h>
#endif

#include "../tb_client.h"

// config.message_size_max - @sizeOf(vsr.Header):
#define MAX_MESSAGE_SIZE (1024 * 1024) - 128

// Synchronization context between the callback and the main thread.
typedef struct completion_context {
    uint8_t reply[MAX_MESSAGE_SIZE];
    int size;
    bool completed;

    // In this example we synchronize using a condition variable:
    #if IS_POSIX
    pthread_mutex_t lock;
    pthread_cond_t cv;
    #else
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cv;
    #endif

} completion_context_t;

void completion_context_init(completion_context_t *ctx);
void completion_context_destroy(completion_context_t *ctx);

// Packet handling functions.
tb_packet_t* acquire_packet(tb_packet_list_t *tb_packet_t);
void release_packet(tb_packet_list_t *packet_list, tb_packet_t *packet);

// Sends and blocks the current thread until the reply arrives.
void send_request(
    tb_client_t client,
    tb_packet_list_t *packets,
    completion_context_t *ctx
);

// For benchmarking purposes.
long long get_time_ms(void);

// Completion function, called by tb_client no notify that a request as completed.
void on_completion(
    uintptr_t context, 
    tb_client_t client, 
    tb_packet_t *packet, 
    const uint8_t *data, 
    uint32_t size
);

int main(int argc, char **argv) {
    printf("TigerBeetle C Sample\n");
    printf("Connecting...\n");
    tb_client_t client;
    tb_packet_list_t packets_pool;
    const char *address = "127.0.0.1:3000";

    TB_STATUS status = tb_client_init(
        &client,              // Output client.
        &packets_pool,        // Output packet list.
        0,                    // Cluster ID.
        address,              // Cluster addresses.
        strlen(address),      //
        32,                   // MaxConcurrency, could be 1, since it's a single-threaded example.
        NULL,                 // No need for a global context.
        &on_completion        // Completion callback.
    );

    if (status != TB_STATUS_SUCCESS) {
        printf("Failed to initialize tb_client\n");
        exit(-1);
    }

    completion_context_t ctx;
    completion_context_init(&ctx);

    tb_packet_t *packet;
    tb_packet_list_t packet_list;

    ////////////////////////////////////////////////////////////
    // Submitting a batch of accounts:                        //
    ////////////////////////////////////////////////////////////

    #define ACCOUNTS_LEN 2
    #define ACCOUNTS_SIZE sizeof(tb_account_t) * ACCOUNTS_LEN
    tb_account_t accounts[ACCOUNTS_LEN];
    
    // Zeroing the memory, so we don't have to initialize every field.
    memset(&accounts, 0, ACCOUNTS_SIZE);
    
    accounts[0].id = 1;
    accounts[0].code = 2;
    accounts[0].ledger = 777;

    accounts[1].id = 2;
    accounts[1].code = 2;
    accounts[1].ledger = 777;
    
    // Acquiring a packet for this request:
    packet = acquire_packet(&packets_pool);
    packet->operation = TB_OPERATION_CREATE_ACCOUNTS;  // The operation to be performed.
    packet->data = accounts;                           // The data to be sent.
    packet->data_size = ACCOUNTS_SIZE;                 //
    packet->user_data = &ctx;                          // User-defined context.
    packet->status = TB_PACKET_OK;                     // Will be set when the reply arrives.

    printf("Creating accounts...\n"); 
    
    packet_list.head = packet;
    packet_list.tail = packet;
    send_request(client, &packet_list, &ctx);

    if (packet->status != TB_PACKET_OK) {
        // Checking if the request failed:
        printf("Error calling create_accounts (ret=%d)\n", packet->status);
        exit(-1);
    }

    if (ctx.size != 0) {
        // Checking for errors creating the accounts:
        tb_create_accounts_result_t *results = (tb_create_accounts_result_t*)ctx.reply;
        int results_len = ctx.size / sizeof(tb_create_accounts_result_t);
        printf("create_account results:\n");
        for(int i=0;i<results_len;i++) {
            printf("index=%d, ret=%d\n", results[i].index, results[i].result);
        }
        exit(-1);
    }

    // Releasing the packet, so it can be used in a next request.
    release_packet(&packets_pool, packet);

    printf("Accounts created successfully\n");
    
    ////////////////////////////////////////////////////////////
    // Submitting multiple batches of transfers:              //
    ////////////////////////////////////////////////////////////

    printf("Creating transfers...\n");
    #define MAX_BATCHES 100
    #define TRANSFERS_PER_BATCH ((MAX_MESSAGE_SIZE) / sizeof(tb_transfer_t))
    long max_latency_ms = 0;
    long total_time_ms = 0;
    for (int i=0; i< MAX_BATCHES;i++) {
        tb_transfer_t transfers[TRANSFERS_PER_BATCH];
        // Zeroing the memory, so we don't have to initialize every field.
        memset(transfers, 0, MAX_MESSAGE_SIZE);
        
        for (int j=0; j<TRANSFERS_PER_BATCH; j++) {
            transfers[j].id = j + 1 + (i * TRANSFERS_PER_BATCH);
            transfers[j].debit_account_id = accounts[0].id;
            transfers[j].credit_account_id = accounts[1].id;
            transfers[j].code = 2;
            transfers[j].ledger = 777;
            transfers[j].amount = 1;
        }

        // Acquiring a packet for this request:
        packet = acquire_packet(&packets_pool);
        packet->operation = TB_OPERATION_CREATE_TRANSFERS;  // The operation to be performed.
        packet->data = transfers;                           // The data to be sent.
        packet->data_size = MAX_MESSAGE_SIZE;               //
        packet->user_data = &ctx;                           // User-defined context.
        packet->status = TB_PACKET_OK;                      // Will be set when the reply arrives.

        long long now = get_time_ms();

        packet_list.head = packet;
        packet_list.tail = packet;
        send_request(client, &packet_list, &ctx);
  
        long elapsed_ms = get_time_ms() - now;
        if (elapsed_ms > max_latency_ms) max_latency_ms = elapsed_ms;
        total_time_ms += elapsed_ms;
        
        if (packet->status != TB_PACKET_OK) {
            // Checking if the request failed:
            printf("Error calling create_transfers (ret=%d)\n", packet->status);
            exit(-1);
        }

        if (ctx.size != 0) {
            // Checking for errors creating the accounts:
            tb_create_transfers_result_t *results = (tb_create_transfers_result_t*)ctx.reply;
            int results_len = ctx.size / sizeof(tb_create_transfers_result_t);
            printf("create_transfers results:\n");
            for(int i=0;i<results_len;i++) {
                printf("index=%d, ret=%d\n", results[i].index, results[i].result);
            }
            exit(-1);
        }

        // Releasing the packet, so it can be used in a next request.
        release_packet(&packets_pool, packet);
    }

    printf("Transfers created successfully\n");
	printf("============================================\n");

    printf("%d transfers per second\n", (MAX_BATCHES * TRANSFERS_PER_BATCH * 1000) / total_time_ms);
	printf("create_transfers max p100 latency per %d transfers = %dms\n", TRANSFERS_PER_BATCH, max_latency_ms);
	printf("total %d transfers in %dms\n", MAX_BATCHES * TRANSFERS_PER_BATCH, total_time_ms);    
    printf("\n");

    ////////////////////////////////////////////////////////////
    // Looking up accounts:                                   //
    ////////////////////////////////////////////////////////////

    printf("Looking up accounts ...\n");
    tb_uint128_t ids[ACCOUNTS_LEN] = { accounts[0].id, accounts[1].id };
    
    // Acquiring a packet for this request:    
    packet = acquire_packet(&packets_pool);
    packet->operation = TB_OPERATION_LOOKUP_ACCOUNTS;
    packet->data = ids;
    packet->data_size = sizeof(tb_uint128_t) * ACCOUNTS_LEN;
    packet->user_data = &ctx;
    packet->status = TB_PACKET_OK;

    packet_list.head = packet;
    packet_list.tail = packet;
    send_request(client, &packet_list, &ctx);
    
    if (packet->status != TB_PACKET_OK) {
        // Checking if the request failed:
        printf("Error calling lookup_accounts (ret=%d)", packet->status);
        exit(-1);
    }

    if (ctx.size == 0) {
        printf("No accounts found");
        exit(-1);
    } else {
        // Printing the account's balance:
        tb_account_t *results = (tb_account_t*)ctx.reply;
        int results_len = ctx.size / sizeof(tb_account_t);
        printf("%d Account(s) found\n", results_len);
        printf("============================================\n");

        for(int i=0;i<results_len;i++) {            
            printf("id=%d\n", (long)results[i].id);
            printf("debits_posted=%d\n", results[i].debits_posted);
            printf("credits_posted=%d\n", results[i].credits_posted);
            printf("\n");
        }
    }

    // Releasing the packet, so it can be used in a next request.
    release_packet(&packets_pool, packet);

    // Cleanup
    completion_context_destroy(&ctx);
    tb_client_deinit(client);
}

tb_packet_t* acquire_packet(tb_packet_list_t *packet_list) {
    
    // This sample is single-threaded,
    // In real use, this function should be thread-safe.
    tb_packet_t *packet = packet_list->head;

    if (packet == NULL) {
        printf("Too many concurrent requests\n");
        exit(-1);
    }

    packet_list->head = packet->next;
    packet->next = NULL;
    
    if (packet_list->head == NULL) {
        packet_list->tail = NULL;
    }

    return packet;
}

void release_packet(tb_packet_list_t *packet_list, tb_packet_t *packet) {
    // This sample is single-threaded,
    // In real use, this function should be thread-safe.
    if (packet_list->head == NULL) {
        packet_list->head = packet;
        packet_list->tail = packet;
    } else {
        packet_list->tail->next = packet;
        packet_list->tail = packet;
    }
}

#if IS_POSIX

void on_completion(
    uintptr_t context, 
    tb_client_t client, 
    tb_packet_t *packet, 
    const uint8_t *data, 
    uint32_t size
) {    
    // The user_data gives context to a request:
    completion_context_t* ctx = (completion_context_t*)packet->user_data;

    // Signaling the main thread we received the reply:
    pthread_mutex_lock(&ctx->lock);

    memcpy (ctx->reply, data, size);
    ctx->size = size;
    ctx->completed = true;
    
    pthread_cond_signal(&ctx->cv);
    pthread_mutex_unlock(&ctx->lock);
}

void send_request(
    tb_client_t client,
    tb_packet_list_t *packets,
    completion_context_t *ctx
) {
    // Locks the mutex:
    if (pthread_mutex_lock(&ctx->lock) != 0) {
        printf("Failed to lock mutex\n");
        exit(-1);
    }

    // Submits the request asynchronously:
    ctx->completed = false;
    tb_client_submit(client, packets);

    // Uses a condvar to sync this thread with the callback:
    while (!ctx->completed) {
        if (pthread_cond_wait(&ctx->cv, &ctx->lock) != 0) {
            printf("Failed to wait condvar\n");
            exit(-1);
        }
    }
    
    if (pthread_mutex_unlock(&ctx->lock) != 0) {
        printf("Failed to unlock mutex\n");
        exit(-1);
    }
}

void completion_context_init(completion_context_t *ctx) {
    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        printf("Failed to initialize mutex\n");
        exit(-1);
    }

    if (pthread_cond_init(&ctx->cv, NULL) != 0) {
        printf("Failed to initialize condition var\n");
        exit(-1);
    }
}

void completion_context_destroy(completion_context_t *ctx) {
    pthread_cond_destroy(&ctx->cv);
    pthread_mutex_destroy(&ctx->lock);
}

long long get_time_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv,NULL) != 0) {
        printf("Failed to get time of day\n");
        exit(-1);
    }
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

#else

void on_completion(
    uintptr_t context, 
    tb_client_t client, 
    tb_packet_t *packet, 
    const uint8_t *data, 
    uint32_t size
) {    
    // The user_data gives context to a request:
    completion_context_t* ctx = (completion_context_t*)packet->user_data;

    // Signaling the main thread we received the reply:
    EnterCriticalSection(&ctx->lock);
    
    memcpy (ctx->reply, data, size);
    ctx->size = size;
    ctx->completed = true;
    
    WakeConditionVariable(&ctx->cv);
    LeaveCriticalSection(&ctx->lock);
}

void send_request(
    tb_client_t client,
    tb_packet_list_t *packets,
    completion_context_t *ctx
) {
    // Locks the mutex:
    EnterCriticalSection(&ctx->lock);

    // Submits the request asynchronously:
    ctx->completed = false;
    tb_client_submit(client, packets);

    // Uses a condvar to sync this thread with the callback:
    while (!ctx->completed) {
        SleepConditionVariableCS (&ctx->cv, &ctx->lock, INFINITE);
    }
    
    LeaveCriticalSection(&ctx->lock);
}

void completion_context_init(completion_context_t *ctx) {
    InitializeCriticalSection(&ctx->lock);
    InitializeConditionVariable(&ctx->cv);
}

void completion_context_destroy(completion_context_t *ctx) {
    DeleteCriticalSection(&ctx->lock);
}

long long get_time_ms(void) {
    return GetTickCount();
}

#endif