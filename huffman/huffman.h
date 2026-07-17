#pragma once
#include <vector>

#define MAX_DEPTH 11

typedef struct {
  char Identifier[8];
  int CompressedDataSize;
  int UncompressedDataSize;
  int CodelengthCount[MAX_DEPTH];
  unsigned char ByteAssignment[256];
} HuffmanPackage;

HuffmanPackage *HuffmanCompression(unsigned char *sourcePtr, int length);
void HuffmanDecompress(HuffmanPackage *inpackage,
                       std::vector<unsigned char> &output);