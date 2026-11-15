"""Render actual production-WARP response CSVs as scientific L/S figures."""
from pathlib import Path
import argparse
import csv
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

parser = argparse.ArgumentParser()
parser.add_argument("detail_csv", type=Path)
parser.add_argument("temporal_csv", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False
names = ["Residual 总强度 1.5", "色度变化强度 1.5", "明度变化强度 1.5", "阴影／结构 1.5",
         "反射／光晕 1.5", "色相变化保护 1", "暗部保护 1", "高光保护 1", "局部失控压缩 1",
         "大范围修正强度 1.5", "细节修正强度 1.5"]
rows = list(csv.DictReader(args.detail_csv.open(encoding="utf-8-sig")))
fig, axes = plt.subplots(3,4,figsize=(17,12),constrained_layout=True)
for control, ax in enumerate(axes.flat):
    if control >= len(names):
        ax.axis("off")
        ax.text(.02,.9,"生产 HLSL · D3D11 WARP\nHue=220°，固定混合 RGB 残差\n\n横轴：原图 HSL 饱和度\n纵轴：原图 HSL 明度\n颜色：相对中性 NR 输出的\n最大 RGB 变化（8-bit 色阶）\n\n这里只定义原图扫描坐标，\n实际控制使用 Oklab。\n不同残差／Hue 会改变响应。",va="top",fontsize=13)
        continue
    data = [r for r in rows if int(r["control"]) == control]
    z = np.array([float(r["delta"])*255 for r in data]).reshape(33,33)
    im = ax.imshow(z,origin="lower",extent=(0,1,0,1),aspect="auto",cmap="magma",vmin=0)
    ax.set(title=names[control],xlabel="原图饱和度 S",ylabel="原图明度 L")
    fig.colorbar(im,ax=ax,fraction=.045,pad=.025).set_label("色阶变化",fontsize=9)
fig.suptitle("069 DLSSNR：每项控制在明度／饱和度坐标上的像素响应",fontsize=20)
fig.savefig(args.output/"detail-response.png",dpi=150)
plt.close(fig)

rows = list(csv.DictReader(args.temporal_csv.open(encoding="utf-8-sig")))
fig, axes = plt.subplots(1,3,figsize=(15,5),constrained_layout=True)
for strength, ax in zip((0,.5,1),axes):
    data=[r for r in rows if float(r["strength"])==strength]
    z=np.array([float(r["delta"])*255 for r in data]).reshape(16,16)
    im=ax.imshow(z,origin="lower",extent=(0,1,0,1),aspect="auto",cmap="magma",vmin=0,vmax=3)
    ax.set(title=f"色度时域稳定 {strength:g}",xlabel="原图饱和度 S",ylabel="原图明度 L")
    fig.colorbar(im,ax=ax,fraction=.045,pad=.025).set_label("相对既有 Anti-Flicker 的色阶变化")
fig.suptitle("生产时域 HLSL · 固定对齐历史与置信度 · 80% 历史权重\n边缘历史拒绝仍生效；实际运动／遮挡会改变响应",fontsize=16)
fig.savefig(args.output/"temporal-response.png",dpi=150)
plt.close(fig)
print("Production response figures rendered.")
