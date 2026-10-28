/* Synthetic RTSP controls, compiled as C against the actual parser input.
 * Track only its malloc/free calls: no parser model and no sanitizer substitute.
 * POLARIS_RTSP_PARSER_SOURCE also selects the exact original for a before control.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "Platform.h"

static size_t live_allocations;
static size_t allocation_attempts;
static size_t fail_at;

static void* parser_malloc(size_t size) {
    ++allocation_attempts;
    if (allocation_attempts == fail_at) return NULL;
    void* value = malloc(size);
    if (value != NULL) ++live_allocations;
    return value;
}

static void parser_free(void* value) {
    if (value != NULL) {
        --live_allocations;
        free(value);
    }
}

/* Private symbols prevent collisions with any normally linked production copy. */
#define malloc parser_malloc
#define free parser_free
#define parseRtspMessage fixture_parseRtspMessage
#define freeMessage fixture_freeMessage
#define createRtspRequest fixture_createRtspRequest
#define createRtspResponse fixture_createRtspResponse
#define getOptionContent fixture_getOptionContent
#define insertOption fixture_insertOption
#define freeOptionList fixture_freeOptionList
#define appendString fixture_appendString
#define serializeRtspMessage fixture_serializeRtspMessage
#ifndef POLARIS_RTSP_PARSER_SOURCE
#error Select the exact original or prepared production RtspParser.c
#endif
#include POLARIS_RTSP_PARSER_SOURCE
#undef malloc
#undef free

static const char normal[] =
    "OPTIONS rtsp://synthetic RTSP/1.0\r\nCSeq: 7\r\nX-Tag: first\r\n\r\n";
static const char duplicate[] =
    "OPTIONS rtsp://synthetic RTSP/1.0\r\nCSeq: 1\r\nX-Tag: first\r\n"
    "CSeq: 9\r\nx-tag: lower\r\nX-Tag: last\r\n\r\n";
/* The final token has no value. Every value token has at least three following
 * bytes for the pinned parser's endCheck comparisons; this is not a truncated-NUL
 * safety assumption. The missing header value reaches ordinary failure cleanup.
 */
static const char malformed[] =
    "OPTIONS rtsp://synthetic RTSP/1.0\r\nCSeq: 1\r\nCSeq: 2\r\nMissing\r\n";

static int require(int condition, const char* description) {
    if (condition) return 0;
    fprintf(stderr, "synthetic RTSP ownership control: %s\n", description);
    return 1;
}

static int begin_control(size_t failure) {
    if (require(live_allocations == 0, "prior parser allocation remains live")) return 1;
    allocation_attempts = 0;
    fail_at = failure;
    return 0;
}

static int same(const char* actual, const char* expected) {
    return actual != NULL && strcmp(actual, expected) == 0;
}

static int successful_request(const char* packet, int duplicates) {
    RTSP_MESSAGE message = {0};
    if (begin_control(0)) return 1;
    int result = fixture_parseRtspMessage(&message, (char*)packet, (int)strlen(packet));
    if (result != RTSP_ERROR_SUCCESS) {
        return require(0, "normal/duplicate request did not parse successfully");
    }
    POPTION_ITEM first = message.options;
    POPTION_ITEM second = first != NULL ? first->next : NULL;
    POPTION_ITEM third = second != NULL ? second->next : NULL;
    int failed = require(message.type == TYPE_REQUEST &&
        same(message.message.request.command, "OPTIONS") &&
        same(message.message.request.target, "rtsp://synthetic") &&
        same(message.protocol, "RTSP/1.0"), "request semantics changed");
    failed |= require(first != NULL && same(first->option, "CSeq") &&
        second != NULL && same(second->option, "X-Tag"), "first header order changed");
    failed |= require(first != NULL && first->option == message.messageBuffer +
        (strstr(packet, "CSeq: ") - packet) && second != NULL &&
        second->option == message.messageBuffer + (strstr(packet, "X-Tag: ") - packet),
        "first header names were replaced");
    failed |= require(message.sequenceNumber == (duplicates ? 9 : 7) &&
        same(fixture_getOptionContent(message.options, "X-Tag"), duplicates ? "last" : "first"),
        "last-value-wins content changed");
    failed |= require(duplicates ? (third != NULL && same(third->option, "x-tag") &&
        same(third->content, "lower") && third->next == NULL) : third == NULL,
        "case-sensitive comparison or unique header list changed");
    fixture_freeMessage(&message);
    failed |= require(live_allocations == 0, "successful message cleanup leaked an allocation");
    return failed;
}

int polaris_rtsp_normal_control(void) { return successful_request(normal, 0); }
int polaris_rtsp_duplicate_control(void) { return successful_request(duplicate, 1); }

static int rejected_request(const char* packet, int expected) {
    RTSP_MESSAGE message = {0};
    int result = fixture_parseRtspMessage(&message, (char*)packet, (int)strlen(packet));
    int failed = require(result == expected, "unexpected failure result");
    if (result == RTSP_ERROR_SUCCESS) fixture_freeMessage(&message);
    failed |= require(message.flags == 0 && message.options == NULL &&
        message.messageBuffer == NULL, "failed parse changed the caller's message");
    failed |= require(live_allocations == 0, "failure cleanup leaked an allocation");
    return failed;
}

int polaris_rtsp_malformed_control(void) {
    if (begin_control(0)) return 1;
    return rejected_request(malformed, RTSP_ERROR_MALFORMED);
}

int polaris_rtsp_oom_control(void) {
    /* One buffer and five header allocations, including both duplicate nodes.
     * Keep the existing allocation attempts and OOM return, rather than skipping
     * a duplicate's allocation to make the leak disappear.
     */
    for (size_t failure = 1; failure <= 6; ++failure) {
        if (begin_control(failure)) return 1;
        if (rejected_request(duplicate, RTSP_ERROR_NO_MEMORY)) return 1;
        if (require(allocation_attempts == failure, "injected allocation failure was not reached")) return 1;
    }
    return 0;
}

#ifdef POLARIS_RTSP_OWNERSHIP_STANDALONE
int main(int argc, char** argv) {
    if (argc == 2) {
        if (!strcmp(argv[1], "normal")) return polaris_rtsp_normal_control();
        if (!strcmp(argv[1], "duplicate")) return polaris_rtsp_duplicate_control();
        if (!strcmp(argv[1], "malformed")) return polaris_rtsp_malformed_control();
        if (!strcmp(argv[1], "oom")) return polaris_rtsp_oom_control();
    }
    fprintf(stderr, "Select synthetic control: normal, duplicate, malformed, oom\n");
    return 2;
}
#endif
