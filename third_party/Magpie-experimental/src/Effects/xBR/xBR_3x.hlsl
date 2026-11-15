//!MAGPIE EFFECT
//!VERSION 4

//!PARAMETER
//!LABEL Y Weight
//!DEFAULT 48
//!MIN 0
//!MAX 100
//!STEP 1
float paramXbrYWeight;

//!PARAMETER
//!LABEL Eq Threshold
//!DEFAULT 15
//!MIN 0
//!MAX 50
//!STEP 1
float paramXbrEqThreshold;

//!PARAMETER
//!LABEL Lv2 Coefficient
//!DEFAULT 2
//!MIN 1
//!MAX 3
//!STEP 0.1
float paramXbrLv2Coefficient;

//!PARAMETER
//!LABEL Preserve Small Details
//!DEFAULT 0
//!OPTION 0 Off
//!OPTION 1 On
int paramPreserveSmallDetails;

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH * 3
//!HEIGHT INPUT_HEIGHT * 3
Texture2D OUTPUT;

//!SAMPLER
//!FILTER POINT
//!ADDRESS CLAMP
SamplerState sam;

//!PASS 1
//!IN INPUT
//!OUT OUTPUT
//!BLOCK_SIZE 8
//!NUM_THREADS 64

#define XBR_SCALE 3.0
#include "xBR_LV2_Common.hlsli"
