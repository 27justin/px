#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <frontend/source.hpp>
#include <frontend/lexer.hpp>
#include <frontend/parser.hpp>
#include <backend/analyzer.hpp>
#include <backend/codegen.hpp>

#include <llvm/Support/Program.h>
#include <llvm/Support/FileSystem.h>

#include <lyra/lyra.hpp>

void compile_binary (const std::vector<std::string> &object_files, const std::vector<std::string> &libraries, const std::string &output);

int main(int argc, char **argv) {
  std::vector<std::string> include_directories;
  std::vector<std::string> source_files;
  std::vector<std::string> link_libraries;
  std::optional<std::string> output_file;

  bool output_object_file = false,
    output_full_binary = true,
    output_llvm_ir = false;

  auto cli = lyra::cli()
    | lyra::opt(include_directories, "includes")
                   ["-I"]
                   .help("Add directory to the list of directories to be searched for source files.")
    | lyra::opt(link_libraries, "libs")
                   ["-l"]
                   .help("Add directory to the list of directories to be searched for source files.")
    | lyra::opt(output_object_file)
                   ["-c"]
                   .help("Output object file")
    | lyra::opt(output_llvm_ir)
                   ["-S"]
                   .help("Output LLVM IR")
    | lyra::opt(output_file, "Output file")
                   ["-o"]
    | lyra::arg(source_files, "input files")
                   ("The source files to compile.");

  auto result = cli.parse({ argc, argv });
  if (!result) {
    std::cerr << "Error: " << result.message() << std::endl;
    return 1;
  }

  if (output_object_file || output_llvm_ir)
    output_full_binary = false;

  std::vector<std::string> object_files;

  for (auto &file : source_files) {
    std::stringstream raw;
    std::ifstream stream(file);
    raw << stream.rdbuf();
    std::string source = raw.str();

    auto src = std::make_shared<source_t>(source, file);
    lexer_t lexer(src);

    try {
      parser_t parser(lexer, src);
      auto tu = parser.parse();

      // for (auto &node : tu.declarations) {
      //   dump_ast(*node);
      // }

      analyzer_t analyzer(src);
      analyzer.set_include_directories(include_directories);

      auto su = analyzer.analyze(tu);

      codegen_t codegen(src, std::move(su));
      codegen.generate();

      if (output_object_file || output_full_binary) {
        object_files.push_back(codegen.compile_to_object(output_object_file ? output_file : std::nullopt));
      }

      if (output_llvm_ir)
        codegen.compile_to_llvm_ir(output_file);

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

  // Link (only if `output_full_binary`)
  if (output_full_binary) {
    compile_binary(object_files, link_libraries, output_file.value_or("a.out"));
  }
}


void compile_binary(const std::vector<std::string> &object_files,
                    const std::vector<std::string> &libraries,
                    const std::string &output) {

    std::filesystem::path out_bin = std::filesystem::absolute(output);

    auto cc_path = llvm::sys::findProgramByName("cc");
    if (!cc_path) {
        throw std::runtime_error("Could not find 'cc' in PATH");
    }

    // 5. Build arguments safely
    std::vector<llvm::StringRef> args;
    args.push_back(*cc_path);
    for (auto &obj : object_files) {
      args.push_back(obj.c_str());
    }
    args.push_back("-o");
    args.push_back(out_bin.c_str());  // Use the absolute bin path

    std::vector<std::string> lib_flags;
    for (const auto& lib : libraries) {
        lib_flags.push_back("-l" + lib);
    }

    for (const auto& flag : lib_flags) {
        args.push_back(flag);
    }

    std::string errMsg;
    int result = llvm::sys::ExecuteAndWait(*cc_path, args, std::nullopt, {}, 0, 0, &errMsg);

    if (result != 0) {
        throw std::runtime_error("Linking failed: " + errMsg);
    }
}
