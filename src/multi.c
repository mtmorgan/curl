#include "curl-common.h"
#include <time.h>

#if LIBCURL_VERSION_MAJOR > 7 || (LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 28)
#define HAS_MULTI_WAIT 1
#endif

#if LIBCURL_VERSION_MAJOR > 7 || (LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 30)
#define HAS_CURLMOPT_MAX_TOTAL_CONNECTIONS 1
#endif

CURLM *global_multi = NULL;

/* Important:
 * The ref->busy field means the handle is used by global_multi system
 * We need this because there is no way to query the multi handle for pending handles
 * The ref->locked is used to lock the handle for any use.
 */

SEXP R_multi_cancel(SEXP handle_ptr){
  reference *ref = get_ref(handle_ptr);
  if(ref->multi.busy){
    massert(curl_multi_remove_handle(global_multi, ref->handle));
    R_ReleaseObject(ref->multi.complete);
    R_ReleaseObject(ref->multi.error);
    reset_multi(ref);
    ref->locked = 0;
    ref->refCount--;
    clean_handle(ref);
  }
  return handle_ptr;
}

SEXP R_multi_add(SEXP handle_ptr, SEXP cb_complete, SEXP cb_error){
  reference *ref = get_ref(handle_ptr);
  if(ref->locked)
    Rf_error("Handle is locked. Probably in use in a connection or async request.");

  /* placeholder body */
  curl_easy_setopt(ref->handle, CURLOPT_WRITEFUNCTION, append_buffer);
  curl_easy_setopt(ref->handle, CURLOPT_WRITEDATA, &(ref->multi.content));

  /* add to scheduler */
  massert(curl_multi_add_handle(global_multi, ref->handle));

  /* set multi callbacks */
  ref->multi.busy = 1;
  R_PreserveObject(ref->multi.complete = cb_complete);
  R_PreserveObject(ref->multi.error = cb_error);

  /* lock and protect handle */
  ref->refCount++;
  ref->locked = 1;
  return handle_ptr;
}

SEXP R_multi_run(SEXP timeout, SEXP total_con, SEXP host_con, SEXP multiplex){

  #ifdef CURLPIPE_MULTIPLEX
    if(asLogical(multiplex))
      massert(curl_multi_setopt(global_multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX));
  #endif

  #ifdef HAS_CURLMOPT_MAX_TOTAL_CONNECTIONS
    massert(curl_multi_setopt(global_multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long) asInteger(total_con)));
    massert(curl_multi_setopt(global_multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long) asInteger(host_con)));
  #endif

  int total_pending = 0;
  int total_success = 0;
  int total_fail = 0;
  int dirty = 0;
  double time_max = asReal(timeout);

  clock_t time_start = clock();
  double seconds_elapsed = 0;
  do {
    dirty = 0;
    if(pending_interrupt())
      break;
    /* Required by old versions of libcurl */
    CURLMcode res = CURLM_CALL_MULTI_PERFORM;
    while(res == CURLM_CALL_MULTI_PERFORM)
      res = curl_multi_perform(global_multi, &(total_pending));

    /* check for multi errors */
    if(res != CURLM_OK)
      break;

    /* check for completed requests */
    int msgq = 0;
    do {
      CURLMsg *m = curl_multi_info_read(global_multi, &msgq);
      if(m && (m->msg == CURLMSG_DONE)){
        dirty = 1;
        reference *ref = NULL;
        CURL *handle = m->easy_handle;
        CURLcode status = m->data.result;
        assert(curl_easy_getinfo(handle, CURLINFO_PRIVATE, (char**) &ref));

        // release the handle so that it can be reused in callback
        massert(curl_multi_remove_handle(global_multi, handle));
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, NULL);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, NULL);

        // execute the success or error callback
        SEXP cb_complete = PROTECT(ref->multi.complete);
        SEXP cb_error = PROTECT(ref->multi.error);
        R_ReleaseObject(ref->multi.complete);
        R_ReleaseObject(ref->multi.error);

        //copy buffer before releasing handle
        SEXP buf = PROTECT(allocVector(RAWSXP, ref->multi.content.size));
        if(ref->multi.content.buf && ref->multi.content.size)
          memcpy(RAW(buf), ref->multi.content.buf, ref->multi.content.size);

        //release handle for use by callbacks
        ref->locked = 0;
        reset_multi(ref);

        // callbacks must be trycatch! we should continue the loop
        if(status == CURLE_OK){
          total_success++;
          if(Rf_isFunction(cb_complete)){
            int ok;
            SEXP out = PROTECT(make_handle_response(ref));
            SET_VECTOR_ELT(out, 5, buf);
            SEXP call = PROTECT(LCONS(cb_complete, LCONS(out, R_NilValue)));
            R_tryEval(call, R_GlobalEnv, &ok);
            UNPROTECT(2);
          }
        } else {
          total_fail++;
          if(Rf_isFunction(cb_error)){
            int ok;
            SEXP buf = PROTECT(mkString(curl_easy_strerror(status)));
            SEXP call = PROTECT(LCONS(cb_error, LCONS(buf, R_NilValue)));
            R_tryEval(call, R_GlobalEnv, &ok);
            UNPROTECT(2);
          }
        }

        // watch out: ref/handle might be modified by callback functions!
        UNPROTECT(3);
        ref->refCount--;
        clean_handle(ref);
      }
    } while (msgq > 0);

    /* check for timeout */
    if(time_max > 0){
      seconds_elapsed = (double) (clock() - time_start) / CLOCKS_PER_SEC;
      if(seconds_elapsed > time_max)
        break;
    }
  } while((dirty || total_pending) && time_max);

  SEXP res = PROTECT(allocVector(VECSXP, 3));
  SET_VECTOR_ELT(res, 0, ScalarInteger(total_success));
  SET_VECTOR_ELT(res, 1, ScalarInteger(total_fail));
  SET_VECTOR_ELT(res, 2, ScalarInteger(total_pending));

  SEXP names = PROTECT(allocVector(STRSXP, 3));
  SET_STRING_ELT(names, 0, mkChar("success"));
  SET_STRING_ELT(names, 1, mkChar("error"));
  SET_STRING_ELT(names, 2, mkChar("pending"));
  setAttrib(res, R_NamesSymbol, names);
  UNPROTECT(2);
  return res;
}
