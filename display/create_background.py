from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
SCALE = 3

def build():
    """生成英文屏幕使用的静态背景，不合成模拟器截图。"""
    image = Image.new('RGB', (240*SCALE, 320*SCALE), (11,18,30))
    draw = ImageDraw.Draw(image)
    def text(x,y,value,size,color=(145,163,184),bold=False):
        font = ImageFont.truetype('C:/Windows/Fonts/arialbd.ttf' if bold else 'C:/Windows/Fonts/arial.ttf',size*SCALE)
        draw.text((x*SCALE,y*SCALE),value,font=font,fill=color)
    for box in [(12,34,228,96),(12,102,228,160),(12,166,228,206),(12,212,228,270)]:
        draw.rounded_rectangle(tuple(v*SCALE for v in box),12*SCALE,fill=(23,34,51))
    text(16,9,'Device Status',18,(233,241,250),True)
    text(194,15,'Local',10)
    text(24,38,'Temperature',11)
    text(191,62,'°C',21,(57,211,231))
    text(24,106,'Elapsed Time',11)
    text(24,169,'Stage',10)
    text(20,219,'Upper',11)
    text(20,246,'Lower',11)
    text(16,278,'Pump/Valve',11)
    image.resize((240,320),Image.Resampling.LANCZOS).save(ROOT/'assets/background.png')

if __name__ == '__main__':
    build()
