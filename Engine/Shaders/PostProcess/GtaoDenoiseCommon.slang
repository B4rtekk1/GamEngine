// XeGTAO's edge-continuity 3x3 kernel. Shared by the standalone denoise pass
// and the fused half-resolution denoise/upsample pass so both stay identical.
float4 gtaoUnpackEdges(uint packed){return saturate(float4((packed>>6)&3,(packed>>4)&3,(packed>>2)&3,packed&3)/3.0);}
void gtaoAddSample(float value,float weight,inout float sum,inout float weightSum){sum+=weight*value;weightSum+=weight;}

// `ao` is the 3x3 neighbourhood in row-major order (index 4 is the centre).
float gtaoDenoise(float ao[9],uint edgeCenter,uint edgeLeft,uint edgeRight,uint edgeTop,uint edgeBottom,float blurAmount){
 float4 center=gtaoUnpackEdges(edgeCenter);
 float4 left=gtaoUnpackEdges(edgeLeft);
 float4 right=gtaoUnpackEdges(edgeRight);
 float4 top=gtaoUnpackEdges(edgeTop);
 float4 bottom=gtaoUnpackEdges(edgeBottom);
 // Reject samples across depth discontinuities on both sides of an edge.
 center*=float4(left.y,right.x,top.w,bottom.z);
 // Allow a small amount of leakage at corners; this prevents isolated
 // pixels from becoming unnecessarily dark after edge rejection.
 float edginess=saturate(1.5-dot(center,1.0.xxxx))/1.5*.5;
 center=saturate(center+edginess);
 const float diagonalWeight=.85*.5;
 float weightTL=diagonalWeight*(center.x*left.z+center.z*top.x);
 float weightTR=diagonalWeight*(center.z*top.y+center.y*right.z);
 float weightBL=diagonalWeight*(center.w*bottom.x+center.x*left.w);
 float weightBR=diagonalWeight*(center.y*right.w+center.w*bottom.y);
 float weightSum=blurAmount,sum=ao[4]*weightSum;
 gtaoAddSample(ao[3],center.x,sum,weightSum);gtaoAddSample(ao[5],center.y,sum,weightSum);
 gtaoAddSample(ao[1],center.z,sum,weightSum);gtaoAddSample(ao[7],center.w,sum,weightSum);
 gtaoAddSample(ao[0],weightTL,sum,weightSum);gtaoAddSample(ao[2],weightTR,sum,weightSum);
 gtaoAddSample(ao[6],weightBL,sum,weightSum);gtaoAddSample(ao[8],weightBR,sum,weightSum);
 return sum/max(weightSum,1e-5);
}
