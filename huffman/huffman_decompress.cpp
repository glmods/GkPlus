#include "huffman.h"

static int DecodeTable[1 << MAX_DEPTH];

static void MakeHuffmanDecodeTable(int *depth, int depthmax,
                                   unsigned char *list);
static int HuffmanDecode(unsigned char *dest, int *source, int *table,
                         int length);

void HuffmanDecompress(HuffmanPackage *inpackage,
                       std::vector<unsigned char> &output) {
  // Step 1: Make the decoding table
  MakeHuffmanDecodeTable(inpackage->CodelengthCount, MAX_DEPTH,
                         inpackage->ByteAssignment);

  output.resize(inpackage->UncompressedDataSize + 16);
  HuffmanDecode(output.data(), (int *)(inpackage + 1), DecodeTable,
                inpackage->UncompressedDataSize);
}

static void MakeHuffmanDecodeTable(int *depth, int depthmax,
                                   unsigned char *list) {
  int thisdepth, depthbit, repcount, repspace, lenbits, temp, count;
  int *outp;
  int o = 0;
  unsigned char *p;
  int *outtbl = DecodeTable;

  lenbits = 0;
  repcount = 1 << depthmax;
  repspace = 1;
  thisdepth = 0;
  depthbit = 4;
  p = (unsigned char *)list + 255;
  while (1) {
    do {
      lenbits++;
      depthbit <<= 1;
      repspace <<= 1;
      repcount >>= 1;
    } while (!(thisdepth = *depth++));
    do {
      if (p < list) {
        temp = 0xff;
      } else {
        temp = lenbits | (*p-- << 8);
      }
      outp = outtbl + (o >> 2);
      count = repcount;
      do {
        *outp = temp;
        outp += repspace;
      } while (--count);
      temp = depthbit;
      do {
        temp >>= 1;
        if (temp & 3)
          return;
        o ^= temp;
      } while (!(o & temp));
    } while (--thisdepth);
  }
}

static constexpr int EDXMASK = ((((1 << (MAX_DEPTH + 1)) - 1) ^ 1) ^ -1);

static int HuffmanDecode(unsigned char *dest, int *source, int *table,
                         int length) {
  unsigned char *start;
  int available, reserve, fill, wid;
  unsigned int bits = 0, resbits;
  unsigned char *p;

  start = dest;
  available = 0;
  reserve = 0;
  wid = 0;
  do {
    available += wid;
    fill = 31 - available;
    bits <<= fill;
    if (fill > reserve) {
      fill -= reserve;
      available += reserve;
      if (reserve) {
        bits = (bits >> reserve) | (resbits << (32 - reserve));
      }
      resbits = *source++;
      reserve = 32;
    }
    bits = (bits >> fill) | (resbits << (32 - fill));
    resbits >>= fill;
    reserve -= fill;
    available = 31;
    goto lpent;
    do {
      bits >>= wid;
      *dest++ = p[1];
    lpent:
      p = (unsigned char *)(((short *)table) + (bits & ~EDXMASK));
    } while ((available -= (wid = *p)) >= 0 && (dest - start) != length);

  } while (available > -32 && (dest - start) != length);
  return dest - start;
}