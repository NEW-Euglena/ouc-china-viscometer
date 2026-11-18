from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
SCALE = 3

def build():
    """生成英文静态背景及默认布局示意图，不使用桌面截屏。"""
    image = Image.new('RGB', (240*SCALE, 320*SCALE), (11,18,30))
    draw = ImageDraw.Draw(image)
    def text(x,y,value,size,color=(145,163,184),bold=False):
        font = ImageFont.truetype('C:/Windows/Fonts/arialbd.ttf' if bold else 'C:/Windows/Fonts/arial.ttf',size*SCALE)
        draw.text((x*SCALE,y*SCALE),value,font=font,fill=color)
    for box in [(12,34,156,96),(12,102,156,160),(12,166,228,206),(12,212,228,270)]:
        draw.rounded_rectangle(tuple(v*SCALE for v in box),12*SCALE,fill=(23,34,51))
    text(16,9,'LIQUID TIMER',18,(233,241,250),True)
    text(24,38,'TEMP (°C)',11)
    text(24,106,'TIME (s)',11)
    text(24,169,'STATUS',10)
    text(20,219,'UPPER',11)
    text(20,246,'LOWER',11)
    text(16,278,'PUMP/B',11)
    text(16,301,'SENSOR',11)
    image.resize((240,320),Image.Resampling.LANCZOS).save(ROOT/'assets/background.png')
    draw.rectangle((164*SCALE,34*SCALE,228*SCALE-1,160*SCALE-1),fill=(16,162,74))
    font = ImageFont.truetype('C:/Windows/Fonts/arial.ttf',16*SCALE)
    box = draw.textbbox((0,0),'START',font=font)
    draw.text((196*SCALE-(box[2]-box[0])/2,97*SCALE-(box[3]-box[1])/2-box[1]),
              'START',font=font,fill=(255,255,255))
    text(24,50,'--.--',40,(57,211,231))
    text(24,124,'--.---',24,(233,241,250),True)
    text(24,181,'NO DATA',16)
    text(76,217,'NO DATA',16)
    text(76,244,'NO DATA',16)
    text(76,275,'--',16)
    text(76,298,'NO DATA',16)
    image.resize((240,320),Image.Resampling.LANCZOS).save(ROOT/'status_ui_preview.png')

if __name__ == '__main__':
    build()
