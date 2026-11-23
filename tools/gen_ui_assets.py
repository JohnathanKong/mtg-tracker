#!/usr/bin/env python3
"""Build clean, native-resolution MTG Knob HUD artwork.

No screenshots, sample commander art, sampled labels, external image/font
packages, or runtime scaling. All decorative pieces are independently drawn
at the exact dimensions used by the 360x360 SH8601 firmware.

Run: python3 tools/gen_ui_assets.py main
"""
from __future__ import annotations
import math
import pathlib
import struct
import sys
import zlib

SIZE = 360
TAU = math.tau


def cap(value, low=0.0, high=1.0):
    return max(low, min(high, value))


def rgbmix(a, b, t):
    return tuple(int(a[i] * (1-t) + b[i] * t) for i in range(3))


GOLD = (222, 172, 87)
BRIGHT = (253, 221, 152)
DARK_GOLD = (99, 71, 42)
TEAL = (26, 99, 105)
IVORY = (243, 222, 190)


class Canvas:
    def __init__(self, width, height):
        self.w, self.h = width, height
        self.data = bytearray(width * height * 4)

    def over(self, x, y, rgb, alpha=255):
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return
        a = max(0, min(255, int(alpha)))
        if a == 0:
            return
        i = (y * self.w + x) * 4
        old = self.data[i+3]
        if a == 255 or old == 0:
            self.data[i:i+4] = bytes((*rgb, a))
            return
        out = a * 255 + old * (255-a)
        self.data[i] = min(255, (rgb[0]*a*255 + self.data[i]*old*(255-a)) // out)
        self.data[i+1] = min(255, (rgb[1]*a*255 + self.data[i+1]*old*(255-a)) // out)
        self.data[i+2] = min(255, (rgb[2]*a*255 + self.data[i+2]*old*(255-a)) // out)
        self.data[i+3] = min(255, (out+127)//255)

    def disc(self, cx, cy, radius, fill, opacity=255, border=None, thickness=1.25):
        xmin=max(0,int(cx-radius-3));xmax=min(self.w,int(cx+radius+4))
        ymin=max(0,int(cy-radius-3));ymax=min(self.h,int(cy+radius+4))
        for y in range(ymin,ymax):
            for x in range(xmin,xmax):
                d=math.hypot(x+.5-cx,y+.5-cy)-radius
                amount=cap(.75-d)
                if amount:
                    self.over(x,y,fill,int(opacity*amount))
                if border:
                    edge=cap(thickness/2+.55-abs(d+thickness/2))
                    if edge:
                        self.over(x,y,border,int(250*edge))

    def line(self, ax, ay, bx, by, thickness, color, opacity=255):
        pad=thickness+2
        xmin=max(0,int(min(ax,bx)-pad));xmax=min(self.w,int(max(ax,bx)+pad+2))
        ymin=max(0,int(min(ay,by)-pad));ymax=min(self.h,int(max(ay,by)+pad+2))
        dx,dy=bx-ax,by-ay
        length2=dx*dx+dy*dy
        for y in range(ymin,ymax):
            for x in range(xmin,xmax):
                t=cap(((x+.5-ax)*dx+(y+.5-ay)*dy)/length2) if length2 else 0
                distance=math.hypot(x+.5-(ax+t*dx),y+.5-(ay+t*dy))
                coverage=cap((thickness+1)*.5-distance)
                if coverage:
                    self.over(x,y,color,int(coverage*opacity))

    def curve(self, points, width, ink, alpha=255, glow=False):
        if glow:
            self.curve(points,width+3.8,ink,int(alpha*.14),False)
            self.curve(points,width+1.4,ink,int(alpha*.30),False)
        p0,p1,p2,p3=points
        samples=max(22,int(math.hypot(p3[0]-p0[0],p3[1]-p0[1])*.7))
        px,py=p0
        for step in range(1,samples+1):
            t=step/samples;u=1-t
            x=u*u*u*p0[0]+3*u*u*t*p1[0]+3*u*t*t*p2[0]+t*t*t*p3[0]
            y=u*u*u*p0[1]+3*u*u*t*p1[1]+3*u*t*t*p2[1]+t*t*t*p3[1]
            self.line(px,py,x,y,width,ink,alpha)
            px,py=x,y

    def rounded(self,x,y,w,h,r,fill1,fill2,border=GOLD,border_width=1.4,
                sheen=True,selected=False):
        left=max(0,int(x-3));right=min(self.w,int(x+w+3))
        top=max(0,int(y-3));bottom=min(self.h,int(y+h+3))
        for iy in range(top,bottom):
            ty=cap((iy-y)/max(1,h-1))
            for ix in range(left,right):
                qx=abs(ix+.5-(x+w/2))-(w/2-r)
                qy=abs(iy+.5-(y+h/2))-(h/2-r)
                dist=math.hypot(max(0,qx),max(0,qy))+min(max(qx,qy),0)-r
                coverage=cap(.6-dist)
                if coverage:
                    mix=ty*.93
                    color=rgbmix(fill1,fill2,mix)
                    # Glass glint near the top, built into the one frame.
                    if sheen and ty < .20:
                        color=rgbmix(color, (95,139,135) if selected else (79,102,97),
                                     .15*(1-ty/.20))
                    self.over(ix,iy,color,int(248*coverage))
                edge=cap(border_width/2+.6-abs(dist+border_width/2))
                if edge:
                    metal=rgbmix(BRIGHT if selected else (239,202,132),
                                 DARK_GOLD, ty*.82)
                    self.over(ix,iy,metal,int(245*edge))
        if sheen and w>25:
            self.line(x+r,y+2,x+w-r,y+2,.85,(250,218,151),140)

    def poly_fill(self, vertices, fill_top, fill_bottom, top_y, bottom_y, opacity=248):
        # Scanline fill of the exact curved outline; no square eraser masks.
        y0=max(0,int(min(p[1] for p in vertices)))
        y1=min(self.h,int(max(p[1] for p in vertices))+1)
        for y in range(y0,y1):
            py=y+.5
            xs=[]
            for i,p in enumerate(vertices):
                q=vertices[(i+1)%len(vertices)]
                if (p[1]<=py<q[1]) or (q[1]<=py<p[1]):
                    xs.append(p[0]+(py-p[1])*(q[0]-p[0])/(q[1]-p[1]))
            xs.sort()
            for k in range(0,len(xs)-1,2):
                l,r=xs[k],xs[k+1]
                tint=rgbmix(fill_top,fill_bottom,cap((y-top_y)/(bottom_y-top_y)))
                for x in range(max(0,int(l)-1),min(self.w,int(r)+2)):
                    a=cap(min(x+.8-l,r-(x-.2)))
                    if a:self.over(x,y,tint,int(opacity*a))


def bez(p0,p1,p2,p3,n=50):
    pts=[]
    for i in range(n+1):
        t=i/n;u=1-t
        pts.append((u**3*p0[0]+3*u*u*t*p1[0]+3*u*t*t*p2[0]+t**3*p3[0],
                    u**3*p0[1]+3*u*u*t*p1[1]+3*u*t*t*p2[1]+t**3*p3[1]))
    return pts


def jewel(canvas, x,y, radius=4, color=(68,195,195)):
    canvas.disc(x,y,radius+2,color,30)
    canvas.disc(x,y,radius,color,225,border=BRIGHT,thickness=1.0)
    canvas.line(x-1.5,y-2,x+.8,y-2,1,IVORY,215)


def filigree(c,x,y,mirror=False,scale=1):
    k=-1 if mirror else 1
    def conv(pts):
        return tuple((x+k*u*scale,y+v*scale) for u,v in pts)
    c.curve(conv(((0,0),(9,-15),(22,-17),(39,-9))),1.7,GOLD,glow=True)
    c.curve(conv(((7,-4),(9,-16),(20,-22),(27,-19))),.95,BRIGHT,190)
    c.curve(conv(((13,0),(19,9),(31,12),(40,3))),1.2,GOLD,180)
    c.curve(conv(((24,-11),(31,-15),(37,-9),(33,-3))),1.1,GOLD,210)
    c.curve(conv(((21,-9),(18,-1),(25,3),(30,2))),1.0,BRIGHT,170)
    jewel(c,x+k*40*scale,y-9*scale,2.1)


def hud():
    c=Canvas(SIZE,SIZE)
    # Opaque lower teal velvet, feathered into user-provided artwork.
    for y in range(151,354):
        fade=cap((y-150)/50)
        for x in range(10,350):
            d=math.hypot(x-180,y-180)
            if d>=171.7:continue
            outer=cap((171.6-d)*1.2)
            if y<=225:
                opacity=int(249*fade*outer)
            else:
                opacity=int(252*outer)
            if not opacity:continue
            nx=(x-180)/180;ny=(y-255)/130
            glimmer=max(0,1-math.hypot(nx,ny)/1.25)
            noise=((x*1747+y*3643+x*y*13)&31)-16
            col=(int(7+7*glimmer+noise*.13),
                 int(23+14*glimmer+noise*.17),
                 int(27+11*glimmer+noise*.12))
            c.over(x,y,col,opacity)
    # A warm metallic Casts / Tax rail. Life crest obscures only its center.
    c.rounded(27,163,306,35,16,(10,55,57),(8,25,32),(205,167,99))
    c.curve(((25,181),(42,155),(73,156),(106,162)),1.45,GOLD,210,True)
    c.curve(((254,162),(285,156),(318,155),(335,181)),1.45,GOLD,210,True)
    c.line(95,166,114,166,.9,BRIGHT,170)
    c.line(246,166,265,166,.9,BRIGHT,170)

    # Life crest: independent curvilinear shield shape, not a crop of old UI.
    verts=[]
    paths=[
        ((180,102),(152,98),(122,115),(110,152)),
        ((110,152),(107,170),(143,176),(180,194)),
        ((180,194),(217,176),(253,170),(250,152)),
        ((250,152),(238,115),(208,98),(180,102)),
    ]
    for seg in paths: verts.extend(bez(*seg,50)[:-1])
    c.poly_fill(verts,(8,47,51),(5,17,28),103,191)
    for seg in paths:
        c.curve(seg,2.0,BRIGHT,245,True)
    # Inner crest edges suggest engraved gold, not extra competing borders.
    c.curve(((124,156),(139,171),(159,174),(180,181)),.65,(136,119,75),160)
    c.curve(((180,181),(201,174),(221,171),(236,156)),.65,(136,119,75),160)
    jewel(c,180,191,3,(48,180,170))
    # Crown shoulder engravings emerge around the frame.
    filigree(c,66,152,False,.90)
    filigree(c,294,152,True,.90)
    c.curve(((28,187),(69,195),(100,197),(128,188)),1.05,(186,134,68),135)
    c.curve(((232,188),(260,197),(291,195),(332,187)),1.05,(186,134,68),135)

    # Commander damage title rail + three independent detachable plaques.
    c.rounded(27,200,307,52,18,(18,53,54),(12,28,36),(174,126,75),1.25)
    c.curve(((65,207),(105,194),(136,197),(154,201)),1.15,GOLD,195)
    c.curve(((206,201),(224,197),(255,194),(295,207)),1.15,GOLD,195)
    c.curve(((62,249),(114,258),(153,252),(180,255)),.8,(155,107,63),160)
    c.curve(((180,255),(207,252),(246,258),(298,249)),.8,(155,107,63),160)
    # Permanently visible buttons: no text baked in.
    for x,op in ((47,'-'),(313,'+')):
        c.disc(x,231,16,(9,41,46),247,border=(230,181,104),thickness=2.0)
        c.disc(x,231,12,(20,68,70),75)
        c.line(x-7,231,x+7,231,2.2,BRIGHT)
        if op=='+':c.line(x,224,x,238,2.2,BRIGHT)
    # Bottom footer filigree; all inside 171px circular safe radius.
    c.curve(((54,302),(82,329),(129,337),(163,323)),1.6,(171,114,63),175,True)
    c.curve(((306,302),(278,329),(231,337),(197,323)),1.6,(171,114,63),175,True)
    c.curve(((73,307),(103,322),(118,320),(143,315)),.8,BRIGHT,125)
    c.curve(((287,307),(257,322),(242,320),(217,315)),.8,BRIGHT,125)
    for x in (110,250):
        jewel(c,x,324,2.0,(37,130,121))
    # One single metallic perimeter ring at its final coordinate.
    for y in range(4,356):
        for x in range(4,356):
            d=math.hypot(x+.5-180,y+.5-180)
            if d<166.7 or d>175: continue
            bright=cap(1.2-abs(d-171.3))
            inner=cap(.75-abs(d-168.3))
            top=cap((225-y)/205)
            if inner:c.over(x,y,(115,76,39),int(inner*160))
            if bright:
                metal=rgbmix((205,130,57),(255,228,161),top)
                c.over(x,y,metal,int(bright*252))
    return c


def plaque(selected=False):
    c=Canvas(68,30)
    first=(88,36,45) if not selected else (23,92,102)
    last=(39,17,29) if not selected else (6,40,52)
    c.rounded(0.5,0.5,67,29,10,first,last,BRIGHT,1.6,True,selected)
    c.line(9,3,58,3,.75,BRIGHT,135)
    for x,k in ((7,1),(61,-1)):
        c.curve(((x,16),(x+k*5,7),(x+k*11,6),(x+k*11,4)),
                .8,GOLD,160)
        c.disc(x,19,1.5,(225,173,95),140)
    return c


def counter(tone='teal',selected=False):
    c=Canvas(106,25)
    if tone=='teal':
        first,last=(10,80,86),(4,34,44)
        metal=(84,216,214) if selected else (105,185,173)
    else:
        first,last=(103,76,33),(40,32,23)
        metal=(254,220,143) if selected else (220,171,87)
    c.rounded(0.5,0.5,105,24,10,first,last,metal,1.65,True,selected)
    c.line(14,3,91,3,.7,BRIGHT,85)
    c.line(37,6,37,19,.75,BRIGHT,115)
    for x in (9,97):
        c.disc(x,12,1.25,metal,160)
    return c


def pencil():
    c=Canvas(33,25)
    c.rounded(.5,.5,32,24,11,(15,65,65),(7,29,39),
              (228,186,105),1.5)
    # Pencil-in-page glyph, distinct from any down arrow.
    c.line(8,7,8,19,1.65,BRIGHT)
    c.line(8,19,22,19,1.65,BRIGHT)
    c.line(11,15,23,4,2.3,(254,233,169))
    c.line(13,18,25,7,2.1,(238,192,109))
    c.line(11,15,10,20,1.4,(253,231,169))
    c.line(10,20,13,18,1.5,(253,231,169))
    c.line(23,4,25,7,1.9,(255,237,179))
    return c


def editor():
    c=Canvas(288,244)
    c.rounded(.5,.5,287,243,18,(18,54,61),(8,21,30),
              (220,178,95),1.6)
    for x in (15,273):
        direction=1 if x==15 else -1
        c.curve(((x,36),(x+direction*4,16),
                 (x+direction*13,14),(x+direction*20,10)),
                1.1,GOLD,210,True)
        c.curve(((x,204),(x+direction*5,227),
                 (x+direction*15,230),(x+direction*24,232)),
                1.0,GOLD,210,True)
        jewel(c,x+direction*22,13,1.8)
    c.line(38,44,250,44,.7,GOLD,120)
    c.line(37,218,251,218,.7,GOLD,115)
    return c


def png_bytes(canvas):
    def chunk(t, payload):
        body=t+payload
        return struct.pack('!I',len(payload))+body+struct.pack('!I',zlib.crc32(body)&0xffffffff)
    raw=b''.join(b'\0'+bytes(canvas.data[y*canvas.w*4:(y+1)*canvas.w*4])
                 for y in range(canvas.h))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!2I5B',canvas.w,canvas.h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b'')


def editor_row(checked):
    c=Canvas(250,39)
    if checked:
        top,bottom=(20,85,85),(10,43,53)
        border=(122,220,184)
    else:
        top,bottom=(25,46,52),(9,24,33)
        border=(138,119,91)
    c.rounded(.5,.5,249,38,9,top,bottom,border,1.3)
    c.line(41,3,215,3,.65,BRIGHT,68 if checked else 37)
    # Restrained gilded leaves at both ends of each selectable row.
    for x,k in ((9,1),(241,-1)):
        c.curve(((x,28),(x+7*k,23),(x+9*k,13),(x+14*k,9)),
                .85,GOLD,120 if checked else 70)
    return c


def editor_checkbox(checked):
    c=Canvas(20,20)
    color=(21,88,81) if checked else (23,39,43)
    border=(130,232,193) if checked else (166,146,112)
    c.rounded(.5,.5,19,19,5,color,(8,29,35),border,1.5)
    if checked:
        c.line(4,10,8,14,2.4,(225,255,233))
        c.line(8,14,16,5,2.4,(225,255,233))
    return c


def main():
    out=pathlib.Path(sys.argv[1] if len(sys.argv)>1 else 'main')
    out.mkdir(parents=True,exist_ok=True)
    assets={
        'hud_chrome':hud(),
        'player_normal':plaque(),
        'player_selected':plaque(True),
        'counter_teal':counter('teal'),
        'counter_brass':counter('brass'),
        'counter_teal_selected':counter('teal',True),
        'counter_brass_selected':counter('brass',True),
        'pencil':pencil(),
        'editor':editor(),
        'editor_row_checked':editor_row(True),
        'editor_row_unchecked':editor_row(False),
        'checkbox_checked':editor_checkbox(True),
        'checkbox_unchecked':editor_checkbox(False),
    }
    for name,canvas in assets.items():
        (out/(name+'.rgba')).write_bytes(canvas.data)
    # QA preview; not embedded in the device binary.
    (out/'hud_chrome_preview.png').write_bytes(png_bytes(assets['hud_chrome']))
    print('Generated native-size layered MTG HUD: '+', '.join(assets))


if __name__=='__main__':
    main()
