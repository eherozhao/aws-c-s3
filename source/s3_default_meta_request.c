#include "aws/s3/private/s3_default_meta_request.h"
#include "aws/s3/private/s3_client_impl.h"
#include "aws/s3/private/s3_meta_request_impl.h"
#include "aws/s3/private/s3_request_messages.h"
#include "aws/s3/private/s3_util.h"
#include <aws/common/string.h>
#include <inttypes.h>

/* Data for aws_s3_meta_request_default's vtable->prepare_request() job */
struct aws_s3_default_prepare_request_job {
    struct aws_allocator *allocator;
    struct aws_s3_request *request;
    /* async step: read request body */
    struct aws_future_bool *step1_read_body;
    /* future to set when this whole job completes */
    struct aws_future_void *on_complete;
};

static void s_s3_meta_request_default_destroy(struct aws_s3_meta_request *meta_request);

static bool s_s3_meta_request_default_update(
    struct aws_s3_meta_request *meta_request,
    uint32_t flags,
    struct aws_s3_request **out_request);

static struct aws_future_void *s_s3_default_prepare_request(struct aws_s3_request *request);

static void s_s3_default_prepare_request_on_read_done(void *user_data);

static void s_s3_default_prepare_request_finish(
    struct aws_s3_default_prepare_request_job *request_prep,
    int error_code);

static void s_s3_meta_request_default_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code);

static struct aws_s3_meta_request_vtable s_s3_meta_request_default_vtable = {
    .update = s_s3_meta_request_default_update,
    .send_request_finish = aws_s3_meta_request_send_request_finish_handle_async_error,
    .prepare_request = s_s3_default_prepare_request,
    .init_signing_date_time = aws_s3_meta_request_init_signing_date_time_default,
    .sign_request = aws_s3_meta_request_sign_request_default,
    .finished_request = s_s3_meta_request_default_request_finished,
    .destroy = s_s3_meta_request_default_destroy,
    .finish = aws_s3_meta_request_finish_default,
};

/* Allocate a new default meta request. */
struct aws_s3_meta_request *aws_s3_meta_request_default_new(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    uint64_t content_length,
    bool should_compute_content_md5,
    const struct aws_s3_meta_request_options *options) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(options);
    AWS_PRECONDITION(options->message);

    struct aws_byte_cursor request_method;
    if (aws_http_message_get_request_method(options->message, &request_method)) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create Default Meta Request; could not get request method from message.");

        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (content_length > SIZE_MAX) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create Default Meta Request; content length of %" PRIu64 " bytes is too large for platform.",
            content_length);

        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_s3_meta_request_default *meta_request_default =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_meta_request_default));

    /* Try to initialize the base type. */
    if (aws_s3_meta_request_init_base(
            allocator,
            client,
            0,
            should_compute_content_md5,
            options,
            meta_request_default,
            &s_s3_meta_request_default_vtable,
            &meta_request_default->base)) {

        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p Could not initialize base type for Default Meta Request.",
            (void *)meta_request_default);

        aws_mem_release(allocator, meta_request_default);
        return NULL;
    }

    meta_request_default->content_length = (size_t)content_length;

    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Created new Default Meta Request.", (void *)meta_request_default);

    return &meta_request_default->base;
}

static void s_s3_meta_request_default_destroy(struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);

    struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
    aws_mem_release(meta_request->allocator, meta_request_default);
}

/* Try to get the next request that should be processed. */
static bool s_s3_meta_request_default_update(
    struct aws_s3_meta_request *meta_request,
    uint32_t flags,
    struct aws_s3_request **out_request) {
    (void)flags;

    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(out_request);

    struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
    struct aws_s3_request *request = NULL;
    bool work_remaining = false;

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_meta_request_lock_synced_data(meta_request);

        if (!aws_s3_meta_request_has_finish_result_synced(meta_request)) {

            /* If the request hasn't been sent, then create and send it now. */
            if (!meta_request_default->synced_data.request_sent) {
                if (out_request == NULL) {
                    goto has_work_remaining;
                }

                request = aws_s3_request_new(meta_request, 0, 1, AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);

                AWS_LOGF_DEBUG(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p: Meta Request Default created request %p",
                    (void *)meta_request,
                    (void *)request);

                meta_request_default->synced_data.request_sent = true;
                goto has_work_remaining;
            }

            /* If the request hasn't been completed, then wait for it to be completed. */
            if (!meta_request_default->synced_data.request_completed) {
                goto has_work_remaining;
            }

            /* If delivery hasn't been attempted yet for the response body, wait for that to happen. */
            if (meta_request->synced_data.num_parts_delivery_completed < 1) {
                goto has_work_remaining;
            }

            goto no_work_remaining;

        } else {

            /* If we are canceling, and the request hasn't been sent yet, then there is nothing to wait for. */
            if (!meta_request_default->synced_data.request_sent) {
                goto no_work_remaining;
            }

            /* If the request hasn't been completed yet, then wait for that to happen. */
            if (!meta_request_default->synced_data.request_completed) {
                goto has_work_remaining;
            }

            /* If some parts are still being delivered to the caller, then wait for those to finish. */
            if (meta_request->synced_data.num_parts_delivery_completed <
                meta_request->synced_data.num_parts_delivery_sent) {
                goto has_work_remaining;
            }

            goto no_work_remaining;
        }

    has_work_remaining:
        work_remaining = true;

    no_work_remaining:

        if (!work_remaining) {
            aws_s3_meta_request_set_success_synced(
                meta_request, meta_request_default->synced_data.cached_response_status);
        }

        aws_s3_meta_request_unlock_synced_data(meta_request);
    }
    /* END CRITICAL SECTION */

    if (work_remaining) {
        if (request != NULL) {
            AWS_ASSERT(out_request != NULL);
            *out_request = request;
        }
    } else {
        AWS_ASSERT(request == NULL);

        aws_s3_meta_request_finish(meta_request);
    }

    return work_remaining;
}

/* Given a request, prepare it for sending based on its description. */
static struct aws_future_void *s_s3_default_prepare_request(struct aws_s3_request *request) {
    AWS_PRECONDITION(request);

    struct aws_s3_meta_request *meta_request = request->meta_request;
    AWS_PRECONDITION(meta_request);

    struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
    AWS_PRECONDITION(meta_request_default);

    struct aws_future_void *asyncstep_prepare_request = aws_future_void_new(request->allocator);

    /* Store data for async job */
    struct aws_s3_default_prepare_request_job *request_prep =
        aws_mem_calloc(request->allocator, 1, sizeof(struct aws_s3_default_prepare_request_job));
    request_prep->allocator = request->allocator;
    request_prep->request = request;
    request_prep->on_complete = aws_future_void_acquire(asyncstep_prepare_request);

    if (meta_request_default->content_length > 0 && request->num_times_prepared == 0) {
        aws_byte_buf_init(&request->request_body, meta_request->allocator, meta_request_default->content_length);

        /* Kick off the async read */
        request_prep->step1_read_body = aws_s3_meta_request_read_body(meta_request, &request->request_body);
        aws_future_bool_register_callback(
            request_prep->step1_read_body, s_s3_default_prepare_request_on_read_done, request_prep);
    } else {
        /* Don't need to read body, jump directly to the last step */
        s_s3_default_prepare_request_finish(request_prep, AWS_ERROR_SUCCESS);
    }

    return asyncstep_prepare_request;
}

/* Completion callback for reading the body stream */
static void s_s3_default_prepare_request_on_read_done(void *user_data) {

    struct aws_s3_default_prepare_request_job *request_prep = user_data;
    struct aws_s3_request *request = request_prep->request;
    struct aws_s3_meta_request *meta_request = request->meta_request;

    int error_code = aws_future_bool_get_error(request_prep->step1_read_body);

    if (error_code != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p: Failed reading request body, error %d (%s)",
            (void *)meta_request,
            error_code,
            aws_error_str(error_code));
        goto finish;
    }

    if (request->request_body.len < request->request_body.capacity) {
        error_code = AWS_ERROR_S3_INCORRECT_CONTENT_LENGTH;
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p: Request body is smaller than 'Content-Length' header said it would be",
            (void *)meta_request);
        goto finish;
    }

finish:
    s_s3_default_prepare_request_finish(request_prep, error_code);
}

/* Finish async preparation of the request */
static void s_s3_default_prepare_request_finish(
    struct aws_s3_default_prepare_request_job *request_prep,
    int error_code) {

    struct aws_s3_request *request = request_prep->request;
    struct aws_s3_meta_request *meta_request = request->meta_request;

    if (error_code != AWS_ERROR_SUCCESS) {
        goto finish;
    }

    struct aws_http_message *message = aws_s3_message_util_copy_http_message_no_body_all_headers(
        meta_request->allocator, meta_request->initial_request_message);

    bool flexible_checksum = meta_request->checksum_config.location != AWS_SCL_NONE;
    if (!flexible_checksum && meta_request->should_compute_content_md5) {
        /* If flexible checksum used, client MUST skip Content-MD5 header computation */
        aws_s3_message_util_add_content_md5_header(meta_request->allocator, &request->request_body, message);
    }

    if (meta_request->checksum_config.validate_response_checksum) {
        struct aws_http_headers *headers = aws_http_message_get_headers(message);
        aws_http_headers_set(headers, g_request_validation_mode, g_enabled);
    }
    aws_s3_message_util_assign_body(
        meta_request->allocator,
        &request->request_body,
        message,
        &meta_request->checksum_config,
        NULL /* out_checksum */);

    aws_s3_request_setup_send_data(request, message);

    aws_http_message_release(message);

    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST, "id=%p: Meta Request prepared request %p", (void *)meta_request, (void *)request);

finish:
    if (error_code == AWS_ERROR_SUCCESS) {
        aws_future_void_set_result(request_prep->on_complete);
    } else {
        aws_future_void_set_error(request_prep->on_complete, error_code);
    }

    aws_future_bool_release(request_prep->step1_read_body);
    aws_future_void_release(request_prep->on_complete);
    aws_mem_release(request_prep->allocator, request_prep);
}

static void s_s3_meta_request_default_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);
    AWS_PRECONDITION(request);

    struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
    AWS_PRECONDITION(meta_request_default);

    if (error_code == AWS_ERROR_SUCCESS && meta_request->headers_callback != NULL &&
        request->send_data.response_headers != NULL) {

        if (meta_request->headers_callback(
                meta_request,
                request->send_data.response_headers,
                request->send_data.response_status,
                meta_request->user_data)) {
            error_code = aws_last_error_or_unknown();
        }

        meta_request->headers_callback = NULL;
    }

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_meta_request_lock_synced_data(meta_request);
        meta_request_default->synced_data.cached_response_status = request->send_data.response_status;
        meta_request_default->synced_data.request_completed = true;
        meta_request_default->synced_data.request_error_code = error_code;

        if (error_code == AWS_ERROR_SUCCESS) {
            aws_s3_meta_request_stream_response_body_synced(meta_request, request);
        } else {
            aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
        }

        aws_s3_meta_request_unlock_synced_data(meta_request);
    }
    /* END CRITICAL SECTION */
}
