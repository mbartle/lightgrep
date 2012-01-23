#pragma once

#include "graph.h"
#include "parser.h"
#include "vm_interface.h"
#include "pattern.h"

void collector(void* userData, const LG_SearchHit* const hit);

//struct STest: public HitCallback {
struct STest {
  std::vector< SearchHit > Hits;
  GraphPtr Fsm;
  ProgramPtr Prog;
  boost::shared_ptr<VmInterface> Grep;

  STest(const std::string& key) {
    std::vector<Pattern> kws;
    kws.push_back(key);
    init(kws);
  }

  STest(const std::vector<std::string>& keys);

  STest(uint32 num, const char** keys) {
    std::vector<Pattern> kws(num);
    for (unsigned int i = 0; i < num; ++i) {
      kws[i] = Pattern(keys[i]);
    }
    init(kws);
  }

  void init(const std::vector<Pattern>& kws);

  void search(const byte* begin, const byte* end, uint64 offset) {
    Grep->search(begin, end, offset, collector, this);
    Grep->closeOut(collector, this);
  }

  void startsWith(const byte* begin, const byte* end, uint64 offset) {
    Grep->startsWith(begin, end, offset, collector, this);
  }
};

void collector(void* userData, const LG_SearchHit* const hit);
