/*
 * merger.h
 *
 * Copyright 2010-2012 Yahoo! Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#ifndef _MERGER_H_
#define _MERGER_H_

#include "bLSM.h"

#include <stasis/common.h>
#include <mutex>

class RateLimiter {
public:
    RateLimiter();
    long aquire();
    long aquire(int permits);

    bool try_aquire(int timeouts);
    bool try_aquire(int permits, int timeout);

    double get_rate() const;
    void set_rate(double rate);
private:
    void sync(unsigned long long now);
    std::chrono::microseconds claim_next(double permits);
private:
    double interval_;
    double max_permits_;
    double stored_permits_;

    unsigned long long next_free_;

    std::mutex mut_;
};

class mergeScheduler {
public:
  mergeScheduler(bLSM * ltable);
  ~mergeScheduler();

  void start();
  void shutdown();

  void * memMergeThread();
  void * diskMergeThread();

private:
  pthread_t mem_merge_thread_;
  pthread_t disk_merge_thread_;
  bLSM * ltable_;
  const double MIN_R;
};

#endif
