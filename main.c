#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>		//malloc, free
#include <string.h>

#include "fmt_jpeg.h"


void active_JPEG_dec(char* path_dep, char* path_des, int fmt);


//��cmd��ʹ��ffmpeg��yuv�ļ�ָ��: ffplay -f rawvideo -pixel_format yuv420p -s 1080*2340 xxx.yuv
int main(int argc, char* argv[])
{
	char path1[100] = "C:\\Users\\93052\\Desktop\\SDcard\\PICTURE\\jpg\\wallpaper0.jpg";
	char path2[100] = "C:\\Users\\93052\\Desktop\\SDcard\\PICTURE\\jpg\\wallpaper0.yuv";
	//string	path_deprt = "D:\\��־��\\��һ����\\��������2021\\data\\";

	active_JPEG_dec(path1, path2, 444);

	return 0;
}



void active_JPEG_dec(char* path_dep, char* path_des, int fmt)
{
	if (fmt != 420 && fmt != 411 && fmt != 444) {
		puts("invalid yuv format.\n");
		return;
	}
	FILE* fp_rd, * fp_wr;
	if ((fp_rd = fopen(path_dep, "rb")) == NULL) {
		puts("111111");
		return;
	}

	//Ԥ��ȡͼƬ�ߴ�Ͳ�����ʽ
	u8 sample_t = 0;
	u16 width = 0, height = 0;
	rd_SOF0_2get_size(fp_rd, &sample_t, &width, &height);
	printf("width=%d, height=%d\n", width, height);
	if (sample_t != 0x11 && sample_t != 0x22) {
		puts("222222");
		return;
	}

	//����YUV��ʽ����U/Vƽ��Ĵ�С
	size_t uvplane_size = 0;
	if (fmt != 444)		uvplane_size = (size_t)((height + 1) / 2) * ((width + 1) / 2);
	else				uvplane_size = (size_t)height * width;

	//����Y/U/V���ڴ�
	u8* yuv_Y = (u8*)malloc(sizeof(u8) * width * height);
	u8* yuv_U = (u8*)malloc(sizeof(u8) * uvplane_size);
	u8* yuv_V = (u8*)malloc(sizeof(u8) * uvplane_size);
	if (yuv_Y == NULL || yuv_U == NULL || yuv_V == NULL) {
		puts("3333333");
		return;
	}

	//����
	if (fmt != 444)
		Dec_JPEG_to_YUV(fp_rd, yuv_Y, yuv_U, yuv_V, 1);
	else
		Dec_JPEG_to_YUV(fp_rd, yuv_Y, yuv_U, yuv_V, 0);

	fclose(fp_rd);
	fp_rd = NULL;

	if ((fp_wr = fopen(path_des, "wb")) == NULL) {
		puts("444444");
		return;
	}
	//д��
	fwrite(yuv_Y, (size_t)width * height, 1, fp_wr);
	fwrite(yuv_U, (size_t)uvplane_size, 1, fp_wr);
	fwrite(yuv_V, (size_t)uvplane_size, 1, fp_wr);

	fclose(fp_wr);
	fp_wr = NULL;

	//�ͷſռ�
	free(yuv_Y);
	free(yuv_U);
	free(yuv_V);

	return;
}

