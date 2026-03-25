#ifndef __CR_PAGE_COALESCE_H__
#define __CR_PAGE_COALESCE_H__

int coalesce_checkpoint_pages_start(void);
int coalesce_checkpoint_pages_enqueue(int pagemap_type, unsigned long img_id);
void coalesce_checkpoint_pages_abort(void);
int coalesce_checkpoint_pages(void);

#endif /* __CR_PAGE_COALESCE_H__ */
