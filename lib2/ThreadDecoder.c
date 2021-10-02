//
// Created by miurahr on 2021/08/07.
//

#include "ThreadDecoder.h"
#include "Buffer.h"
#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#ifndef _WIN32
int ppmd_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, unsigned long nsec) {
    //https://gist.github.com/jbenet/1087739
    struct timespec abstime;
#ifdef __MACH__ // OS X does not have clock_gettime, use clock_get_time
    clock_serv_t cclock;
    mach_timespec_t mts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);
    abstime.tv_sec = mts.tv_sec;
    abstime.tv_nsec = mts.tv_nsec;
#else
    clock_gettime(CLOCK_REALTIME, &abstime);
#endif
    abstime.tv_nsec += nsec;
    if (abstime.tv_nsec >= 1000000000) {
        abstime.tv_nsec = abstime.tv_nsec - 1000000000;
        abstime.tv_sec = abstime.tv_sec + 1;
    }
    return pthread_cond_timedwait(cond, mutex, &abstime);
}
#else

static DWORD nsec_to_ms(unsigned long nsec) {
    DWORD t;

    if (nsec == 0) {
        return INFINITE;
    }
    t = nsec / 1000000;
    if (t < 0) {
        t = 1;
    }
    return t;
}

int ppmd_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, unsigned long nsec) {
	pthread_testcancel();
    if (cond == NULL || mutex == NULL ) {
        return 1;
    }

	if (!SleepConditionVariableCS(cond, mutex, nsec_to_ms(nsec))) return ETIMEDOUT;
	/* We can have a spurious wakeup after the timeout */
	if (!_pthread_rel_time_in_ms(mutex)) return ETIMEDOUT;
	return 0;
}
#endif

Byte Ppmd_thread_Reader(const void *p) {
    BufferReader *bufferReader = (BufferReader *)p;
    ppmd_info *threadInfo = bufferReader->t;
    ppmd_thread_control_t *tc = (ppmd_thread_control_t *)threadInfo->t;
    InBuffer *inBuffer = threadInfo->in;
    if (inBuffer->pos == inBuffer->size) {
        pthread_mutex_lock(&tc->mutex);
        pthread_cond_broadcast(&tc->inEmpty);
        while (inBuffer->pos == inBuffer->size) {
            pthread_cond_wait(&tc->notEmpty, &tc->mutex);
        }
        pthread_mutex_unlock(&tc->mutex);
    }
    return *((const Byte *)inBuffer->src + inBuffer->pos++);
}

Bool Ppmd_thread_decode_init(ppmd_info *threadInfo, ISzAllocPtr allocator) {
    threadInfo->t = ISzAlloc_Alloc(allocator, sizeof(ppmd_thread_control_t));
    if (threadInfo->t != NULL) {
        ppmd_thread_control_t *threadControl = (ppmd_thread_control_t *)threadInfo->t;
        pthread_mutex_init(&threadControl->mutex, NULL);
        pthread_cond_init(&threadControl->inEmpty, NULL);
        pthread_cond_init(&threadControl->notEmpty, NULL);
        return True;
    }
    return False;
}

static void *
Ppmd8T_decode_run(void *p) {
    ppmd_info *threadInfo = (ppmd_info *)p;
    ppmd_thread_control_t *tc = (ppmd_thread_control_t *)threadInfo->t;
    threadInfo->finished = False;
    CPpmd8 * cPpmd8 = (CPpmd8 *)(threadInfo->cPpmd);
    BufferReader *reader = (BufferReader *) cPpmd8->Stream.In;
    int max_length = threadInfo->max_length;

    int i = 0;
    int result;
    while (i < max_length ) {
        Bool inbuf_empty = reader->inBuffer->size == reader->inBuffer->pos;
        Bool outbuf_full = threadInfo->out->size == threadInfo->out->pos;
        if (inbuf_empty || outbuf_full) {
            break;
        }
        int c = Ppmd8_DecodeSymbol(cPpmd8);
        if (c == PPMD8_RESULT_EOF) {
            result = PPMD8_RESULT_EOF;
            goto exit;
        } else if (c == PPMD8_RESULT_ERROR) {
            result = PPMD8_RESULT_ERROR;
            goto exit;
        }
        pthread_mutex_lock(&tc->mutex);
        *((Byte *)threadInfo->out->dst + threadInfo->out->pos++) = (Byte) c;
        pthread_mutex_unlock(&tc->mutex);
        i++;
    }
    // when success return produced size
    result = i;

    exit:
    pthread_mutex_lock(&tc->mutex);
    threadInfo->result = result;
    threadInfo->finished = True;
    pthread_mutex_unlock(&tc->mutex);
    return NULL;
}

int Ppmd8T_decode(CPpmd8 *cPpmd8, OutBuffer *out, int max_length, ppmd_info *threadInfo) {
    ppmd_thread_control_t *tc = (ppmd_thread_control_t *)threadInfo->t;
    pthread_mutex_lock(&tc->mutex);
    BufferReader *reader = (BufferReader *) cPpmd8->Stream.In;
    threadInfo->cPpmd = (void *) cPpmd8;
    threadInfo->max_length = max_length;
    threadInfo->out = out;
    threadInfo->result = 0;
    Bool exited = threadInfo->finished;
    threadInfo->finished = False;
    pthread_mutex_unlock(&tc->mutex);

    if (exited) {
        pthread_create(&(tc->handle), NULL, Ppmd8T_decode_run, threadInfo);
        pthread_mutex_lock(&tc->mutex);
        pthread_mutex_unlock(&tc->mutex);
    } else {
        pthread_mutex_lock(&tc->mutex);
        if (reader->inBuffer->pos < reader->inBuffer->size) {
            pthread_cond_broadcast(&tc->notEmpty);
            pthread_mutex_unlock(&tc->mutex);
        } else {
            pthread_mutex_unlock(&tc->mutex);
            pthread_cancel(tc->handle);
            threadInfo->finished = True;
            return PPMD8_RESULT_ERROR;  // error
        }
    }
    pthread_mutex_lock(&tc->mutex);
    unsigned long wait = 50000;
    while(True) {
        ppmd_timedwait(&tc->inEmpty, &tc->mutex, wait);
        if (threadInfo->finished) {
            // when finished, the input buffer will be empty,
            // so check finished status before checking buffer.
            goto finished;
        }
        if (reader->inBuffer->pos == reader->inBuffer->size) {
            goto inempty;
        }
    }
finished:
    pthread_mutex_unlock(&tc->mutex);
    pthread_join(tc->handle, NULL);
    return threadInfo->result;

inempty:
    pthread_mutex_unlock(&tc->mutex);
    return 0;
}

void Ppmd8T_Free(CPpmd8 *cPpmd8, ppmd_info *threadInfo, ISzAllocPtr allocator) {
    ppmd_thread_control_t *tc = (ppmd_thread_control_t *)threadInfo->t;
    if (!(threadInfo->finished)) {
        pthread_cancel(tc->handle);
        threadInfo->finished = True;
    }
    ISzAlloc_Free(allocator, tc);
    Ppmd8_Free(cPpmd8, allocator);
}