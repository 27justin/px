#pragma once

#include <memory>

#include "frontend/diagnostic.hpp"
#include "frontend/path.hpp"
#include "frontend/source.hpp"
#include "frontend/token.hpp"
#include "frontend/lexer.hpp"
#include "frontend/ast.hpp"

struct translation_unit_t {
  std::shared_ptr<source_t> source;
  std::vector<SP<ast_node_t>> declarations;
  std::vector<specialized_path_t> imports;
};

struct parse_error_t {
  diagnostic_stack_t diagnostics;
};

class parser_t {
public:
  static constexpr const char UNEXPECTED_TOKEN[] = "Unexpected token {}";
  static constexpr const char UNEXPECTED_TOKEN_DETAIL[] =
      "Unexpected token `{}`, expected `{}`";
  static constexpr const char UNEXPECTED_TOKEN_ANY_DETAIL[] = "Unexpected token `{}`, expected any of: {}";

  parser_t(lexer_t &lexer, std::shared_ptr<source_t> source) : lexer(lexer), source(source), token() {}

  translation_unit_t parse();

  diagnostic_stack_t diagnostics;
private:
  lexer_t &lexer;
  token_t token;
  std::shared_ptr<source_t> source;

  void consume();

  void expect(token_type_t);
  void expect_any(std::vector<token_type_t>);

  bool maybe(token_type_t);

  bool peek(token_type_t);
  token_type_t peek_any(std::vector<token_type_t>);

  specialized_path_t parse_specialized_path();
  template_path_t parse_template_path();

  type_decl_t parse_type();

  SP<ast_node_t> parse_struct();
  SP<ast_node_t> parse_binding();
  SP<ast_node_t> parse_contract();
  SP<ast_node_t> parse_runtime_binding();
  SP<ast_node_t> parse_identifier();

  function_parameter_t parse_function_parameter();
  SP<ast_node_t> parse_function();
  SP<ast_node_t> parse_type_alias();
  SP<ast_node_t> parse_block();
  SP<ast_node_t> parse_statement();
  SP<ast_node_t> parse_return();
  SP<ast_node_t> parse_primary(bool allow_struct_literal = true);
  SP<ast_node_t> parse_expression(int min_binding_power = 0, bool allow_struct_literal = true);
  SP<ast_node_t> parse_slice();
  SP<ast_node_t> parse_array_initializer();
  attribute_decl_t parse_attributes();

  tuple_decl_t parse_tuple_type();
  SP<ast_node_t> parse_tuple_expression();

  union_decl_t parse_union_type();

  SP<ast_node_t> parse_enum();

  SP<ast_node_t> parse_if();
  SP<ast_node_t> parse_while();
  SP<ast_node_t> parse_for();
  SP<ast_node_t> parse_defer();

  specialized_path_t parse_import();

  bool is_simple_path();
  bool is_templated_path();
  bool is_specialized_path();

  std::vector<SP<ast_node_t>> parse_function_arguments();
  struct_expr_t parse_struct_intialization();

  void parse_generic_specifier(template_segment_t &segment);
  bool is_controlflow(SP<ast_node_t>);

  binop_type_t binop_type(const token_t &);
};

