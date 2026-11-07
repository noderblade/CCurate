#ifndef CCURATE_PREVIEW_WINDOWS_H
#define CCURATE_PREVIEW_WINDOWS_H
#include <poppler.h>
PopplerDocument *cc_win_pdf_document(int fd, GError **error);
#endif
