#include "event.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#endif

#define EVENT_BATCH_MAX 1024

struct event_loop {
    int handle;
};

#ifdef __linux__
static int change_interest (event_loop_t * loop, int operation, int fd,
    void * data, int wants_read, int wants_write);
static unsigned int build_interest (int wants_read, int wants_write);
#else
static int change_interest (event_loop_t * loop, int fd, void * data,
    int wants_read, int wants_write);
static void convert_event (const struct kevent * source, event_t * event);
#endif

/* ── 공개 함수 ── */

/* 플랫폼 분기(#ifdef)는 이 파일 안에만 둔다 (CON-02) */
event_loop_t * create_event_loop (void) {
    event_loop_t * loop = malloc(sizeof(*loop));
    if (loop == NULL) {
        return NULL;
    }
#ifdef __linux__
    loop->handle = epoll_create1(0);
#else
    loop->handle = kqueue();
#endif
    if (loop->handle < 0) {
        free(loop);
        return NULL;
    }
    return loop;
}

void destroy_event_loop (event_loop_t * loop) {
    close(loop->handle);
    free(loop);
}

#ifdef __linux__

int add_event (event_loop_t * loop, int fd, void * data, int wants_read,
    int wants_write) {
    return change_interest(loop, EPOLL_CTL_ADD, fd, data, wants_read,
        wants_write);
}

int modify_event (event_loop_t * loop, int fd, void * data, int wants_read,
    int wants_write) {
    return change_interest(loop, EPOLL_CTL_MOD, fd, data, wants_read,
        wants_write);
}

void delete_event (event_loop_t * loop, int fd) {
    struct epoll_event unused;
    memset(&unused, 0, sizeof(unused));
    epoll_ctl(loop->handle, EPOLL_CTL_DEL, fd, &unused);
}

/* 레벨 트리거다: 다 읽지 못한 데이터는 다음 바퀴에 다시 알려 준다 (CON-03) */
int wait_events (event_loop_t * loop, event_t * events, int capacity,
    int timeout_ms) {
    struct epoll_event entries[EVENT_BATCH_MAX];
    int limit = capacity < EVENT_BATCH_MAX ? capacity : EVENT_BATCH_MAX;
    int count = epoll_wait(loop->handle, entries, limit, timeout_ms);
    for (int i = 0; i < count; i++) {
        events[i].data = entries[i].data.ptr;
        events[i].readable = (entries[i].events & EPOLLIN) != 0;
        events[i].writable = (entries[i].events & EPOLLOUT) != 0;
        events[i].failed = (entries[i].events & (EPOLLERR | EPOLLHUP)) != 0;
    }
    return count;
}

#else

int add_event (event_loop_t * loop, int fd, void * data, int wants_read,
    int wants_write) {
    return change_interest(loop, fd, data, wants_read, wants_write);
}

int modify_event (event_loop_t * loop, int fd, void * data, int wants_read,
    int wants_write) {
    return change_interest(loop, fd, data, wants_read, wants_write);
}

void delete_event (event_loop_t * loop, int fd) {
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(loop->handle, changes, 2, NULL, 0, NULL);
}

int wait_events (event_loop_t * loop, event_t * events, int capacity,
    int timeout_ms) {
    struct kevent entries[EVENT_BATCH_MAX];
    int limit = capacity < EVENT_BATCH_MAX ? capacity : EVENT_BATCH_MAX;
    struct timespec timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_nsec = (long) (timeout_ms % 1000) * 1000000L;
    int count = kevent(loop->handle, NULL, 0, entries, limit,
        timeout_ms < 0 ? NULL : &timeout);
    for (int i = 0; i < count; i++) {
        convert_event(&entries[i], &events[i]);
    }
    return count;
}

#endif

/* ── 도우미 함수 ── */

#ifdef __linux__

static int change_interest (event_loop_t * loop, int operation, int fd,
    void * data, int wants_read, int wants_write) {
    struct epoll_event entry;
    memset(&entry, 0, sizeof(entry));
    entry.events = build_interest(wants_read, wants_write);
    entry.data.ptr = data;
    return epoll_ctl(loop->handle, operation, fd, &entry);
}

static unsigned int build_interest (int wants_read, int wants_write) {
    unsigned int interest = 0;
    if (wants_read) {
        interest |= EPOLLIN;
    }
    if (wants_write) {
        interest |= EPOLLOUT;
    }
    return interest;
}

#else

/* 읽기·쓰기 필터를 항상 둘 다 등록하고 켜고 끄기만 해서 ENOENT를 피한다 */
static int change_interest (event_loop_t * loop, int fd, void * data,
    int wants_read, int wants_write) {
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ,
        EV_ADD | (wants_read ? EV_ENABLE : EV_DISABLE), 0, 0, data);
    EV_SET(&changes[1], fd, EVFILT_WRITE,
        EV_ADD | (wants_write ? EV_ENABLE : EV_DISABLE), 0, 0, data);
    return kevent(loop->handle, changes, 2, NULL, 0, NULL);
}

/*
 * EOF는 읽기 알림으로 바꿔서 read가 0이나 에러를 직접 확인하게 한다.
 * EOF에 에러 번호가 붙어 있으면 연결이 깨진 것이다.
 */
static void convert_event (const struct kevent * source, event_t * event) {
    event->data = source->udata;
    event->readable = source->filter == EVFILT_READ;
    event->writable = source->filter == EVFILT_WRITE;
    event->failed = (source->flags & EV_ERROR) != 0
        || ((source->flags & EV_EOF) != 0 && source->fflags != 0);
}

#endif
