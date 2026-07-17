#include <fstream>
#include <huffman.h>
#include <iostream>
#include <vector>

using huffman_ifstream = std::basic_ifstream<unsigned char>;
using huffman_ofstream = std::basic_ofstream<unsigned char>;

static int compress(huffman_ifstream &input, huffman_ofstream &output) {
  auto len{input.tellg()};
  input.seekg(0);

  std::vector<unsigned char> input_data;
  input_data.resize(len);
  input.read(input_data.data(), len);

  auto pkg{HuffmanCompression(input_data.data(), input_data.size())};

  output.write((unsigned char *)pkg,
               pkg->CompressedDataSize + sizeof(HuffmanPackage));

  return 0;
}

static int decompress(huffman_ifstream &input, huffman_ofstream &output) {
  auto len{input.tellg()};
  input.seekg(0);

  std::vector<unsigned char> input_data;
  input_data.resize(len);
  input.read(input_data.data(), len);
  auto pkg{reinterpret_cast<HuffmanPackage *>(input_data.data())};
  std::string id{pkg->Identifier, 8};

  if (id != "REBCRIF1") {
    std::cerr << "ERROR: Unknown file type\n";
    return 4;
  }

  std::vector<unsigned char> output_data;
  HuffmanDecompress(pkg, output_data);

  output.write(output_data.data(), output_data.size());
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc < 4) {
    std::cerr << "Usage: " << argv[0] << " mode input_file output_file\n"
              << "mode can be any of: compress decompress\n";
    return 1;
  }

  huffman_ifstream input{argv[2], std::ios::binary | std::ios::ate};
  if (!input.is_open()) {
    std::cerr << "ERROR: cannot open file `" << argv[2] << "'\n";
    return 2;
  }

  huffman_ofstream output(argv[3], std::ios::binary);
  if (!output.is_open()) {
    std::cerr << "ERROR: cannot open file `" << argv[3] << "'\n";
    return 3;
  }

  std::string mode{argv[1]};
  if (mode == "compress") {
    return compress(input, output);
  } else if (mode == "decompress") {
    return decompress(input, output);
  } else {
    std::cerr << "ERROR: invalid mode `" << mode << "'\n";
    return 4;
  }
}