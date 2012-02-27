// must include <fstream> before options.h, because of <iosfwd> usage
#include <iostream>
#include <fstream>
#include <stdexcept>

#include <boost/tokenizer.hpp>

#include "options.h"
#include "utility.h"

bool Options::readKeyFile(const std::string& keyFilePath, std::vector<Pattern>& keys) const {
  std::ifstream keyFile(keyFilePath.c_str(), std::ios::in);
  if (keyFile) {
    uint32 i = 0;
    while (keyFile) {
      char line[8192];
      keyFile.getline(line, 8192);
      std::string lineS(line);
      parseLine(i++, lineS, keys);
    }
    return !keys.empty();
  }
  else {
    std::cerr << "Could not open keywords file " <<  keyFilePath << std::endl;
    return false;
  }
}

std::ostream& Options::openOutput() const {
  if (Output == "-") {
    return std::cout;
  }
  else {
    OutputFile.clear();
    OutputFile.open(Output.c_str(), std::ios::out);
    if (!OutputFile) {
      THROW_RUNTIME_ERROR_WITH_OUTPUT("Could not open output file " << Output);
    }
    return OutputFile;
  }
}

std::vector<Pattern> Options::getKeys() const {
  std::vector<Pattern> ret;
  if (!CmdLinePatterns.empty()) {
    for (std::string p : CmdLinePatterns) {
      parseLine(0, p, ret);
    }
  }
  else {
    for (std::string kf : KeyFiles) {
      readKeyFile(kf, ret);
    }
  }
  return ret;
}

void setBool(const std::string& s, bool& b) {
  int zeroCmp = s.compare("0"),
      oneCmp  = s.compare("1");
  if (0 == oneCmp) {
    b = true;
    return;
  }
  else if (0 == zeroCmp) {
    b = false;
    return;
  }
  // don't set if unrecognized
}

bool Options::parseLine(uint32 keyIndex, const std::string& line, std::vector<Pattern>& keys) const {
  typedef boost::tokenizer<boost::char_separator<char>> tokenizer;

  if (!line.empty()) {
    const tokenizer tokens(line, boost::char_separator<char>("\t"));
    unsigned int num = 0;
    for (tokenizer::const_iterator it(tokens.begin()); it != tokens.end(); ++it) {
      ++num;
    }
    if (num > 0) {
      tokenizer::const_iterator curTok(tokens.begin());
      Pattern p(*curTok++, LiteralMode, CaseInsensitive, keyIndex, "");
      std::string encodings(Encoding); // comma-separated

      if (4 == num) {
        setBool(*curTok++, p.FixedString);
        setBool(*curTok++, p.CaseInsensitive);
        encodings = *curTok;
      }
      const tokenizer encList(encodings, boost::char_separator<char>(","));
      if (encList.begin() != encList.end()) {
        for (tokenizer::const_iterator enc(encList.begin()); enc != encList.end(); ++enc) {
          p.Encoding = *enc;
          keys.push_back(p);
        }
        return true;
      }
    }
  }
  return false;
}
