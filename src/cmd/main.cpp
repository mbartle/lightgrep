#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <set>

#include <boost/bind.hpp>
#include <boost/program_options.hpp>
#include <boost/timer.hpp>
#include <boost/graph/graphviz.hpp>

#include "encodings.h"
#include "handles.h"
#include "hitwriter.h"
#include "lightgrep_c_api.h"
#include "matchgen.h"
#include "options.h"
#include "optparser.h"
#include "patterninfo.h"
#include "utility.h"

#include "include_boost_thread.h"


#ifdef LIGHTGREP_CUSTOMER
// check this out: http://stackoverflow.com/questions/2751870/how-exactly-does-the-double-stringize-trick-work
#define STRINGIZE(whatever) #whatever
#define JUMP_THROUGH_A_HOOP(yo) STRINGIZE(yo)
#define CUSTOMER_NAME JUMP_THROUGH_A_HOOP(LIGHTGREP_CUSTOMER)
#endif

// <magic_incantation>
// this ridiculous piece of crap you see here is necessary to get
// boost_thread static libraries to link on Windows using MinGW
// found it in the boost issue tracker
namespace boost {
  void tss_cleanup_implemented() { }
}
// </magic_incantation>

namespace po = boost::program_options;

void startup(boost::shared_ptr<ProgramHandle> prog, const PatternInfo& pinfo, const Options& opts);

uint64 readNext(FILE* file, byte* buf, unsigned int blockSize) {
  return fread((void*)buf, 1, blockSize, file);
}

void printHelp(const po::options_description& desc) {
  std::cout
    << "lightgrep, Copyright (c) 2010-2011, Lightbox Technologies, Inc."
    << "\nCreated " << __DATE__ << "\n\n"
    << "Usage: lightgrep [OPTION]... PATTERN_FILE [FILE]\n\n"
    #ifdef LIGHTGREP_CUSTOMER
    << "This copy provided EXCLUSIVELY to " << CUSTOMER_NAME << "\n\n"
    #endif
    << desc << std::endl;
}

bool addPattern(
  LG_HPARSER parser,
  uint32 i,
  uint32 patIdx,
  uint32 encIdx,
  const LG_KeyOptions& keyOpts,
  PatternInfo& pinfo)
{
  pinfo.Table.push_back(std::make_pair(i, encIdx));

  if (lg_add_keyword(parser, pinfo.Patterns[i].c_str(), patIdx, &keyOpts)) {
    return true;
  }
  else {
    std::cerr << lg_error(parser) << " on pattern "
              << i << ", '" << pinfo.Patterns[i] << "'" << std::endl;
    return false;
  }
}

boost::shared_ptr<ParserHandle> parsePatterns(const Options& opts,
                                              PatternInfo& pinfo,
                                              uint32& numErrors)
{
  numErrors = 0;
  // get patterns from options
  std::cerr << pinfo.Patterns.size() << " pattern";
  if (pinfo.Patterns.size() != 1) {
    std::cerr << 's';
  }
  std::cerr << std::endl;

  if (pinfo.Patterns.empty()) {
    return boost::shared_ptr<ParserHandle>();
  }

  // find total length of patterns -- or 1 if tlen is 0
  const uint32 tlen = std::max(std::accumulate(
    pinfo.Patterns.begin(), pinfo.Patterns.end(), 0,
    boost::bind(std::plus<uint32>(), _1, boost::bind(&std::string::size, _2))),
    1
  );

  boost::shared_ptr<ParserHandle> parser(lg_create_parser(tlen),
                                         lg_destroy_parser);

  // set up parsing options
  LG_KeyOptions keyOpts;
  keyOpts.CaseInsensitive = !opts.CaseSensitive;
  keyOpts.FixedString = opts.LiteralMode;

  // parse patterns
  uint32 patIdx = 0;

  if (opts.getEncoding() & CP_ASCII) {
    keyOpts.Encoding = LG_SUPPORTED_ENCODINGS[LG_ENC_ASCII];

    for (uint32 i = 0; i < pinfo.Patterns.size(); ++i, ++patIdx) {
      if (!addPattern(parser.get(), i, patIdx, LG_ENC_ASCII, keyOpts, pinfo)) {
        ++numErrors;
      }
    }
  }

  if (opts.getEncoding() & CP_UCS16) {
    keyOpts.Encoding = LG_SUPPORTED_ENCODINGS[LG_ENC_UTF_16];

    for (uint32 i = 0; i < pinfo.Patterns.size(); ++i, ++patIdx) {
      if (!addPattern(parser.get(), i, patIdx, LG_ENC_UTF_16, keyOpts, pinfo)) {
        ++numErrors;
      }
    }
  }
  return parser;
}

boost::shared_ptr<ProgramHandle> buildProgram(LG_HPARSER parser, const Options& opts) {
  LG_ProgramOptions progOpts;
  progOpts.Determinize = opts.Determinize;

  return boost::shared_ptr<ProgramHandle>(
    lg_create_program(parser, &progOpts),
    lg_destroy_program
  );
}

boost::shared_ptr<ProgramHandle> createProgram(const Options& opts, PatternInfo& pinfo) {

  uint32 numErrors;
  boost::shared_ptr<ParserHandle> parser(parsePatterns(opts, pinfo, numErrors));

  boost::shared_ptr<ProgramHandle> prog;
  if (numErrors < pinfo.Patterns.size()) {
    // build the program
    prog = buildProgram(parser.get(), opts);
    if (lg_ok(prog.get())) {
      GraphPtr g(parser->Impl->Fsm);
      std::cerr << g->numVertices() << " vertices" << std::endl;

      ProgramPtr p(prog->Impl->Prog);
      std::cerr << p->size() << " instructions" << std::endl;    
    }
    else {
      std::cerr << lg_error(prog.get()) << std::endl;
      prog.reset();
    }
  }

  return prog;
}

class SearchController {
public:
  SearchController(uint32 blkSize): BlockSize(blkSize), BytesSearched(0), TotalTime(0.0),
    Cur(new byte[blkSize]), Next(new byte[blkSize]) {}

  bool searchFile(boost::shared_ptr<ContextHandle> search, HitCounterInfo* hinfo, FILE* file, LG_HITCALLBACK_FN callback);

  uint32 BlockSize;
  uint64 BytesSearched;
  double TotalTime;
  boost::scoped_array<byte> Cur,
                            Next;
};

bool SearchController::searchFile(boost::shared_ptr<ContextHandle> searcher, HitCounterInfo* hinfo, FILE* file, LG_HITCALLBACK_FN callback) {
  boost::timer searchClock;
  uint64 blkSize = 0,
         offset = 0;

  blkSize = readNext(file, Cur.get(), BlockSize);
  if (!feof(file)) {
    do {
      // read the next block on a separate thread
      boost::packaged_task<uint64> task(boost::bind(&readNext, file, Next.get(), BlockSize));
      boost::unique_future<uint64> sizeFut = task.get_future();
      boost::thread exec(boost::move(task));

      // search cur block
      lg_search(searcher.get(), (char*)Cur.get(), (char*)Cur.get() + blkSize, offset, hinfo, callback);

      offset += blkSize;
      if (offset % (1024 * 1024 * 1024) == 0) { // should change this due to the block size being variable
        double lastTime = searchClock.elapsed();
        uint64 units = offset >> 20;
        double bw = units / lastTime;
        units >>= 10;
        std::cerr << units << " GB searched in " << lastTime << " seconds, " << bw << " MB/s avg" << std::endl;
      }
      blkSize = sizeFut.get(); // block on i/o thread completion
      Cur.swap(Next);
    } while (!feof(file)); // note file is shared btwn threads, but safely
  }

  // assert: all data has been read, offset + blkSize == file size,
  // cur is last block
  lg_search(searcher.get(), (char*)Cur.get(), (char*)Cur.get() + blkSize, offset, hinfo, callback);
  lg_closeout_search(searcher.get(), hinfo, callback);
  offset += blkSize;  // be sure to count the last block

  TotalTime += searchClock.elapsed();
  BytesSearched += offset;
  return true;
}

void search(const Options& opts) {
  // try to open our input
  FILE* file = opts.Input == "-" ? stdin : fopen(opts.Input.c_str(), "rb");
  if (!file) {
    std::cerr << "Could not open file " << opts.Input << std::endl;
    return;
  }

  setbuf(file, 0); // unbuffered, bitte

  // parse patterns and get index and encoding info for hit writer
  PatternInfo pinfo;
  pinfo.Patterns = opts.getKeys();

  boost::shared_ptr<ProgramHandle> prog(createProgram(opts, pinfo));
  if (!prog) {
    return;
  }

  // setup hit callback
  LG_HITCALLBACK_FN callback;
  boost::scoped_ptr<HitCounterInfo> hinfo;

  if (opts.NoOutput) {
    callback = &nullWriter;
    hinfo.reset(new HitCounterInfo);
  }
  else if (opts.PrintPath) {
    callback = &pathWriter;
    hinfo.reset(new PathWriterInfo(opts.Input, opts.openOutput(), pinfo));
  }
  else {
    callback = &hitWriter;
    hinfo.reset(new HitWriterInfo(opts.openOutput(), pinfo));
  }

  // search
  LG_ContextOptions ctxOpts;
  ctxOpts.TraceBegin = opts.DebugBegin;
  ctxOpts.TraceEnd = opts.DebugEnd;

  boost::shared_ptr<ContextHandle> searcher(
    lg_create_context(prog.get(), &ctxOpts),
    lg_destroy_context
  );

  SearchController ctrl(opts.BlockSize);
  ctrl.searchFile(searcher, hinfo.get(), file, callback);

  std::cerr << ctrl.BytesSearched << " bytes" << std::endl;
  std::cerr << ctrl.TotalTime << " searchTime" << std::endl;
  if (ctrl.TotalTime > 0.0) {
    std::cerr << ctrl.BytesSearched/ctrl.TotalTime/(1 << 20);
  }
  else {
    std::cerr << "+inf";
  }
  std::cerr << " MB/s avg" << std::endl;
  std::cerr << hinfo->NumHits << " hits" << std::endl;

  fclose(file);
}

bool writeGraphviz(const Options& opts) {
  if (opts.getKeys().empty()) {
    return false;
  }

  PatternInfo pinfo;
  pinfo.Patterns = opts.getKeys();

  // parse patterns
  uint32 numErrors;
  boost::shared_ptr<ParserHandle> parser(parsePatterns(opts, pinfo, numErrors));
  std::cerr << "numErrors = " << numErrors << std::endl;
  if (numErrors == 0) {
    // build the program to force determinization
    boost::shared_ptr<ProgramHandle> prog(buildProgram(parser.get(), opts));
    if (!lg_ok(prog.get())) {
      std::cerr << lg_error(prog.get()) << std::endl;
      return false;
    }

    // break on through the C API to print the graph
    GraphPtr g(parser->Impl->Fsm);
    std::cerr << g->numVertices() << " vertices" << std::endl;
    writeGraphviz(opts.openOutput(), *g);
    return true;
  }
  return false;
}

void writeProgram(const Options& opts) {
  if (opts.getKeys().empty()) {
    return;
  }

  PatternInfo pinfo;
  pinfo.Patterns = opts.getKeys();

  boost::shared_ptr<ProgramHandle> prog;

  {
    // parse patterns
    uint32 numErrors;
    boost::shared_ptr<ParserHandle> parser(parsePatterns(opts, pinfo, numErrors));
    // build the program
    prog = buildProgram(parser.get(), opts);
    if (!lg_ok(prog.get())) {
      std::cerr << lg_error(prog.get()) << std::endl;
      return;
    }
 
    GraphPtr g(parser->Impl->Fsm);
    std::cerr << g->numVertices() << " vertices" << std::endl;
  }

  // break on through the C API to print the program
  ProgramPtr p(prog->Impl->Prog);
  std::cerr << p->size() << " instructions" << std::endl;
  std::ostream& out(opts.openOutput());
  out << *p << std::endl;
}

void writeSampleMatches(const Options& opts) {
  if (opts.getKeys().empty()) {
    return;
  }

  const std::vector<std::string>& pats(opts.getKeys());
  for (std::vector<std::string>::const_iterator i(pats.begin()); i != pats.end(); ++i) {
    // parse the pattern

    PatternInfo pinfo;
    pinfo.Patterns.push_back(*i);

    uint32 numErrors;
    boost::shared_ptr<ParserHandle> parser(parsePatterns(opts, pinfo, numErrors));
    if (numErrors == 0) {
      // break on through the C API to get the graph
      GraphPtr g(parser->Impl->Fsm);

      std::set<std::string> matches;
      matchgen(*g, matches, opts.Limit);

      std::copy(matches.begin(), matches.end(), std::ostream_iterator<std::string>(opts.openOutput(), "\n"));
    }
  }
}

void startServer(const Options& opts) {
  PatternInfo pinfo;
  pinfo.Patterns = opts.getKeys();

  uint32 numErrors;
  boost::shared_ptr<ParserHandle> parser(parsePatterns(opts, pinfo, numErrors));
  if (parser && numErrors == 0) {
    boost::shared_ptr<ProgramHandle> prog(buildProgram(parser.get(), opts));
    if (prog) {
      startup(prog, pinfo, opts);
      return;
    }
  }
  THROW_RUNTIME_ERROR_WITH_OUTPUT("Could not parse patterns at server startup");
}

int main(int argc, char** argv) {
  Options opts;
  po::options_description desc("Allowed Options");
  try {
    parse_opts(argc, argv, desc, opts);

    if (opts.Command == "search") {
      search(opts);
    }
    else if (opts.Command == "server") {
      startServer(opts);
    }
    else if (opts.Command == "help") {
      printHelp(desc);
    }
    else if (opts.Command == "graph") {
      return writeGraphviz(opts) ? 0: 1;
    }
    else if (opts.Command == "prog") {
      writeProgram(opts);
    }
    else if (opts.Command == "samp") {
      writeSampleMatches(opts);
    }
    else {
      // this should be impossible
      std::cerr << "Unrecognized command. Use --help for list of options."
                << std::endl;
      return 1;
    }
  }
  catch (std::exception& err) {
    std::cerr << "Error: " << err.what() << "\n\n";
    printHelp(desc);
    return 1;
  }
  return 0;
}
