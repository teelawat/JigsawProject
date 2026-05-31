import os
from PIL import Image, ImageChops

# Resolve paths relative to this script's directory
script_dir = os.path.dirname(os.path.abspath(__file__))
repo_dir = os.path.dirname(script_dir)
test_images_dir = os.path.join(repo_dir, "test_images")

o = Image.open(os.path.join(test_images_dir, "test.png"))
e = Image.open(os.path.join(test_images_dir, "encrypted.png"))
r = Image.open(os.path.join(test_images_dir, "restored.png"))
w = Image.open(os.path.join(test_images_dir, "wrong_seed.png"))

def count_diff(a, b):
    d = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
    return sum(1 for p in d.getdata() if p[0] > 0 or p[1] > 0 or p[2] > 0)

total = o.width * o.height
de = count_diff(o, e)
dr = count_diff(o, r)
dw = count_diff(o, w)

print("=== PNG Lossless Pixel Comparison ===")
print("Total pixels   :", total)
print("Encrypted diff :", de, "(%.1f%%)" % (de/total*100))
print("Restored  diff :", dr, "(%.4f%%) <- MUST BE 0" % (dr/total*100))
print("WrongSeed diff :", dw, "(%.1f%%)" % (dw/total*100))
print()
if dr == 0:
    print("PASS - Algorithm is 100% correct! Encrypt+Decrypt = identity")
else:
    print("FAIL - %d pixels differ after decrypt" % dr)
