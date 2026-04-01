/******************************************************************************
 * FreeRTOS IPC Timing Demo with MPU Support
 *
 * This demo is inspired by the S3K kernel IPC timing example. It demonstrates
 * Inter-Process Communication (IPC) timing measurements using FreeRTOS queues.
 *
 * Two tasks (Client and Server) communicate via a queue, measuring the cycle
 * count using the RISC-V rdcycle instruction. The timing is synchronized to
 * time slices for consistent measurements.
 *
 * This version is modified to work with the MPU port using xTaskCreateRestricted()
 * to properly configure memory protection regions for each task.
 *
 * Timing Model (matching S3K IPC):
 *   - Server records cycle count BEFORE blocking on queue receive (data[1])
 *   - Server records cycle count AFTER sending reply (data[0])
 *   - Client records cycle count BEFORE sending (start)
 *   - Client records cycle count AFTER receiving reply (end)
 *   - call = data[0] - start (time from client start to server reply complete)
 *   - replyrecv = end - data[1] (time from server waiting to client receive complete)
 *   - rtt = call + replyrecv (total round-trip time)
 *
 * This demo outputs timing data in CSV format matching S3K: call,replyrecv,rtt
 ******************************************************************************/

/* Standard includes. */
#include <stdio.h>
#include <stdint.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Priorities at which the tasks are created - equal priority.
 * Note: Add portPRIVILEGE_BIT to run tasks in privileged mode so they can
 * access the cycle counter (rdcycle instruction). In MPU ports, tasks are
 * unprivileged by default and cannot access CSRs. */
#define mainIPC_TASK_PRIORITY           ( ( tskIDLE_PRIORITY + 2 ) | portPRIVILEGE_BIT )

/* The number of items the queue can hold. */
#define mainQUEUE_LENGTH                ( 2 )

/* The rate at which the test runs. */
#define mainTEST_PERIOD_MS              pdMS_TO_TICKS( 1000UL )

/* Number of warm-up iterations before measurement. */
#define mainWARMUP_ITERATIONS           ( 5 )

/* Number of measurement iterations. */
#define mainMEASUREMENT_ITERATIONS      ( 100 )

/*-----------------------------------------------------------*/

/* Message structure for IPC timing - matches S3K msg.data fields. */
typedef struct
{
    uint64_t cycle_after;           /* Server cycle after sending reply */
    uint64_t cycle_before;           /* Server cycle before waiting (receive) */
    uint32_t ulIteration;       /* Iteration number */
} IPCMessage_t;

/*-----------------------------------------------------------*/

/* Externs for memory region boundaries from linker script */
extern char _text[];
extern char _etext[];
extern char _rodata[];
extern char _erodata[];
extern char _data[];
extern char _edata[];
extern char _bss[];
extern char _ebss[];

/*-----------------------------------------------------------*/

/* Static stack buffers for tasks - must be aligned for MPU */
static StackType_t xClientTaskStack[ configMINIMAL_STACK_SIZE * 2 ] __attribute__((aligned( configMINIMAL_STACK_SIZE * 2 * sizeof( StackType_t ) )));
static StackType_t xServerTaskStack[ configMINIMAL_STACK_SIZE * 2 ] __attribute__((aligned( configMINIMAL_STACK_SIZE * 2 * sizeof( StackType_t ) )));

/*-----------------------------------------------------------*/

/*
 * The tasks as described in the comments at the top of this file.
 */
static void prvClientTask( void * pvParameters );
static void prvServerTask( void * pvParameters );

/*-----------------------------------------------------------*/

/* The queue used by both tasks. */
static QueueHandle_t xQueue = NULL;

/* Task handles for FreeRTOS info. */
static TaskHandle_t xClientTaskHandle = NULL;
static TaskHandle_t xServerTaskHandle = NULL;

/*-----------------------------------------------------------*/

static inline uint64_t rdcycle( void )
{
    uint64_t ulCycle;
    __asm__ volatile( "rdcycle %0" : "=r" ( ulCycle ) );
    return ulCycle;
}

/*-----------------------------------------------------------*/

void main_ipc( void )
{
    BaseType_t xResult;

    printf( "IPC Timing Demo starting...\n" );

    /* Create the queue. */
    xQueue = xQueueCreate( mainQUEUE_LENGTH, sizeof( IPCMessage_t ) );

    if( xQueue != NULL )
    {
        /* Define MPU regions for client task */
        TaskParameters_t xClientTaskParameters =
        {
            .pvTaskCode     = prvClientTask,
            .pcName         = "IPC_Client",
            .usStackDepth   = ( configMINIMAL_STACK_SIZE * 2 ) / sizeof( StackType_t ),
            .pvParameters   = NULL,
            .uxPriority     = mainIPC_TASK_PRIORITY,
            .puxStackBuffer = xClientTaskStack,
            .xRegions       =
            {
                /* Region 0: Code and read-only data */
                { _text, ( size_t )( _erodata - _text ), portMPU_REGION_READ | portMPU_REGION_EXECUTE },
                /* Region 1: RAM data and bss */
                { _data, ( size_t )( _ebss - _data ), portMPU_REGION_READ | portMPU_REGION_WRITE },
                /* Region 2: ns16550 */
                { (void *)0x10000000UL, 0x1000, portMPU_REGION_READ | portMPU_REGION_WRITE },
                /* Regions 3-6: Unused */
                { 0, 0, 0 },
                { 0, 0, 0 },
                { 0, 0, 0 },
                { 0, 0, 0 },
                /* Region 7: Reserved for stack (FreeRTOS manages this) */
                { 0, 0, 0 },
            }
        };

        /* Define MPU regions for server task */
        TaskParameters_t xServerTaskParameters =
        {
            .pvTaskCode     = prvServerTask,
            .pcName         = "IPC_Server",
            .usStackDepth   = ( configMINIMAL_STACK_SIZE * 2 ) / sizeof( StackType_t ),
            .pvParameters   = NULL,
            .uxPriority     = mainIPC_TASK_PRIORITY,
            .puxStackBuffer = xServerTaskStack,
            .xRegions       =
            {
                /* Region 0: Code and read-only data */
                { _text, ( size_t )( _erodata - _text ), portMPU_REGION_READ | portMPU_REGION_EXECUTE },
                /* Region 1: RAM data and bss */
                { _data, ( size_t )( _ebss - _data ), portMPU_REGION_READ | portMPU_REGION_WRITE },
                /* Region 2: UART device at 0x40004000 */
                { ( void * ) 0x40004000UL, 0x1000, portMPU_REGION_READ | portMPU_REGION_WRITE },
                /* Regions 3-6: Unused */
                { 0, 0, 0 },
                { 0, 0, 0 },
                { 0, 0, 0 },
                { 0, 0, 0 },
                /* Region 7: Reserved for stack (FreeRTOS manages this) */
                { 0, 0, 0 },
            }
        };

        /* Create the client task with restricted MPU regions. */
        xResult = xTaskCreateRestricted( &xClientTaskParameters, &xClientTaskHandle );
        if( xResult != pdPASS )
        {
            printf( "ERROR: Failed to create client task!\n" );
            for( ; ; );
        }

        /* Grant client task access to the queue. */
        vGrantAccessToKernelObject( xClientTaskHandle, ( int32_t ) xQueue );

        /* Create the server task with restricted MPU regions. */
        xResult = xTaskCreateRestricted( &xServerTaskParameters, &xServerTaskHandle );
        if( xResult != pdPASS )
        {
            printf( "ERROR: Failed to create server task!\n" );
            for( ; ; );
        }

        /* Grant server task access to the queue. */
        vGrantAccessToKernelObject( xServerTaskHandle, ( int32_t ) xQueue );

        printf( "Tasks created successfully, starting scheduler...\n" );

        /* Start the tasks running. */
        vTaskStartScheduler();
    }
    else
    {
        printf( "ERROR: Failed to create queue!\n" );
    }

    /* If all is well, the scheduler will now be running, and the following
     * line will never be reached. */
    for( ; ; )
    {
    }
}

/*-----------------------------------------------------------*/

static void prvClientTask( void * pvParameters )
{
    TickType_t xNextWakeTime;
    IPCMessage_t xMessage;
    IPCMessage_t xReceivedMessage;
    uint64_t start_cycle;
    uint64_t end_cycle;
    uint64_t call_time;
    uint64_t reply_recv_time;
    uint64_t round_trip_time;
    int i;

    /* Prevent the compiler warning about the unused parameter. */
    ( void ) pvParameters;

    /* Initialise xNextWakeTime - this only needs to be done once. */
    xNextWakeTime = xTaskGetTickCount();

    /* Print CSV header matching S3K format. */
    printf( "call,replyrecv,rtt\n" );

    /* Warm-up phase: 5 iterations as in S3K. */
    for( i = 0; i < mainWARMUP_ITERATIONS; i++ )
    {
        /* Synchronize to time slice. */
        vTaskDelayUntil( &xNextWakeTime, mainTEST_PERIOD_MS );

        /* Prepare message. */
        xMessage.ulIteration = i;

        /* Record cycle count before send (like S3K start). */
        start_cycle = rdcycle();

        /* Send message to server (blocking). */
        xQueueSend( xQueue, &xMessage, 0 );

        /* Receive reply from server (blocking). */
        xQueueReceive( xQueue, &xReceivedMessage, 0 );

        /* Record cycle count after receive (like S3K end). */
        end_cycle = rdcycle();
    }

    /* Measurement phase: 100 iterations. */
    for( i = 0; i < mainMEASUREMENT_ITERATIONS; i++ )
    {
        /* Synchronize to time slice. */
        vTaskDelayUntil( &xNextWakeTime, mainTEST_PERIOD_MS );

        /* Prepare message. */
        xMessage.ulIteration = mainWARMUP_ITERATIONS + i;

        /* Record cycle count before send (like S3K start). */
        start_cycle = rdcycle();

        /* Send message to server (blocking - will block until server receives). */
        xQueueSend( xQueue, &xMessage, 0 );

        /* Receive reply from server (blocking - will block until server sends). */
        xQueueReceive( xQueue, &xReceivedMessage, 0 );

        /* Record cycle count after receive (like S3K end). */
        end_cycle = rdcycle();

        /* Calculate timings matching S3K model exactly:
         * call = data[0] - start (time from client start to server reply complete)
         * replyrecv = end - data[1] (time from server wait start to client end)
         * rtt = call + replyrecv
         */
        call_time = xReceivedMessage.cycle_after - start_cycle;
        reply_recv_time = end_cycle - xReceivedMessage.cycle_before;
        round_trip_time = call_time + reply_recv_time;

        /* Output CSV row matching S3K format exactly. */
        printf( "%u,%u,%u\n",
                ( unsigned int ) ( call_time & 0xFFFFFFFFULL ),
                ( unsigned int ) ( reply_recv_time & 0xFFFFFFFFULL ),
                ( unsigned int ) ( round_trip_time & 0xFFFFFFFFULL ) );
    }

    /* After measurements, suspend ourselves. */
    vTaskSuspend( NULL );
}

/*-----------------------------------------------------------*/

static void prvServerTask( void * pvParameters )
{
    IPCMessage_t xReceivedMessage;
    IPCMessage_t xReplyMessage;
    uint64_t server_before;
    uint64_t server_after;

    /* Prevent the compiler warning about the unused parameter. */
    ( void ) pvParameters;

    for( ; ; )
    {
        /* Record cycle count BEFORE waiting on queue (like S3K data[1]).
         * This captures when the server starts waiting. */
        server_before = rdcycle();

        /* Wait for a message from the client (blocking).
         * This simulates s3k_ipc_replyrecv() which blocks until message arrives
         * and then sends reply when returning. */
        xQueueReceive( xQueue, &xReceivedMessage, portMAX_DELAY );

        /* Record cycle count AFTER processing and before sending reply (like S3K data[0]).
         * In S3K, the kernel sends reply atomically when returning from replyrecv.
         * Here we record before xQueueSend to approximate this timing. */
        server_after = rdcycle();

        /* Prepare reply message with server timestamps and original iteration. */
        xReplyMessage.cycle_after = server_after;
        xReplyMessage.cycle_before = server_before;
        xReplyMessage.ulIteration = xReceivedMessage.ulIteration;

        /* Send reply back to client.
         * This completes the IPC round-trip. */
        xQueueSend( xQueue, &xReplyMessage, 0 );
    }
}
/*-----------------------------------------------------------*/
