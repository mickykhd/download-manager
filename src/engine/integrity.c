#include "internal.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* --- Minimal public-domain SHA-256 (based on the standard reference) --- */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    uint32_t datalen;
} sha256_ctx;

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x,2) ^ ROTR(x,13) ^ ROTR(x,22))
#define EP1(x) (ROTR(x,6) ^ ROTR(x,11) ^ ROTR(x,25))
#define SIG0(x) (ROTR(x,7) ^ ROTR(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_init(sha256_ctx *c) {
    c->datalen = 0; c->bitlen = 0;
    c->state[0]=0x6a09e667; c->state[1]=0xbb67ae85; c->state[2]=0x3c6ef372;
    c->state[3]=0xa54ff53a; c->state[4]=0x510e527f; c->state[5]=0x9b05688c;
    c->state[6]=0x1f83d9ab; c->state[7]=0x5be0cd19;
}

static void sha256_transform(sha256_ctx *c, const uint8_t *d) {
    uint32_t a,b,e,f,g,h,t1,t2,m[64];
    uint32_t cc;
    int i,j;
    for (i=0,j=0;i<16;i++,j+=4)
        m[i]=(d[j]<<24)|(d[j+1]<<16)|(d[j+2]<<8)|d[j+3];
    for (;i<64;i++)
        m[i]=SIG1(m[i-2])+m[i-7]+SIG0(m[i-15])+m[i-16];
    a=c->state[0]; b=c->state[1]; cc=c->state[2]; e=c->state[4];
    f=c->state[5]; g=c->state[6]; h=c->state[7];
    uint32_t dd=c->state[3];
    for (i=0;i<64;i++) {
        t1=h+EP1(e)+CH(e,f,g)+K[i]+m[i];
        t2=EP0(a)+MAJ(a,b,cc);
        h=g; g=f; f=e; e=dd+t1; dd=cc; cc=b; b=a; a=t1+t2;
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=dd;
    c->state[4]+=e; c->state[5]+=f; c->state[6]+=g; c->state[7]+=h;
}

static void sha256_update(sha256_ctx *c, const uint8_t *d, size_t len) {
    for (size_t i=0;i<len;i++) {
        c->data[c->datalen++]=d[i];
        if (c->datalen==64) { sha256_transform(c,c->data); c->bitlen+=512; c->datalen=0; }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t *hash) {
    uint32_t i=c->datalen;
    c->data[i++]=0x80;
    if (c->datalen<56) { while(i<56) c->data[i++]=0; }
    else { while(i<64) c->data[i++]=0; sha256_transform(c,c->data); memset(c->data,0,56); }
    c->bitlen+=(uint64_t)c->datalen*8;
    for (int k=0;k<8;k++) c->data[63-k]=(uint8_t)(c->bitlen>>(k*8));
    sha256_transform(c,c->data);
    for (i=0;i<4;i++)
        for (int k=0;k<8;k++)
            hash[k*4+i]=(uint8_t)((c->state[k]>>(24-i*8))&0xff);
}

cdm_status cdm_verify_sha256(const char *path, const char *expected_hex) {
    if (!expected_hex || !*expected_hex) return CDM_OK;

    FILE *f = fopen(path, "rb");
    if (!f) return CDM_ERR_IO;

    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_update(&c, buf, n);
    fclose(f);

    uint8_t digest[32];
    sha256_final(&c, digest);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);

    /* case-insensitive compare, tolerate leading/trailing spaces */
    const char *e = expected_hex;
    while (*e == ' ') e++;
    for (int i = 0; i < 64; i++) {
        char a = hex[i];
        char b = e[i];
        if (b >= 'A' && b <= 'F') b = (char)(b - 'A' + 'a');
        if (a != b) return CDM_ERR_INTEGRITY;
    }
    return CDM_OK;
}
