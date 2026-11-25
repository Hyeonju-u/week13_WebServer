#ifndef EVENT_H
#define EVENT_H

typedef struct event_loop event_loop_t;

typedef struct {
    void * data;
    int readable;
    int writable;
    int failed;
} event_t;

event_loop_t * create_event_loop (void);
void destroy_event_loop (event_loop_t * loop);
int add_event (event_loop_t * loop, int fd, void * data, int wants_read,
    int wants_write);
int modify_event (event_loop_t * loop, int fd, void * data, int wants_read,
    int wants_write);
void delete_event (event_loop_t * loop, int fd);
int wait_events (event_loop_t * loop, event_t * events, int capacity,
    int timeout_ms);

#endif
