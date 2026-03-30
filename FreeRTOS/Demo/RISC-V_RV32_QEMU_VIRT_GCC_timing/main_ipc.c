/*
 * FreeRTOS V202212.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */

/******************************************************************************
 * FreeRTOS IPC Timing Demo
 *
 * This demo is inspired by the S3K kernel IPC timing example. It demonstrates
 * Inter-Process Communication (IPC) timing measurements using FreeRTOS queues.
 *
 * Two tasks (Client and Server) communicate via a queue, measuring the cycle
 * count using the RISC-V rdcycle instruction. The timing is synchronized to
 * time slices for consistent measurements.
 *
 * Timing Model (matching S3K IPC):
 *   - Server records cycle count BEFORE blocking on queue receive (data[1])
 *   - Server records cycle count AFTER sending reply (data[0])
 *   - Client records cycle count BEFORE sending (start)
 *   - Client records cycle count AFTER receiving reply (end)
 *   - call = data[0] - start (time from client send to server reply complete)
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

/* Priorities at which the tasks are created - equal priority. */
#define mainIPC_TASK_PRIORITY           ( tskIDLE_PRIORITY + 2 )

/* The number of items the queue can hold. */
#define mainQUEUE_LENGTH                ( 2 )

/* The rate at which the test runs. */
#define mainTEST_PERIOD_MS              pdMS_TO_TICKS( 100UL )

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
    /* Create the queue. */
    xQueue = xQueueCreate( mainQUEUE_LENGTH, sizeof( IPCMessage_t ) );

    if( xQueue != NULL )
    {
        /* Create the client task. */
        xTaskCreate( prvClientTask,
                     "IPC_Client",
                     configMINIMAL_STACK_SIZE * 2,
                     NULL,
                     mainIPC_TASK_PRIORITY,
                     &xClientTaskHandle );

        /* Create the server task with equal priority. */
        xTaskCreate( prvServerTask,
                     "IPC_Server",
                     configMINIMAL_STACK_SIZE * 2,
                     NULL,
                     mainIPC_TASK_PRIORITY,
                     &xServerTaskHandle );

        /* Start the tasks running. */
        vTaskStartScheduler();
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
