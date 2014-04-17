/*
 * Copyright 2013 University of Chicago and Argonne National Laboratory
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
 * limitations under the License
 */


/*
 * sync.c
 *
 *  Created on: Aug 20, 2012
 *      Author: wozniak
 */

#include <assert.h>

#include <mpi.h>

#include "backoffs.h"
#include "common.h"
#include "debug.h"
#include "messaging.h"
#include "mpe-tools.h"
#include "refcount.h"
#include "server.h"
#include "steal.h"
#include "sync.h"

static inline adlb_code msg_from_target(int target, int response);
static adlb_code msg_from_other_server(int other_server, 
                  int target);
static inline adlb_code msg_shutdown(adlb_sync_mode mode, int sync_target, bool* done);

static adlb_code xlb_handle_subscribe_sync(int rank,
        const struct packed_sync *hdr, bool defer_svr_ops);

static adlb_code enqueue_deferred_notify(int rank,
      const struct packed_sync *hdr);

static adlb_code enqueue_pending(xlb_pending_kind kind, int rank,
                         const struct packed_sync *hdr, void *extra_data);

static inline bool sync_accept_required(adlb_sync_mode mode);

typedef struct {
  int64_t sent;     /** Sent to other servers */
  int64_t accepted; /** Accepted from other servers */
} xlb_sync_type_counter;

static xlb_sync_type_counter xlb_sync_perf_counters[ADLB_SYNC_ENUM_COUNT];
static const char *xlb_sync_type_name[ADLB_SYNC_ENUM_COUNT];

#define xlb_add_sync_type_name(name) \
            xlb_sync_type_name[name] = #name;

/*
  Pending sync requests that we deferred
  Implemented with FIFO ring buffer since we need FIFO in case refcount
  incr is followed by decr: processing in LIFO order could result in
  premature free.
 */
xlb_pending *xlb_pending_syncs = NULL;
int xlb_pending_sync_count = 0;
int xlb_pending_sync_head = 0;
int xlb_pending_sync_size = 0; // Malloced size

adlb_code
xlb_sync_init(void)
{
  xlb_pending_sync_size = PENDING_SYNC_INIT_SIZE;

  // Optionally have different min size - otherwise we won't cover the
  // resizing cases in testing`
  long tmp;
  adlb_code rc = xlb_env_long("ADLB_DEBUG_SYNC_BUFFER_SIZE", &tmp);
  ADLB_CHECK(rc);

  if (rc != ADLB_NOTHING)
  {
    assert(tmp > 0 && tmp <= INT_MAX);
    xlb_pending_sync_size = (int)tmp;
  }

  xlb_pending_sync_count = 0;
  xlb_pending_sync_head = 0;
  xlb_pending_syncs = malloc(sizeof(xlb_pending_syncs[0]) *
                                (size_t)xlb_pending_sync_size);
  CHECK_MSG(xlb_pending_syncs != NULL, "could not allocate memory");

  if (xlb_perf_counters_enabled)
  {
    for (int i = 0; i < ADLB_SYNC_ENUM_COUNT; i++)
    {
      xlb_sync_perf_counters[i].sent = 0;
      xlb_sync_perf_counters[i].accepted = 0;
    }
    
    // Register human-readable names
    xlb_add_sync_type_name(ADLB_SYNC_REQUEST);
    xlb_add_sync_type_name(ADLB_SYNC_STEAL);
    xlb_add_sync_type_name(ADLB_SYNC_REFCOUNT);
    xlb_add_sync_type_name(ADLB_SYNC_SUBSCRIBE);
    xlb_add_sync_type_name(ADLB_SYNC_NOTIFY);
  }
  return ADLB_SUCCESS;
}

void xlb_sync_finalize(void)
{
  free(xlb_pending_syncs);
  xlb_pending_sync_count = 0;
  xlb_pending_sync_size = 0;
}

void xlb_print_sync_counters(void)
{
  if (!xlb_perf_counters_enabled)
  {
    return;
  }

  for (int i = 0; i < ADLB_SYNC_ENUM_COUNT; i++)
  {
    PRINT_COUNTER("SYNC_SENT_%s=%"PRId64"\n", xlb_sync_type_name[i],
                  xlb_sync_perf_counters[i].sent);
    PRINT_COUNTER("SYNC_ACCEPTED_%s=%"PRId64"\n", xlb_sync_type_name[i],
                  xlb_sync_perf_counters[i].accepted);
  }
}


adlb_code
xlb_sync(int target)
{
  char hdr_storage[PACKED_SYNC_SIZE];
  struct packed_sync *hdr = (struct packed_sync *)hdr_storage;
#ifndef NDEBUG
  // Avoid send uninitialized bytes for memory checking tools
  memset(hdr, 0, PACKED_SYNC_SIZE);
#endif
  hdr->mode = ADLB_SYNC_REQUEST;
  return xlb_sync2(target, hdr, NULL);
}

/*
   While attempting a sync, one of three things may happen:
   1) The target responds.  It either accepts or rejects the sync
      request.  If it rejects, this process retries
   2) Another server interrupts this process with a sync request.
      This process either accepts and serves the request; stores the
      request in xlb_pending_syncs to process later, or rejects it
   3) The master server tells this process to shut down
   These numbers correspond to the variables in the function
 */
adlb_code
xlb_sync2(int target, const struct packed_sync *hdr, int *response)
{
  TRACE_START;
  DEBUG("\t xlb_sync() target: %i", target);
  int rc = ADLB_SUCCESS;

  MPE_LOG(xlb_mpe_dmn_sync_start);

  // Track sent sync message and response
  MPI_Request isend_request, accept_request;
  // Response from target if needed 
  int accept_response;
  bool accept_required = sync_accept_required(hdr->mode);

  MPI_Status status;
  /// MPI_Request request1, request2;
  int flag1 = 0, flag2 = 0, flag3 = 0;

  assert(!xlb_server_sync_in_progress);
  xlb_server_sync_in_progress = true;

  // Flag completion of sync, either successfully or aborting if
  // shutting down
  bool done = false;
  // If one of the requests is still pending
  bool requests_pending = false;

  if (!xlb_server_shutting_down)
  {

    if (xlb_perf_counters_enabled)
    {
      assert(hdr->mode >= 0 && hdr->mode < ADLB_SYNC_ENUM_COUNT);
      xlb_sync_perf_counters[hdr->mode].sent++;
    }

    if (accept_required)
    {
      IRECV2(&accept_response, 1, MPI_INT, target, ADLB_TAG_SYNC_RESPONSE,
            &accept_request);
    }

    /*
     * Send initial request.
     *
     * Use non-blocking send to eliminate chance of blocking here if
     * receiver's buffers are full, so that we can serve other sync
     * requests no matter what.
     */
    ISEND(hdr, (int)PACKED_SYNC_SIZE, MPI_BYTE, target,
          ADLB_TAG_SYNC_REQUEST, &isend_request);
    requests_pending = true;
    
    DEBUG("server_sync: [%d] waiting for sync response from %d",
                          xlb_comm_rank, target);
  }
  else
  {
    // Check that we're not due to shut down because of a previously
    // received shutdown message before going into sync loop
    done = true;
    rc = ADLB_SHUTDOWN;
    DEBUG("server_sync: [%d] shutting down before sync to %d",
                          xlb_comm_rank, target);
  }

  /*
   * Must loop until Isend completes at a minimum.
   * We don't just block on it because we want to service any incoming
   * requests.
   * If we need an accept response, must also loop until we receive
   * it from the target.
   */
  while (!done)
  {
    TRACE("xlb_sync: loop");
    
    if (accept_required)
    {
      // Check for response from target
      MPI_TEST(&accept_request, &flag1);
      
      if (flag1)
      {
        rc = msg_from_target(target, accept_response);
        ADLB_CHECK(rc);
        
        if (response != NULL)
          *response = accept_response;
        requests_pending = false; // ISend must have completed too
        done = true;
        break;
      }
    }
    else
    {
      // just check that send went through
      MPI_TEST(&isend_request, &flag1);
      
      if (flag1)
      {
        requests_pending = false;
        done = true;
        break;
      }
    }

    IPROBE(MPI_ANY_SOURCE, ADLB_TAG_SYNC_REQUEST, &flag2, &status);
    if (flag2)
    {
      rc = msg_from_other_server(status.MPI_SOURCE, target);
      ADLB_CHECK(rc);
    }

    // TODO: this isn't necessary if master server syncs before shutdown
    IPROBE(MPI_ANY_SOURCE, ADLB_TAG_SHUTDOWN_SERVER, &flag3,
           &status);
    if (flag3)
    {
      rc = msg_shutdown(hdr->mode, target, &done);
      ADLB_CHECK(rc);
      rc = ADLB_SHUTDOWN;
    }

    if (!flag1 && !flag2 && !flag3)
    {
      // TODO: generally we don't want to wait longer than needed for
      //  a response, and we should get a response pretty quickly.
      // Maybe just go to no backoffs...
      // xlb_backoff_sync();
    }
  }

  if (requests_pending)
  {
    CANCEL(&isend_request);
    if (accept_required)
    {
      CANCEL(&accept_request);
    }
  }

  DEBUG("server_sync: [%d] sync with %d successful", xlb_comm_rank, target);
  xlb_server_sync_in_progress = false;
  TRACE_END;
  MPE_LOG(xlb_mpe_dmn_sync_end);

  return rc;
}

static adlb_code
send_subscribe_sync(adlb_sync_mode mode,
      int target, adlb_datum_id id, adlb_subscript sub)
{
  char req_storage[PACKED_SYNC_SIZE]; // Temporary stack storage for struct
  struct packed_sync *req = (struct packed_sync *)req_storage;
  req->mode = mode;
  req->subscribe.id = id;
  req->subscribe.subscript_len = (int)sub.length;

  bool inlined_subscript; 
  if (sub.length <= SYNC_DATA_SIZE)
  {
    if (sub.length > 0)
    {
      memcpy(req->sync_data, sub.key, sub.length);
    }
    inlined_subscript = true;
  }
  else
  {
    inlined_subscript = false;
  }

  // Send sync message without waiting for response
  int rc = xlb_sync2(target, req, NULL);
  ADLB_CHECK(rc);

  if (!inlined_subscript)
  {
    // send subscript separately with special tag
    // TODO: could block here, although large subscripts
    // are generally quite rare.
    SEND(sub.key, (int)sub.length, MPI_BYTE, target, ADLB_TAG_SYNC_SUB);
  }
  
  return ADLB_SUCCESS;
}

adlb_code
xlb_sync_subscribe(int target, adlb_datum_id id, adlb_subscript sub,
                   bool *subscribed)
{
  adlb_code ac = send_subscribe_sync(ADLB_SYNC_SUBSCRIBE, target, id, sub);
  ADLB_CHECK(ac);

  // We will get notification later
  *subscribed = true;
  return ADLB_SUCCESS;
}

adlb_code
xlb_sync_notify(int target, adlb_datum_id id, adlb_subscript sub)
{
  // Send notification
  adlb_code ac = send_subscribe_sync(ADLB_SYNC_NOTIFY, target, id, sub);
  ADLB_CHECK(ac);

  return ADLB_SUCCESS;
}

/**
   @return adlb_code
 */
static inline adlb_code
msg_from_target(int target, int response)
{
  TRACE_START;
  CHECK_MSG(response, "Unexpected sync response: %i", response);
  // Accepted
  DEBUG("server_sync: [%d] sync accepted by %d.", xlb_comm_rank, target);
  TRACE_END
  return ADLB_SUCCESS;
}

/*
 * Don't send accept: just return immediately
 */
static inline bool sync_accept_required(adlb_sync_mode mode)
{
  if (mode == ADLB_SYNC_SUBSCRIBE ||
      mode == ADLB_SYNC_NOTIFY ||
      mode == ADLB_SYNC_REFCOUNT)
  {
    return false;
  }
  else
  {
    return true;
  }
}

static adlb_code msg_from_other_server(int other_server, int target)
{
  TRACE_START;
  MPI_Status status;
  adlb_code code;

  // Store on stack - skip malloc
  char hdr_storage[PACKED_SYNC_SIZE];
  struct packed_sync *other_hdr = (struct packed_sync *)hdr_storage;

  RECV(other_hdr, (int)PACKED_SYNC_SIZE, MPI_BYTE, other_server, ADLB_TAG_SYNC_REQUEST);

  /* Serve another server
   * We need to avoid the case of circular deadlock, e.g. where A is waiting
   * to serve B, which is waiting to serve C, which is waiting to serve A, 
   * so don't serve higher ranked servers until we've finished our
   * sync request. We choose this ordering because the master server is
   * somewhat more likely to be busy and should be unblocked. */
  if (other_server < xlb_comm_rank)
  {
    // accept incoming sync
    DEBUG("server_sync: [%d] interrupted by incoming sync request from %d",
                        xlb_comm_rank, other_server);
    
    code = xlb_accept_sync(other_server, other_hdr, true);
    ADLB_CHECK(code);
  }
  else if (other_hdr->mode == ADLB_SYNC_STEAL)
  {
    // TODO: clean up code
    // Prefer rejecting steal immediately to accepting it later:
    // If we're busy syncing here, there's a good chance we're out of
    // work too
    const int rejected_response = 0;
    // This shouldn't block, since sender should have posted buffer
    SEND(&rejected_response, 1, MPI_INT, other_server, ADLB_TAG_SYNC_RESPONSE);
  }
  else
  {
    // Don't handle right away, defer it
    code = enqueue_pending(DEFERRED_SYNC, other_server, other_hdr, NULL);
    ADLB_CHECK(code);
  }
  TRACE_END;
  return ADLB_SUCCESS;
}

/*
  One we are ready to accept sync, do whatever processing needed to service
  hdr: header data.  Must copy to take ownership
  defer_svr_ops: true if we should defer any potential server->server ops
 */
adlb_code xlb_accept_sync(int rank, const struct packed_sync *hdr,
                          bool defer_svr_ops)
{
  adlb_sync_mode mode = hdr->mode;
  adlb_code code = ADLB_ERROR;
  
  if (xlb_perf_counters_enabled)
  {
    assert(mode >= 0 && mode < ADLB_SYNC_ENUM_COUNT);
    xlb_sync_perf_counters[mode].accepted++;
  }

  if (sync_accept_required(mode))
  {
    // Notify the waiting caller
    const int accepted_response = 1;
    // This shouldn't block, since sender should have posted buffer
    SEND(&accepted_response, 1, MPI_INT, rank, ADLB_TAG_SYNC_RESPONSE);
  }

  if (mode == ADLB_SYNC_REQUEST)
  {
    code = xlb_serve_server(rank);
  }
  else if (mode == ADLB_SYNC_STEAL)
  {
    // Respond to steal
    code = xlb_handle_steal(rank, &hdr->steal, (int*)hdr->sync_data);
  }
  else if (mode == ADLB_SYNC_REFCOUNT)
  {
    /*
      We defer handling of server->server refcounts to avoid potential
      deadlocks if the refcount decrement triggers a cycle of reference
      count decrements between servers and a deadlock.  Deferring
      processing also has the benefit of giving the fastest possible
      response to the other servers.  One downside is that we can't pass
      errors all the way back to the caller - we will simply report them
      and continue.

      Rules about safety of deferring refcounts:
       -> refcount increments - need to apply increment before processing
            any operation that could decrement refcount
       -> read refcount decrements - safe to defer indefinitely,
            but delays freeing memory
       -> write refcount decrements - safe to defer indefinitely, 
            but will delay notifications
     */

    if (defer_svr_ops)
    {
      DEBUG("Defer refcount for <%"PRId64">", hdr->incr.id);
      code = enqueue_pending(ACCEPTED_RC, rank, hdr, NULL);
      ADLB_CHECK(code);
    }
    else
    {
      DEBUG("Update refcount now for <%"PRId64">", hdr->incr.id);
      adlb_data_code dc = xlb_incr_rc_local(hdr->incr.id,
                                  hdr->incr.change, true);
      CHECK_MSG(dc == ADLB_DATA_SUCCESS, "Unexpected error in refcount");
    }
    // Then we're done - already sent sync response to caller
    return ADLB_SUCCESS;
  }
  else if (mode == ADLB_SYNC_SUBSCRIBE)
  {
    code = xlb_handle_subscribe_sync(rank, hdr, defer_svr_ops);
  }
  else if (mode == ADLB_SYNC_NOTIFY)
  {
    if (defer_svr_ops)
    {
      DEBUG("Defer notification for <%"PRId64">", hdr->subscribe.id);
      code = enqueue_deferred_notify(rank, hdr);
    }
    else
    {
      DEBUG("Handle notification now for <%"PRId64">", hdr->subscribe.id);
      code = xlb_handle_notify_sync(rank, &hdr->subscribe, hdr->sync_data,
                                    NULL);
    }
  }
  else
  {
    printf("Invalid sync mode: %d\n", mode);
    return ADLB_ERROR;
  }
  return code;
}

static adlb_code xlb_handle_subscribe_sync(int rank,
        const struct packed_sync *hdr, bool defer_svr_ops)
{
  adlb_data_code dc;
  adlb_code ac;

  MPI_Status status;

  const struct packed_subscribe_sync *sub_hdr = &hdr->subscribe;
  const void *sync_data = hdr->sync_data;

  void *malloced_subscript = NULL;
  adlb_subscript sub;
  sub.length = (size_t)sub_hdr->subscript_len;

  if (sub_hdr->subscript_len == 0)
  {
    sub.key = NULL; 
  }
  else if (sub_hdr->subscript_len <= SYNC_DATA_SIZE)
  {
    // subscript small enough to store inline
    sub.key = sync_data;
  }
  else
  {
    assert(sub_hdr->subscript_len <= ADLB_DATA_SUBSCRIPT_MAX);
    malloced_subscript = malloc((size_t)sub_hdr->subscript_len);
    ADLB_MALLOC_CHECK(malloced_subscript);
    
    // receive subscript as separate message with special tag
    RECV(malloced_subscript, sub_hdr->subscript_len, MPI_BYTE,
         rank, ADLB_TAG_SYNC_SUB);
    sub.key = malloced_subscript;
  }

  // call data module to subscribe
  bool subscribed;
  dc = xlb_data_subscribe(sub_hdr->id, sub, rank, &subscribed);
  ADLB_DATA_CHECK(dc);

  if (!subscribed)
  {
    // Is ready, need to get notification back to caller
    if (defer_svr_ops)
    {
      // Enqueue it for later sending
      ac = enqueue_pending(UNSENT_NOTIFY, rank, hdr,
                             malloced_subscript);
      ADLB_CHECK(ac);
      malloced_subscript = NULL; // Gave ownership to pending list
    } else {
      // Notify right away.
      // We avoid deadlock since caller doesn't wait for accept
      ac = xlb_sync_notify(rank, sub_hdr->id, sub);
      ADLB_CHECK(ac);
    }
  }

  if (malloced_subscript != NULL)
  {
    free(malloced_subscript);
  }
  return ADLB_SUCCESS;
}

/*
 * req_hdr: header from the subscribe request
 * malloc_subscript: memory for longer subscripts
 */
adlb_code xlb_send_unsent_notify(int rank,
        const struct packed_sync *req_hdr, void *malloced_subscript)
{
  adlb_subscript sub;
  sub.length = (size_t)req_hdr->subscribe.subscript_len;

  if (sub.length == 0)
  {
    sub.key = NULL; 
  }
  else if (sub.length <= SYNC_DATA_SIZE)
  {
    // subscript was stored inline
    sub.key = req_hdr->sync_data;
  }
  else
  {
    assert(sub.length <= ADLB_DATA_SUBSCRIPT_MAX);
    assert(malloced_subscript != NULL);
    sub.key = malloced_subscript;
  }

  adlb_code ac = xlb_sync_notify(rank, req_hdr->subscribe.id, sub);
  ADLB_CHECK(ac);

  if (malloced_subscript != NULL)
  {
    free(malloced_subscript);
  }

  return ADLB_SUCCESS;
}

/*
 * Enqueue a notification for later processing.
 * This will receive any additional messages sent by the caller.
 */
static adlb_code enqueue_deferred_notify(int rank,
      const struct packed_sync *hdr)
{
  MPI_Status status;

  void *malloced_subscript = NULL;
  int sub_length = hdr->subscribe.subscript_len;

  // Get subscript now to avoid having unreceived message sitting around
  if (sub_length > SYNC_DATA_SIZE)
  {
    assert(sub_length <= ADLB_DATA_SUBSCRIPT_MAX);
    malloced_subscript = malloc((size_t)sub_length);
    ADLB_MALLOC_CHECK(malloced_subscript);
    
    // receive subscript as separate message with special tag
    RECV(malloced_subscript, sub_length, MPI_BYTE,
         rank, ADLB_TAG_SYNC_SUB);
  }

  adlb_code rc = enqueue_pending(DEFERRED_NOTIFY, rank, hdr,
                                  malloced_subscript);
  ADLB_CHECK(rc);

  return ADLB_SUCCESS;
}

/*
 * Handle a notification, either a deferred one or one with a waiting
 * caller.
 * extra_data: if not NULL and extra data is needed, assume we should
 *            receive it from caller
 */
adlb_code xlb_handle_notify_sync(int rank,
        const struct packed_subscribe_sync *hdr, const void *sync_data,
        void *extra_data)
{
  MPI_Status status;

  void *malloced_subscript = NULL;
  adlb_subscript sub;
  sub.length = (size_t)hdr->subscript_len;

  if (hdr->subscript_len == 0)
  {
    sub.key = NULL; 
  }
  else if (hdr->subscript_len <= SYNC_DATA_SIZE)
  {
    // subscript small enough to store inline
    sub.key = sync_data;
  }
  else if (extra_data != NULL)
  {
    sub.key = malloced_subscript = extra_data;
  }
  else
  {
    assert(hdr->subscript_len <= ADLB_DATA_SUBSCRIPT_MAX);
    malloced_subscript = malloc((size_t)hdr->subscript_len);
    ADLB_MALLOC_CHECK(malloced_subscript);
    
    // receive subscript as separate message with special tag
    RECV(malloced_subscript, hdr->subscript_len, MPI_BYTE,
         rank, ADLB_TAG_SYNC_SUB);
    sub.key = malloced_subscript;
  }

  turbine_engine_code tc;
  // process notification
  if (adlb_has_sub(sub))
  {
    tc = turbine_sub_close(hdr->id, sub, &xlb_server_ready_work);
    ADLB_TURBINE_CHECK(tc);
  }
  else
  {
    tc = turbine_close(hdr->id, &xlb_server_ready_work);
    ADLB_TURBINE_CHECK(tc);
  }

  if (malloced_subscript != NULL)
  {
    free(malloced_subscript);
  }
  return ADLB_SUCCESS;

}

/*
  Add pending sync
  hdr: sync header.  This function will make a copy of it
  returns: ADLB_SUCCESS, or ADLB_ERROR on unexpected error
 */
static adlb_code enqueue_pending(xlb_pending_kind kind, int rank,
                     const struct packed_sync *hdr, void *extra_data)
{
  assert(xlb_pending_sync_count <= xlb_pending_sync_size);
  if (xlb_pending_sync_count == xlb_pending_sync_size)
  {
    xlb_pending_sync_size *= 2;
    DEBUG("Resizing to accommodate %i pending", xlb_pending_sync_size);
    xlb_pending_syncs = realloc(xlb_pending_syncs,
                      sizeof(xlb_pending_syncs[0]) * (size_t)xlb_pending_sync_size);
    CHECK_MSG(xlb_pending_syncs != NULL, "could not allocate memory");
    /* Entries are in: [head..count) ++ [0..head)
     * Copy [0..head) to [count..count+head) to account for new size
     * End result is all entries in [head..head+count] */
    if (xlb_pending_sync_head != 0)
    {
      memcpy(&xlb_pending_syncs[xlb_pending_sync_count], xlb_pending_syncs,
            sizeof(xlb_pending_syncs[0]) * (size_t)xlb_pending_sync_head);
    }
  }
 
  int tail = (xlb_pending_sync_head + xlb_pending_sync_count)
             % xlb_pending_sync_size;
  xlb_pending *entry = &xlb_pending_syncs[tail];
  entry->kind = kind;
  entry->rank = rank;
  entry->extra_data = extra_data;
  entry->hdr = malloc(PACKED_SYNC_SIZE);
  CHECK_MSG(entry->hdr != NULL, "could not allocate memory");
  memcpy(entry->hdr, hdr, PACKED_SYNC_SIZE);
  xlb_pending_sync_count++;
  return ADLB_SUCCESS;
}


// Shrink to half of previous size
adlb_code xlb_pending_shrink(void)
{
  // Short names for readability
  const int new_size = xlb_pending_sync_size / 2;
  const int old_size = xlb_pending_sync_size;
  const int count = xlb_pending_sync_count;
  const int head = xlb_pending_sync_head;
  assert(head <= old_size);
  assert(count <= new_size);
  /* 
    Need to pack into smaller new array
    Entries are in [head..head+count).
  */
  if (head + count > new_size)
  {
    if (head + count > old_size)
    {
      /*
        If wrapped around, we have [head..old_size) ++ [0..nwrapped)

        Move [0..nwrapped) to [count-nwrapped..count) and
             [head..old_size) to [0..count-nwrapped)
        Destination will be unused bc. nwrapped <= head bc. in this case
        we're using less than half of the array.
      */
      int nwrapped = (head + count) % old_size;
      // Use memmove since might overlap
      memmove(&xlb_pending_syncs[count-nwrapped], xlb_pending_syncs,
              sizeof(xlb_pending_syncs[0]) * (size_t)nwrapped);
      memcpy(xlb_pending_syncs, &xlb_pending_syncs[head],
              sizeof(xlb_pending_syncs[0]) * (size_t)(count - nwrapped));
      xlb_pending_sync_head = 0;
    }
    else
    {
      // No wrapping, but past end of new array.
      // Just move to front with memmove since might overlap
      memmove(xlb_pending_syncs, &xlb_pending_syncs[head],
              sizeof(xlb_pending_syncs[0]) * (size_t)count);
      xlb_pending_sync_head = 0;
    }
  }

  xlb_pending_sync_size = new_size;
  xlb_pending_syncs = realloc(xlb_pending_syncs,
    sizeof(xlb_pending_syncs[0]) * (size_t)xlb_pending_sync_size);

  // realloc shouldn't really fail when shrinking
  assert(xlb_pending_syncs != NULL);

  return ADLB_SUCCESS;
}

static inline adlb_code
msg_shutdown(adlb_sync_mode mode, int sync_target, bool* done)
{
  TRACE_START;
  DEBUG("server_sync: [%d] cancelled by shutdown!", xlb_comm_rank);

  if (mode == ADLB_SYNC_REQUEST)
  {
    /* We're not going to follow up the sync request with an actual
     * request.  To avoid the target getting stuck waiting for work,
     * We send them a dummy piece of work. */
    SEND_TAG(sync_target, ADLB_TAG_DO_NOTHING);
  }
  else if (mode == ADLB_SYNC_STEAL)
  {
    // Don't do anything, target doesn't expect response from this rank.
    // There also won't be any work in system given we're shutting down
  }
  else if (mode == ADLB_SYNC_REFCOUNT)
  {
    // Don't do anything, target doesn't expect response from this rank.
  }
  else
  {
    ERR_PRINTF("Unexpected sync mode %i\n", (int)mode);
    return ADLB_ERROR;
  }

  *done = true;
  TRACE_END;
  return ADLB_SUCCESS;
}

