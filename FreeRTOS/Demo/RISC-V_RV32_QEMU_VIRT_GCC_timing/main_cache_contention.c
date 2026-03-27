/*
 * FreeRTOS V202212.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */

#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"

#define MATRIX_SIZE 16
#define ITERATIONS 5
#define MM_REPEATS 2

static inline uint64_t get_cycle_count(void) {
  uint64_t cycles;
  __asm__ volatile("rdcycle %0" : "=r"(cycles));
  return cycles;
}

void mm(int n, volatile int m1[n][n], volatile int m2[n][n],
        volatile int res[n][n]) {
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      int sum = 0;
      for (int k = 0; k < n; k++) {
        int op1 = m1[i][k];
        int op2 = m2[k][j];
        sum += op1 * op2;
      }
      res[i][j] = sum;
    }
  }
}

static void prvCacheContentionTask(void *pvParameters) {
  volatile int m1[MATRIX_SIZE][MATRIX_SIZE];
  volatile int m2[MATRIX_SIZE][MATRIX_SIZE];
  volatile int res[MATRIX_SIZE][MATRIX_SIZE];
  int task_id = *(int *)pvParameters;

  for (int i = 0; i < MATRIX_SIZE; ++i)
    for (int j = 0; j < MATRIX_SIZE; ++j) {
      m1[i][j] = i + j;
      m2[i][j] = i - j;
    }

  for (int iter = 0; iter < ITERATIONS; ++iter) {

    uint64_t start = get_cycle_count();

    for (int repeat = 0; repeat < MM_REPEATS; ++repeat) {
      mm(MATRIX_SIZE, m1, m2, res);
    }

    uint64_t end = get_cycle_count();
    printf("%d,%d,%d,%d,%d\r\n", task_id, iter, (unsigned long)start,
           (unsigned long)end, (unsigned long)(end - start));
  }

  printf("Task %d finished\r\n", task_id);
}

void main_cache_contention(void) {
  int task1_id = 1;
  int task2_id = 2;

  xTaskCreate(prvCacheContentionTask, "CC1",
              MATRIX_SIZE * MATRIX_SIZE * MATRIX_SIZE +
                  configMINIMAL_STACK_SIZE,
              &task1_id, tskIDLE_PRIORITY + 1, NULL);

  xTaskCreate(prvCacheContentionTask, "CC2",
              MATRIX_SIZE * MATRIX_SIZE * MATRIX_SIZE +
                  configMINIMAL_STACK_SIZE,
              &task2_id, tskIDLE_PRIORITY + 1, NULL);

  printf("task,iter,start,end,duration\r\n");
  vTaskStartScheduler();
}
