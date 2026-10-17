#ifndef XML2_ENCODING_H
#define XML2_ENCODING_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static inline int UTF8ToUTF16LE(unsigned char* outb, int *outlen, const unsigned char* in, int *inlen) {
    if (!outb || !outlen || !in || !inlen) return -1;
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    int max_wchars = *outlen / 2;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)in, *inlen, (wchar_t*)outb, max_wchars);
    if (wlen <= 0 && *inlen > 0) {
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) return -1;
        return -2;
    }
    ((wchar_t*)outb)[wlen] = 0;
    *outlen = wlen * 2;
    return *outlen;
#else
    int written = 0;
    for (int i = 0; i < *inlen && written + 2 <= *outlen; i++) {
        outb[written++] = in[i];
        outb[written++] = 0;
    }
    *outlen = written;
    return written;
#endif
}

static inline int UTF16BEToUTF8(unsigned char* outb, int *outlen, const unsigned char* in, int *inlen) {
    if (!outb || !outlen || !in || !inlen) return -1;
    int out_idx = 0;
    for (int i = 0; i + 1 < *inlen && out_idx + 1 < *outlen; i += 2) {
        uint16_t ch = ((uint16_t)in[i] << 8) | in[i + 1];
        if (ch == 0) break;
        if (ch < 0x80) {
            outb[out_idx++] = (unsigned char)ch;
        } else if (ch < 0x800) {
            if (out_idx + 2 >= *outlen) break;
            outb[out_idx++] = (unsigned char)(0xC0 | (ch >> 6));
            outb[out_idx++] = (unsigned char)(0x80 | (ch & 0x3F));
        } else {
            if (out_idx + 3 >= *outlen) break;
            outb[out_idx++] = (unsigned char)(0xE0 | (ch >> 12));
            outb[out_idx++] = (unsigned char)(0x80 | ((ch >> 6) & 0x3F));
            outb[out_idx++] = (unsigned char)(0x80 | (ch & 0x3F));
        }
    }
    outb[out_idx] = 0;
    *outlen = out_idx;
    return out_idx;
}

static inline int UTF16LEToUTF8(unsigned char* outb, int *outlen, const unsigned char* in, int *inlen) {
    if (!outb || !outlen || !in || !inlen) return -1;
    int out_idx = 0;
    for (int i = 0; i + 1 < *inlen && out_idx + 1 < *outlen; i += 2) {
        uint16_t ch = (uint16_t)in[i] | ((uint16_t)in[i + 1] << 8);
        if (ch == 0) break;
        if (ch < 0x80) {
            outb[out_idx++] = (unsigned char)ch;
        } else if (ch < 0x800) {
            if (out_idx + 2 >= *outlen) break;
            outb[out_idx++] = (unsigned char)(0xC0 | (ch >> 6));
            outb[out_idx++] = (unsigned char)(0x80 | (ch & 0x3F));
        } else {
            if (out_idx + 3 >= *outlen) break;
            outb[out_idx++] = (unsigned char)(0xE0 | (ch >> 12));
            outb[out_idx++] = (unsigned char)(0x80 | ((ch >> 6) & 0x3F));
            outb[out_idx++] = (unsigned char)(0x80 | (ch & 0x3F));
        }
    }
    outb[out_idx] = 0;
    *outlen = out_idx;
    return out_idx;
}
#endif
