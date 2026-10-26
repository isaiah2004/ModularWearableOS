/* Desktop-only keyed quality decisions. Not linked into device firmware. */
#ifndef NOS_QUALITY_TAPE_H
#define NOS_QUALITY_TAPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {unsigned frame,key;int kind,value,used;} qt_entry;
static struct {qt_entry *v;size_t n,cap;FILE *out;int replay;} QT;
static void qt_fail(const char *why,unsigned frame,int kind,unsigned key) {
    fprintf(stderr,"FAIL quality tape: %s frame=%u kind=%d key=%08x\n",why,frame,kind,key);
    exit(3);
}
static qt_entry *qt_append(unsigned frame,int kind,unsigned key,int value) {
    if(QT.n==QT.cap) {
        size_t cap=QT.cap?QT.cap*2:256;
        qt_entry *v=realloc(QT.v,cap*sizeof *v);
        if(!v) qt_fail("allocation",frame,kind,key);
        QT.v=v;QT.cap=cap;
    }
    qt_entry *e=&QT.v[QT.n++];*e=(qt_entry){frame,key,kind,value,0};return e;
}
static void qt_open(const char *path,int replay) {
    memset(&QT,0,sizeof QT);QT.replay=replay;
    FILE *f=fopen(path,replay?"r":"w");
    if(!f) qt_fail("cannot open",0,0,0);
    if(!replay) {QT.out=f;fprintf(f,"NOS_QUALITY_V1\n");return;}
    char header[40];
    if(!fgets(header,sizeof header,f) || strcmp(header,"NOS_QUALITY_V1\n")) qt_fail("bad header",0,0,0);
    unsigned frame,key;int kind,value,fields;
    while((fields=fscanf(f,"%u %d %x %d",&frame,&kind,&key,&value))==4) {
        if(kind<0 || kind>5 || value<0 || value>(kind>=2?1:2) || (kind==5 && key>6) ||
           (QT.n && frame<QT.v[QT.n-1].frame)) qt_fail("invalid entry",frame,kind,key);
        for(size_t i=QT.n;i && QT.v[i-1].frame==frame;i--)
            if(QT.v[i-1].kind==kind && QT.v[i-1].key==key) qt_fail("duplicate entry",frame,kind,key);
        qt_append(frame,kind,key,value);
    }
    if(fields!=EOF) qt_fail("malformed entry",0,0,0);
    fclose(f);
}
static int qt_decide(unsigned frame,int kind,unsigned key,int proposed) {
    size_t lo=0,hi=QT.n;
    while(lo<hi) {size_t mid=lo+(hi-lo)/2;if(QT.v[mid].frame<frame) lo=mid+1;else hi=mid;}
    for(size_t i=lo;i<QT.n && QT.v[i].frame==frame;i++)
        if(QT.v[i].kind==kind && QT.v[i].key==key) {
            if(!QT.replay && QT.v[i].value!=proposed) qt_fail("divergent repeated decision",frame,kind,key);
            QT.v[i].used=1;return QT.v[i].value;
        }
    if(QT.replay) qt_fail("missing decision",frame,kind,key);
    if(QT.n && frame<QT.v[QT.n-1].frame) qt_fail("frame order",frame,kind,key);
    qt_append(frame,kind,key,proposed)->used=1;
    if(fprintf(QT.out,"%u %d %08x %d\n",frame,kind,key,proposed)<0) qt_fail("write",frame,kind,key);
    return proposed;
}
static void qt_close(unsigned last_frame) {
    if(QT.replay) for(size_t i=0;i<QT.n;i++)
        if(QT.v[i].frame<=last_frame && !QT.v[i].used) qt_fail("unused decision",QT.v[i].frame,QT.v[i].kind,QT.v[i].key);
    if(QT.out && fclose(QT.out)) qt_fail("close",0,0,0);
    free(QT.v);memset(&QT,0,sizeof QT);
}
#endif
