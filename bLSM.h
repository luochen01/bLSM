/*
 * blsm.h
 *
 * Copyright 2009-2012 Yahoo! Inc.
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
#ifndef _LOGSTORE_H_
#define _LOGSTORE_H_

#include <stasis/common.h>

#include <vector>

#include "diskTreeComponent.h"
#include "memTreeComponent.h"
#include "tupleMerger.h"
#include "mergeManager.h"
#include "mergeStats.h"

class bLSM {
public:

  class iterator;

  static int limit;

  // We want datapages to be as small as possible, assuming they don't force an extra seek to traverse the bottom level of internal nodes.
  // Internal b-tree mem requirements:
  //  - Assume keys are small (compared to stasis pages) so we can ignore all but the bottom level of the tree.
  //
  //  |internal nodes| ~= (|key| * |tree|) / (datapage_size * |stasis PAGE_SIZE|)
  //
  // Plugging in the numbers today:
  //
  //  6GB ~= 100B * 500 GB / (datapage_size * 4KB)
  //  (100B * 500GB) / (6GB * 4KB) = 2.035
  // RCS: Set this to 1 so that we do (on average) one seek per b-tree read.
  bLSM(int log_mode = 0, pageid_t max_c0_size = 100 * 1024 * 1024, pageid_t internal_region_size = 16384, pageid_t datapage_region_size = 256000, pageid_t datapage_size = 1);

    ~bLSM();

    double * R() { return &r_val; }

    //user access functions
    dataTuple * findTuple(int xid, const dataTuple::key_t key, size_t keySize);

    dataTuple * findTuple_first(int xid, dataTuple::key_t key, size_t keySize);

private:
    dataTuple * insertTupleHelper(dataTuple *tuple);
public:
    void insertManyTuples(struct dataTuple **tuples, int tuple_count);
    void insertTuple(struct dataTuple *tuple);
    /** This test and set has strange semantics on two fronts:
     *
     * 1) It is not atomic with respect to non-testAndSet operations (which is fine in theory, since they have no barrier semantics, and we don't have a use case to support the extra overhead)
     * 2) If tuple2 is not null, it looks at tuple2's key instead of tuple's key.  This means you can atomically set the value of one key based on the value of another (if you want to...)
     */
    bool testAndSetTuple(struct dataTuple *tuple, struct dataTuple *tuple2);

    //other class functions
    recordid allocTable(int xid);
    void openTable(int xid, recordid rid);
    void flushTable();    

    void replayLog();
    void logUpdate(dataTuple * tup);

    static void init_stasis();
    static void deinit_stasis();

    inline uint64_t get_epoch() { return epoch; }

    void registerIterator(iterator * it);
    void forgetIterator(iterator * it);
    void bump_epoch() ;

    inline diskTreeComponent * get_tree_c2(){return tree_c2;}
    inline diskTreeComponent * get_tree_c1(){return tree_c1;}
    inline diskTreeComponent * get_tree_c1_mergeable(){return tree_c1_mergeable;}
    inline diskTreeComponent * get_tree_c1_prime(){return tree_c1_prime;}

    inline void set_tree_c1(diskTreeComponent *t){tree_c1=t;                      bump_epoch(); }
    inline void set_tree_c1_mergeable(diskTreeComponent *t){tree_c1_mergeable=t;  bump_epoch(); }
    inline void set_tree_c1_prime(diskTreeComponent *t){tree_c1_prime=t;  bump_epoch(); }
    inline void set_tree_c2(diskTreeComponent *t){tree_c2=t;                      bump_epoch(); }
    pthread_cond_t c0_needed;
    pthread_cond_t c0_ready;
    pthread_cond_t c1_needed;
    pthread_cond_t c1_ready;

    inline memTreeComponent::rbtree_ptr_t get_tree_c0(){return tree_c0;}
    inline memTreeComponent::rbtree_ptr_t get_tree_c0_mergeable(){return tree_c0_mergeable;}
    void set_tree_c0(memTreeComponent::rbtree_ptr_t newtree){tree_c0 = newtree;                     bump_epoch(); }

    bool get_c0_is_merging() { return c0_is_merging; }
    void set_c0_is_merging(bool is_merging) { c0_is_merging = is_merging; }
    void set_tree_c0_mergeable(memTreeComponent::rbtree_ptr_t newtree){tree_c0_mergeable = newtree; bump_epoch(); }
    lsn_t get_log_offset();
    void truncate_log();

    void update_persistent_header(int xid, lsn_t log_trunc = INVALID_LSN);

    inline tupleMerger * gettuplemerger(){return tmerger;}
    
public:

    struct table_header {
        recordid c2_root;     //tree root record --> points to the root of the b-tree
        recordid c2_state;    //tree state --> describes the regions used by the index tree
        recordid c2_dp_state; //data pages state --> regions used by the data pages
        recordid c1_root;
        recordid c1_state;
        recordid c1_dp_state;
        recordid merge_manager;
        lsn_t    log_trunc;
    };
    rwlc * header_mut;
    pthread_mutex_t tick_mut;
    pthread_mutex_t rb_mut;
    int64_t max_c0_size;
    // these track the effectiveness of snowshoveling
    int64_t mean_c0_run_length;
    int64_t num_c0_mergers;

    mergeManager * merge_mgr;

    stasis_log_t * log_file;
    int log_mode;
    int batch_size;
    bool recovering;

    bool accepting_new_requests;
    inline bool is_still_running() { return !shutting_down_; }
    inline void stop() {
      rwlc_writelock(header_mut);
      if(!shutting_down_) {
        shutting_down_ = true;
        flushTable();
        c0_flushing = true;
        c1_flushing = true;
      }
      rwlc_unlock(header_mut);
      // XXX must need to do other things! (join the threads?)
    }

private:    
    double r_val;
    recordid table_rec;
    struct table_header tbl_header;
    uint64_t epoch;
    diskTreeComponent *tree_c2; //big tree
    diskTreeComponent *tree_c1; //small tree
    diskTreeComponent *tree_c1_mergeable; //small tree: ready to be merged with c2
    diskTreeComponent *tree_c1_prime; //small tree: ready to be merged with c2
    memTreeComponent::rbtree_ptr_t tree_c0; // in-mem red black tree
    memTreeComponent::rbtree_ptr_t tree_c0_mergeable; // in-mem red black tree: ready to be merged with c1.
    bool c0_is_merging;

public:
    bool c0_flushing;
    bool c1_flushing; // this needs to be set to true at shutdown, or when the c0-c1 merger is waiting for c1-c2 to finish its merge

    lsn_t current_timestamp;
    lsn_t expiry;

    //DATA PAGE SETTINGS
    pageid_t internal_region_size; // in number of pages
    pageid_t datapage_region_size; // "
    pageid_t datapage_size;        // "
private:
    tupleMerger *tmerger;

    std::vector<iterator *> its;

public:
    bool shutting_down_;

    bool mightBeOnDisk(dataTuple * t) {
      if(tree_c1) {
        if(!tree_c1->bloom_filter) { DEBUG("no c1 bloom filter\n"); return true; }
        if(stasis_bloom_filter_lookup(tree_c1->bloom_filter,           (const char*)t->strippedkey(), t->strippedkeylen())) { DEBUG("in c1\n"); return true; }
      }
      if(tree_c1_prime) {
        if(!tree_c1_prime->bloom_filter) { DEBUG("no c1' bloom filter\n");  return true; }
        if(stasis_bloom_filter_lookup(tree_c1_prime->bloom_filter,     (const char*)t->strippedkey(), t->strippedkeylen())) { DEBUG("in c1'\n"); return true; }
      }
      return mightBeAfterMemMerge(t);
    }

    bool mightBeAfterMemMerge(dataTuple * t) {

      if(tree_c1_mergeable) {
        if(!tree_c1_mergeable->bloom_filter) { DEBUG("no c1m bloom filter\n"); return true; }
        if(stasis_bloom_filter_lookup(tree_c1_mergeable->bloom_filter,     (const char*)t->strippedkey(), t->strippedkeylen())) { DEBUG("in c1m'\n");return true; }
      }


      if(tree_c2) {
        if(!tree_c2->bloom_filter) { DEBUG("no c2 bloom filter\n");  return true; }
        if(stasis_bloom_filter_lookup(tree_c2->bloom_filter,           (const char*)t->strippedkey(), t->strippedkeylen())) { DEBUG("in c2\n");return true; }
      }
      return false;
    }

    template<class ITRA, class ITRN>
    class mergeManyIterator {
    public:
      explicit mergeManyIterator(ITRA* a, ITRN** iters, int num_iters, dataTuple*(*merge)(const dataTuple*,const dataTuple*), int (*cmp)(const dataTuple*,const dataTuple*)) :
        num_iters_(num_iters+1),
        first_iter_(a),
        iters_((ITRN**)malloc(sizeof(*iters_) * num_iters)),          // exactly the number passed in
        current_((dataTuple**)malloc(sizeof(*current_) * (num_iters_))),  // one more than was passed in
        last_iter_(-1),
        cmp_(cmp),
        merge_(merge),
        dups((int*)malloc(sizeof(*dups)*num_iters_))
        {
        current_[0] = first_iter_->next_callerFrees();
        for(int i = 1; i < num_iters_; i++) {
          iters_[i-1] = iters[i-1];
          current_[i] = iters_[i-1] ? iters_[i-1]->next_callerFrees() : NULL;
        }
      }
      ~mergeManyIterator() {
        delete(first_iter_);
        for(int i = 0; i < num_iters_; i++) {
            if(i != last_iter_) {
                if(current_[i]) dataTuple::freetuple(current_[i]);
            }
        }
        for(int i = 1; i < num_iters_; i++) {
          delete iters_[i-1];
        }
        free(current_);
        free(iters_);
        free(dups);
      }
      dataTuple * peek() {
          dataTuple * ret = next_callerFrees();
          last_iter_ = -1; // don't advance iterator on next peek() or getnext() call.
          return ret;
      }
      dataTuple * next_callerFrees() {
        int num_dups = 0;
        if(last_iter_ != -1) {
          // get the value after the one we just returned to the user
          //datatuple::freetuple(current_[last_iter_]); // should never be null
          if(last_iter_ == 0) {
              current_[last_iter_] = first_iter_->next_callerFrees();
          } else if(last_iter_ != -1){
              current_[last_iter_] = iters_[last_iter_-1]->next_callerFrees();
          } else {
              // last call was 'peek'
          }
        }
        // find the first non-empty iterator.  (Don't need to special-case ITRA since we're looking at current.)
        int min = 0;
        while(min < num_iters_ && !current_[min]) {
          min++;
        }
        if(min == num_iters_) { return NULL; }
        // examine current to decide which tuple to return.
        for(int i = min+1; i < num_iters_; i++) {
          if(current_[i]) {
            int res = cmp_(current_[min], current_[i]);
            if(res > 0) { // min > i
              min = i;
              num_dups = 0;
            } else if(res == 0) { // min == i
              dups[num_dups] = i;
              num_dups++;
            }
          }
        }
        dataTuple * ret;
        if(!merge_) {
            ret = current_[min];
        } else {
            // XXX use merge function to build a new ret.
            abort();
        }
        // advance the iterators that match the tuple we're returning.
        for(int i = 0; i < num_dups; i++) {
            dataTuple::freetuple(current_[dups[i]]); // should never be null
            current_[dups[i]] = iters_[dups[i]-1]->next_callerFrees();
        }
        last_iter_ = min; // mark the min iter to be advance at the next invocation of next().  This saves us a copy in the non-merging case.
        return ret;

      }
    private:
      int      num_iters_;
      ITRA  *  first_iter_;
      ITRN  ** iters_;
      dataTuple ** current_;
      int      last_iter_;


      int  (*cmp_)(const dataTuple*,const dataTuple*);
      dataTuple*(*merge_)(const dataTuple*,const dataTuple*);

      // temporary variables initiaized once for effiency
      int * dups;

    };


    class iterator {
  public:
      explicit iterator(bLSM* ltable)
      : ltable(ltable),
        epoch(ltable->get_epoch()),
        merge_it_(NULL),
        last_returned(NULL),
        key(NULL),
        valid(false),
        reval_count(0) {
        rwlc_readlock(ltable->header_mut);
        pthread_mutex_lock(&ltable->rb_mut);
        ltable->registerIterator(this);
        pthread_mutex_unlock(&ltable->rb_mut);
        validate();
        //        rwlc_unlock(ltable->header_mut);
      }

      explicit iterator(bLSM* ltable,dataTuple *key)
      : ltable(ltable),
        epoch(ltable->get_epoch()),
        merge_it_(NULL),
        last_returned(NULL),
        key(key),
        valid(false),
        reval_count(0)
      {
        rwlc_readlock(ltable->header_mut);
        pthread_mutex_lock(&ltable->rb_mut);
        ltable->registerIterator(this);
        pthread_mutex_unlock(&ltable->rb_mut);
        validate();
        //        rwlc_unlock(ltable->header_mut);
      }

      ~iterator() {
        //        rwlc_readlock(ltable->header_mut);
        pthread_mutex_lock(&ltable->rb_mut);
        ltable->forgetIterator(this);
        invalidate();
        pthread_mutex_unlock(&ltable->rb_mut);
        if(last_returned) dataTuple::freetuple(last_returned);
        rwlc_unlock(ltable->header_mut);
      }
  private:
      dataTuple * getnextHelper() {
        //          rwlc_readlock(ltable->header_mut);
          revalidate();
          dataTuple * tmp = merge_it_->next_callerFrees();
          if(last_returned && tmp) {
              int res = dataTuple::compare(last_returned->strippedkey(), last_returned->strippedkeylen(), tmp->strippedkey(), tmp->strippedkeylen());
              if(res >= 0) {
		  int al = last_returned->strippedkeylen();
                  char * a =(char*)malloc(al + 1);
                  memcpy(a, last_returned->strippedkey(), al);
                  a[al] = 0;
                  int bl = tmp->strippedkeylen();
                  char * b =(char*)malloc(bl + 1);
                  memcpy(b, tmp->strippedkey(), bl);
                  b[bl] = 0;
                  printf("blsm.h assert fail: out of order tuples %d should be < 0.  %s <=> %s\n", res, a, b);
                  free(a);
                  free(b);
              }

          }
          if(last_returned) {
              dataTuple::freetuple(last_returned);
          }
          last_returned = tmp;
          //          rwlc_unlock(ltable->header_mut);
          return last_returned;
      }
  public:
      dataTuple * getnextIncludingTombstones() {
          dataTuple * ret = getnextHelper();
          ret = ret ? ret->create_copy() : NULL;
          return ret;
      }

      dataTuple * getnext() {
          dataTuple * ret;
          while((ret = getnextHelper()) && ret->isDelete()) { }  // getNextHelper handles its own memory.
          ret = ret ? ret->create_copy() : NULL; // XXX hate making copy!  Caller should not manage our memory.
          return ret;
      }

      void invalidate() {
//        assert(!trywritelock(ltable->header_lock,0));
        if(valid) {
          delete merge_it_;
          merge_it_ = NULL;
          valid = false;
        }
      }

  private:
      inline void init_helper();

    explicit iterator() { abort(); }
    void operator=(iterator & t) { abort(); }
    int operator-(iterator & t) { abort(); }

  private:
    static const int C1           = 0;
    static const int C1_MERGEABLE = 1;
    static const int C2           = 2;
      bLSM * ltable;
      uint64_t epoch;
      typedef mergeManyIterator<
         memTreeComponent::batchedRevalidatingIterator,
         memTreeComponent::iterator> inner_merge_it_t;
      typedef mergeManyIterator<
        inner_merge_it_t,
        diskTreeComponent::iterator> merge_it_t;

      merge_it_t* merge_it_;

      dataTuple * last_returned;
      dataTuple * key;
      bool valid;
      int reval_count;
      static const int reval_period = 100;
      void revalidate() {
        if(reval_count == reval_period) {
          rwlc_unlock(ltable->header_mut);
          reval_count = 0;
          rwlc_readlock(ltable->header_mut);
        } else {
          reval_count++;
        }
        if(!valid) {
          validate();
        } else {
          assert(epoch == ltable->get_epoch());
        }
      }


      void validate() {
         memTreeComponent::batchedRevalidatingIterator * c0_it;
         memTreeComponent::iterator *c0_mergeable_it[1];
        diskTreeComponent::iterator * disk_it[4];
        epoch = ltable->get_epoch();

        dataTuple *t;
        if(last_returned) {
          t = last_returned;
        } else if(key) {
          t = key;
        } else {
          t = NULL;
        }

        c0_it              = new  memTreeComponent::batchedRevalidatingIterator(ltable->get_tree_c0(), 100, &ltable->rb_mut,  t);
        c0_mergeable_it[0] = new  memTreeComponent::iterator            (ltable->get_tree_c0_mergeable(),                            t);
        if(ltable->get_tree_c1_prime()) {
          disk_it[0] = ltable->get_tree_c1_prime()->open_iterator(t);
        } else {
          disk_it[0] = NULL;
        }
        disk_it[1]         = ltable->get_tree_c1()->open_iterator(t);
        if(ltable->get_tree_c1_mergeable()) {
          disk_it[2]         = ltable->get_tree_c1_mergeable()->open_iterator(t);
        } else {
          disk_it[2] = NULL;
        }
        disk_it[3]         = ltable->get_tree_c2()->open_iterator(t);

        inner_merge_it_t * inner_merge_it =
               new inner_merge_it_t(c0_it, c0_mergeable_it, 1, NULL, dataTuple::compare_obj);
        merge_it_ = new merge_it_t(inner_merge_it, disk_it, 4, NULL, dataTuple::compare_obj); // XXX Hardcodes comparator, and does not handle merges
        if(last_returned) {
          dataTuple * junk = merge_it_->peek();
          if(junk && !dataTuple::compare(junk->strippedkey(), junk->strippedkeylen(), last_returned->strippedkey(), last_returned->strippedkeylen())) {
            // we already returned junk
            dataTuple::freetuple(merge_it_->next_callerFrees());
          }
        }
        valid = true;
      }
  };

};

#endif
