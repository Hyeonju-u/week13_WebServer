#ifndef SIGNALS_H
#define SIGNALS_H

int install_signal_handlers (void);
int is_shutdown_requested (void);
void request_shutdown (void);

#endif
