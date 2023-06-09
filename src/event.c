//
// Created by yukino on 2023/4/29.
//

#include <sys/epoll.h>
#include <sys/time.h>
#include <time.h>
#include "event.h"
#include "error.h"
#include "zmalloc.h"
#include "log.h"

int createEpollData(eventLoop *el) {
    epollData *state = zmalloc(sizeof(epollData));
    if (NULL == state) {
        serverLog(LL_WARNING, "malloc epoll data failed.");
        return -1;
    }

    state->events = zmalloc(sizeof(struct epoll_event) * el->size);
    if (NULL == state->events) {
        serverLog(LL_WARNING, "malloc epoll event failed.");
        zfree(state);
        return -1;
    }

    state->epollFd = epoll_create(EPOLL_SIZE);
    if (-1 == state->epollFd) {
        serverLog(LL_WARNING, "create epoll socket failed.");
        zfree(state->events);
        zfree(state);
        return -1;
    }
    el->epData = state;
    el->timeEventNextId = 0;
    el->timeEventHead = NULL;
    el->lastTime = time(NULL);
    el->beforeSleep = NULL;
    el->flags = 0;

    return 0;
}

eventLoop *createEventLoop(int maxSize) {
    eventLoop *el = NULL;

    el = zmalloc(sizeof(eventLoop));
    if (NULL == el) {
        serverLog(LL_WARNING, "malloc event loop failed.");
        return NULL;
    }
    el->maxfd = -1;
    el->size = maxSize;
    el->fileEvents = zmalloc(sizeof(fileEvent) * maxSize);
    if (NULL == el->fileEvents) {
        zfree(el);
        serverLog(LL_WARNING, "malloc fileEvents failed.");
        return NULL;
    }
    el->firedFileEvents = zmalloc(sizeof(firedFileEvent) * maxSize);
    if (NULL == el->firedFileEvents) {
        zfree(el->fileEvents);
        zfree(el);
        serverLog(LL_WARNING, "malloc firedFileEvents failed.");
        return NULL;
    }

    if (-1 == createEpollData(el)) {
        zfree(el->fileEvents);
        zfree(el->firedFileEvents);
        zfree(el);
        return NULL;
    }

    return el;
}

int addEpollEvent(eventLoop *el, int fd, int mask) {
    struct epoll_event ee = {0};
    int op;

    if (el->fileEvents[fd].mask == EVENT_NONE) {
        op = EPOLL_CTL_ADD;
    } else {
        op = EPOLL_CTL_MOD;
    }

    ee.events = 0;
    mask |= el->fileEvents[fd].mask;
    if (mask & EVENT_READABLE) ee.events |= EPOLLIN;
    if (mask & EVENT_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    if (-1 == epoll_ctl(el->epData->epollFd, op, fd,&ee)) {
        return -1;
    }

    return 0;
}

void deleteEpollEvent(eventLoop *el, int fd, int delMask) {
    epollData *state = el->epData;
    struct epoll_event ee = {0};
    int mask = el->fileEvents[fd].mask & (~delMask);

    ee.events = 0;
    if (mask & EVENT_READABLE) {
        ee.events |= EPOLLIN;
    }

    if(mask & EVENT_WRITABLE) {
        ee.events |= EPOLLOUT;
    }

    if (EVENT_NONE != mask) {
        epoll_ctl(state->epollFd, EPOLL_CTL_MOD, fd, &ee);
    } else {
        epoll_ctl(state->epollFd, EPOLL_CTL_DEL, fd, &ee);
    }
}

void deleteFileEvent(eventLoop *el, int fd, int mask) {
    if (fd >= el->size) {
        serverLog(LL_WARNING, "fd out of range.");
        return;
    }
    fileEvent *fe = &el->fileEvents[fd];
    if (EVENT_NONE == fe->mask) {
        return;
    }

    deleteEpollEvent(el, fd, mask);
    fe->mask = fe->mask & (~mask);

    if (fd == el->maxfd && fe->mask == EVENT_NONE) {
        int j;

        for (j = el->maxfd - 1; j >= 0; j--) {
            if (el->fileEvents[j].mask != EVENT_NONE) {
                break;
            }
            el->maxfd = j;
        }
    }
}

int eventPoll(eventLoop *el, struct timeval *tvp) {
    epollData *state = el->epData;
    int retVal, numEvents = 0;

    retVal = epoll_wait(state->epollFd,state->events,el->size,
                        tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    if (retVal > 0) {
        int j;

        numEvents = retVal;
        for (j = 0; j < numEvents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;
            if (e->events & EPOLLIN) mask  |= EVENT_READABLE;
            if (e->events & EPOLLOUT) mask |= EVENT_WRITABLE;
            if (e->events & EPOLLERR) mask |= EVENT_WRITABLE | EVENT_READABLE;
            if (e->events & EPOLLHUP) mask |= EVENT_WRITABLE | EVENT_READABLE;
            el->firedFileEvents[j].fd = e->data.fd;
            el->firedFileEvents[j].mask = mask;
        }
    }
    return numEvents;
}

int createFileEvent(eventLoop *el, int fd, int mask, void *proc, void *clientData) {
    if(-1 == fd) {
        serverLog(LL_WARNING, "invalid server fd when create file event.");
        return ERROR_FAILED;
    }
    if(fd >= el->size) {
        serverLog(LL_WARNING, "server fd out of range.");
        return ERROR_FAILED;
    }

    fileEvent *fe = &el->fileEvents[fd];
    if (NULL == fe) {
        serverLog(LL_WARNING, "fileEvent is null.");
        return ERROR_FAILED;
    }

    if (-1 == addEpollEvent(el, fd, mask))  {
        serverLog(LL_WARNING, "add epoll event failed.");
        return ERROR_FAILED;
    }

    fe->mask |= mask;
    if(mask & EVENT_READABLE) {
        fe->rFileProc = proc;
    }
    if (mask & EVENT_WRITABLE) {
        fe->wFileProc = proc;
    }

    fe->clientData = clientData;

    if (fd > el->maxfd) {
        el->maxfd = fd;
    }

    return ERROR_SUCCESS;
}

void setBeforeSleepProc(eventLoop *el, beforeSleepProc *beforeSleep) {
    el->beforeSleep = beforeSleep;
}

static void getTime(long *seconds, long *milliseconds)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}

static void addMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    getTime(&cur_sec, &cur_ms);
    when_sec = cur_sec + milliseconds / 1000;
    when_ms = cur_ms + milliseconds % 1000;
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}

unsigned long createTimeEvent(eventLoop *el, long long milliseconds,
                            timeProc *proc, void *clientData,
                            eventFinalizerProc *finalizerProc)
{
    long long id = el->timeEventNextId++;
    timeEvent *te;

    te = zmalloc(sizeof(*te));
    if (NULL == te) {
        return ERROR_FAILED;
    }
    te->id = id;

    /* 计算时间事件下一次执行的时间 */
    addMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;

    /* 头插到eventLoop->timeEventHead链表 */
    te->prev = NULL;
    te->next = el->timeEventHead;
    te->refcount = 0;
    if (te->next) {
        te->next->prev = te;
    }
    el->timeEventHead = te;
    return ERROR_SUCCESS;
}

timeEvent *searchNearestTimer(eventLoop *el)
{
    timeEvent *te = el->timeEventHead;
    timeEvent *nearest = NULL;

    while(te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
            (te->when_sec == nearest->when_sec &&
             te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

int processTimeEvents(eventLoop *el) {
    int processed = 0;
    timeEvent *te;
    long long maxId;
    time_t now = time(NULL);

    /* 上次执行时间比当前时间更大，说明系统时间混乱了。这里将所有时间时间when_sec设置为0，
     * 这样会导致时间事件提前执行，提前执行事件的危害比延后执行的小 */
    if (now < el->lastTime) {
        te = el->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
    el->lastTime = now;

    /* 遍历时间事件 */
    te = el->timeEventHead;
    maxId = el->timeEventNextId-1;
    while(te) {
        long now_sec, now_ms;
        long long id;

        /* EVENT_DELETED_EVENT_ID 代表时间事件已经删除，将其从链表中移除 */
        if (te->id == EVENT_DELETED_EVENT_ID) {
            timeEvent *next = te->next;
            if (te->refcount) {
                te = next;
                continue;
            }
            if (te->prev)
                te->prev->next = te->next;
            else
                el->timeEventHead = te->next;
            if (te->next)
                te->next->prev = te->prev;
            if (te->finalizerProc)
                te->finalizerProc(el, te->clientData);
            zfree(te);
            te = next;
            continue;
        }

        if (te->id > maxId) {
            te = te->next;
            continue;
        }

        /* 如果时间事件以达到执行时间，则执行timeProc函数，该函数返回下次执行时间的间隔 */
        getTime(&now_sec, &now_ms);
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retVal;

            id = te->id;
            te->refcount++;
            retVal = te->timeProc(el, id, te->clientData);
            te->refcount--;
            processed++;

            /* 事件下次执行间隔时间等于AE_NOMORE，代表下次不再执行，需要删除时间事件 */
            if (retVal != EVENT_NOMORE) {

                /* 重新计算时间 */
                addMillisecondsToNow(retVal,&te->when_sec,&te->when_ms);
            } else {

                /* 标记删除 */
                te->id = EVENT_DELETED_EVENT_ID;
            }
        }

        /* 处理下一个时间事件 */
        te = te->next;
    }
    return processed;
}

int processEvents(eventLoop *el, int flags)
{
    int processed = 0, numEvents;

    if (!(flags & EVENT_TIME_EVENTS) && !(flags & EVENT_FILE_EVENTS)) {
        return 0;
    }

    if (el->maxfd != -1 ||
        ((flags & EVENT_TIME_EVENTS) && !(flags & EVENT_DONT_WAIT))) {
        int j;
        timeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        if (flags & EVENT_TIME_EVENTS && !(flags & EVENT_DONT_WAIT))

            /* 查找当前最先执行的时间事件，如果能找到，则该事件执行时间减去当前时间作为进程最大阻塞时间 */
            shortest = searchNearestTimer(el);
        if (shortest) {
            long now_sec, now_ms;

            getTime(&now_sec, &now_ms);
            tvp = &tv;

            long long ms =
                    (shortest->when_sec - now_sec)*1000 +
                    shortest->when_ms - now_ms;

            if (ms > 0) {
                tvp->tv_sec = ms/1000;
                tvp->tv_usec = (ms % 1000)*1000;
            } else {
                tvp->tv_sec = 0;
                tvp->tv_usec = 0;
            }
        } else {

            /* 不阻塞，将超时时间设置为0 */
            if (flags & EVENT_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {

                /* 阻塞，设置为NULL */
                tvp = NULL; /* wait forever */
            }
        }

        if (el->flags & EVENT_DONT_WAIT) {
            tv.tv_sec = tv.tv_usec = 0;
            tvp = &tv;
        }

        /* 进程阻塞前，执行钩子函数beforeSleep */
        if (el->beforeSleep != NULL && flags & EVENT_CALL_BEFORE_SLEEP)
            el->beforeSleep(el);

        numEvents = eventPoll(el, tvp);
        for (j = 0; j < numEvents; j++) {
            fileEvent *fe = &el->fileEvents[el->firedFileEvents[j].fd];
            int mask = el->firedFileEvents[j].mask;
            int fd = el->firedFileEvents[j].fd;
            if(fe->mask & mask & EVENT_READABLE) {
                fe->rFileProc(el, fd, fe->clientData, mask);
            }
            if (fe->mask & mask & EVENT_WRITABLE) {
                fe->wFileProc(el, fd, fe->clientData, mask);
            }
            processed++;
        }
}
    /* 检查时间事件是否就绪 */
    if (flags & EVENT_TIME_EVENTS) {
        processed += processTimeEvents(el);
    }

    return processed;
}