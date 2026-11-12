/* Decoder for the PKWARE Data Compression Library "implode" format (the
 * files inside Alien Trilogy's installer archives, *.SHR).  A stream starts
 * with two bytes: literal coding (0 = plain 8-bit literals, 1 = Huffman) and
 * the dictionary size (4, 5 or 6 = 1, 2 or 4 KB).  Then, bit by bit (LSB
 * first): 0 + literal, or 1 + length code + distance code; length 519 ends
 * the stream.  The Huffman tables are fixed and stored here in the format's
 * compact form: each byte = (count - 1) << 4 | code length. */
#include <stdlib.h>
#include <string.h>
#include "setup.h"

static const u8 LITLEN[] = {
    11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8, 9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24,
    12, 11, 7, 9, 11, 12, 6, 7, 22, 5, 7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
    8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27, 44, 253, 253, 253, 252, 252, 252,
    13, 12, 45, 12, 45, 12, 61, 12, 45, 44, 173};
static const u8 LENLEN[] = {2, 35, 36, 53, 38, 23};
static const u8 DISTLEN[] = {2, 20, 53, 230, 247, 151, 248};
static const u16 LBASE[16] = {3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
static const u8 LEXTRA[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};

typedef struct { short count[14], sym[256]; } Huff;
typedef struct { const u8 *in; size_t n, pos; u32 buf; int cnt; int err; } Bits;

static void build(Huff *h, const u8 *rep, int nrep)
{
    u8 len[256];
    int n = 0;
    for (int i = 0; i < nrep; i++)
        for (int k = 0; k <= rep[i] >> 4 && n < 256; k++) len[n++] = rep[i] & 15;
    memset(h->count, 0, sizeof h->count);
    for (int s = 0; s < n; s++) h->count[len[s]]++;
    short offs[14];
    offs[1] = 0;
    for (int l = 1; l < 13; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int s = 0; s < n; s++) if (len[s]) h->sym[offs[len[s]]++] = (short)s;
}

static u32 getbits(Bits *b, int need)
{
    while (b->cnt < need) {
        if (b->pos >= b->n) { b->err = 1; return 0; }
        b->buf |= (u32)b->in[b->pos++] << b->cnt;
        b->cnt += 8;
    }
    u32 v = b->buf & ((1u << need) - 1);
    b->buf >>= need;
    b->cnt -= need;
    return v;
}

/* the codes are stored bit-inverted, first bit first */
static int decode(Bits *b, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 14; l++) {
        code |= (int)getbits(b, 1) ^ 1;
        int c = h->count[l];
        if (code < first + c) return h->sym[index + (code - first)];
        index += c;
        first = (first + c) << 1;
        code <<= 1;
    }
    b->err = 1;
    return 0;
}

/* in -> malloc'ed out (*outn bytes); NULL on a broken stream */
u8 *explode(const u8 *in, size_t n, size_t *outn)
{
    static Huff lit, len, dist;
    static int ready;
    if (!ready) {
        build(&lit, LITLEN, sizeof LITLEN);
        build(&len, LENLEN, sizeof LENLEN);
        build(&dist, DISTLEN, sizeof DISTLEN);
        ready = 1;
    }
    Bits b = {in, n, 0, 0, 0, 0};
    u32 coded = getbits(&b, 8), dict = getbits(&b, 8);
    if (b.err || coded > 1 || dict < 4 || dict > 6) return NULL;
    size_t cap = n * 4 + 65536, o = 0;
    u8 *out = malloc(cap);
    for (;;) {
        if (b.err) { free(out); return NULL; }
        if (getbits(&b, 1)) {
            int s = decode(&b, &len);
            u32 l = LBASE[s] + getbits(&b, LEXTRA[s]);
            if (l == 519) break;
            int sh = l == 2 ? 2 : (int)dict;
            u32 d = ((u32)decode(&b, &dist) << sh) + getbits(&b, sh) + 1;
            if (d > o) { free(out); return NULL; }
            if (o + l > cap) { cap = cap * 2 + l; out = realloc(out, cap); }
            for (u32 i = 0; i < l; i++, o++) out[o] = out[o - d];
        } else {
            if (o + 1 > cap) { cap *= 2; out = realloc(out, cap); }
            out[o++] = (u8)(coded ? decode(&b, &lit) : (int)getbits(&b, 8));
        }
    }
    *outn = o;
    return out;
}

/* Alien Trilogy's installer archive (*.SHR): a 226-byte header (file name at
   0x3a, packed size at 0xc2) and one imploded file */
u8 *shr_unpack(const u8 *shr, size_t n, char *name, size_t namesz, size_t *outn)
{
    if (n < 0xe2) return NULL;
    size_t k = 0;
    for (; k + 1 < namesz && 0x3a + k < 0xc2 && shr[0x3a + k]; k++) name[k] = (char)shr[0x3a + k];
    name[k] = 0;
    u32 packed = shr[0xc2] | shr[0xc3] << 8 | shr[0xc4] << 16 | (u32)shr[0xc5] << 24;
    if (!k || 0xe2 + (size_t)packed > n) return NULL;
    return explode(shr + 0xe2, packed, outn);
}
