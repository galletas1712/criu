#ifndef __CR_PAGE_COALESCE_H__
#define __CR_PAGE_COALESCE_H__

#include "types.h"

struct compact_page_stream;

int coalesce_checkpoint_pages_start(void);
int coalesce_checkpoint_pages_enqueue(int pagemap_type, unsigned long img_id);
int coalesce_checkpoint_pages_open_stream(u32 pages_id, struct compact_page_stream **out);
int coalesce_checkpoint_pages_write_stream(struct compact_page_stream *stream, int pipe, unsigned long len);
int coalesce_checkpoint_pages_close_stream(struct compact_page_stream *stream);
void coalesce_checkpoint_pages_discard_stream(struct compact_page_stream *stream);
void coalesce_checkpoint_pages_abort(void);
int coalesce_checkpoint_pages(void);

#endif /* __CR_PAGE_COALESCE_H__ */
