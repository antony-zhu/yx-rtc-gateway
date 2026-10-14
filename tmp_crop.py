from PIL import Image
im=Image.open(r"D:\codex\yx_gateway\sch4hi-4.png")
print(im.size)
im.crop((0,100,1700,1700)).save(r"D:\codex\yx_gateway\sch4crop.png")
