#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "crc32.h"
#include "adler32.h"
#include "deflate_reader.h"
#include "png_reader.h"

/* http://www.libpng.org/pub/png/spec/1.2/PNG-Contents.html */

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
 #define TYPE_ANCILLARY 0x00000020
 #define TYPE_PRIVATE   0x00002000
 #define TYPE_SAFE2COPY 0x20000000

 #define TYPE_IHDR      0x52444849
 #define TYPE_PLTE      0x45544C50
 #define TYPE_IDAT      0x54414449
 #define TYPE_IEND      0x444E4549

#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
 #define TYPE_ANCILLARY 0x20000000
 #define TYPE_PRIVATE   0x00200000
 #define TYPE_SAFE2COPY 0x00000020

 #define TYPE_IHDR      0x49484452
 #define TYPE_PLTE      0x504C5445
 #define TYPE_IDAT      0x49444154
 #define TYPE_IEND      0x49454E44
#endif

struct lib_png_reader *
lib_png_new(lib_reader_read_t read, void *read_p)
{
	struct lib_png_reader *pr = (struct lib_png_reader *) malloc(sizeof(struct lib_png_reader));
	if (pr == NULL)
		return NULL;

	memset(pr, 0, sizeof(struct lib_png_reader));
	pr->read = read;
	pr->read_p = read_p;

	return pr;
}

ssize_t
lib_png_chunk_read(struct lib_png_reader *pr)
{
	struct lib_png_chunk *c = &pr->chunk;

	if (c->in_chunk)
		return -1;

	ssize_t res = pr->read(pr->read_p, (uint8_t *) &c->len, 4);
	if (res < 0)
		return res;
	if (res < 4)
		return -1; // not enough bytes

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	c->len = __builtin_bswap32(c->len);
#endif

	res = pr->read(pr->read_p, (uint8_t *) &c->type, 4);
	if (res < 0)
		return res;
	if (res < 4)
		return -1; // not enough bytes

	c->crc = lib_crc32((uint8_t *) &c->type, 4, LIB_CRC32_INIT);
	c->in_chunk = true;

	return c->len;
}

ssize_t
lib_png_chunk_read_data(struct lib_png_reader *pr, uint8_t *buf, size_t buf_len)
{
	struct lib_png_chunk *c = &pr->chunk;

	if (buf_len > c->len)
		buf_len = c->len;

	ssize_t res = pr->read(pr->read_p, buf, buf_len);
	if (res < 0)
		return res;
	if (res < buf_len)
		return -1; // not enough bytes

	c->len -= buf_len;
	c->crc = lib_crc32(buf, buf_len, c->crc);

	if (c->len == 0)
	{
		uint32_t crc_check;
		ssize_t res = pr->read(pr->read_p, (uint8_t *) &crc_check, 4);
		if (res < 0)
			return res;
		if (res < 4)
			return -1; // not enough bytes

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		crc_check = __builtin_bswap32(crc_check);
#endif
		if (c->crc != crc_check)
			return -1; // crc32 mismatch

		c->in_chunk = false;
	}

	return buf_len;
}

ssize_t
lib_png_chunk_read_idat(struct lib_png_reader *pr, uint8_t *buf, size_t buf_len)
{
	ssize_t res2 = 0;
	while (buf_len > 0)
	{
		while (!pr->chunk.in_chunk)
		{
			ssize_t res = lib_png_chunk_read(pr);
			if (res < 0)
				return res;
			if (pr->chunk.type == TYPE_IDAT)
				break;

			if ((pr->chunk.type & TYPE_ANCILLARY) == 0)
				return -1; // unknown critical chunk

			// skip chunk
			while (pr->chunk.in_chunk)
			{
				uint8_t buf[256];
				res = lib_png_chunk_read_data(pr, buf, sizeof(buf));
				if (res < 0)
					return res;
			}
		}

		ssize_t res = lib_png_chunk_read_data(pr, buf, buf_len);
		if (res < 0)
			return res;

		buf_len -= res;
		buf = &buf[res];
		res2 += res;
	}

	return res2;
}

static inline ssize_t
lib_png_deflate_read(struct lib_png_reader *pr, uint8_t *buf, size_t buf_len)
{
	ssize_t res = lib_deflate_read(pr->dr, buf, buf_len);
	if (res < 0)
		return res;
	if (res < buf_len)
		return -1;
	pr->adler = lib_adler32(buf, buf_len, pr->adler);

	return res;
}

static inline int
lib_png_decode(struct lib_png_reader *pr, uint32_t width, uint32_t height, uint32_t scanline_width, uint8_t *dst, int dst_width, int dst_height, int dst_pixlen, int dst_linelen)
{
	memset(pr->scanline, 0, scanline_width);

	uint32_t y;
	for (y=0; y<height; y++)
	{
		uint8_t filter_type;
		ssize_t res = lib_png_deflate_read(pr, &filter_type, 1);
		if (res < 0)
			return res;

		if (filter_type < 2)
		{
			ssize_t res = lib_png_deflate_read(pr, pr->scanline, scanline_width);
			if (res < 0)
				return res;
		}
		else
		{
			ssize_t res = lib_png_deflate_read(pr, &pr->scanline[scanline_width], scanline_width);
			if (res < 0)
				return res;
		}

		uint32_t x;
		uint8_t prev[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		uint8_t up_prev[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		switch(filter_type)
		{
			case 0: // none
				break;

			case 1: // sub
				for (x=0; x<scanline_width; x++)
				{
					prev[x % pr->scanline_bpp] = pr->scanline[x] = pr->scanline[x] + prev[x % pr->scanline_bpp];
				}
				break;

			case 2: // up
				for (x=0; x<scanline_width; x++)
				{
					pr->scanline[x] += pr->scanline[scanline_width + x];
				}
				break;

			case 3: // average
				for (x=0; x<scanline_width; x++)
				{
					uint32_t left = prev[x % pr->scanline_bpp];
					uint32_t up   = pr->scanline[x];
					prev[x % pr->scanline_bpp] = pr->scanline[x] = ((left + up) >> 1) + pr->scanline[scanline_width + x]; 
				}
				break;

			case 4: // paeth
				for (x=0; x<scanline_width; x++)
				{
					int32_t a = prev[x % pr->scanline_bpp]; // left
					int32_t b = pr->scanline[x]; // up
					int32_t c = up_prev[x % pr->scanline_bpp]; // up-left
					up_prev[x % pr->scanline_bpp] = b;
					int32_t p = a + b - c;
					int32_t pa = p < a ? a - p : p - a;
					int32_t pb = p < b ? b - p : p - b;
					int32_t pc = p < c ? c - p : p - c;
					if (pa <= pb && pa <= pc)
					{
						prev[x % pr->scanline_bpp] = pr->scanline[x] = a + pr->scanline[scanline_width + x];
					}
					else if (pb <= pc)
					{
						prev[x % pr->scanline_bpp] = pr->scanline[x] = b + pr->scanline[scanline_width + x];
					}
					else
					{
						prev[x % pr->scanline_bpp] = pr->scanline[x] = c + pr->scanline[scanline_width + x];
					}
				}
				break;

			default:
				return -1;
		}

		for (x=0; x<width; x++)
		{
			uint16_t r=0, g=0, b=0, a=0;
			switch (pr->ihdr.color_type)
			{
				case 0:
					a = 0xffff;
					switch (pr->ihdr.bit_depth)
					{
						case 16:
							r = g = b = (pr->scanline[x*2 + 0] << 8) | pr->scanline[x*2 + 1];
							break;
						case 8:
							r = g = b = pr->scanline[x] * 0x0101;
							break;
						case 4:
							r = g = b = ((pr->scanline[x>>1] >> (((x^1)&1)*4)) & 15) * 0x1111;
							break;
						case 2:
							r = g = b = ((pr->scanline[x>>2] >> (((x^3)&3)*2)) & 3) * 0x5555;
							break;
						case 1:
							r = g = b = ((pr->scanline[x>>3] >> ((x^7)&7)) & 1) * 0xffff;
							break;
					}
					break;
				case 2:
					a = 0xffff;
					switch (pr->ihdr.bit_depth)
					{
						case 16:
							r = (pr->scanline[x*6 + 0] << 8) | pr->scanline[x*6 + 1];
							g = (pr->scanline[x*6 + 2] << 8) | pr->scanline[x*6 + 3];
							b = (pr->scanline[x*6 + 4] << 8) | pr->scanline[x*6 + 5];
							break;
						case 8:
							r = pr->scanline[x*3 + 0] * 0x0101;
							g = pr->scanline[x*3 + 1] * 0x0101;
							b = pr->scanline[x*3 + 2] * 0x0101;
							break;
					}
					break;
				case 3:
					a = 0xffff;
					switch (pr->ihdr.bit_depth)
					{
						case 8:
							b = pr->scanline[x];
							break;
						case 4:
							b = (pr->scanline[x>>1] >> (((x^1)&1)*4)) & 15;
							break;
						case 2:
							b = (pr->scanline[x>>2] >> (((x^3)&3)*2)) & 3;
							break;
						case 1:
							b = (pr->scanline[x>>3] >> ((x^7)&7)) & 1;
							break;
					}
					if (b < pr->palette_len)
					{
						r = pr->palette[b*3 + 0] * 0x0101;
						g = pr->palette[b*3 + 1] * 0x0101;
						b = pr->palette[b*3 + 2] * 0x0101;
					}
					else
					{
						r = g = b = 0; // or should this be an error?
					}
					break;
				case 4:
					switch (pr->ihdr.bit_depth)
					{
						case 16:
							r = g = b = (pr->scanline[x*4 + 0] << 8) | pr->scanline[x*4 + 1];
							a = (pr->scanline[x*4 + 2] << 8) | pr->scanline[x*4 + 3];
							break;
						case 8:
							r = g = b = pr->scanline[x*2 + 0] * 0x0101;
							a = pr->scanline[x*2 + 1] * 0x0101;
							break;
					}
					break;
				case 6:
					switch (pr->ihdr.bit_depth)
					{
						case 16:
							r = (pr->scanline[x*8 + 0] << 8) | pr->scanline[x*8 + 1];
							g = (pr->scanline[x*8 + 2] << 8) | pr->scanline[x*8 + 3];
							b = (pr->scanline[x*8 + 4] << 8) | pr->scanline[x*8 + 5];
							a = (pr->scanline[x*8 + 6] << 8) | pr->scanline[x*8 + 7];
							break;
						case 8:
							r = pr->scanline[x*4 + 0] * 0x0101;
							g = pr->scanline[x*4 + 1] * 0x0101;
							b = pr->scanline[x*4 + 2] * 0x0101;
							a = pr->scanline[x*4 + 3] * 0x0101;
							break;
					}
					break;
			}

			uint32_t grey = ( r + g + b + 1 ) / 3;
			if (a != 0 && x < dst_width && y < dst_height)
			{
				int ptr = y * dst_linelen + x * dst_pixlen;
				if (a == 0xffff)
				{
					dst[ptr] = grey >> 8;
				}
				else
				{
					uint32_t v = dst[ptr] * 0x0101 * (0xffff - a) + grey * a;
					v += 0x8000;
					v /= 0xffff;
					dst[ptr] = v >> 8;
				}
			}
		}
	}

	return 0;
}

int
lib_png_load_image(struct lib_png_reader *pr, uint8_t *dst, int dst_width, int dst_height, int dst_linelen)
{
	static const uint8_t png_sig[8] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
	};

	uint8_t sig[8];
	ssize_t res = pr->read(pr->read_p, sig, 8);
	if (res < 0)
		return res;
	if (res < 8)
		return -1; // not enough bytes

	if (memcmp(sig, png_sig, 8) != 0)
		return -1; // signature mismatch

	res = lib_png_chunk_read(pr);
	if (res < 0)
		return res;
	if (pr->chunk.type != TYPE_IHDR)
		return -1;

	res = lib_png_chunk_read_data(pr, (uint8_t *) &pr->ihdr, sizeof(struct lib_png_ihdr));
	if (res < 0)
		return res;
	if (res < sizeof(struct lib_png_ihdr))
		return -1; // chunk too small?

	if (pr->chunk.len != 0)
		return -1;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	pr->ihdr.width  = __builtin_bswap32(pr->ihdr.width);
	pr->ihdr.height = __builtin_bswap32(pr->ihdr.height);
#endif
	// verify png_ihdr
	if (pr->ihdr.width == 0 || pr->ihdr.width > 0x80000000)
		return -1;
	if (pr->ihdr.height == 0 || pr->ihdr.height > 0x80000000)
		return -1;
	switch (pr->ihdr.color_type)
	{
		case 0:
			if (pr->ihdr.bit_depth != 1
			 && pr->ihdr.bit_depth != 2
			 && pr->ihdr.bit_depth != 4
			 && pr->ihdr.bit_depth != 8
			 && pr->ihdr.bit_depth != 16)
				return -1;
			break;

		case 2:
		case 4:
		case 6:
			if (pr->ihdr.bit_depth != 8
			 && pr->ihdr.bit_depth != 16)
				return -1;
			break;

		case 3:
			if (pr->ihdr.bit_depth != 1
			 && pr->ihdr.bit_depth != 2
			 && pr->ihdr.bit_depth != 4
			 && pr->ihdr.bit_depth != 8)
				return -1;
			break;

		default:
			return -1;
	}
	if (pr->ihdr.compression_method != 0)
		return -1;
	if (pr->ihdr.filter_method != 0)
		return -1;
	if (pr->ihdr.interlace_method > 1)
		return -1;

	// read chunks until we locate the IDAT chunk
	while (1)
	{
		res = lib_png_chunk_read(pr);
		if (res < 0)
			return res;
		if (pr->chunk.type == TYPE_IDAT)
			break;

		if (pr->chunk.type == TYPE_PLTE)
		{
			if (pr->palette != NULL)
				return -1; // only one palette allowed
			if (pr->chunk.len > 768 || pr->chunk.len % 3 != 0)
				return -1; // should be a multiple of 3

			pr->palette = (uint8_t *) malloc(pr->chunk.len);
			if (pr->palette == NULL)
				return -1; // out of memory
			pr->palette_len = pr->chunk.len / 3;

			res = lib_png_chunk_read_data(pr, pr->palette, pr->chunk.len);
			if (res < 0)
				return res;
			continue;
		}

		if ((pr->chunk.type & TYPE_ANCILLARY) == 0)
			return -1; // unknown critical chunk

		// skip chunk
		while (pr->chunk.in_chunk)
		{
			uint8_t buf[256];
			res = lib_png_chunk_read_data(pr, buf, sizeof(buf));
			if (res < 0)
				return res;
		}
	}

	// do we have a palette?
	if (pr->ihdr.color_type == 3 && pr->palette == NULL)
		return -1; // no palette found
	if (pr->ihdr.color_type == 0 && pr->palette != NULL)
		return -1; // no palette allowed
	if (pr->ihdr.color_type == 4 && pr->palette != NULL)
		return -1; // no palette allowed

	// allocate scanline
	uint32_t bits_per_pixel = pr->ihdr.bit_depth;
	if (pr->ihdr.color_type == 2)
		bits_per_pixel *= 3; // r, g, b
	if (pr->ihdr.color_type == 4)
		bits_per_pixel *= 2; // grey, alpha
	if (pr->ihdr.color_type == 6)
		bits_per_pixel *= 4; // r, g, b, alpha

	uint32_t scanline_bits = pr->ihdr.width * bits_per_pixel;

	pr->scanline_bpp   = (bits_per_pixel + 7) >> 3;
	pr->scanline_width = (scanline_bits + 7) >> 3;

	pr->scanline = (uint8_t *) malloc(pr->scanline_width * 2);
	if (pr->scanline == NULL)
		return -1; // out of memory

	// start parsing IDAT
	uint8_t rfc1950_hdr[2];
	res = lib_png_chunk_read_idat(pr, rfc1950_hdr, 2);
	if (res < 0)
		return res;
	if (res < 2)
		return -1;

	if ((rfc1950_hdr[0] & 0x0f) != 0x08) // should be deflate algorithm
		return -1;
	if (rfc1950_hdr[0] > 0x78) // max window size is 32 KB
		return -1;
	if (rfc1950_hdr[1] & 0x20) // preset dictionary not allowed
		return -1;
	if (((rfc1950_hdr[0] << 8) + rfc1950_hdr[1]) % 31 != 0) // check checksum
		return -1;

	pr->dr = lib_deflate_new((lib_reader_read_t) &lib_png_chunk_read_idat, pr);
	if (pr->dr == NULL)
		return -1; // out of memory

	pr->adler = LIB_ADLER32_INIT;

	if (pr->ihdr.interlace_method == 0)
	{
		res = lib_png_decode(pr, pr->ihdr.width, pr->ihdr.height, pr->scanline_width, dst, dst_width, dst_height, 1, dst_linelen);
		if (res < 0)
			return res;
	}
	else
	{
		/* pass 1 */
		uint32_t width  = ( pr->ihdr.width  + 7 ) >> 3;
		uint32_t height = ( pr->ihdr.height + 7 ) >> 3;

		if (width && height)
		{
			uint32_t scanline_width = (width * bits_per_pixel + 7) >> 3;
			res = lib_png_decode(pr, width, height, scanline_width, dst, dst_width, dst_height, 8, dst_linelen*8);
			if (res < 0)
				return res;
		}

		/* pass 2 */
		width  = ( pr->ihdr.width  + 3 ) >> 3;
		height = ( pr->ihdr.height + 7 ) >> 3;

		if (width && height)
		{
			uint32_t scanline_width = (width * bits_per_pixel + 7) >> 3;
			res = lib_png_decode(pr, width, height, scanline_width, &dst[4], dst_width, dst_height, 8, dst_linelen*8);
			if (res < 0)
				return res;
		}

		/* pass 3 */
		width  = ( pr->ihdr.width  + 3 ) >> 2;
		height = ( pr->ihdr.height + 3 ) >> 3;

		if (width && height)
		{
			uint32_t scanline_width = (width * bits_per_pixel + 7) >> 3;
			res = lib_png_decode(pr, width, height, scanline_width, &dst[4*dst_linelen], dst_width, dst_height, 4, dst_linelen*8);
			if (res < 0)
				return res;
		}

		/* pass 4 */
		width  = ( pr->ihdr.width  + 1 ) >> 2;
		height = ( pr->ihdr.height + 3 ) >> 2;

		if (width && height)
		{
			uint32_t scanline_width = (width * bits_per_pixel + 7) >> 3;
			res = lib_png_decode(pr, width, height, scanline_width, &dst[2], dst_width, dst_height, 4, dst_linelen*4);
			if (res < 0)
				return res;
		}

		/* pass 5 */
		width  = ( pr->ihdr.width  + 1 ) >> 1;
		height = ( pr->ihdr.height + 1 ) >> 2;

		if (width && height)
		{
			uint32_t scanline_width = (width * bits_per_pixel + 7) >> 3;
			res = lib_png_decode(pr, width, height, scanline_width, &dst[2*dst_linelen], dst_width, dst_height, 2, dst_linelen*4);
			if (res < 0)
				return res;
		}

		/* pass 6 */
		width  = ( pr->ihdr.width  + 0 ) >> 1;
		height = ( pr->ihdr.height + 1 ) >> 1;

		if (width && height)
		{
			uint32_t scanline_width = (width * bits_per_pixel + 7) >> 3;
			res = lib_png_decode(pr, width, height, scanline_width, &dst[1], dst_width, dst_height, 2, dst_linelen*2);
			if (res < 0)
				return res;
		}

		/* pass 7 */
		width  = pr->ihdr.width;
		height = ( pr->ihdr.height ) >> 1;

		if (width && height)
		{
			uint32_t scanline_width = (width * bits_per_pixel + 7) >> 3;
			res = lib_png_decode(pr, width, height, scanline_width, &dst[dst_linelen], dst_width, dst_height, 1, dst_linelen*2);
			if (res < 0)
				return res;
		}
	}

	// ensure we're at the end of the deflate stream
	{
		uint8_t test_eos;
		ssize_t res = lib_deflate_read(pr->dr, &test_eos, 1);
		if (res < 0)
			return res;
		if (res != 0)
			return -1;
	}

	// verify adler
	uint32_t adler_chk;
	res = lib_png_chunk_read_idat(pr, (uint8_t *) &adler_chk, 4);
	if (res < 0)
		return res;
	if (res < 4)
		return -1;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	adler_chk = __builtin_bswap32(adler_chk);
#endif
	if (pr->adler != adler_chk)
		return -1; // deflate checksum failed

	// verify IEND

	// read chunks until we locate the IEND chunk
	while (1)
	{
		res = lib_png_chunk_read(pr);
		if (res < 0)
			return res;
		if (pr->chunk.type == TYPE_IEND)
			break;

		if ((pr->chunk.type & TYPE_ANCILLARY) == 0)
			return -1; // unknown critical chunk

		// skip chunk
		while (pr->chunk.in_chunk)
		{
			uint8_t buf[256];
			res = lib_png_chunk_read_data(pr, buf, sizeof(buf));
			if (res < 0)
				return res;
		}
	}

	if (pr->chunk.len != 0)
		return -1; // should be 0

	res = lib_png_chunk_read_data(pr, NULL, 1);
	if (res < 0)
		return res;

	return 0;
}

void
lib_png_destroy(struct lib_png_reader *pr)
{
	if (pr->palette)
		free(pr->palette);
	pr->palette = NULL;

	if (pr->scanline)
		free(pr->scanline);
	pr->scanline = NULL;

	if (pr->dr)
		lib_deflate_destroy(pr->dr);
	pr->dr = NULL;

	free(pr);
}
