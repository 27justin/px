#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <backend/analyzer.hpp>
#include <backend/codegen.hpp>
#include <frontend/lexer.hpp>
#include <frontend/parser.hpp>
#include <frontend/source.hpp>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>

#include <lld/Common/Driver.h>
#include <llvm/Support/raw_ostream.h>

LLD_HAS_DRIVER(elf)
// LLD_HAS_DRIVER(coff)
// LLD_HAS_DRIVER(mingw)
// LLD_HAS_DRIVER(macho)
// LLD_HAS_DRIVER(wasm)

#include <lyra/lyra.hpp>

void
compile_binary(const std::vector<std::string> &object_files,
               const std::vector<std::string> &libraries,
               const std::string              &output);

int
main(int argc, char **argv) {
  std::vector<std::string>   include_directories;
  std::vector<std::string>   source_files;
  std::vector<std::string>   link_libraries;
  std::optional<std::string> output_file;
  std::optional<std::string> opt_level;

  bool output_object_file = false, output_full_binary = true, output_llvm_ir = false,
       output_ast = false;

  auto cli = lyra::cli() |
             lyra::opt(include_directories, "includes")["-I"].help(
               "Add directory to the list of directories to be searched for source files.") |
             lyra::opt(link_libraries, "libs")["-l"].help(
               "Add directory to the list of directories to be searched for source files.") |
             lyra::opt(output_object_file)["-c"].help("Output object file") |
             lyra::opt(output_llvm_ir)["-S"].help("Output LLVM IR") |
             lyra::opt(opt_level, "Optimization Level")["-O"].help("-Os, -Oz, -Og, -O0..3") |
             lyra::opt(output_file, "Output file")["-o"] |
             lyra::opt(output_ast)["-dump-ast"].help("Output AST representation") |
             lyra::arg(source_files, "input files")("The source files to compile.");

  auto result = cli.parse({ argc, argv });
  if (!result) {
    std::cerr << "Error: " << result.message() << std::endl;
    return 1;
  }

  if (output_object_file || output_llvm_ir)
    output_full_binary = false;

  // Add CWD to the include directories
  include_directories.push_back(std::filesystem::current_path().string());

  std::vector<std::string> object_files;

  for (auto &file : source_files) {
    std::stringstream raw;
    std::ifstream     stream(file);
    raw << stream.rdbuf();
    std::string source = raw.str();

    auto    src = std::make_shared<source_t>(source, file);
    lexer_t lexer(src);

    try {
      parser_t parser(lexer, src);
      auto     tu = parser.parse();

      analyzer_t analyzer(src);
      analyzer.set_include_directories(include_directories);

      auto su = analyzer.analyze(tu);

      if (!output_ast) {
        codegen_t codegen(src, std::move(su));

        if (opt_level)
          codegen.set_opt_level(opt_level.value());

        codegen.generate();

        if (output_object_file || output_full_binary) {
          object_files.push_back(
            codegen.compile_to_object(output_object_file ? output_file : std::nullopt));
        }

        if (output_llvm_ir)
          codegen.compile_to_llvm_ir(output_file);
      } else {
        for (auto &ast_node : su.unit.declarations) {
          dump_ast(*ast_node);
        }
      }
    } catch (const parse_error_t &err) {
      for (auto &msg : err.diagnostics.messages) {
        std::cerr << serialize(msg) << "\n";
      }
      std::exit(1);
    } catch (const analyze_error_t &err) {
      for (auto &msg : err.diagnostics.messages) {
        std::cerr << serialize(msg) << "\n";
      }
      std::exit(1);
    }
  }

  // Link (only if `output_full_binary`)
  if (output_full_binary && !output_ast) {
    compile_binary(object_files, link_libraries, output_file.value_or("a.out"));
    // Remove temporary object files
    for (auto &obj_file : object_files) {
      std::filesystem::remove(obj_file);
    }
  }
}

std::string
find_linker() {
#ifdef _WIN32
  // MSVC
  if (auto link = llvm::sys::findProgramByName("link"))
    return *link;

  // MingW
  if (auto gcc = llvm::sys::findProgramByName("gcc"))
    return *gcc;
#else
  // Linux/macOS
  if (auto cc = llvm::sys::findProgramByName("cc"))
    return *cc;
#endif
  throw std::runtime_error("No linker found in PATH");
}

void
compile_binary(const std::vector<std::string> &object_files,
               const std::vector<std::string> &libraries,
               const std::string              &output) {
  auto linker   = find_linker();
  bool is_msvc  = (linker.find("link") != std::string::npos);
  bool is_linux = !is_msvc;

  std::filesystem::path out_bin = std::filesystem::absolute(output);

  // LLVM takes a StringRef that itself has to be stored somewhere on
  // the heap.
  // Therefore two vectors...
  std::vector<std::string> args;

  args.push_back(linker);

  for (auto &obj : object_files) {
    args.push_back(obj.c_str());
  }

  if (is_linux) {
    args.push_back("-o");
    args.push_back(out_bin.string());
    for (auto &lib : libraries)
      args.push_back("-l" + lib);
  } else {
    args.push_back("/OUT:" + out_bin.string());
    for (auto &lib : libraries)
      args.push_back(lib + ".lib");
  }

  {
    std::vector<llvm::StringRef> llvm_args;
    for (auto const &arg : args)
      llvm_args.push_back(arg);

    std::string err;
    int         result = llvm::sys::ExecuteAndWait(linker, llvm_args, std::nullopt, {}, 0, 0, &err);

    if (result != 0) {
      throw std::runtime_error("Linking failed: " + err);
    }
  }
}
