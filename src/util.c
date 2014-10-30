#include "util.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <sys/time.h>
#include "main.h"
#include "endian.h"

#define ZIPMAP_BIGLEN 254
#define ZIPMAP_END 255
#define ZIPMAP_BIGLEN_NEW 65279
#define ZIPMAP_VALUE_MAX_FREE 3
#define ZIPMAP_LEN_BYTES(_l) (((_l) < ZIPMAP_BIGLEN) ? 1 : sizeof(unsigned int)+1)

#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

#define ZIPLIST_HEAD 0
#define ZIPLIST_TAIL 1

/* Return the UNIX time in microseconds */
long long ustime(void) {
    struct timeval tv; 
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
long long mstime(void) {
        return ustime()/1000;
}

/* Convert a long long into a string. Returns the number of
 * characters needed to represent the number, that can be shorter if passed
 * buffer length is not enough to store the whole number. */
int ll2string(char *s, size_t len, long long value) {
    char buf[32], *p;
    unsigned long long v;
    size_t l;

    if (len == 0) return 0;
    v = (value < 0) ? -value : value;
    p = buf+31; /* point to the last character */
    do {
        *p-- = '0'+(v%10);
        v /= 10;
    } while(v);
    if (value < 0) *p-- = '-';
    p++;
    l = 32-(p-buf);
    if (l+1 > len) l = len-1; /* Make sure it fits, including the nul term */
    memcpy(s,p,l);
    s[l] = '\0';
    return l;
}

/* Convert a string into a long long. Returns 1 if the string could be parsed
 * into a (non-overflowing) long long, 0 otherwise. The value will be set to
 * the parsed value when appropriate. */
int string2ll(char *s, size_t slen, long long *value) {
    char *p = s;
    size_t plen = 0;
    int negative = 0;
    unsigned long long v;

    if (plen == slen)
        return 0;

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0') {
        if (value != NULL) *value = 0;
        return 1;
    }

    if (p[0] == '-') {
        negative = 1;
        p++; plen++;

        /* Abort on only a negative sign. */
        if (plen == slen)
            return 0;
    }

    /* First digit should be 1-9, otherwise the string should just be 0. */
    if (p[0] >= '1' && p[0] <= '9') {
        v = p[0]-'0';
        p++; plen++;
    } else if (p[0] == '0' && slen == 1) {
        *value = 0;
        return 1;
    } else {
        return 0;
    }

    while (plen < slen && p[0] >= '0' && p[0] <= '9') {
        if (v > (ULLONG_MAX / 10)) /* Overflow. */
            return 0;
        v *= 10;

        if (v > (ULLONG_MAX - (p[0]-'0'))) /* Overflow. */
            return 0;
        v += p[0]-'0';

        p++; plen++;
    }

    /* Return if not all bytes were used. */
    if (plen < slen)
        return 0;

    if (negative) {
        if (v > ((unsigned long long)(-(LLONG_MIN+1))+1)) /* Overflow. */
            return 0;
        if (value != NULL) *value = -v;
    } else {
        if (v > LLONG_MAX) /* Overflow. */
            return 0;
        if (value != NULL) *value = v;
    }
    return 1;
}

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

listTypeIterator *listTypeInitIterator(unsigned char *subject, int index, unsigned char direction) {
    listTypeIterator *li = malloc(sizeof(listTypeIterator));
    li->subject = subject;
    li->direction = direction;
    li->zi = ziplistIndex(subject,index);
    return li;
}

sds listTypeGet(listTypeEntry *entry) {
    sds value = NULL;
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;

    if (ziplistGet(entry->zi,&vstr,&vlen,&vlong)) {
        if (vstr) {
            value = sdsnewlen((char*)vstr,vlen);
        } else {
            value = sdsfromlonglong(vlong);
        }    
    }    
    return value;
}

void listTypeReleaseIterator(listTypeIterator *li) {
    free(li);
}

int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    entry->li = li;
    entry->zi = li->zi;
    if (entry->zi != NULL) {
        if (li->direction == REDIS_TAIL)
            li->zi = ziplistNext(li->subject,li->zi);
        else
            li->zi = ziplistPrev(li->subject,li->zi);
        return 1;
    }
    return 0;
}

/*-----------------------------------------------------------------------------
 * set API
 *----------------------------------------------------------------------------*/
setTypeIterator *setTypeInitIterator(unsigned char* subject) {
    setTypeIterator *si = malloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = REDIS_ENCODING_INTSET;
    si->ii = 0;
    return si;
}

void setTypeReleaseIterator(setTypeIterator *si) {
    free(si);
}

int setTypeNext(setTypeIterator *si ,int64_t *llele) {
    if (!intsetGet((intset*)si->subject,si->ii++,llele))
        return -1;
    return si->encoding;
}

/*-----------------------------------------------------------------------------
 * Ziplist-backed sorted set API
 *----------------------------------------------------------------------------*/

double zzlGetScore(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    char buf[128];
    double score;

    ziplistGet(sptr,&vstr,&vlen,&vlong);
    if (vstr) {
        memcpy(buf,vstr,vlen);
        buf[vlen] = '\0';
        score = strtod(buf,NULL);
    } else {
        score = vlong;
    }

    return score;
}

void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;

    _eptr = ziplistNext(zl,*sptr);
    if (_eptr != NULL) {
        _sptr = ziplistNext(zl,_eptr);
    } else {
        /* No next entry. */
        _sptr = NULL;
    }

    *eptr = _eptr;
    *sptr = _sptr;
}

/*-----------------------------------------------------------------------------
 * Zipmap API
 *----------------------------------------------------------------------------*/
/* Decode the encoded length pointed by 'p' */
static unsigned int zipmapDecodeLength(unsigned char *p) {
    unsigned int len = *p;

    if (len < ZIPMAP_BIGLEN) return len;
    memcpy(&len,p+1,sizeof(unsigned int));
    memrev32ifbe(&len);
    return len;
}

/* Encode the length 'l' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. */
static unsigned int zipmapEncodeLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        return ZIPMAP_LEN_BYTES(len);
    } else {
        if (len < ZIPMAP_BIGLEN) {
            p[0] = len;
            return 1;
        } else {
            p[0] = ZIPMAP_BIGLEN;
            memcpy(p+1,&len,sizeof(len));
            memrev32ifbe(p+1);
            return 1+sizeof(len);
        }
    }
}

/* Return the total amount used by a key (encoded length + payload) */
static unsigned int zipmapRawKeyLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);
    return zipmapEncodeLength(NULL,l) + l;
}

/* Return the total amount used by a value
 * (encoded length + single byte free count + payload) */
static unsigned int zipmapRawValueLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);
    unsigned int used;

    used = zipmapEncodeLength(NULL,l);
    used += p[used] + 1 + l;
    return used;
}

/* Call it before to iterate trought elements via zipmapNext() */
unsigned char *zipmapRewind(unsigned char *zm) {
    return zm[0] == ZIPMAP_END ? (zm+3) : (zm+1);
}

/* This function is used to iterate through all the zipmap elements.
 * In the first call the first argument is the pointer to the zipmap + 1.
 * In the next calls what zipmapNext returns is used as first argument.
 * Example:
 *
 * unsigned char *i = zipmapRewind(my_zipmap);
 * while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
 *     printf("%d bytes key at $p\n", klen, key);
 *     printf("%d bytes value at $p\n", vlen, value);
 * }
 */
unsigned char *zipmapNext(unsigned char *zm, unsigned char **key, unsigned int *klen, unsigned char **value, unsigned int *vlen) {
    if (zm[0] == ZIPMAP_END) return NULL;
    if (key) {
        *key = zm;
        *klen = zipmapDecodeLength(zm);
        *key += ZIPMAP_LEN_BYTES(*klen);
    }
    zm += zipmapRawKeyLength(zm);
    if (value) {
        *value = zm+1;
        *vlen = zipmapDecodeLength(zm);
        *value += ZIPMAP_LEN_BYTES(*vlen);
    }
    zm += zipmapRawValueLength(zm);
    return zm;
}

/* Return the number of entries inside a zipmap */
unsigned int zipmapLen(unsigned char *zm) {
    unsigned int len = 0;
    unsigned char *p;

    /* backward compatibility */
    if (zm[0] != ZIPMAP_END) {
        if (zm[0] < ZIPMAP_BIGLEN) {
            len = zm[0];
            return len;
        } else {
            goto iterate;
        }
    }
    if (!(zm[1] == ZIPMAP_BIGLEN && zm[2] == ZIPMAP_END)) {
        len = (zm[1]<<8) + zm[2];
        return len;
    } else {
        goto iterate;
    }

iterate:
    p = zipmapRewind(zm);
    while((p = zipmapNext(p,NULL,NULL,NULL,NULL)) != NULL) len++;

    /* Re-store length if small enough */
    if (len < ZIPMAP_BIGLEN && zm[0] == ZIPMAP_BIGLEN) {
        zm[0] = len;
    } else if (len < ZIPMAP_BIGLEN_NEW && zm[0] == ZIPMAP_END) {
        zm[1] = (len & 0x0000FF00) >> 8;
        zm[2] = len & 0x000000FF;
    }
    return len;
}


/* Return the value at pos, given an encoding. */
static int64_t _intsetGetEncoded(intset *is, int pos, uint8_t enc) {
    int64_t v64;
    int32_t v32;
    int16_t v16;

    if (enc == INTSET_ENC_INT64) {
        memcpy(&v64,((int64_t*)is->contents)+pos,sizeof(v64));
        memrev64ifbe(&v64);
        return v64;
    } else if (enc == INTSET_ENC_INT32) {
        memcpy(&v32,((int32_t*)is->contents)+pos,sizeof(v32));
        memrev32ifbe(&v32);
        return v32;
    } else {
        memcpy(&v16,((int16_t*)is->contents)+pos,sizeof(v16));
        memrev16ifbe(&v16);
        return v16;
    }
}

/* Return the value at pos, using the configured encoding. */
static int64_t _intsetGet(intset *is, int pos) {
    return _intsetGetEncoded(is,pos,is->encoding);
}

/* Sets the value to the value at the given position. When this position is
 * out of range the function returns 0, when in range it returns 1. */
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value) {
    if (pos < is->length) {
        *value = _intsetGet(is,pos);
        return 1;
    }
    return 0;
}

/*-----------------------------------------------------------------------------
 * Ziplist API
 *----------------------------------------------------------------------------*/
#define ZIP_END 255
#define ZIP_BIGLEN 254

/* Different encoding/length possibilities */
#define ZIP_STR_06B (0 << 6)
#define ZIP_STR_14B (1 << 6)
#define ZIP_STR_32B (2 << 6)
#define ZIP_INT_16B (0xc0 | 0<<4)
#define ZIP_INT_32B (0xc0 | 1<<4)
#define ZIP_INT_64B (0xc0 | 2<<4)
#define ZIP_INT_24B (0xc0 | 3<<4)
#define ZIP_INT_8B 0xfe
/* 4 bit integer immediate encoding */
#define ZIP_INT_IMM_MASK 0x0f
#define ZIP_INT_IMM_MIN 0xf1    /* 11110001 */
#define ZIP_INT_IMM_MAX 0xfd    /* 11111101 */
#define ZIP_INT_IMM_VAL(v) (v & ZIP_INT_IMM_MASK)

#define INT24_MAX 0x7fffff
#define INT24_MIN (-INT24_MAX - 1)

/* Macro's to determine type */
#define ZIP_IS_STR(enc) (((enc) & 0xc0) < 0xc0)
#define ZIP_IS_INT(enc) (!ZIP_IS_STR(enc) && ((enc) & 0x30) < 0x30)
#define ZIP_STR_MASK 0xc0
#define ZIP_INT_MASK 0x30

/* Utility macros */
#define ZIPLIST_BYTES(zl)       (*((uint32_t*)(zl)))
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)((zl)+sizeof(uint32_t))))
#define ZIPLIST_LENGTH(zl)      (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))
#define ZIPLIST_HEADER_SIZE     (sizeof(uint32_t)*2+sizeof(uint16_t))
#define ZIPLIST_ENTRY_HEAD(zl)  ((zl)+ZIPLIST_HEADER_SIZE)
#define ZIPLIST_ENTRY_TAIL(zl)  ((zl)+ZIPLIST_TAIL_OFFSET(zl))
#define ZIPLIST_ENTRY_END(zl)   ((zl)+ZIPLIST_BYTES(zl)-1)

/* Extract the encoding from the byte pointed by 'ptr' and set it into
 *  * 'encoding'. */
#define ZIP_ENTRY_ENCODING(ptr, encoding) do {  \
    (encoding) = (ptr[0]); \
        if ((encoding) < ZIP_STR_MASK) (encoding) &= ZIP_STR_MASK; \
} while(0)
/* We know a positive increment can only be 1 because entries can only be
 * pushed one at a time. */
#define ZIPLIST_INCR_LENGTH(zl,incr) { \
    if (ZIPLIST_LENGTH(zl) < UINT16_MAX) ZIPLIST_LENGTH(zl)+=incr; }


/* Decode the length encoded in 'ptr'. The 'encoding' variable will hold the
 *  * entries encoding, the 'lensize' variable will hold the number of bytes
 *   * required to encode the entries length, and the 'len' variable will hold the
 *    * entries length. */
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len) do {                    \
    ZIP_ENTRY_ENCODING((ptr), (encoding));                                     \
    if ((encoding) < ZIP_STR_MASK) {                                           \
        if ((encoding) == ZIP_STR_06B) {                                       \
            (lensize) = 1;                                                     \
            (len) = (ptr)[0] & 0x3f;                                           \
        } else if ((encoding) == ZIP_STR_14B) {                                \
            (lensize) = 2;                                                     \
            (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1];                       \
        } else if (encoding == ZIP_STR_32B) {                                  \
            (lensize) = 5;                                                     \
            (len) = ((ptr)[1] << 24) |                                         \
            ((ptr)[2] << 16) |                                         \
            ((ptr)[3] <<  8) |                                         \
            ((ptr)[4]);                                                \
        } else {                                                               \
            assert(NULL);                                                      \
        }                                                                      \
    } else {                                                                   \
        (lensize) = 1;                                                         \
        (len) = zipIntSize(encoding);                                          \
    }                                                                          \
} while(0);

#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize) do {                          \
    if ((ptr)[0] < ZIP_BIGLEN) {                                               \
        (prevlensize) = 1;                                                     \
    } else {                                                                   \
        (prevlensize) = 5;                                                     \
    }                                                                          \
} while(0);
/* Decode the length of the previous element, from the perspective of the entry
 *  * pointed to by 'ptr'. */
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen) do {                     \
    ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);                                  \
    if ((prevlensize) == 1) {                                                  \
        (prevlen) = (ptr)[0];                                                  \
    } else if ((prevlensize) == 5) {                                           \
        assert(sizeof((prevlensize)) == 4);                                    \
        memcpy(&(prevlen), ((char*)(ptr)) + 1, 4);                             \
        memrev32ifbe(&prevlen);                                                \
    }                                                                          \
} while(0);

typedef struct zlentry {
    unsigned int prevrawlensize, prevrawlen;
    unsigned int lensize, len;
    unsigned int headersize;
    unsigned char encoding;
    unsigned char *p;
} zlentry;

static zlentry zipEntry(unsigned char *p);

/* Read integer encoded as 'encoding' from 'p' */
static int64_t zipLoadInteger(unsigned char *p, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64, ret = 0;

    if (encoding == ZIP_INT_8B) {
        ret = ((int8_t*)p)[0];
    } else if (encoding == ZIP_INT_16B) {
        memcpy(&i16,p,sizeof(i16));
        memrev16ifbe(&i16);
        ret = i16;
    } else if (encoding == ZIP_INT_24B) {
        i32 = 0; 
        memcpy(((uint8_t*)&i32)+1,p,sizeof(i32)-sizeof(uint8_t));
        memrev32ifbe(&i32);
        ret = i32>>8;
    } else if (encoding == ZIP_INT_32B) {
        memcpy(&i32,p,sizeof(i32));
        memrev16ifbe(&i32);
        ret = i32;
    } else if (encoding == ZIP_INT_64B) {
        memcpy(&i64,p,sizeof(i64));
        memrev16ifbe(&i64);
        ret = i64;
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        ret = (encoding & ZIP_INT_IMM_MASK)-1;
    } else {
        assert(NULL);
    }
    return ret;
}


/* Get entry pointer to by 'p' and store in either 'e' or 'v' depending
 ** on the encoding of the entry. 'e' is always set to NULL to be able
 ** to find out whether the string pointer or the integer value was set.
 ** Return 0 if 'p' points to the end of the zipmap, 1 otherwise. */
unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *sval) {
    zlentry entry;
    if (p == NULL || p[0] == ZIP_END) return 0;
    if (sstr) *sstr = NULL;

    entry = zipEntry(p);
    if (ZIP_IS_STR(entry.encoding)) {
        if (sstr) {
            *slen = entry.len;
            *sstr = p+entry.headersize;
        }
    } else {
        if (sval) {
            *sval = zipLoadInteger(p+entry.headersize,entry.encoding);
        }
    }
    return 1;
}

static unsigned int zipIntSize(unsigned char encoding) {
    switch(encoding) {
    case ZIP_INT_8B:  return 1;
    case ZIP_INT_16B: return 2;
    case ZIP_INT_24B: return 3;
    case ZIP_INT_32B: return 4;
    case ZIP_INT_64B: return 8;
        default: return 0;
    }
    assert(NULL);
    return 0;
}

/* Return a struct with all information about an entry. */
static zlentry zipEntry(unsigned char *p) {
    zlentry e;

    ZIP_DECODE_PREVLEN(p, e.prevrawlensize, e.prevrawlen);
    ZIP_DECODE_LENGTH(p + e.prevrawlensize, e.encoding, e.lensize, e.len);
    e.headersize = e.prevrawlensize + e.lensize;
    e.p = p;
    return e;

}

/* Return the total number of bytes used by the entry at "p". */
static unsigned int zipRawEntryLength(unsigned char *p) {
    unsigned int prevlensize, encoding, lensize, len;
    ZIP_DECODE_PREVLENSIZE(p, prevlensize);
    ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
    return prevlensize + lensize + len;

}

/* Returns an offset to use for iterating with ziplistNext. When the given
 * index is negative, the list is traversed back to front. When the list
 * doesn't contain an element at the provided index, NULL is returned. */
unsigned char *ziplistIndex(unsigned char *zl, int index) {
    unsigned char *p;
    zlentry entry;
    if (index < 0) {
        index = (-index)-1;
        p = ZIPLIST_ENTRY_TAIL(zl);
        if (p[0] != ZIP_END) {
            entry = zipEntry(p);
            while (entry.prevrawlen > 0 && index--) {
                p -= entry.prevrawlen;
                entry = zipEntry(p);
            }
        }
    } else {
        p = ZIPLIST_ENTRY_HEAD(zl);
        while (p[0] != ZIP_END && index--) {
            p += zipRawEntryLength(p);
        }
    }
    return (p[0] == ZIP_END || index > 0) ? NULL : p;
}

/* Return pointer to next entry in ziplist.
 *
 * zl is the pointer to the ziplist
 * p is the pointer to the current element
 *
 * The element after 'p' is returned, otherwise NULL if we are at the end. */
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p) {
    ((void) zl);

    /* "p" could be equal to ZIP_END, caused by ziplistDelete,
     * and we should return NULL. Otherwise, we should return NULL
     * when the *next* element is ZIP_END (there is no next entry). */
    if (p[0] == ZIP_END) {
        return NULL;
    } else {
        p = p+zipRawEntryLength(p);
        return (p[0] == ZIP_END) ? NULL : p;
    }
}

/* Return pointer to previous entry in ziplist. */
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p) {
    zlentry entry;

    /* Iterating backwards from ZIP_END should return the tail. When "p" is
     * equal to the first element of the list, we're already at the head,
     * and should return NULL. */
    if (p[0] == ZIP_END) {
        p = ZIPLIST_ENTRY_TAIL(zl);
        return (p[0] == ZIP_END) ? NULL : p;
    } else if (p == ZIPLIST_ENTRY_HEAD(zl)) {
        return NULL;
    } else {
        entry = zipEntry(p);
        assert(entry.prevrawlen > 0);
        return p-entry.prevrawlen;
    }
}

/* Return length of ziplist. */
unsigned int ziplistLen(unsigned char *zl) {
    unsigned int len = 0;
    if (ZIPLIST_LENGTH(zl) < UINT16_MAX) {
        len = ZIPLIST_LENGTH(zl);
    } else {
        unsigned char *p = zl+ZIPLIST_HEADER_SIZE;
        while (*p != ZIP_END) {
            p += zipRawEntryLength(p);
            len++;
        }

        /* Re-store length if small enough */
        if (len < UINT16_MAX) ZIPLIST_LENGTH(zl) = len;
    }
    return len;
}



hashTypeIterator *hashTypeInitIterator(unsigned char *subject) {
    hashTypeIterator *hi = malloc(sizeof(hashTypeIterator));
    hi->subject = subject;
    hi->fptr = NULL;
    hi->vptr = NULL;

    return hi; 
}

void hashTypeReleaseIterator(hashTypeIterator *hi) {
    free(hi);
}

int hashTypeNext(hashTypeIterator *hi) {
    unsigned char *zl;
    unsigned char *fptr, *vptr;

    zl = hi->subject;
    fptr = hi->fptr;
    vptr = hi->vptr;

    if (fptr == NULL) {
        /* Initialize cursor */
        assert(vptr == NULL);
        fptr = ziplistIndex(zl, 0);
    } else {
        /* Advance cursor */
        assert(vptr != NULL);
        fptr = ziplistNext(zl, vptr);
    }
    if (fptr == NULL) return 0; 

    /* Grab pointer to the value (fptr points to the field) */
    vptr = ziplistNext(zl, fptr);
    assert(vptr != NULL);

    /* fptr, vptr now point to the first or next pair */
    hi->fptr = fptr;
    hi->vptr = vptr;
    return 1;
}

void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
        unsigned char **vstr,
        unsigned int *vlen,
        long long *vll)
{
    int ret;

    if (what & REDIS_HASH_KEY) {
        ret = ziplistGet(hi->fptr, vstr, vlen, vll);
        assert(ret);
    } else {
        ret = ziplistGet(hi->vptr, vstr, vlen, vll);
        assert(ret);
    }
}

sds hashTypeCurrentObject(hashTypeIterator *hi, int what) {
    sds dst;

    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
    if (vstr) {
        dst = sdsnewlen((char*)vstr,vlen);
    } else {
        dst = sdsfromlonglong(vll);
    }


    return dst;
}

void _parsePanic(char *msg, char *file, int line) {
    fprintf(stderr,"!!! Software Failure. Press left mouse button to continue");
    fprintf(stderr,"Guru Meditation: %s #%s:%d",msg,file,line);
#ifdef HAVE_BACKTRACE
    fprintf(stderr,"(forcing SIGSEGV in order to print the stack trace)");
    *((char*)-1) = 'x';
#endif
}

