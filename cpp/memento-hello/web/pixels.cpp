#include "doc.h"

namespace web {
namespace {
int maxi(int a,int b) { return a>b?a:b; }
int mini(int a,int b) { return a<b?a:b; }
int length(int v,int reference,int fallback=0) {
    if(v==BoxStyle::Auto) return fallback;
    if(v<=-10000 && v>=-20000) return reference<0?fallback:reference*(-10000-v)/100;
    return v;
}
struct Flow {
    int left, top, width, x, y, height=0;
    bool space=false;
    Flow(int l,int t,int w):left(l),top(t),width(maxi(1,w)),x(l),y(t){}
    void newline(int minimum=0) { y+=maxi(height,minimum); x=left; height=0; space=false; }
    int bottom() const {return y+height;}
};
}

// All coordinates are device pixels in the page. The fixed system font is
// measured by the browser, rather than assuming that a CSS pixel is a cell.
class PixelLayout {
    Document &d;
    int width,cw,lh,viewportHeight;
    Box &box(uint32_t i) {return ((Box *)d.boxes_.data)[i];}
    const Item &item(uint32_t i) {return ((const Item *)d.items_.data)[i];}
    int extra(const Box &b) const {return b.padding[1]+b.padding[3]+b.border[1]+b.border[3];}
    int vertical(const Box &b) const {return b.padding[0]+b.padding[2]+b.border[0]+b.border[2];}
    void edges(Box &b,int reference) {
        for(int k=0;k<4;++k) {
            b.margin[k]=length(b.style.box.margin[k],reference);
            b.padding[k]=maxi(0,length(b.style.box.padding[k],reference));
            b.border[k]=maxi(0,length(b.style.box.border[k],reference));
        }
    }
    int constrain(int value,int low,int high,int ref) const {
        return maxi(0,maxi(length(low,ref),mini(value,length(high,ref,0x3fffffff))));
    }
    int intrinsic(uint32_t i,int ref) {
        Box &b=box(i); edges(b,ref);
        if(b.style.box.width!=BoxStyle::Auto)
            return maxi(0,length(b.style.box.width,ref))+(b.style.box.sizing?0:extra(b));
        if(b.style.display==CssStyle::D_FLEX) {
            int extent=0,count=0;bool column=b.style.box.direction!=0;
            for(uint32_t c=b.firstChild;c;c=box(c).next) {
                Box &ch=box(c);
                if(ch.style.display==CssStyle::D_NONE)continue;
                if(ch.anonymous && ch.style.pre!=1) {
                    bool blank=true;
                    for(uint32_t k=ch.firstItem;k<ch.endItem;++k) {
                        const Item &it=item(k);
                        for(uint32_t j=0;j<it.len;++j)if(!isSpace(d.text(it.off)[j])){blank=false;break;}
                    }
                    if(blank)continue;
                }
                int child=intrinsic(c,ref)+ch.margin[1]+ch.margin[3];
                if(column)extent=maxi(extent,child);else extent+=child;
                ++count;
            }
            if(!column)extent+=maxi(0,count-1)*length(b.style.box.gap[1],ref);
            return extent+extra(b);
        }
        int n=0;
        for(uint32_t k=b.firstItem;k<b.endItem;++k) {
            const Item &it=item(k);
            if(it.kind==Item::TEXT && !(it.style&ST_BUTTON_EDGE)) n+=int(it.len)*cw*(b.big?2:1);
            if(it.kind==Item::IMG && it.img && d.imageCount()>=it.img) {
                const Image &im=((const Image *)d.images_.data)[it.img-1];
                if(im.w) {n+=im.w; k+=it.off;}
            }
            if(n>8192) {n=8192;break;}
        }
        return maxi(extra(b),n+extra(b));
    }
    void appendLine(Line &l) {
        if(!d.lines_.append(&l,sizeof(l))) d.oom_=true;
    }
    void fragment(const Item &it,uint32_t off,int n,Flow &f,bool big) {
        if(n<=0) return;
        int cell=cw*(big?2:1), height=lh*(big?2:1);
        Run r{};r.off=off;r.len=(uint16_t)n;r.x=f.x;r.style=it.style;
        r.fg=it.fg;r.bg=it.bg;r.link=it.link;
        r.fgRgb=it.fgRgb;r.bgRgb=it.bgRgb;
        Line l{};l.firstRun=(uint32_t)(d.runs_.len/sizeof(Run));l.nRuns=1;
        l.big=big;l.row=f.y;l.pixelHeight=height;l.link=-1;
        if(!d.runs_.append(&r,sizeof(r))) d.oom_=true;
        appendLine(l);f.x+=n*cell;f.height=maxi(f.height,height);
    }
    void text(const Item &it,Flow &f,const Box &b) {
        if (it.style&ST_BUTTON_EDGE) return;
        const char *s=d.text(it.off);uint32_t i=0;
        int cell=cw*(b.big?2:1),height=lh*(b.big?2:1);
        int controlIndex=d.linkControl(it.link);
        bool pre=b.style.pre==1;
        bool control=(it.style&ST_CTRL)!=0 && (controlIndex<0 || !d.control(controlIndex).isButton());
        if(control) {
            if(f.x>f.left && f.x+(int)it.len*cell>f.left+f.width) f.newline();
            int ci=d.linkControl(it.link);
            int n=(int)it.len;
            if(ci>=0 && !d.control(ci).isButton())n=mini(n,maxi(1,f.width/cell));
            fragment(it,it.off,n,f,b.big);return;
        }
        while(i<it.len && !d.oom_) {
            if(s[i]=='\n' || s[i]=='\r') {
                if(pre) f.newline(height); else f.space=true;
                ++i;continue;
            }
            if(s[i]==' ' || s[i]=='\t') {
                if(pre) {fragment(it,it.off+i,1,f,b.big);}
                else f.space=true;
                ++i;continue;
            }
            uint32_t j=i;while(j<it.len && s[j]!=' ' && s[j]!='\t' && s[j]!='\n' && s[j]!='\r')++j;
            int word=int(j-i),space=f.space && f.x>f.left?cell:0;
            if(!pre && f.x>f.left && f.x+space+word*cell>f.left+f.width) f.newline();
            else f.x+=space;
            f.space=false;
            while(i<j) {
                int room=pre?480:maxi(1,(f.left+f.width-f.x)/cell);
                int n=mini(480,mini(room,int(j-i)));
                fragment(it,it.off+i,n,f,b.big);i+=(uint32_t)n;
                if(!pre && i<j) f.newline();
            }
        }
    }
    void picture(const Item &it,Flow &f,Box &b) {
        if(!it.img || d.imageCount()<it.img)return;
        const Image &im=((const Image *)d.images_.data)[it.img-1];
        if(!im.w || !im.h)return;
        int w=b.style.box.width==BoxStyle::Auto?mini(im.w,f.width):f.width;
        int h=b.definiteHeight?b.contentH:w*im.h/im.w;
        if(f.x>f.left && f.x+w>f.left+f.width)f.newline();
        Line l{};l.row=f.y;l.pixelHeight=maxi(1,h);l.img=it.img;l.imgX=(uint16_t)maxi(0,f.x);
        l.imgW=(uint16_t)mini(65535,maxi(1,w));l.imgH=(uint16_t)mini(65535,maxi(1,h));l.link=it.link;
        appendLine(l);f.x+=w;f.height=maxi(f.height,h);
    }
    void move(uint32_t i,int dx,int dy) {
        Box &b=box(i);
        for(uint32_t k=i;k<b.endBox;++k) {box(k).x+=dx;box(k).y+=dy;}
        for(uint32_t k=b.firstLine;k<b.endLine;++k) {
            Line &l=((Line *)d.lines_.data)[k];l.row+=dy;
            if(l.img) l.imgX=(uint16_t)maxi(0,int(l.imgX)+dx);
            for(uint32_t r=0;r<l.nRuns;++r)((Run *)d.runs_.data)[l.firstRun+r].x+=dx;
        }
    }
    void alignText(Box &b,int left,int available) {
        if(b.style.align<=0)return;
        // Multiple styled fragments on the same baseline move together.
        for(uint32_t k=b.firstLine;k<b.endLine;) {
            bool inBlock=false;
            for(uint32_t c=b.firstChild;c;c=box(c).next) {
                const Box &child=box(c);
                if(child.block && k>=child.firstLine && k<child.endLine) {
                    k=child.endLine;inBlock=true;break;
                }
            }
            if(inBlock)continue;
            Line &l=((Line *)d.lines_.data)[k];int end=left;uint32_t j=k;
            while(j<b.endLine && d.line(j).row==l.row) {
                const Line &a=d.line(j);
                if(a.img)end=maxi(end,a.imgX+a.imgW);
                for(uint32_t r=0;r<a.nRuns;++r) {const Run &u=d.run(a.firstRun+r);end=maxi(end,u.x+u.len*cw*(a.big?2:1));}
                ++j;
            }
            int dx=maxi(0,available-(end-left));if(b.style.align==1)dx/=2;
            for(uint32_t n=k;n<j;++n) {
                Line &a=((Line *)d.lines_.data)[n];if(a.img)a.imgX=(uint16_t)(a.imgX+dx);
                for(uint32_t r=0;r<a.nRuns;++r)((Run *)d.runs_.data)[a.firstRun+r].x+=dx;
            }
            // Inline descendants' geometry follows its rendered fragments.
            for(uint32_t n=b.firstChild;n && n<b.endBox;++n) {
                Box &child=box(n);
                if(!child.block && child.y==l.row)child.x+=dx;
            }
            k=j;
        }
    }
    void inlineBox(uint32_t i,Flow &f) {
        Box &b=box(i);b.firstLine=(uint32_t)d.lineCount();edges(b,f.width);
        int sx=f.x,sy=f.y;
        f.x+=b.margin[3]+b.padding[3]+b.border[3];
        sequence(i,f);
        b.x=sx;b.y=sy;b.w=maxi(0,f.x-sx);b.h=maxi(0,f.bottom()-sy);
        b.endLine=(uint32_t)d.lineCount();
        if(b.endLine>b.firstLine) {
            int left=0x3fffffff,top=0x3fffffff,right=0,bottom=0;
            for(uint32_t k=b.firstLine;k<b.endLine;++k) {
                const Line &l=d.line(k);top=mini(top,l.row);bottom=maxi(bottom,l.row+l.height());
                if(l.img){left=mini(left,l.imgX);right=maxi(right,l.imgX+l.imgW);}
                for(uint32_t r=0;r<l.nRuns;++r){const Run &u=d.run(l.firstRun+r);left=mini(left,u.x);right=maxi(right,u.x+u.len*cw*(l.big?2:1));}
            }
            if(left!=0x3fffffff){b.x=left;b.y=top;b.w=right-left;b.h=bottom-top;}
        }
        b.contentW=b.w;b.contentH=b.h;
        b.x-=b.padding[3]+b.border[3];b.y-=b.padding[0]+b.border[0];
        b.w+=extra(b);b.h+=vertical(b);
        f.x+=b.padding[1]+b.border[1]+b.margin[1];
        b.clientW=b.scrollW=b.w-b.border[1]-b.border[3];
        b.clientH=b.scrollH=b.h-b.border[0]-b.border[2];
    }
    void sequence(uint32_t i,Flow &f) {
        Box &b=box(i);uint32_t child=b.firstChild,k=b.firstItem;
        while((k<b.endItem || child) && !d.oom_) {
            if(child && box(child).firstItem<=k) {
                uint32_t ci=child;child=box(ci).next;Box &c=box(ci);
                if(c.style.display==CssStyle::D_NONE) {k=maxi((int)k,(int)c.endItem);continue;}
                if(c.block || c.style.display==CssStyle::D_FLEX) {
                    if(f.height || f.x>f.left)f.newline();
                    layoutBox(ci,f.left,f.y,f.width);
                    f.y=maxi(f.y,c.y+c.h+c.margin[2]);f.x=f.left;
                } else if(c.atomic) {
                    edges(c,f.width);
                    int w=intrinsic(ci,f.width)+c.margin[1]+c.margin[3];
                    if(f.x>f.left && f.x+w>f.left+f.width)f.newline();
                    layoutBox(ci,f.x,f.y,f.width,mini(maxi(0,w-c.margin[1]-c.margin[3]),f.width));
                    f.x=c.x+c.w+c.margin[1];f.height=maxi(f.height,c.margin[0]+c.h+c.margin[2]);
                } else inlineBox(ci,f);
                k=maxi((int)k,(int)c.endItem);continue;
            }
            if(k>=b.endItem)break;
            const Item &it=item(k++);
            switch(it.kind) {
            case Item::TEXT:text(it,f,b);break;
            case Item::BR:f.newline(lh*(b.big?2:1));break;
            case Item::HR: {
                if(f.height)f.newline();
                Line l{};l.hr=1;l.row=f.y;l.pixelHeight=1;l.link=-1;
                l.imgX=(uint16_t)maxi(0,f.left);l.imgW=(uint16_t)mini(65535,f.width);appendLine(l);f.y+=1;break;
            }
            case Item::IMG: {
                const Image *im=it.img && d.imageCount()>=it.img?((const Image *)d.images_.data)+it.img-1:nullptr;
                if(im && im->w && im->h){picture(it,f,b);k+=it.off;}break;
            }
            case Item::BLOCK:
                if(it.len){Item marker=it;marker.kind=Item::TEXT;text(marker,f,b);f.space=true;}
                break;
            }
        }
    }
    struct FlexItem {uint32_t box;int basis,size,main,cross;};
    void flex(uint32_t i,Flow &f,int fixedHeight) {
        Box &b=box(i);bool column=b.style.box.direction!=0;
        int gap=length(b.style.box.gap[column?0:1],f.width),crossGap=length(b.style.box.gap[column?1:0],f.width);
        if(column && fixedHeight<0) {
            int y=f.top;
            for(uint32_t c=b.firstChild;c;c=box(c).next) {
                if(box(c).style.display==CssStyle::D_NONE)continue;
                layoutBox(c,f.left,y,f.width);
                int align=box(c).style.box.self>=0?box(c).style.box.self:b.style.box.align;
                int spare=maxi(0,f.width-box(c).w-box(c).margin[1]-box(c).margin[3]);
                if(align==1 || align==2)move(c,align==1?spare/2:spare,0);
                y=box(c).y+box(c).h+box(c).margin[2]+(box(c).next?gap:0);
            }
            f.y=y;return;
        }
        Buf list{true};
        for(uint32_t c=b.firstChild;c;c=box(c).next) {
            Box &ch=box(c);
            if(ch.style.display==CssStyle::D_NONE)continue;
            if(ch.anonymous && ch.style.pre!=1) {
                bool blank=true;
                for(uint32_t k=ch.firstItem;k<ch.endItem;++k) {
                    const Item &it=item(k);
                    for(uint32_t j=0;j<it.len;++j)if(!isSpace(d.text(it.off)[j])){blank=false;break;}
                }
                if(blank)continue;
            }
            edges(ch,f.width);FlexItem v{};v.box=c;
            int raw=column?length(ch.style.box.height,fixedHeight,lh):intrinsic(c,f.width);
            v.basis=maxi(0,length(ch.style.box.basis,column?fixedHeight:f.width,raw));
            if(ch.style.box.basis!=BoxStyle::Auto && !ch.style.box.sizing)v.basis+=column?vertical(ch):extra(ch);
            v.size=v.basis;
            v.main=ch.margin[column?0:3]+ch.margin[column?2:1];
            v.cross=ch.margin[column?3:0]+ch.margin[column?1:2];
            if(!list.append(&v,sizeof(v))){d.oom_=true;return;}
        }
        FlexItem *a=(FlexItem *)list.data;int n=(int)(list.len/sizeof(FlexItem));
        int available=column?fixedHeight:f.width,cross=0;
        for(int start=0;start<n;) {
            int end=start,used=0,grow=0;int64_t shrink=0;
            while(end<n) {
                int need=a[end].basis+a[end].main+(end>start?gap:0);
                if(!column && b.style.box.wrap && end>start && used+need>available)break;
                used+=need;grow+=box(a[end].box).style.box.grow;
                shrink+=(int64_t)a[end].basis*box(a[end].box).style.box.shrink;++end;
            }
            int free=available>0?available-used:0;
            int final=gap*maxi(0,end-start-1);
            int64_t remainder=0;
            for(int j=start;j<end;++j) {
                Box &ch=box(a[j].box);int size=a[j].basis;
                // Carry fractional pixels so many small bars still fill the row.
                if(free>0 && grow) {
                    int64_t share=(int64_t)free*ch.style.box.grow+remainder;
                    size+=int(share/grow);remainder=share%grow;
                }
                if(free<0 && shrink) {
                    int64_t share=(int64_t)free*a[j].basis*ch.style.box.shrink+remainder;
                    size+=int(share/shrink);remainder=share%shrink;
                }
                int ex=column?vertical(ch):extra(ch);
                size=constrain(size-(ch.style.box.sizing?0:ex),column?ch.style.box.minHeight:ch.style.box.minWidth,
                               column?ch.style.box.maxHeight:ch.style.box.maxWidth,available)+(ch.style.box.sizing?0:ex);
                a[j].size=maxi(ex,size);final+=a[j].size+a[j].main;
            }
            int spare=maxi(0,available-final),offset=0,spacing=gap,count=end-start;
            switch(b.style.box.justify) {
            case 1:offset=spare/2;break;case 2:offset=spare;break;
            case 3:if(count>1)spacing+=spare/(count-1);break;
            case 4:spacing+=spare/count;offset=spare/count/2;break;
            case 5:spacing+=spare/(count+1);offset=spare/(count+1);break;
            }
            int position=offset,maxCross=0;
            for(int j=start;j<end;++j) {
                Box &ch=box(a[j].box);
                if(column)layoutBox(a[j].box,f.left,f.top+position,f.width,-1,a[j].size);
                else layoutBox(a[j].box,f.left+position,f.top+cross,a[j].size+a[j].main,a[j].size);
                maxCross=maxi(maxCross,(column?ch.w:ch.h)+a[j].cross);position+=a[j].size+a[j].main+spacing;
            }
            if(!column && fixedHeight>0 && !b.style.box.wrap)maxCross=maxi(maxCross,fixedHeight);
            for(int j=start;j<end;++j) {
                Box &ch=box(a[j].box);int align=ch.style.box.self>=0?ch.style.box.self:b.style.box.align;
                int slack=maxi(0,(column?f.width:maxCross)-(column?ch.w:ch.h)-a[j].cross);
                if(align==1 || align==2)move(a[j].box,column?(align==1?slack/2:slack):0,column?0:(align==1?slack/2:slack));
                if(align==3 && (column?ch.style.box.width:ch.style.box.height)==BoxStyle::Auto) {
                    if(column){ch.w+=slack;ch.clientW+=slack;ch.contentW+=slack;ch.scrollW=maxi(ch.scrollW,ch.clientW);}
                    else {ch.h+=slack;ch.clientH+=slack;ch.contentH+=slack;ch.scrollH=maxi(ch.scrollH,ch.clientH);}
                }
            }
            if(column)cross=maxi(cross,position-spacing);else cross+=maxCross+(end<n?crossGap:0);
            start=end;
        }
        f.y=f.top+cross;
    }
    void layoutBox(uint32_t i,int x,int y,int available,int forcedWidth=-1,int forcedHeight=-1) {
        Box &b=box(i);const BoxStyle &s=b.style.box;
        if (b.style.display==CssStyle::D_NONE) return;
        edges(b,available);
        int ex=extra(b),vy=vertical(b);
        int w=forcedWidth>=0?forcedWidth:s.width==BoxStyle::Auto?maxi(0,available-b.margin[1]-b.margin[3]):
              length(s.width,available)+(s.sizing?0:ex);
        w=constrain(w-(s.sizing?0:ex),s.minWidth,s.maxWidth,available)+(s.sizing?0:ex);
        b.w=maxi(ex,w);b.x=x+b.margin[3];b.y=y+b.margin[0];
        int spare=maxi(0,available-b.w-b.margin[1]-b.margin[3]);
        if(s.margin[3]==BoxStyle::Auto)b.x+=s.margin[1]==BoxStyle::Auto?spare/2:spare;
        b.contentW=maxi(0,b.w-ex);
        Flow f(b.x+b.border[3]+b.padding[3],b.y+b.border[0]+b.padding[0],b.contentW);
        b.firstLine=(uint32_t)d.lineCount();
        int heightRef=i==0?viewportHeight>0?viewportHeight:-1:box(b.parent).definiteHeight?box(b.parent).contentH:-1;
        int explicitHeight=forcedHeight>=0?maxi(0,forcedHeight-vy):length(s.height,heightRef,-1);
        if(forcedHeight<0 && explicitHeight>=0 && s.sizing)explicitHeight=maxi(0,explicitHeight-vy);
        b.definiteHeight=explicitHeight>=0 || (i==0 && viewportHeight>0);
        b.contentH=explicitHeight>=0?explicitHeight:i==0?viewportHeight:0;
        if(b.style.display==CssStyle::D_FLEX)flex(i,f,explicitHeight);
        else sequence(i,f);
        b.endLine=(uint32_t)d.lineCount();
        int natural=maxi(0,f.bottom()-f.top);
        b.contentH=maxi(0,constrain((explicitHeight>=0?explicitHeight:natural)+(s.sizing?vy:0),s.minHeight,s.maxHeight,heightRef)-(s.sizing?vy:0));
        b.h=b.contentH+vy;
        b.clientW=b.w-b.border[1]-b.border[3];b.clientH=b.h-b.border[0]-b.border[2];
        b.scrollW=b.clientW;b.scrollH=maxi(b.clientH,natural+b.padding[0]+b.padding[2]);
        for(uint32_t c=b.firstChild;c;c=box(c).next) {
            const Box &ch=box(c);
            b.scrollW=maxi(b.scrollW,ch.x+maxi(ch.w,ch.scrollW)-b.x-b.border[3]+b.padding[1]);
            b.scrollH=maxi(b.scrollH,ch.y+maxi(ch.h,ch.scrollH)-b.y-b.border[0]+b.padding[2]);
        }
        for(uint32_t k=b.firstLine;k<b.endLine;++k) {
            const Line &l=d.line(k);if(l.img)b.scrollW=maxi(b.scrollW,l.imgX+l.imgW-b.x);
            for(uint32_t r=0;r<l.nRuns;++r){const Run &u=d.run(l.firstRun+r);b.scrollW=maxi(b.scrollW,u.x+u.len*cw*(l.big?2:1)-b.x);}
        }
        if(b.style.display!=CssStyle::D_FLEX && !b.anonymous)alignText(b,f.left,b.contentW);
    }
public:
    PixelLayout(Document &doc,int w,int c,int h,int vh):d(doc),width(maxi(1,w)),cw(maxi(1,c)),lh(maxi(1,h)),viewportHeight(vh){}
    void run() {
        d.lines_.clear();d.runs_.clear();
        if(!d.boxCount()){d.rows_=0;return;}
        for(uint32_t k=0;k<d.boxCount();++k) {Box &b=box(k);b.x=b.y=b.w=b.h=b.contentW=b.contentH=b.clientW=b.clientH=b.scrollW=b.scrollH=0;}
        layoutBox(0,0,0,width,width);
        d.rows_=maxi(box(0).h,box(0).scrollH);
    }
};

void Document::layoutPixels(int width,int cellWidth,int lineHeight,int viewportHeight) {
    pixels_=true;cellPx_=cellWidth;rowPx_=lineHeight;
    PixelLayout l(*this,width,cellWidth,lineHeight,viewportHeight);l.run();cols_=width;
}
const Box *Document::boxForNode(uint32_t uid) const {
    if(!uid)return nullptr;
    for(size_t i=1;i<boxCount();++i)if(box(i).uid==uid)return &box(i);
    return nullptr;
}
const Box *Document::boxForId(const char *id) const {
    for(size_t i=1;i<boxCount();++i)if(!strcmp(str(box(i).id),id))return &box(i);
    return nullptr;
}
int Document::pixelLinkAt(int x,int y) const {
    for(size_t i=boxCount();i>1;--i) {
        const Box &b=box(i-1);
        if(b.link>=0 && x>=b.x && x<b.x+b.w && y>=b.y && y<b.y+b.h)return b.link;
    }
    for(size_t i=lineCount();i>0;--i) {
        const Line &l=line(i-1);if(y<l.row || y>=l.row+l.height())continue;
        if(l.img && x>=l.imgX && x<l.imgX+l.imgW)return l.link;
        for(uint32_t r=0;r<l.nRuns;++r) {const Run &u=run(l.firstRun+r);if(x>=u.x && x<u.x+u.len*cellPx_*(l.big?2:1))return u.link;}
    }
    return -1;
}
bool Document::pixelLinkPoint(int link,int &x,int &y) const {
    for(size_t i=1;i<boxCount();++i)if(box(i).link==link && box(i).w && box(i).h){x=box(i).x;y=box(i).y;return true;}
    for(size_t i=0;i<lineCount();++i) {
        const Line &l=line(i);if(l.img && l.link==link){x=l.imgX;y=l.row;return true;}
        for(uint32_t r=0;r<l.nRuns;++r)if(run(l.firstRun+r).link==link){x=run(l.firstRun+r).x;y=l.row;return true;}
    }
    return false;
}
} // namespace web
