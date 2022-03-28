#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmt_jpeg.h"


//double数据的四舍五入
inline int round_double(double val)
{
	double res = (val > 0.0) ? (val + 0.5) : (val - 0.5);
	return (int)res;
}


//2Byte小端转换为大端
inline  u16 rd_BigEnd16(u16 val)
{
	u8	b1 = (u8)(val);
	u8	b2 = (u8)(val >> 8);
	u16 temp = (u16)b2 + ((u16)b1 << 8);
	return temp;
}


//对数组全部元素进行求和,用于求DC/AC_NRCodes数组的sum来确定DC/AC_Values数组的长度
static u16 sum_arr(u8* arr, u8 len_arr)
{
	u16 sum = 0;
	for (u8 i = 0; i < len_arr; i++)
		sum += arr[i];
	return sum;
}


//为了方便对齐8/16pixel的MCU, 预读取SOF0的clrY_sample用于判断jpeg的采样比率
//为了方便开辟空间, 预读取SOF0的height/width
void rd_SOF0_2get_size(FILE* fp, u8* pY_sample, u16* pimw, u16* pimh)
{
	u16 seg_label_t = 0;
	u16 seg_size_t = 0;
	u8  dump = 0;					//占位字节,用于保存不需管的字节

	fread(&seg_label_t, 2, 1, fp);	//SOI

	fread(&seg_label_t, 2, 1, fp);	//find SOF0 label
	while (seg_label_t != 0xC0FF)
	{
		if (seg_label_t == 0x01FF)	//TEM
			fread(&seg_label_t, 2, 1, fp);
		else {
			fread(&seg_size_t, 2, 1, fp);
			fseek(fp, rd_BigEnd16(seg_size_t) - 2, SEEK_CUR);
			fread(&seg_label_t, 2, 1, fp);
		}
	}
	fread(&seg_size_t, 2, 1, fp);	//seg_len
	fread(&dump, 1, 1, fp);			//acc
	fread(pimh, 2, 1, fp);
	*pimh = rd_BigEnd16(*pimh);		//imheight
	fread(pimw, 2, 1, fp);
	*pimw = rd_BigEnd16(*pimw);		//imwidth
	fread(&dump, 1, 1, fp);			//color_component
	fread(&dump, 1, 1, fp);			//clrY_id = 0x01
	fread(pY_sample, 1, 1, fp);		//0x11(444) or 0x22(420)

	fseek(fp, 0, SEEK_SET);			//rewind to the start of the file
	return;
}



//======================== 解码核心函数 ======================== 解码核心函数 =========================



void Inverse_DCT(double res[][8], const double block[][8])
{
	double temp[8][8] = { 0.0 };
	for (u8 i = 0; i < 8; i++) {
		for (u8 j = 0; j < 8; j++) {
			for (u8 t = 0; t < 8; t++) {
				temp[i][j] += DCT_T[i][t] * block[t][j];
			}
		}
	}
	for (u8 i = 0; i < 8; i++) {
		for (u8 j = 0; j < 8; j++) {
			for (u8 t = 0; t < 8; t++) {
				res[i][j] += temp[i][t] * DCT[t][j];
			}
		}
	}
}



//生成DHT的时候顺便计算相同码字长度下的(最大码字+1),生成由码长定位码字的映射表len_tbl(注意参数max_inThisLen[17], len_tbl[17][130])
void Build_Huffman_Table(const u8* nr_codes, const u8* std_tbl, HuffType* huffman_tbl, u16* max_inThisLen, u8* huff_len_tbl[])
{
	u8 pos_in_tbl = 0;
	u16 code_val = 0;

	for (int k = 1; k <= 16; k++)
	{
		u8 num = 0;
		for (int j = 1; j <= nr_codes[k - 1]; j++)
		{
			huff_len_tbl[k][num] = std_tbl[pos_in_tbl];
			num++;
			huffman_tbl[std_tbl[pos_in_tbl]].code = code_val;
			huffman_tbl[std_tbl[pos_in_tbl]].len = k;
			pos_in_tbl++;
			code_val++;
		}
		max_inThisLen[k] = code_val;
		code_val <<= 1;
	}
	//jpeg的Huffman编码不会出现1bit的码字,为了不在读取第一比特为1就因为[1]=0而错误地认为第一个比特是某个码字...
	//保险起见应该无论第一个比特是啥都认为当前比特大于max(虽然[1]本来就是0),所以强制len=1时的max码字为0b
	max_inThisLen[0] = 0;		//此条语句其实冗余了
	//max_in_ThisLen[1] = 0;


	//<注意>
	//就是因为上方论述才导致那张超清少女手机壁纸(phonepaper.jpeg)解码失败，根本原因是其LumiAC的Huffman码是有len=1的情况
	//其实此图片的码长为1的码字只有一个,即0b,码长为2的码字为空,码长为3的码字为100b,101b,码长为4的...
	//因此由于len=2没有归零所以得到结果: max[1]=1b=1, max[2]=10b=2, max[3]=110b=6, max[4]=...
	//max[]是递增的,此jpeg的码表不存在2bit码字,大于3bit的码字必定也大于2bit的max,因此max[2]不归零并不影响解码正确性
	//另外,huffman解码时首bit遇到0则判断属于1bit长度的码字,所以上面的语句要注释掉以允许max[2]=1
	//这样当遇到bit流的首bit为1时,因为下面restore_RLE()函数用>=来判断max码字,因此可以保证不会坠入1bit码字的判定中
}



//newBytePos初始化为负数,当其为负数时说明进入函数时应该重新读取新的newByte
//回复为rle数组的同时顺便恢复成8×8的矩阵形式
void rd_restore_HuffBit_RLE_Mat(int* rle, int temp[][8], int* pnewByte, int* pnewBytePos, int* prev_DC, \
	const u16* max_first_DC, const u16* max_first_AC, u8* huff_lentbl_DC[], u8* huff_lentbl_AC[], \
	HuffType* rd_hufftbl_DC, HuffType* rd_hufftbl_AC, FILE* fp, const u8* dc_nrcodes, const u8* ac_nrcodes)
{
	//首先出现的是RLE编码的Huffman码字,然后根据码字低字节得知下个码字的码长,并读取幅度码字
	static u8 mmask[8] = { 1,2,4,8,16,32,64,128 };
	int value = 0, code_len = 0;
	u8 rle_idx = 0;
	BitString bitcode_1, bitcode_2;										//RLE编码对儿的第一项(修复前nrz/len修复后nrz)和第二项(magnitude)
	bitcode_1.code = 0; bitcode_1.len = 0;
	bitcode_2.code = 0; bitcode_2.len = 0;
	u8 xx = 0, yy = 0;

	//outer-func default newByte=0, newBytePos=-1
	while (value >= (int)max_first_DC[code_len])						//必须是 >=, 因为max指示的值是当前最大码字+1,可能是后面码字的前缀
	{
		if (*pnewBytePos < 0) {
			if (*pnewByte == 0xFF)	fread(pnewByte, 1, 1, fp);			//0xFF之后会紧跟一个无意义的0x00(忽略掉)
			fread(pnewByte, 1, 1, fp);
			*pnewBytePos = 7;
		}
		value <<= 1;
		value = value + ((*pnewByte & mmask[*pnewBytePos]) ? 1 : 0);
		(*pnewBytePos)--;
		code_len++;
	}
	for (int p = 0; p < dc_nrcodes[code_len - 1]; p++) {				//DC映射表其实不会太长(远小于130)
		if (rd_hufftbl_DC[huff_lentbl_DC[code_len][p]].code == value) {	//在码长映射表中寻找与value相符合的码字
			bitcode_1.code = huff_lentbl_DC[code_len][p];				//这里没有存码字(如110b),而是直接存(如0x5)
			bitcode_1.len = code_len;									//注意DC部分的nrz肯定是0,故省略0x05成0x5
			rle[rle_idx] = bitcode_1.code;								//DC的nrz肯定是0,不过为了兼容AC还是保存了0
			rle_idx++;
			value = 0;
			code_len = 0;
			break;
		}
	}
	if (bitcode_1.code == 0) {
		bitcode_2.code = 0;
		bitcode_2.len = 0;
		rle[0] = 0;														//将第一项从Bit编码修复为RLE编码的DC_nrz=0
		rle[rle_idx] = bitcode_2.code + *prev_DC;						//差分编码是0时表示当前实际DC值等于prev_DC
		temp[0][0] = rle[rle_idx];
		yy++;
		rle_idx++;
	}
	else {
		bitcode_2.len = bitcode_1.code & 0x0F;
		rle[0] = 0;														//修复第一项
		for (int p = 0; p < bitcode_2.len; p++) {						//第一项指明了幅度码字的码长
			if (*pnewBytePos < 0) {
				if (*pnewByte == 0xFF)	fread(pnewByte, 1, 1, fp);		//0xFF之后会紧跟一个无意义的0x00(忽略掉)
				fread(pnewByte, 1, 1, fp);
				*pnewBytePos = 7;
			}
			value <<= 1;
			//value |= (*newByte & mmask[*newBytePos]);
			value = value + ((*pnewByte & mmask[*pnewBytePos]) ? 1 : 0);
			(*pnewBytePos)--;
			code_len++;
			if ((p == 0) && (value == 0))
				bitcode_2.code = -1;									//暂时把code置负,用于指示首个bit不是1(在幅度编码中代表负数)
		}
		if (bitcode_2.code < 0) {
			int tt = (-1) * (1 << bitcode_2.len) + 1;
			bitcode_2.code = value + tt + *prev_DC;
			rle[rle_idx] = bitcode_2.code;
			temp[0][0] = rle[rle_idx];
			yy++;
			rle_idx++;
		}
		else {
			bitcode_2.code = value + *prev_DC;
			rle[rle_idx] = bitcode_2.code;
			temp[0][0] = rle[rle_idx];
			yy++;
			rle_idx++;
		}
		*prev_DC = bitcode_2.code;										//更新prev_DC
		value = 0;
		code_len = 0;
		bitcode_1.code = 0;	bitcode_1.len = 0;
		bitcode_2.code = 0;	bitcode_2.len = 0;
	}

	//AC part
	int nums = 0;
	//记录RLE编码的原始数据中AC部分的长度(63),注意连零的个数也会记录...
	//(不然长度无法自增到63,另外结尾不出现0时也是不存在EOB的)
	for (nums = 0; nums < 63; nums++)
	{
		while (value >= (int)max_first_AC[code_len])
		{
			if (*pnewBytePos < 0) {
				if (*pnewByte == 0xFF)	fread(pnewByte, 1, 1, fp);		//0xFF之后会紧跟一个无意义的0x00(忽略掉)
				fread(pnewByte, 1, 1, fp);
				*pnewBytePos = 7;
			}
			value <<= 1;
			value = value + ((*pnewByte & mmask[*pnewBytePos]) ? 1 : 0);
			(*pnewBytePos)--;
			code_len++;
		}
		for (int p = 0; p < ac_nrcodes[code_len - 1]; p++) {
			if (rd_hufftbl_AC[huff_lentbl_AC[code_len][p]].code == value) {	//在码长映射表中寻找与value相符合的码字
				bitcode_1.code = huff_lentbl_AC[code_len][p];			//这里是直接存坐标映射(也就是码字对应的值)
				bitcode_1.len = code_len;
				rle[rle_idx] = bitcode_1.code;
				rle_idx++;
				value = 0;
				code_len = 0;
				break;
			}
		}
		if (bitcode_1.code == 0xF0) {									//处理AC出现的(15,0)
			bitcode_2.code = 0;
			bitcode_2.len = 0;
			rle[rle_idx - 1] = 15;
			rle[rle_idx] = bitcode_2.code;
			rle_idx++;
			for (u8 z = 0; z < 16; z++) {								//（15,0）实际上是16个0
				temp[xx][yy] = 0;
				yy++;
				if (yy >= 8) { xx++; yy = 0; }
			}
			nums += 15;
		}
		else if (bitcode_1.code == 0x00) {								//EOB:处理AC出现的(0,0)
			bitcode_2.code = 0;
			bitcode_2.len = 0;
			rle[rle_idx - 1] = 0;
			rle[rle_idx] = bitcode_2.code;
			rle_idx++;
			while (xx <= 7) {
				temp[xx][yy] = 0;
				yy++;
				if (yy >= 8) { xx++; yy = 0; }
			}
			nums = 63;													//auto-break the loop
			//break;
		}
		else {
			bitcode_2.len = bitcode_1.code & 0x0F;
			rle[rle_idx - 1] >>= 4;										//将第一项从Bit编码(如0x25)修复为RLE编码(如0x02)
			for (u8 z = 0; z < rle[rle_idx - 1]; z++) {
				temp[xx][yy] = 0;
				yy++;
				if (yy >= 8) { xx++; yy = 0; }
			}
			nums += rle[rle_idx - 1];
			for (int p = 0; p < bitcode_2.len; p++) {					//第一项指明了幅度码字的码长
				if (*pnewBytePos < 0) {
					if (*pnewByte == 0xFF)	fread(pnewByte, 1, 1, fp);	//0xFF之后会紧跟一个无意义的0x00(忽略掉)
					fread(pnewByte, 1, 1, fp);
					*pnewBytePos = 7;
				}
				value <<= 1;
				value = value + ((*pnewByte & mmask[*pnewBytePos]) ? 1 : 0);
				(*pnewBytePos)--;
				code_len++;
				if ((p == 0) && (value == 0))
					bitcode_2.code = -1;								//暂时把code置负,用于指示首个bit不是1(在幅度编码中代表负数)
			}
			if (bitcode_2.code < 0) {
				int tt = (-1) * (1 << bitcode_2.len) + 1;
				bitcode_2.code = value + tt;
				rle[rle_idx] = bitcode_2.code;
				rle_idx++;
				temp[xx][yy] = bitcode_2.code;
				yy++;
				if (yy >= 8) { xx++; yy = 0; }
				value = 0;
				code_len = 0;
			}
			else {
				bitcode_2.code = value;
				rle[rle_idx] = bitcode_2.code;
				rle_idx++;
				temp[xx][yy] = bitcode_2.code;
				yy++;
				if (yy >= 8) { xx++; yy = 0; }
				value = 0;
				code_len = 0;
			}
		}
		bitcode_1.code = 0;	bitcode_1.len = 0;
		bitcode_2.code = 0;	bitcode_2.len = 0;
	}
}


//调用函数之前已经反差分解码过并已写成原始矩阵格式,函数内部反量化并反ZigZag处理
//void rd_restore_InvQuantize(double afterInvQ[][8], int rle[], const u8* Qm)
void rd_restore_InvQuantize(double afterInvQ[][8], int temp[][8], const u8* Qm)
{
	//inv-Quantize
	for (u8 i = 0; i < 8; i++) {
		for (u8 j = 0; j < 8; j++) {
			temp[i][j] = temp[i][j] * Qm[i * 8 + j];
		}
	}
	//inv-ZigZag
	for (u8 i = 0; i < 8; i++) {
		for (u8 j = 0; j < 8; j++) {
			afterInvQ[RLE_ZigZag[i * 8 + j][0]][RLE_ZigZag[i * 8 + j][1]] = (double)temp[i][j];
		}
	}
}




//======================== 超级核心函数 ======================== 超级核心函数 =========================




/*
 * intro:	jpeg解码
 * param:	if <bool> toYUV420 == false
 *				输出的yuv_Y,yuv_U,yuv_V必须是yuv444格式的数组,即YUV三个分量应该同等大小
 *			else
 *				输出的yuv_Y,yuv_U,yuv_V必须是yuv420格式大小
 * note:	本函数没有使用fclose()来关闭文件
 *			本函数暂只能解码444和420采样率的JPEG图片至YUV，现在422采样率代码段是空的
 *			注意jpeg解码时应该根据文件生成对应的DQT和DHT, 为了兼容性也绝不能使用标准表
 *			注意jpeg文件中除了段头之外其余数据中若出现0xFF后都会紧跟一个无意义的0x00
 *			注意jpeg文件还存在很多用于包含相机信息的段, 另外jpeg会容忍段头出现的若干个0xFF(函数还不支持此条要求)
 */
void Dec_JPEG_to_YUV(FILE* fp, u8* yuv_Y, u8* yuv_U, u8* yuv_V, u8 toYUV420)
{
	struct s_JPEG_header Jheader;

	//u8	rd_Lumi_DC_NRCodes[16] = { 0 }, rd_Lumi_AC_NRCodes[16] = { 0 };
	//u8	rd_Chromi_DC_NRCodes[16] = { 0 }, rd_Chromi_AC_NRCodes[16] = { 0 };
	//u8* rd_Lumi_DC_Values = NULL, * rd_Chromi_DC_Values = NULL;
	//u8* rd_Lumi_AC_Values = NULL, * rd_Chromi_AC_Values = NULL;

	u16 height = 0, width = 0;
	u16 seg_label_t = 0;
	u16 seg_size_t = 0;
	u16 res_sync = 0;

	fread(&Jheader.SOI, 2, 1, fp);
	Jheader.SOI = rd_BigEnd16(Jheader.SOI);

	fread(&seg_label_t, 2, 1, fp);
	while (seg_label_t != 0xDAFF)
	{
		switch (seg_label_t)
		{
		case 0xE0FF:							// APP0/JFIF (是最常见的APPn)
			fread(&seg_size_t, 2, 1, fp);
			Jheader.APP0 = rd_BigEnd16(seg_label_t);			Jheader.app0len = rd_BigEnd16(seg_size_t);
			fread(Jheader.app0id, 1, 5, fp);
			fread(&Jheader.jfifver, 2, 1, fp);					Jheader.jfifver = rd_BigEnd16(Jheader.jfifver);
			fread(&Jheader.xyunit, 1, 1, fp);
			fread(&Jheader.xden, 2, 1, fp);						Jheader.xden = rd_BigEnd16(Jheader.xden);
			fread(&Jheader.yden, 2, 1, fp);						Jheader.yden = rd_BigEnd16(Jheader.yden);
			fread(&Jheader.thumbh, 1, 1, fp);
			fread(&Jheader.thumbv, 1, 1, fp);
			break;

		case 0xE1FF:							// APP1/Exif (包含相机信息等)
			fread(&seg_size_t, 2, 1, fp);
			fseek(fp, rd_BigEnd16(seg_size_t) - 2, SEEK_CUR);
			break;

		case 0xDBFF:							// DQT
			fread(&seg_size_t, 2, 1, fp);
			if (seg_size_t == 0x8400)			//0x84 = 132 = 2+(1+64)*2	->	单个label包含两个DQT的情况
			{
				for (int t = 0; t < 2; t++)
				{
					u8	dqt_id_t = 0;
					fread(&dqt_id_t, 1, 1, fp);
					if (dqt_id_t == 0) {
						Jheader.DQT0.label = rd_BigEnd16(seg_label_t);
						Jheader.DQT0.dqtlen = rd_BigEnd16(seg_size_t);
						Jheader.DQT0.dqtacc_id = dqt_id_t;
						fread(Jheader.DQT0.QTable, 1, 64, fp);
					}
					else if (dqt_id_t == 1) {
						Jheader.DQT1.label = rd_BigEnd16(seg_label_t);
						Jheader.DQT1.dqtlen = rd_BigEnd16(seg_size_t);
						Jheader.DQT1.dqtacc_id = dqt_id_t;
						fread(Jheader.DQT1.QTable, 1, 64, fp);
					}
					else;
				}
			}
			else if (seg_size_t == 0x4300)		//0x43 = 67 = 2+1+64		->	每个label各包含一个DQT的情况(更常见)
			{
				fseek(fp, -4, SEEK_CUR);		//rewind 2+2 Byte
				for (int t = 0; t < 2; t++)
				{
					u8	dqt_id_t = 0;
					fread(&seg_label_t, 2, 1, fp);
					fread(&seg_size_t, 2, 1, fp);
					fread(&dqt_id_t, 1, 1, fp);
					if (dqt_id_t == 0) {
						Jheader.DQT0.label = rd_BigEnd16(seg_label_t);
						Jheader.DQT0.dqtlen = rd_BigEnd16(seg_size_t);
						Jheader.DQT0.dqtacc_id = dqt_id_t;
						fread(Jheader.DQT0.QTable, 1, 64, fp);
					}
					else if (dqt_id_t == 1) {
						Jheader.DQT1.label = rd_BigEnd16(seg_label_t);
						Jheader.DQT1.dqtlen = rd_BigEnd16(seg_size_t);
						Jheader.DQT1.dqtacc_id = dqt_id_t;
						fread(Jheader.DQT1.QTable, 1, 64, fp);
					}
					else;
				}
			}
			else;
			break;

		case 0xC0FF:							// SOF0
			fread(&seg_size_t, 2, 1, fp);
			Jheader.SOF0 = rd_BigEnd16(seg_label_t);			Jheader.sof0len = rd_BigEnd16(seg_size_t);
			fread(&Jheader.sof0acc, 1, 1, fp);
			fread(&Jheader.imheight, 2, 1, fp);					Jheader.imheight = rd_BigEnd16(Jheader.imheight);	//image height/pixel
			fread(&Jheader.imwidth, 2, 1, fp);					Jheader.imwidth = rd_BigEnd16(Jheader.imwidth);		//image width/pixel
			width = Jheader.imwidth;
			height = Jheader.imheight;
			fread(&Jheader.clrcomponent, 1, 1, fp);
			for (int t = 0; t < Jheader.clrcomponent; t++) {	//clr_c=3:YCbCr, =4:CMYK, 但是JFIF只支持YCbCr(YUV)
				u8	clr_id_t = 0, clr_hvsample_t = 0, clr_Qid_t = 0;
				fread(&clr_id_t, 1, 1, fp);
				fread(&clr_hvsample_t, 1, 1, fp);
				fread(&clr_Qid_t, 1, 1, fp);
				if (clr_id_t == 0x01) {
					Jheader.clrY_id = clr_id_t;
					Jheader.clrY_sample = clr_hvsample_t;
					Jheader.clrY_QTable = clr_Qid_t;
				}
				else if (clr_id_t == 0x02) {
					Jheader.clrU_id = clr_id_t;
					Jheader.clrU_sample = clr_hvsample_t;
					Jheader.clrU_QTable = clr_Qid_t;
				}
				else if (clr_id_t == 0x03) {
					Jheader.clrV_id = clr_id_t;
					Jheader.clrV_sample = clr_hvsample_t;
					Jheader.clrV_QTable = clr_Qid_t;
				}
				else;
			}
			break;

		case 0xC4FF:							// DHT
			fread(&seg_size_t, 2, 1, fp);
			seg_size_t = rd_BigEnd16(seg_size_t);

			//如果DHT段的容量大于0xE0则暂时认为这是单个label包含4个表的情况
			//大多数情况下都是独立的四个较小的独立的DHT表: 每个label包含一个表(共4个重复DHT段)
			if (seg_size_t < 0x00E0)
				fseek(fp, -4, SEEK_CUR);			//rewind 2+2 Byte
			for (u8 t = 0; t < 4; t++)
			{
				u8 dht_id_t = 0;
				if (seg_size_t < 0x00E0) {
					fread(&seg_label_t, 2, 1, fp);
					fread(&seg_size_t, 2, 1, fp);
					seg_size_t = rd_BigEnd16(seg_size_t);
				}
				fread(&dht_id_t, 1, 1, fp);
				if (dht_id_t == 0x00) {			//Lumi DC
					Jheader.DHT_DC0.label = rd_BigEnd16(seg_label_t);
					Jheader.DHT_DC0.hufftype_id = dht_id_t;
					fread(Jheader.DHT_DC0.DC_NRcodes, 1, 16, fp);
					u16 Values_len = sum_arr(Jheader.DHT_DC0.DC_NRcodes, 16);
					if (Values_len == 0)		return;
					Jheader.DHT_DC0.dhtlen = 16 + Values_len + 1 + 2;			//codes + values + id + self_seg_len
					Jheader.DHT_DC0.DC_Values = (u8*)malloc(sizeof(u8) * Values_len);
					fread(Jheader.DHT_DC0.DC_Values, 1, Values_len, fp);
				}
				else if (dht_id_t == 0x01) {	//Chromi DC
					Jheader.DHT_DC1.label = rd_BigEnd16(seg_label_t);
					Jheader.DHT_DC1.hufftype_id = dht_id_t;
					fread(Jheader.DHT_DC1.DC_NRcodes, 1, 16, fp);
					u16 Values_len = sum_arr(Jheader.DHT_DC1.DC_NRcodes, 16);
					if (Values_len == 0)		return;
					Jheader.DHT_DC1.dhtlen = 16 + Values_len + 1 + 2;			//codes + values + id + self_seg_len
					Jheader.DHT_DC1.DC_Values = (u8*)malloc(sizeof(u8) * Values_len);
					fread(Jheader.DHT_DC1.DC_Values, 1, Values_len, fp);
				}
				else if (dht_id_t == 0x10) {	//Lumi AC
					Jheader.DHT_AC0.label = rd_BigEnd16(seg_label_t);
					Jheader.DHT_AC0.hufftype_id = dht_id_t;
					fread(Jheader.DHT_AC0.AC_NRcodes, 1, 16, fp);
					u16 Values_len = sum_arr(Jheader.DHT_AC0.AC_NRcodes, 16);
					if (Values_len == 0)		return;
					Jheader.DHT_AC0.dhtlen = 16 + Values_len + 1 + 2;			//codes + values + id + self_seg_len
					Jheader.DHT_AC0.AC_Values = (u8*)malloc(sizeof(u8) * Values_len);
					fread(Jheader.DHT_AC0.AC_Values, 1, Values_len, fp);
				}
				else if (dht_id_t == 0x11) {	//Chromi AC
					Jheader.DHT_AC1.label = rd_BigEnd16(seg_label_t);
					Jheader.DHT_AC1.hufftype_id = dht_id_t;
					fread(Jheader.DHT_AC1.AC_NRcodes, 1, 16, fp);
					u16 Values_len = sum_arr(Jheader.DHT_AC1.AC_NRcodes, 16);
					if (Values_len == 0)		return;
					Jheader.DHT_AC1.dhtlen = 16 + Values_len + 1 + 2;	//codes + values + id + self_seg_len
					Jheader.DHT_AC1.AC_Values = (u8*)malloc(sizeof(u8)* Values_len);
					fread(Jheader.DHT_AC1.AC_Values, 1, Values_len, fp);
				}
				else;
			}
			break;

		case 0xDDFF:							// DRI
			fread(&seg_size_t, 2, 1, fp);
			fread(&res_sync, 2, 1, fp);
			res_sync = rd_BigEnd16(res_sync);
			break;

		case 0x01FF:							// TEM (label only)
			break;

		default:								// PhotoShop图片的APP13/14, ICC色彩联盟的APP2, 剩余的APPn 和 罕见的标识符就直接跳过
			fread(&seg_size_t, 2, 1, fp);
			fseek(fp, rd_BigEnd16(seg_size_t) - 2, SEEK_CUR);
			break;
		}

		fread(&seg_label_t, 2, 1, fp);
	}

	//JPEG固定SOS段的后面就是图像的压缩数据
	//读SOS段之前先把前面的DHT处理成Huffman表和Huffman码长查找表
	//以下的变量为防止函数占用太多堆栈已经放在JPEG.h文件里(全局)
	u16	max_first_Lumi_DC[17] = { 0 }, max_first_Lumi_AC[17] = { 0 }, max_first_Chromi_DC[17] = { 0 }, max_first_Chromi_AC[17] = { 0 };
	//动态申请二级指针的空间(每一项对应的空间大小是不一样的)
	u8* HuffTbl_len_Y_DC[17] = { NULL }, * HuffTbl_len_Y_AC[17] = { NULL }, * HuffTbl_len_UV_DC[17] = { NULL }, * HuffTbl_len_UV_AC[17] = { NULL };
	for (u8 m = 0; m < 16; m++) {
		if (Jheader.DHT_DC0.DC_NRcodes[m] != 0)		HuffTbl_len_Y_DC[m + 1] = (u8*)malloc(sizeof(u8) * Jheader.DHT_DC0.DC_NRcodes[m]);
		if (Jheader.DHT_AC0.AC_NRcodes[m] != 0)		HuffTbl_len_Y_AC[m + 1] = (u8*)malloc(sizeof(u8) * Jheader.DHT_AC0.AC_NRcodes[m]);
		if (Jheader.DHT_DC1.DC_NRcodes[m] != 0)		HuffTbl_len_UV_DC[m + 1] = (u8*)malloc(sizeof(u8) * Jheader.DHT_DC1.DC_NRcodes[m]);
		if (Jheader.DHT_AC1.AC_NRcodes[m] != 0)		HuffTbl_len_UV_AC[m + 1] = (u8*)malloc(sizeof(u8) * Jheader.DHT_AC1.AC_NRcodes[m]);

		if (HuffTbl_len_Y_DC[m + 1] != NULL)	memset(HuffTbl_len_Y_DC[m + 1], 0, Jheader.DHT_DC0.DC_NRcodes[m]);
		if (HuffTbl_len_Y_AC[m + 1] != NULL)	memset(HuffTbl_len_Y_AC[m + 1], 0, Jheader.DHT_AC0.AC_NRcodes[m]);
		if (HuffTbl_len_UV_DC[m + 1] != NULL)	memset(HuffTbl_len_UV_DC[m + 1], 0, Jheader.DHT_DC1.DC_NRcodes[m]);
		if (HuffTbl_len_UV_AC[m + 1] != NULL)	memset(HuffTbl_len_UV_AC[m + 1], 0, Jheader.DHT_AC1.AC_NRcodes[m]);
	}

	//u8 HuffTbl_len_Y_DC[17][130] = { 0 }, HuffTbl_len_Y_AC[17][130] = { 0 }, HuffTbl_len_UV_DC[17][130] = { 0 }, HuffTbl_len_UV_AC[17][130] = { 0 };
	HuffType* rd_HuffTbl_Y_DC = (HuffType*)malloc(sizeof(HuffType) * Jheader.DHT_DC0.dhtlen - 2 - 1 - 16);
	HuffType* rd_HuffTbl_Y_AC = (HuffType*)malloc(sizeof(HuffType) * 256);
	HuffType* rd_HuffTbl_UV_DC = (HuffType*)malloc(sizeof(HuffType) * Jheader.DHT_DC1.dhtlen - 2 - 1 - 16);
	HuffType* rd_HuffTbl_UV_AC = (HuffType*)malloc(sizeof(HuffType) * 256);
	if (rd_HuffTbl_Y_DC == NULL || rd_HuffTbl_Y_AC == NULL || rd_HuffTbl_UV_DC == NULL || rd_HuffTbl_UV_AC == NULL)
		return;
	memset(rd_HuffTbl_Y_DC, 0, sizeof(HuffType)* Jheader.DHT_DC0.dhtlen - 2 - 1 - 16);
	memset(rd_HuffTbl_Y_AC, 0, sizeof(HuffType) * 256);
	memset(rd_HuffTbl_UV_DC, 0, sizeof(HuffType)* Jheader.DHT_DC1.dhtlen - 2 - 1 - 16);
	memset(rd_HuffTbl_UV_AC, 0, sizeof(HuffType) * 256);

	//void Build_Huffman_Table(const u8* nr_codes, const u8* std_tbl, HuffType* huffman_tbl, u16* max_inThisLen, u8* huff_len_tbl[])
	Build_Huffman_Table(Jheader.DHT_DC0.DC_NRcodes, Jheader.DHT_DC0.DC_Values, rd_HuffTbl_Y_DC, max_first_Lumi_DC, HuffTbl_len_Y_DC);
	Build_Huffman_Table(Jheader.DHT_AC0.AC_NRcodes, Jheader.DHT_AC0.AC_Values, rd_HuffTbl_Y_AC, max_first_Lumi_AC, HuffTbl_len_Y_AC);
	Build_Huffman_Table(Jheader.DHT_DC1.DC_NRcodes, Jheader.DHT_DC1.DC_Values, rd_HuffTbl_UV_DC, max_first_Chromi_DC, HuffTbl_len_UV_DC);
	Build_Huffman_Table(Jheader.DHT_AC1.AC_NRcodes, Jheader.DHT_AC1.AC_Values, rd_HuffTbl_UV_AC, max_first_Chromi_AC, HuffTbl_len_UV_AC);

	//DC/AC_Values已经用不到了
	free(Jheader.DHT_DC0.DC_Values);	Jheader.DHT_DC0.DC_Values = NULL;
	free(Jheader.DHT_DC1.DC_Values);	Jheader.DHT_DC1.DC_Values = NULL;
	free(Jheader.DHT_AC0.AC_Values);	Jheader.DHT_AC0.AC_Values = NULL;
	free(Jheader.DHT_AC1.AC_Values);	Jheader.DHT_AC1.AC_Values = NULL;
	puts("Build Huffman Table Finished.");

	// SOS
	fread(&seg_size_t, 2, 1, fp);
	Jheader.SOS = rd_BigEnd16(seg_label_t);					Jheader.soslen = rd_BigEnd16(seg_size_t);
	fread(&Jheader.component, 1, 1, fp);
	fread(&Jheader.Y_id_dht, 2, 1, fp);						Jheader.Y_id_dht = rd_BigEnd16(Jheader.Y_id_dht);
	fread(&Jheader.U_id_dht, 2, 1, fp);						Jheader.U_id_dht = rd_BigEnd16(Jheader.U_id_dht);
	fread(&Jheader.V_id_dht, 2, 1, fp);						Jheader.V_id_dht = rd_BigEnd16(Jheader.V_id_dht);
	fread(&Jheader.SpectrumS, 1, 1, fp);		//the following three items are fixed
	fread(&Jheader.SpectrumE, 1, 1, fp);
	fread(&Jheader.SpectrumC, 1, 1, fp);

	// 读取原始压缩数据
	int newByte = 0, newBytePos = -1;
	int	prev_DC_Y = 0, prev_DC_U = 0, prev_DC_V = 0;
	if (Jheader.clrY_sample == 0x11)						//jpeg in yuv444
	{
		u16 res_sync_count = 0;	//DRI段需要的复位计数器
		for (int yPos = 0; yPos < height; yPos += 8) {
			for (int xPos = 0; xPos < width; xPos += 8) {
				res_sync_count++;
				if (res_sync != 0 && res_sync_count > res_sync) {	//DRI复位差分编码和复位Huffman编码
					newByte = 0, newBytePos = -1;
					prev_DC_Y = 0, prev_DC_U = 0, prev_DC_V = 0;
					fseek(fp, 2, SEEK_CUR);							//跳过RSTm标签0xFFDm
					res_sync_count = 1;
				}
				int		MCU_Y[8][8] = { 0 };
				double	res_IDCT_Y[8][8] = { 0.0 };
				double	res_afterInvQ_Y[8][8] = { 0.0 };
				int		rle_Y[128] = { 0 };
				int		mat_Y[8][8] = { 0 };	//由rle直接恢复出的原始矩阵,还没有反量化和反Zigzag

				//Y-channel
				rd_restore_HuffBit_RLE_Mat(rle_Y, mat_Y, &newByte, &newBytePos, &prev_DC_Y, max_first_Lumi_DC, max_first_Lumi_AC, HuffTbl_len_Y_DC, \
					HuffTbl_len_Y_AC, rd_HuffTbl_Y_DC, rd_HuffTbl_Y_AC, fp, Jheader.DHT_DC0.DC_NRcodes, Jheader.DHT_AC0.AC_NRcodes);
				rd_restore_InvQuantize(res_afterInvQ_Y, mat_Y, Jheader.DQT0.QTable);
				Inverse_DCT(res_IDCT_Y, res_afterInvQ_Y);
				for (int i = 0; i < 8; i++) {
					for (int j = 0; j < 8; j++) {
						MCU_Y[i][j] = round_double(res_IDCT_Y[i][j] + 128.0);
						if (MCU_Y[i][j] < 0x00)		MCU_Y[i][j] = 0x00;
						if (MCU_Y[i][j] > 0xFF)		MCU_Y[i][j] = 0xFF;
					}
				}

				int		MCU_U[8][8] = { 0 }, MCU_V[8][8] = { 0 };
				double	res_IDCT_U[8][8] = { 0.0 }, res_IDCT_V[8][8] = { 0.0 };
				double	res_afterInvQ_U[8][8] = { 0.0 }, res_afterInvQ_V[8][8] = { 0.0 };
				int		rle_U[128] = { 0 }, rle_V[128] = { 0 };
				int		mat_U[8][8] = { 0 }, mat_V[8][8] = { 0 };	//由rle直接恢复出的原始矩阵,还没有反量化和反Zigzag

				//U-channel
				rd_restore_HuffBit_RLE_Mat(rle_U, mat_U, &newByte, &newBytePos, &prev_DC_U, max_first_Chromi_DC, max_first_Chromi_AC, HuffTbl_len_UV_DC, \
					HuffTbl_len_UV_AC, rd_HuffTbl_UV_DC, rd_HuffTbl_UV_AC, fp, Jheader.DHT_DC1.DC_NRcodes, Jheader.DHT_AC1.AC_NRcodes);
				rd_restore_InvQuantize(res_afterInvQ_U, mat_U, Jheader.DQT1.QTable);
				Inverse_DCT(res_IDCT_U, res_afterInvQ_U);
				for (int i = 0; i < 8; i++) {
					for (int j = 0; j < 8; j++) {
						MCU_U[i][j] = round_double(res_IDCT_U[i][j] + 128.0);
						if (MCU_U[i][j] < 0x00)		MCU_U[i][j] = 0x00;
						if (MCU_U[i][j] > 0xFF)		MCU_U[i][j] = 0xFF;
					}
				}

				//V-channel
				rd_restore_HuffBit_RLE_Mat(rle_V, mat_V, &newByte, &newBytePos, &prev_DC_V, max_first_Chromi_DC, max_first_Chromi_AC, HuffTbl_len_UV_DC, \
					HuffTbl_len_UV_AC, rd_HuffTbl_UV_DC, rd_HuffTbl_UV_AC, fp, Jheader.DHT_DC1.DC_NRcodes, Jheader.DHT_AC1.AC_NRcodes);
				rd_restore_InvQuantize(res_afterInvQ_V, mat_V, Jheader.DQT1.QTable);
				Inverse_DCT(res_IDCT_V, res_afterInvQ_V);
				for (int i = 0; i < 8; i++) {
					for (int j = 0; j < 8; j++) {
						MCU_V[i][j] = round_double(res_IDCT_V[i][j] + 128.0);
						if (MCU_V[i][j] < 0x00)		MCU_V[i][j] = 0x00;
						if (MCU_V[i][j] > 0xFF)		MCU_V[i][j] = 0xFF;
					}
				}

				for (u8 i = 0; i < 8; i++) {		//write Y to YUV
					if (yPos + i >= height)							//图片Y平面的下侧边缘
						break;
					for (u8 j = 0; j < 8; j++) {
						if (xPos + j >= width)						//图片Y平面的右侧边缘
							break;//continue;
						yuv_Y[(yPos + i) * width + xPos + j] = MCU_Y[i][j];
					}
				}
				if (toYUV420) {						//(jpeg444)write UV to YUV_420
					for (u8 i = 0; i < 4; i++) {
						if (yPos / 2 + i >= (height + 1) / 2)		//图片UV420平面的下侧边缘(已考虑奇数高度的图片)
							break;
						for (u8 j = 0; j < 4; j++) {
							if (xPos / 2 + j >= (width + 1) / 2)	//图片UV420平面的右侧边缘(已考虑奇数宽度的图片)
								break;//continue;
							double U_t = 0.0, V_t = 0.0;
							U_t += MCU_U[i * 2 + 0][j * 2 + 0];
							U_t += MCU_U[i * 2 + 0][j * 2 + 1];
							U_t += MCU_U[i * 2 + 1][j * 2 + 0];
							U_t += MCU_U[i * 2 + 1][j * 2 + 1];
							yuv_U[(yPos / 2 + i) * ((width + 1) / 2) + xPos / 2 + j] = (u8)(U_t / 4.0 + 0.5);	//U: width/2

							V_t += MCU_V[i * 2 + 0][j * 2 + 0];
							V_t += MCU_V[i * 2 + 0][j * 2 + 1];
							V_t += MCU_V[i * 2 + 1][j * 2 + 0];
							V_t += MCU_V[i * 2 + 1][j * 2 + 1];
							yuv_V[(yPos / 2 + i) * ((width + 1) / 2) + xPos / 2 + j] = (u8)(V_t / 4.0 + 0.5);	//V: width/2
						}
					}
				}
				else {								//(jpeg444)write UV to YUV_444
					for (u8 i = 0; i < 8; i++) {
						if (yPos + i >= height)						//图片UV平面的下侧边缘
							break;
						for (u8 j = 0; j < 8; j++) {
							if (xPos + j >= width)					//图片UV平面的右侧边缘
								break;//continue;
							yuv_U[(yPos + i) * width + xPos + j] = MCU_U[i][j];
							yuv_V[(yPos + i) * width + xPos + j] = MCU_V[i][j];
						}
					}
				}
			}
		}
	}
	else if (Jheader.clrY_sample == 0x22)					//yuv411(e.g. yuv420p = l420)
	{
		u16 res_sync_count = 0;	//DRI段需要的复位计数器
		for (int yPos = 0; yPos < height; yPos += 16) {
			for (int xPos = 0; xPos < width; xPos += 16) {
				for (int u = 0; u < 4; u++)
				{
					res_sync_count++;
					if (res_sync != 0 && res_sync_count > res_sync) {	//DRI复位差分编码和复位Huffman编码
						newByte = 0, newBytePos = -1;
						prev_DC_Y = 0, prev_DC_U = 0, prev_DC_V = 0;
						fseek(fp, 2, SEEK_CUR);							//跳过RSTm标签0xFFDm
						res_sync_count = 1;
					}
					int		MCU_Y[8][8] = { 0 };
					double	res_IDCT_Y[8][8] = { 0.0 };
					double	res_afterInvQ_Y[8][8] = { 0.0 };
					int		rle_Y[128] = { 0 };
					int		mat_Y[8][8] = { 0 };	//由rle直接恢复出的原始矩阵,还没有反量化和反Zigzag

					//Y-channel
					rd_restore_HuffBit_RLE_Mat(rle_Y, mat_Y, &newByte, &newBytePos, &prev_DC_Y, max_first_Lumi_DC, max_first_Lumi_AC, HuffTbl_len_Y_DC, \
						HuffTbl_len_Y_AC, rd_HuffTbl_Y_DC, rd_HuffTbl_Y_AC, fp, Jheader.DHT_DC0.DC_NRcodes, Jheader.DHT_AC0.AC_NRcodes);
					rd_restore_InvQuantize(res_afterInvQ_Y, mat_Y, Jheader.DQT0.QTable);
					Inverse_DCT(res_IDCT_Y, res_afterInvQ_Y);
					for (int i = 0; i < 8; i++) {
						for (int j = 0; j < 8; j++) {
							MCU_Y[i][j] = round_double(res_IDCT_Y[i][j] + 128.0);
							if (MCU_Y[i][j] < 0x00)		MCU_Y[i][j] = 0x00;
							if (MCU_Y[i][j] > 0xFF)		MCU_Y[i][j] = 0xFF;
						}
					}
					for (u8 i = 0; i < 8; i++) {	//write Y to YUV
						if (yPos + i + (u / 2) * 8 >= height)		//图片Y平面的下侧边缘
							break;
						for (u8 j = 0; j < 8; j++) {
							if (xPos + j + (u % 2) * 8 >= width)	//图片Y平面的右侧边缘
								break;//continue;
							yuv_Y[(yPos + i + (u / 2) * 8) * width + xPos + j + (u % 2) * 8] = MCU_Y[i][j];
						}
					}
				}

				int		MCU_U[8][8] = { 0 }, MCU_V[8][8] = { 0 };
				double	res_IDCT_U[8][8] = { 0.0 }, res_IDCT_V[8][8] = { 0.0 };
				double	res_afterInvQ_U[8][8] = { 0.0 }, res_afterInvQ_V[8][8] = { 0.0 };
				int		rle_U[128] = { 0 }, rle_V[128] = { 0 };
				int		mat_U[8][8] = { 0 }, mat_V[8][8] = { 0 };	//由rle直接恢复出的原始矩阵,还没有反量化和反Zigzag

				//U-channel
				rd_restore_HuffBit_RLE_Mat(rle_U, mat_U, &newByte, &newBytePos, &prev_DC_U, max_first_Chromi_DC, max_first_Chromi_AC, HuffTbl_len_UV_DC, \
					HuffTbl_len_UV_AC, rd_HuffTbl_UV_DC, rd_HuffTbl_UV_AC, fp, Jheader.DHT_DC1.DC_NRcodes, Jheader.DHT_AC1.AC_NRcodes);
				rd_restore_InvQuantize(res_afterInvQ_U, mat_U, Jheader.DQT1.QTable);
				Inverse_DCT(res_IDCT_U, res_afterInvQ_U);
				for (int i = 0; i < 8; i++) {
					for (int j = 0; j < 8; j++) {
						MCU_U[i][j] = round_double(res_IDCT_U[i][j] + 128.0);
						if (MCU_U[i][j] < 0x00)		MCU_U[i][j] = 0x00;
						if (MCU_U[i][j] > 0xFF)		MCU_U[i][j] = 0xFF;
					}
				}

				//V-channel
				rd_restore_HuffBit_RLE_Mat(rle_V, mat_V, &newByte, &newBytePos, &prev_DC_V, max_first_Chromi_DC, max_first_Chromi_AC, HuffTbl_len_UV_DC, \
					HuffTbl_len_UV_AC, rd_HuffTbl_UV_DC, rd_HuffTbl_UV_AC, fp, Jheader.DHT_DC1.DC_NRcodes, Jheader.DHT_AC1.AC_NRcodes);
				rd_restore_InvQuantize(res_afterInvQ_V, mat_V, Jheader.DQT1.QTable);
				Inverse_DCT(res_IDCT_V, res_afterInvQ_V);
				for (int i = 0; i < 8; i++) {
					for (int j = 0; j < 8; j++) {
						MCU_V[i][j] = round_double(res_IDCT_V[i][j] + 128.0);
						if (MCU_V[i][j] < 0x00)		MCU_V[i][j] = 0x00;
						if (MCU_V[i][j] > 0xFF)		MCU_V[i][j] = 0xFF;
					}
				}

				if (toYUV420) {						//(jpeg420)write UV to YUV_420
					for (u8 i = 0; i < 8; i++) {
						if (yPos / 2 + i >= (height + 1) / 2)		//图片UV420平面的下侧边缘(已考虑奇数高度的图片)
							break;
						for (u8 j = 0; j < 8; j++) {
							if (xPos / 2 + j >= (width + 1) / 2)	//图片UV420平面的右侧边缘(已考虑奇数宽度的图片)
								break;//continue;
							yuv_U[(yPos / 2 + i) * ((width + 1) / 2) + xPos / 2 + j] = MCU_U[i][j];	//U: width/2
							yuv_V[(yPos / 2 + i) * ((width + 1) / 2) + xPos / 2 + j] = MCU_V[i][j];	//V: width/2
						}
					}
				}
				else {							//(jpeg420)write UV to YUV_444
					for (u8 i = 0; i < 8; i++) {
						for (u8 j = 0; j < 8; j++) {
							for (u8 u = 0; u < 4; u++) {
								if (yPos + i * 2 + u / 2 >= height || xPos + j * 2 + u % 2 >= width)
									break;//continue;
								yuv_U[(yPos + i * 2 + u / 2) * width + xPos + j * 2 + u % 2] = MCU_U[i][j];
								yuv_V[(yPos + i * 2 + u / 2) * width + xPos + j * 2 + u % 2] = MCU_V[i][j];
							}
						}
					}
				}
			}
		}
	}
	else if (Jheader.clrY_sample == 0x21)					//yuv422
	{
		//yuv422
	}
	else;

	// EOI
	fread(&seg_label_t, 2, 1, fp);
	Jheader.EOI = rd_BigEnd16(seg_label_t);

	// free all malloc space
	for (u8 m = 1; m < 17; m++) {
		free(HuffTbl_len_Y_DC[m]);	HuffTbl_len_Y_DC[m] = NULL;
		free(HuffTbl_len_Y_AC[m]);	HuffTbl_len_Y_AC[m] = NULL;
		free(HuffTbl_len_UV_DC[m]);	HuffTbl_len_UV_DC[m] = NULL;
		free(HuffTbl_len_UV_AC[m]);	HuffTbl_len_UV_AC[m] = NULL;
	}
	free(rd_HuffTbl_Y_DC);
	free(rd_HuffTbl_Y_AC);
	free(rd_HuffTbl_UV_DC);
	free(rd_HuffTbl_UV_AC);
	free(Jheader.DHT_DC0.DC_Values);
	free(Jheader.DHT_DC1.DC_Values);
	free(Jheader.DHT_AC0.AC_Values);
	free(Jheader.DHT_AC1.AC_Values);
	return;
}


