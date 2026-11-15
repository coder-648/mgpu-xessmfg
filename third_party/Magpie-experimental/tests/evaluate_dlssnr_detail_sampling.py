"""Offline interpolation/quantization comparisons; no product shader change."""
import json
import sys
from pathlib import Path
import numpy as np

def kernel(x):
    x = np.abs(x)
    return np.where(x < 1, (1.5*x-2.5)*x*x+1,
                    np.where(x < 2, ((-.5*x+2.5)*x-4)*x+2, 0))

t = np.linspace(0, 1, 10001)
weights = np.stack([kernel(t-i) for i in (-1,0,1,2)], axis=1)
weights /= weights.sum(axis=1, keepdims=True)
low_side = weights @ np.array([0,0,0,.05])
high_side = weights @ np.array([0,.05,.05,.05])
catmull_min = float(low_side.min())
catmull_max = float(high_side.max())
# Spatially stable 4x4 Bayer: zero mean before final quantization; no frame seed.
bayer = np.array([[0,8,2,10],[12,4,14,6],[3,11,1,9],[15,7,13,5]])
dither = (bayer+.5)/16-.5
gradient = np.broadcast_to(np.linspace(10,20,1024), (64,1024))
offset = np.tile(dither, (16,256))
plain = np.floor(gradient+.5)
ordered = np.floor(gradient+offset+.5)
result = {
    "evidence": "Offline float64, synthetic step/gradient; not GPU or perceived video quality",
    "catmullStepMin": catmull_min, "catmullStepMax": catmull_max,
    "catmullOvershoot8bitCodes": max(-catmull_min,catmull_max-.05)*255,
    "linearStepOvershoot": 0,
    "ditherMeanBeforeQuantization": float(offset.mean()),
    "plainQuantizationRMSErrorCodes": float(np.sqrt(np.mean((plain-gradient)**2))),
    "orderedQuantizationRMSErrorCodes": float(np.sqrt(np.mean((ordered-gradient)**2))),
    "decision": "Retain Catmull-Rom and no added dither; compare real content before changing defaults",
}
Path(sys.argv[1]).write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
print(json.dumps(result,ensure_ascii=False))
