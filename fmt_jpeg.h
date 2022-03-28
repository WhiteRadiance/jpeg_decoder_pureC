#ifndef _FMT_JPEG_H
#define _FMT_JPEG_H
#include <stdio.h>
/*
 * 此文件对Plexer写的C++版本的JPEG_codec的VS工程简化为C语言版本的解码库(已删除编码部分) ---2021/7/8
 * JPEG是对YUV进行处理的,编码只有量化会引入有损的,不过正反DCT前后存在浮点数的精度误差(数学上无误差)
 * JPEG编码流程: 拆分成宏块MCU => Forward-DCT => 量化 => Zigzag => RLE/Huffman编码 => 生成bit码流
 * JPEG解码流程: 将码流还原成RLE/Huffman编码 => 还原为矩阵 => 反Zigzag量化 => Inverse-DCT => 拼接MCU
 */

typedef unsigned char	u8;
typedef unsigned short	u16;

typedef struct s_DQT {
	u16		label;			//Define Quantization Table = 0xFFDB
	u16		dqtlen;			//0x43 = 2+1+64=67
	u8		dqtacc_id;		//acc=[7:4] 0:8bit, 1:16bit, id=[3:0] #0-3 (up to 4 tables)
	u8		QTable[64];		//init QTable[128] if acc=1 e.g. 16bit
}DQT;

typedef struct s_DHT_DC {
	u16		label;			//Define DC Huffman Table = 0xFFC4
	u16		dhtlen;			//0x1F = 2+1+16+12
	u8		hufftype_id;	//type=[7:4] 0:DC, 1:AC, /id=[3:0] 0:Lumi, 1:Chromi
	u8		DC_NRcodes[16];	//how many codes in each code_length (1-16)
	u8*		DC_Values;		//12 Byte if using recommended Huffman table
}DHT_DC;

typedef struct s_DHT_AC {
	u16		label;			//Define AC Huffman Table = 0xFFC4
	u16		dhtlen;			//0xB5 = 2+1+16+162
	u8		hufftype_id;	//0x10 is AC for Lumi(Y), 0x11 is AC for Chromi(UV)
	u8		AC_NRcodes[16];
	u8*		AC_Values;		//162 Byte if using recommended Huffman table
}DHT_AC;


/*
 * JPEG使用大端模式,fwrite为小端模式,fwrite(0xFFD8)后会写入0xD8FF
 *
 * 下面结构体忽略了Exif结构或一些罕见的标识符:
 *		各标识符段之间无论出现多少0xFF都是合法的(至少要有一个)
 *	   >以下补充的标签后都有2字节指示段的长度:
 *		(APP1	0xE1		Exif struct seg			相机信息，作者版权信息等)
 *		APPn:	0xE1-0xEF	Applications			其他应用数据块(n=1,2,3,...,15)
 *		COM:	0xFE		Comments				注释块
 *		DNL:	0xDC		Def Number of Lines		影响SOF0，不过通常不支持，忽略其内容
 *		DRI:	0xDD		Def Restart Interval	DC部分的差分编码复位间隔，共6字节，前4为0xFFDD0004，后2表示n个MCU会出现RSTm标识符
 *		DAC:	0xCC		Def Arithmetic Table	算术编码表，因为版权的法律问题，不能生产算术编码的JPEG图片，只能忽略其内容
 *     >以下补充的标签仅仅是个标签，其后不存在属于该段的任何内容，也没有段的长度信息:
 *		(SOI)(EOI)
 *		TEM:	0x01		未知意义					未知意义，直接掠过
 *		RSTm:	0xD0-0xD7	#m reset label			每n(DRI中指定)个MCU后的用于sync(同步)的复位标签(m=0,1,2,...,7)，超过7后从0继续
 *     >其实还有一些极其罕见的标识符，为防止疏漏，编程时应判断当前标签是否为常用标签，否则忽略其内容，而不应直接判断它是否为罕见标签:
 *		SOF1-15:0xC1-0xCF	<不存在SOF4/8/12, 因为0xC4=DHT, 0xC8=JPG, 0xCC=DAC>
 *		JPG:	0xC8		保留/解码故障
 *		DHP:	0xDE
 *		EXP:	0xDF
 *		JPG0:	0xF0
 *		JPG13:	0xFD
 */
struct s_JPEG_header {
	u16		SOI;			//Start Of Image = 0xFFD8

	u16		APP0;			//Application 0 = 0xFFE0
	u16		app0len;			//APP0's len, =16 if no thumbnail image (almost)
	char	app0id[5];		//"JFIF\0" = 0x4A46494600
	u16		jfifver;		//JFIF's version: 0x0101(v1.1) or 0x0102(v1.2)
	u8		xyunit;				//x/y density's unit, 0:no unit, 1:dot/inch, 2:dot/cm
	u16		xden;			//x density
	u16		yden;			//y density
	u8		thumbh;			//thumbnail horizon(width)
	u8		thumbv;			//thumbnail vertical(height)

	DQT		DQT0;			//DQT for Luminance(Y)
	DQT		DQT1;			//DQT for Chrominance(CbCr)

	u16		SOF0;			//Start Of Frame = 0xFFC0
	u16		sof0len;		//len = 17 for 24bit TrueColor
	u8		sof0acc;		//acc = 0 (8 bit/sample), optional: 0x08(almost), 0x12, 0x16
	u16		imheight;		//image height/pixel
	u16		imwidth;		//image width/pixel
	u8		clrcomponent;	//color components, =3 if YUV, =4 if CMYK, JFIF only use 3
	//CMYK:Cyan-Magenta-Yellow-blacK color space is used in print industry
	u8		clrY_id;		//Y_component_id=1
	u8		clrY_sample;		//0x11 if yuv444, 0x22 if yuv411(420)
	u8		clrY_QTable;	//Y -> QTable #0
	u8		clrU_id;		//U_component_id=2
	u8		clrU_sample;		//U_Horizon_sample_factor=1, U_Vertical_sample_factor=1
	u8		clrU_QTable;	//U -> QTable #1
	u8		clrV_id;		//V_component_id=3
	u8		clrV_sample;		//V_Hor_sam_factor=1, V_Ver_sam_factor=1
	u8		clrV_QTable;	//V -> QTable #1

	DHT_DC	DHT_DC0;		//DHT for Luminance
	DHT_AC	DHT_AC0;
	DHT_DC	DHT_DC1;		//DHT for Chrominance
	DHT_AC	DHT_AC1;

	u16		SOS;			//Start Of Scan = 0xFFDA
	u16		soslen;			//0x0C
	u8		component;		//Should equal to color component
	u16		Y_id_dht;		//Y: id=1, use #0 DC and #0 AC = 0x0100
	u16		U_id_dht;		//U: id=2, use #1 DC and #1 AC = 0x0211
	u16		V_id_dht;		//V: id=3, use #1 DC and #1 AC = 0x0311
	//always fit
	u8		SpectrumS;		//spectrum start = 0x00
	u8		SpectrumE;		//spectrum end = 0x3F
	u8		SpectrumC;		//spectrum choose = 0x00

	//Here is filled with Compressed-Image-Data

	u16		EOI;			//End Of Image = 0xFFD9
};


typedef	struct s_BitString {
	int code;
	u8	len;
}BitString;

typedef struct s_BitString HuffType;

static const double DCT[8][8] = {		//Y = DCT{X} = DXD^(-1) = DXD^T
	{0.35355,	0.35355,	0.35355,	0.35355,	0.35355,	0.35355,	0.35355,	0.35355},
	{0.49039,	0.41573,	0.27779,	0.09756,   -0.09756,   -0.27779,   -0.41573,   -0.49039},
	{0.46194,	0.19134,   -0.19134,   -0.46194,   -0.46194,   -0.19134,	0.19134,	0.46194},
	{0.41573,  -0.09755,   -0.49039,   -0.27779,	0.27779,	0.49039,	0.09755,   -0.41573},
	{0.35355,  -0.35355,   -0.35355,	0.35355,	0.35355,   -0.35355,   -0.35355,	0.35355},
	{0.27779,  -0.49039,	0.09755,	0.41573,   -0.41573,   -0.09755,	0.49039,   -0.27779},
	{0.19134,  -0.46194,	0.46194,   -0.19134,   -0.19134,	0.46194,   -0.46194,	0.19134},
	{0.09755,  -0.27779,	0.41573,   -0.49039,	0.49039,   -0.41573,	0.27779,   -0.09754}
};

static const double DCT_T[8][8] = {	//X = IDCT{Y} = D^(-1)YD = D^TYD
	{0.35355,	0.49039,	0.46194,	0.41573,	0.35355,	0.27779,	0.19134,	0.09755},
	{0.35355,	0.41573,	0.19134,   -0.09755,   -0.35355,   -0.49039,   -0.46194,   -0.27779},
	{0.35355,	0.27779,   -0.19134,   -0.49039,   -0.35355,	0.09755,	0.46194,	0.41573},
	{0.35355,	0.09755,   -0.46194,   -0.27779,	0.35355,	0.41573,   -0.19134,   -0.49039},
	{0.35355,  -0.09755,   -0.46194,	0.27779,	0.35355,   -0.41573,   -0.19134,	0.49039},
	{0.35355,  -0.27779,   -0.19134,	0.49039,   -0.35355,   -0.09755,	0.46194,   -0.41573},
	{0.35355,  -0.41573,	0.19134,	0.09755,   -0.35355,	0.49039,   -0.46194,	0.27779},
	{0.35355,  -0.49039,	0.46194,   -0.41573,	0.35355,   -0.27779,	0.19134,   -0.09754}
};

static const u8 RLE_ZigZag[64][2] = {
	{0, 0}, {0, 1}, {1, 0}, {2, 0}, {1, 1}, {0, 2}, {0, 3}, {1, 2},
	{2, 1}, {3, 0}, {4, 0}, {3, 1}, {2, 2}, {1, 3}, {0, 4}, {0, 5},
	{1, 4}, {2, 3}, {3, 2}, {4, 1}, {5, 0}, {6, 0}, {5, 1}, {4, 2},
	{3, 3}, {2, 4}, {1, 5}, {0, 6}, {0, 7}, {1, 6}, {2, 5}, {3, 4},
	{4, 3}, {5, 2}, {6, 1}, {7, 0}, {7, 1}, {6, 2}, {5, 3}, {4, 4},
	{3, 5}, {2, 6}, {1, 7}, {2, 7}, {3, 6}, {4, 5}, {5, 4}, {6, 3},
	{7, 2}, {7, 3}, {6, 4}, {5, 5}, {4, 6}, {3, 7}, {4, 7}, {5, 6},
	{6, 5}, {7, 4}, {7, 5}, {6, 6}, {5, 7}, {6, 7}, {7, 6}, {7, 7}
};

/*
HuffType*	HuffTbl_Y_DC = NULL;		//size <= 16
HuffType*	HuffTbl_Y_AC = NULL;		//size = 256
HuffType*	HuffTbl_UV_DC = NULL;		//size <= 16
HuffType*	HuffTbl_UV_AC = NULL;		//size = 256
*/

int		round_double(double val);
u16		rd_BigEnd16(u16 val);
void	rd_SOF0_2get_size(FILE* fp, u8* pY_sample, u16* imw, u16* imh);


void	Inverse_DCT(double res[][8], const double block[][8]);
void	Build_Huffman_Table(const u8* nr_codes, const u8* std_tbl, HuffType* huffman_tbl, u16* max_in_ThisLen, u8* huff_len_tbl[]);
void	rd_restore_HuffBit_RLE_Mat(int* rle, int temp[][8], int* pnewByte, int* pnewBytePos, int* prev_DC, const u16* max_first_DC, \
	const u16* max_first_AC, u8* huff_lentbl_DC[], u8* huff_lentbl_AC[], HuffType* rd_hufftbl_DC, \
	HuffType* rd_hufftbl_AC, FILE* fp, const u8* dc_nrcodes, const u8* ac_nrcodes);
void	rd_restore_InvQuantize(double afterInvQ[][8], int temp[][8], const u8* Qm);
void	Dec_JPEG_to_YUV(FILE* fp, u8* yuv_Y, u8* yuv_U, u8* yuv_V, u8 toYUV420);



#endif

