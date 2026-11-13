from PIL import Image, ImageDraw, ImageFilter
import math
S=1024
def squircle_mask(s, r):
    m=Image.new('L',(s,s),0); d=ImageDraw.Draw(m); d.rounded_rectangle([0,0,s-1,s-1],radius=r,fill=255); return m
# фон: диагональный градиент
bg=Image.new('RGB',(S,S))
c1=(38,30,86); c2=(14,58,74)
px=bg.load()
for y in range(S):
    for x in range(S):
        t=(x+y)/(2*S)
        px[x,y]=tuple(int(c1[i]*(1-t)+c2[i]*t) for i in range(3))
img=Image.new('RGBA',(S,S),(0,0,0,0))
img.paste(bg,(0,0),squircle_mask(S,230))
# лёгкое свечение сверху
glow=Image.new('RGBA',(S,S),(0,0,0,0)); g=ImageDraw.Draw(glow)
g.ellipse([S*0.15,S*0.05,S*0.85,S*0.75],fill=(143,128,255,70))
glow=glow.filter(ImageFilter.GaussianBlur(120))
img=Image.alpha_composite(img,Image.composite(glow,Image.new('RGBA',(S,S),(0,0,0,0)),squircle_mask(S,230)))
d=ImageDraw.Draw(img)
# энсо — незамкнутый круг
cx,cy,R=S/2,S*0.52,S*0.33
w=34
for i in range(0,300):
    a=math.radians(-60+i*1.0)
    k=0.55+0.45*math.sin(i/300*math.pi)
    x=cx+R*math.cos(a); y=cy+R*math.sin(a)
    rr=w*k/2
    d.ellipse([x-rr,y-rr,x+rr,y+rr],fill=(232,228,255,int(200*k)))
# пилюля-остров
pw,ph=S*0.56,S*0.2
x0,y0=cx-pw/2,cy-ph/2
shadow=Image.new('RGBA',(S,S),(0,0,0,0)); sd=ImageDraw.Draw(shadow)
sd.rounded_rectangle([x0,y0+18,x0+pw,y0+ph+18],radius=ph/2,fill=(0,0,0,150))
shadow=shadow.filter(ImageFilter.GaussianBlur(24))
img=Image.alpha_composite(img,shadow); d=ImageDraw.Draw(img)
d.rounded_rectangle([x0,y0,x0+pw,y0+ph],radius=ph/2,fill=(6,6,10,255),outline=(255,255,255,40),width=4)
# точка-акцент и полоска
dr=ph*0.26
d.ellipse([x0+ph*0.5-dr,cy-dr,x0+ph*0.5+dr,cy+dr],fill=(150,135,255,255))
d.rounded_rectangle([x0+ph*0.95,cy-ph*0.09,x0+pw-ph*0.5,cy+ph*0.09],radius=ph*0.09,fill=(240,240,245,255))
img.save('logo1024.png')
sizes=[16,20,24,32,40,48,64,128,256]
ims=[img.resize((s,s),Image.LANCZOS) for s in sizes]
ims[-1].save('island.ico',sizes=[(s,s) for s in sizes],append_images=ims[:-1])
img.resize((256,256),Image.LANCZOS).save('logo256.png')
img.resize((512,512),Image.LANCZOS).save('logo512.png')
print('ok')
