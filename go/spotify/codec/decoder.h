#ifndef R2_VORBIS_DECODER_H
#define R2_VORBIS_DECODER_H
typedef struct r2v_decoder r2v_decoder;
r2v_decoder *r2v_new(void);
void r2v_close(r2v_decoder *);
int r2v_feed(r2v_decoder *, const unsigned char *, int);
int r2v_read(r2v_decoder *, unsigned char *, int);
int r2v_rate(r2v_decoder *);
int r2v_check_codebook(void);
#endif
