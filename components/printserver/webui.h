/* Internal WebUI/REST API for the AirPrint cat printer.
 * Lives in the printserver component so it can reuse the same socket workers
 * that serve IPP. All state is queried directly from the owning subsystems. */
#ifndef WEBUI_H
#define WEBUI_H

#include <stddef.h>

/* Serve GET /, /index.html and GET API endpoints (no body on GET). */
void webui_serve(int fd, const char *path);

/* Serve POST API endpoints with an already-buffered form body. */
void webui_api(int fd, const char *path, const char *body, size_t body_len);

#endif
