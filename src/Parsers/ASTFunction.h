#pragma once

#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTIdentifier_fwd.h>
#include <Parsers/ASTWithAlias.h>


namespace DB
{

class ASTSelectWithUnionQuery;

/** AST for function application or operator.
  */
class ASTFunction : public ASTWithAlias
{
public:
    String name;
    ASTPtr arguments;
    /// parameters - for parametric aggregate function. Example: quantile(0.9)(x) - what in first parens are 'parameters'.
    ASTPtr parameters;

    /// proton: starts. If code_name exists, we shall show 'code_name' instead of 'name(...)'
    String code_name;
    /// proton: ends.

    bool is_window_function = false;

    // We have to make these fields ASTPtr because this is what the visitors
    // expect. Some of them take const ASTPtr & (makes no sense), and some
    // take ASTPtr & and modify it. I don't understand how the latter is
    // compatible with also having an owning `children` array -- apparently it
    // leads to some dangling children that are not referenced by the fields of
    // the AST class itself. Some older code hints at the idea of having
    // ownership in `children` only, and making the class fields to be raw
    // pointers of proper type (see e.g. IAST::set), but this is not compatible
    // with the visitor interface.

    String window_name;
    ASTPtr window_definition;

    /// do not print empty parentheses if there are no args - compatibility with new AST for data types and engine names.
    bool no_empty_args = false;

    /** Get text identifying the AST node. */
    String getID(char delim) const override;

    ASTPtr clone() const override;

    void updateTreeHashImpl(SipHash & hash_state) const override;

    ASTSelectWithUnionQuery * tryGetQueryArgument() const;

    ASTPtr toLiteral() const;  // Try to convert functions like Array or Tuple to a literal form.

    std::string getWindowDescription() const;

protected:
    void formatImplWithoutAlias(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const override;
    void appendColumnNameImpl(WriteBuffer & ostr) const override;
};


template <typename... Args>
std::shared_ptr<ASTFunction> makeASTFunction(const String & name, Args &&... args)
{
    auto function = std::make_shared<ASTFunction>();

    function->name = name;
    function->arguments = std::make_shared<ASTExpressionList>();
    function->children.push_back(function->arguments);

    function->arguments->children = { std::forward<Args>(args)... };

    return function;
}

/// ASTFunction Helpers: hide casts and semantic.

String getFunctionName(const IAST * ast);
std::optional<String> tryGetFunctionName(const IAST * ast);
bool tryGetFunctionNameInto(const IAST * ast, String & name);

inline String getFunctionName(const ASTPtr & ast) { return getFunctionName(ast.get()); }
inline std::optional<String> tryGetFunctionName(const ASTPtr & ast) { return tryGetFunctionName(ast.get()); }
inline bool tryGetFunctionNameInto(const ASTPtr & ast, String & name) { return tryGetFunctionNameInto(ast.get(), name); }

}
