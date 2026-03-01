#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <frontend/source.hpp>
#include <frontend/lexer.hpp>
#include <frontend/parser.hpp>
#include <backend/analyzer.hpp>
#include <backend/codegen.hpp>

int main(int argc, char **argv) {
  for (auto i = 1; i < argc; ++i) {
    if (std::filesystem::exists(argv[i])) {
      std::filesystem::path file {argv[i]};
      std::stringstream raw;
      std::ifstream stream(argv[i]);
      raw << stream.rdbuf();
      std::string source = raw.str();

      auto src = std::make_shared<source_t>(source, argv[1]);
      lexer_t lexer(src);

      // while (!lexer.eof()) {
      //   token_t t = lexer.next();
      //   std::cout << "("<<(int)t.type<<"): " << src.string(t.location) << "\n";
      // }

      try {
        parser_t parser(lexer, src);
        auto tu = parser.parse();

        for (auto &node : tu.declarations) {
          dump_ast(*node);
        }

        analyzer_t analyzer(src);
        auto su = analyzer.analyze(tu);

        // for (auto &node : su.unit.declarations) {
        //   dump_ast(*node);
        // }

        codegen_t codegen(std::move(su));
        codegen.generate();
        codegen.compile_to_object(file.replace_extension("").filename());
      } catch (const parse_error_t &err) {
        for (auto &msg : err.diagnostics.messages) {
          std::cerr << serialize(msg) << "\n";
        }
      } catch (const analyze_error_t &err) {
        for (auto &msg : err.diagnostics.messages) {
          std::cerr << serialize(msg) << "\n";
        }
      }
    }
  }
}

