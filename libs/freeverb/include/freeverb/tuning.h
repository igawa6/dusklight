// Reverb model tuning values
//
// Written by Jezar at Dreampoint, June 2000
// http://www.dreampoint.co.uk
// This code is public domain

#ifndef _tuning_
#define _tuning_

constexpr int	numcombs		= 8;
constexpr int	numallpasses	= 4;
constexpr float	muted			= 0;
constexpr float	fixedgain		= 0.015f;
constexpr float scalewet		= 3;
constexpr float scaledry		= 2;
constexpr float scaledamp		= 0.4f;
constexpr float scaleroom		= 0.28f;
constexpr float offsetroom		= 0.7f;
constexpr float initialroom		= 0.5f;
constexpr float initialdamp		= 0.5f;
constexpr float initialwet		= 1/scalewet;
constexpr float initialdry		= 0;
constexpr float initialwidth	= 1;
constexpr float initialmode		= 0;
constexpr float freezemode		= 0.5f;
constexpr float srscale         = 48000.0f/32000.0f;
constexpr int	stereospread	= 23*srscale;

constexpr int combtuningL1		= 1116*srscale;
constexpr int combtuningR1		= combtuningL1+stereospread;
constexpr int combtuningL2		= 1188*srscale;
constexpr int combtuningR2		= combtuningL2+stereospread;
constexpr int combtuningL3		= 1277*srscale;
constexpr int combtuningR3		= combtuningL3+stereospread;
constexpr int combtuningL4		= 1356*srscale;
constexpr int combtuningR4		= combtuningL4+stereospread;
constexpr int combtuningL5		= 1422*srscale;
constexpr int combtuningR5		= combtuningL5+stereospread;
constexpr int combtuningL6		= 1491*srscale;
constexpr int combtuningR6		= combtuningL6+stereospread;
constexpr int combtuningL7		= 1557*srscale;
constexpr int combtuningR7		= combtuningL7+stereospread;
constexpr int combtuningL8		= 1617*srscale;
constexpr int combtuningR8		= combtuningL8+stereospread;
constexpr int allpasstuningL1	= 556*srscale;
constexpr int allpasstuningR1	= allpasstuningL1+stereospread;
constexpr int allpasstuningL2	= 441*srscale;
constexpr int allpasstuningR2	= allpasstuningL2+stereospread;
constexpr int allpasstuningL3	= 341*srscale;
constexpr int allpasstuningR3	= allpasstuningL3+stereospread;
constexpr int allpasstuningL4	= 225*srscale;
constexpr int allpasstuningR4	= allpasstuningL4+stereospread;

#endif//_tuning_

//ends
