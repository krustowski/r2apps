#include "../decoder.h"
#include "../vendor/tremor/ivorbiscodec.h"
#include "../vendor/tremor/codebook.h"
#include <string.h>
#include <stddef.h>
extern void *r2v_calloc(size_t,size_t);
extern void r2v_free(void *);
struct r2v_decoder {
 ogg_sync_state sync;
 ogg_stream_state stream;
 vorbis_info info;
 vorbis_comment comment;
 vorbis_dsp_state dsp;
 vorbis_block block;
 int stream_ready, ready, headers, failed;
};
r2v_decoder *r2v_new(void) {
 r2v_decoder *d=r2v_calloc(1,sizeof(*d));if(!d)return 0;
 ogg_sync_init(&d->sync);vorbis_info_init(&d->info);vorbis_comment_init(&d->comment);return d;
}
void r2v_close(r2v_decoder *d) {
 if(!d)return;
 if(d->ready){vorbis_block_clear(&d->block);vorbis_dsp_clear(&d->dsp);}
 if(d->stream_ready)ogg_stream_clear(&d->stream);
 vorbis_comment_clear(&d->comment);vorbis_info_clear(&d->info);ogg_sync_clear(&d->sync);r2v_free(d);
}
int r2v_feed(r2v_decoder *d,const unsigned char *data,int n) {
 if(!d||d->failed||n<0||n>32768)return -1;
 char *buffer=ogg_sync_buffer(&d->sync,n);if(!buffer)return -1;
 memcpy(buffer,data,n);return ogg_sync_wrote(&d->sync,n);
}
int r2v_rate(r2v_decoder *d){return d&&d->ready?(int)d->info.rate:0;}
int r2v_read(r2v_decoder *d,unsigned char *out,int bytes) {
 if(!d||d->failed||bytes<4)return -1;
 for(;;){
  if(d->ready){
   ogg_int32_t **pcm;int samples=vorbis_synthesis_pcmout(&d->dsp,&pcm);
   if(samples>0){
    if(samples>bytes/4)samples=bytes/4;
    for(int i=0;i<samples;i++)for(int ch=0;ch<2;ch++){
     int source=d->info.channels==1?0:ch;ogg_int32_t v=pcm[source][i]>>9;
     if(v>32767)v=32767;if(v< -32768)v= -32768;
     out[i*4+ch*2]=(unsigned char)v;out[i*4+ch*2+1]=(unsigned char)(v>>8);
    }
    vorbis_synthesis_read(&d->dsp,samples);return samples*4;
   }
  }
  if(d->stream_ready){
   ogg_packet packet;int ret=ogg_stream_packetout(&d->stream,&packet);
   if(ret<0){d->failed=1;return -1;}
   if(ret==1){
    if(d->headers<3){
     if(vorbis_synthesis_headerin(&d->info,&d->comment,&packet)<0){d->failed=1;return -1;}
     d->headers++;
     if(d->headers==3){
      if(d->info.channels<1||d->info.channels>2||d->info.rate<8000||d->info.rate>96000||vorbis_synthesis_init(&d->dsp,&d->info)){d->failed=1;return -1;}
      vorbis_block_init(&d->dsp,&d->block);d->ready=1;return 0;
     }
    }else if(vorbis_synthesis(&d->block,&packet)==0)vorbis_synthesis_blockin(&d->dsp,&d->block);
    else {d->failed=1;return -1;}
    continue;
   }
  }
  ogg_page page;int ret=ogg_sync_pageout(&d->sync,&page);
  if(ret==0)return 0;
  if(ret<0){d->failed=1;return -1;}
  if(!d->stream_ready){ogg_stream_init(&d->stream,ogg_page_serialno(&page));d->stream_ready=1;}
  if(ogg_stream_pagein(&d->stream,&page)<0){d->failed=1;return -1;}
 }
}

/* Linked only when referenced by the native regression build. */
int r2v_check_codebook(void) {
 const int count=16384;
 long *lengths=r2v_calloc(count,sizeof(*lengths));if(!lengths)return -1;
 for(int i=0;i<count;i++)lengths[i]=14;
 static_codebook input={0};input.entries=count;input.dim=1;input.lengthlist=lengths;
 codebook decoded;
 int result=vorbis_book_init_decode(&decoded,&input);
 vorbis_book_clear(&decoded);r2v_free(lengths);return result;
}
