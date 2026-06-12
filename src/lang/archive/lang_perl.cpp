/////////////////////////////////////////////////////////////////////////////
// Purpose:   Perl FFI::Platypus code generator
// Author:    Ralph Walden
// Copyright: Copyright (c) 2026 KeyWorks Software (Ralph Walden)
// License:   Apache License -- see ../LICENSE
/////////////////////////////////////////////////////////////////////////////

#include "lang_perl.h"

#include "../file_writer.h"
#include "lang_common.h"
#include "perl_type_map.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <print>
#include <set>
#include <unordered_set>
#include <vector>

// NOLINTBEGIN(readability-magic-string)

namespace fs = std::filesystem;

// Strip "wx" / "kwx" prefix from a class name.
static std::string PerlStripPrefix(const std::string& name)
{
    if (name.size() > WX_PREFIX.length() && name.starts_with(WX_PREFIX))
    {
        return name.substr(WX_PREFIX.length());
    }
    if (name.size() > KWX_PREFIX.length() && name.starts_with(KWX_PREFIX))
    {
        return name.substr(KWX_PREFIX.length());
    }
    return name;
}

// "wxButton" -> "Button.pm"
static std::string PerlPMFileName(const std::string& class_name)
{
    return PerlStripPrefix(class_name) + ".pm";
}

// "wxButton" -> "wx::FFI::Raw::Button"
static std::string PerlRawPackageName(const std::string& class_name)
{
    return "wx::FFI::Raw::" + PerlStripPrefix(class_name);
}

// "wxButton" -> "wx::Button"
static std::string PerlOOPackageName(const std::string& class_name)
{
    return "wx::" + PerlStripPrefix(class_name);
}

[[nodiscard]] static bool IsVoidReturn(const std::string& return_type)
{
    return return_type.empty() || return_type == "void";
}

[[nodiscard]] static bool IsStringReturn(const FunctionDecl& func)
{
    if (func.return_type == "TString" || func.return_type == "TStringOut")
    {
        return true;
    }
    if (func.return_macro == "TClass" && func.return_arg == "wxString")
    {
        return true;
    }
    return false;
}

[[nodiscard]] static bool IsBoolReturn(const FunctionDecl& func)
{
    return func.return_type == "TBool" || func.return_type == "TBoolInt";
}

// True if function returns a class pointer (not wxString).
[[nodiscard]] static bool IsClassReturn(const FunctionDecl& func)
{
    return (func.return_macro == "TClass" || func.return_macro == "TSelf" ||
            func.return_macro == "TClassRef") &&
           func.return_arg != "wxString" && !func.return_arg.empty();
}

// True if first param is TClass(ClassName) matching the function's own class (pseudo-self).
[[nodiscard]] static bool HasPseudoSelf(const FunctionDecl& func)
{
    if (func.has_self || func.class_name.empty() || func.params.empty())
    {
        return false;
    }
    const Param& first = func.params[0];
    return first.macro_name == "TClass" && first.macro_arg == func.class_name;
}

// Find the nearest parent class that has methods (is actually wrapped).
static std::string FindWrappedParent(const std::string& class_name, const ParsedFFI& ffi,
                                     const std::unordered_set<std::string>& wrapped_classes)
{
    std::unordered_map<std::string, std::string>::const_iterator iter =
        ffi.parent_map.find(class_name);
    std::set<std::string> visited;
    while (iter != ffi.parent_map.end())
    {
        const std::string& parent = iter->second;
        if (visited.contains(parent))
        {
            break;
        }
        visited.insert(parent);
        if (wrapped_classes.contains(parent))
        {
            return parent;
        }
        iter = ffi.parent_map.find(parent);
    }
    return "";
}

// -----------------------------------------------------------------
// OO Layer Helpers: method parameter conversion
// -----------------------------------------------------------------

struct PerlIdiomParam
{
    std::string name;       // Perl parameter name
    std::string call_expr;  // Expression passed to the raw FFI call
    std::string pre_call;   // Statement before the call (empty if none)
    std::string post_call;  // Statement after the call (empty if none)
};

// Convert one Param into idiomatic Perl method parameters.
// TSelf is skipped; TClass(wxString) gets wxString lifecycle handling.
static std::vector<PerlIdiomParam> ConvertToPerlMethodParam(const Param& param)
{
    std::vector<PerlIdiomParam> result;

    if (param.macro_name == "TSelf")
    {
        return result;
    }

    // TClass(wxString) -> wxString lifecycle
    if (param.macro_name == "TClass" && param.macro_arg == "wxString")
    {
        const std::string name =
            PerlEscapeName(param.param_name.empty() ? "str" : param.param_name);
        const std::string wx_str = "_ws_" + name;
        result.push_back({
            name,
            "$" + wx_str,
            "my $" + wx_str + " = kwxPerl::String::to_wx($" + name + ");",
            "kwxPerl::String::delete_wx($" + wx_str + ");",
        });
        return result;
    }

    // TClass(other) / TClassRef -> extract pointer
    if (param.macro_name == "TClass" || param.macro_name == "TClassRef")
    {
        const std::string name =
            PerlEscapeName(param.param_name.empty() ? "obj" : param.param_name);
        result.push_back({
            name,
            "defined $" + name + " ? $" + name + "->{ptr} : undef",
            "",
            "",
        });
        return result;
    }

    // Geometry expansion: TPoint, TSize, TRect, TVector
    if (param.macro_name == "TPoint" || param.macro_name == "TSize" ||
        param.macro_name == "TRect" || param.macro_name == "TVector" ||
        param.macro_name == "TPointLong" || param.macro_name == "TSizeLong" ||
        param.macro_name == "TRectLong" || param.macro_name == "TVectorLong")
    {
        for (auto& n: PerlSplitMacroArg(param.macro_arg))
        {
            const std::string escaped = PerlEscapeName(n);
            result.push_back({ escaped, "$" + escaped, "", "" });
        }
        return result;
    }

    // Output geometry parameters -> pass-through
    if (param.macro_name == "TPointOut" || param.macro_name == "TSizeOut" ||
        param.macro_name == "TRectOut" || param.macro_name == "TVectorOut" ||
        param.macro_name == "TPointOutVoid" || param.macro_name == "TSizeOutVoid" ||
        param.macro_name == "TRectOutVoid" || param.macro_name == "TVectorOutVoid" ||
        param.macro_name == "TSizeOutDouble")
    {
        for (auto& n: PerlSplitMacroArg(param.macro_arg))
        {
            const std::string escaped = PerlEscapeName(n);
            result.push_back({ escaped, "$" + escaped, "", "" });
        }
        return result;
    }

    if (param.macro_name == "TColorRGB")
    {
        for (auto& n: PerlSplitMacroArg(param.macro_arg))
        {
            const std::string escaped = PerlEscapeName(n);
            result.push_back({ escaped, "$" + escaped, "", "" });
        }
        return result;
    }

    // Array types: count + pointer pass-through
    if (param.macro_name == "TArrayString" || param.macro_name == "TArrayInt" ||
        param.macro_name == "TByteString" || param.macro_name == "TByteStringLazy")
    {
        std::vector<std::string> names = PerlSplitMacroArg(param.macro_arg);
        if (names.size() >= 2)
        {
            result.push_back({ PerlEscapeName(names[0]), "$" + PerlEscapeName(names[0]), "", "" });
            result.push_back({ PerlEscapeName(names[1]), "$" + PerlEscapeName(names[1]), "", "" });
        }
        return result;
    }

    if (param.macro_name == "TArrayObjectOutVoid")
    {
        const std::string name =
            PerlEscapeName(param.param_name.empty() ? "arr" : param.param_name);
        result.push_back({ name, "$" + name, "", "" });
        return result;
    }

    // TBool -> convert to int 0/1
    if (param.macro_name == "TBool" || param.raw_type == "TBool")
    {
        const std::string name =
            PerlEscapeName(param.param_name.empty() ? "flag" : param.param_name);
        result.push_back({ name, "$" + name + " ? 1 : 0", "", "" });
        return result;
    }

    // Pass-through for everything else
    const std::string name = PerlEscapeName(param.param_name.empty() ? "arg" : param.param_name);
    result.push_back({ name, "$" + name, "", "" });
    return result;
}

// -----------------------------------------------------------------
// OO Layer Helpers: constructor %args parameter conversion
// -----------------------------------------------------------------

struct PerlConstructorArg
{
    std::string arg_key;    // Hash key: "parent", "id", etc.
    std::string call_expr;  // Expression for the FFI call
    std::string pre_call;   // Code before the call
    std::string post_call;  // Code after the call
};

// Default value for a constructor hash-arg based on type and name.
static std::string PerlArgDefault(const Param& param, const std::string& name)
{
    if (param.macro_name == "TClass" && param.macro_arg == "wxString")
    {
        return "''";
    }
    if (param.raw_type == "TString")
    {
        return "''";
    }
    if (param.macro_name == "TClass" || param.macro_name == "TClassRef")
    {
        return "";  // handled with defined check
    }
    if (param.macro_name == "TBool" || param.raw_type == "TBool")
    {
        return "0";
    }
    if (name == "id")
    {
        return "-1";
    }
    if (name == "style" || name == "styles")
    {
        return "0";
    }
    if (name == "x" || name == "y" || name == "left" || name == "top")
    {
        return "-1";
    }
    if (name == "w" || name == "h" || name == "width" || name == "height")
    {
        return "-1";
    }
    return "0";
}

// Convert one Param to constructor %args entries.
static std::vector<PerlConstructorArg> ConvertToPerlCtorParam(const Param& param)
{
    std::vector<PerlConstructorArg> result;

    if (param.macro_name == "TSelf")
    {
        return result;
    }

    // TClass(wxString) -> string lifecycle
    if (param.macro_name == "TClass" && param.macro_arg == "wxString")
    {
        const std::string name =
            PerlEscapeName(param.param_name.empty() ? "str" : param.param_name);
        const std::string wx_str = "_ws_" + name;
        result.push_back({
            name,
            "$" + wx_str,
            "my $" + wx_str + " = kwxPerl::String::to_wx($args{" + name + "} // '');",
            "kwxPerl::String::delete_wx($" + wx_str + ");",
        });
        return result;
    }

    // TClass(other) / TClassRef -> extract pointer
    if (param.macro_name == "TClass" || param.macro_name == "TClassRef")
    {
        const std::string name =
            PerlEscapeName(param.param_name.empty() ? "obj" : param.param_name);
        result.push_back({
            name,
            "defined $args{" + name + "} ? $args{" + name + "}->{ptr} : undef",
            "",
            "",
        });
        return result;
    }

    // Geometry expansion
    if (param.macro_name == "TPoint" || param.macro_name == "TSize" ||
        param.macro_name == "TRect" || param.macro_name == "TVector" ||
        param.macro_name == "TPointLong" || param.macro_name == "TSizeLong" ||
        param.macro_name == "TRectLong" || param.macro_name == "TVectorLong")
    {
        for (auto& n: PerlSplitMacroArg(param.macro_arg))
        {
            const std::string escaped = PerlEscapeName(n);
            const std::string default_val = PerlArgDefault(param, escaped);
            result.push_back(
                { escaped, std::format("$args{{{}}} // {}", escaped, default_val), "", "" });
        }
        return result;
    }

    // Output geometry -> pass-through
    if (param.macro_name == "TPointOut" || param.macro_name == "TSizeOut" ||
        param.macro_name == "TRectOut" || param.macro_name == "TVectorOut" ||
        param.macro_name == "TPointOutVoid" || param.macro_name == "TSizeOutVoid" ||
        param.macro_name == "TRectOutVoid" || param.macro_name == "TVectorOutVoid" ||
        param.macro_name == "TSizeOutDouble")
    {
        for (auto& n: PerlSplitMacroArg(param.macro_arg))
        {
            const std::string escaped = PerlEscapeName(n);
            result.push_back({ escaped, std::format("$args{{{}}}", escaped), "", "" });
        }
        return result;
    }

    if (param.macro_name == "TColorRGB")
    {
        for (auto& n: PerlSplitMacroArg(param.macro_arg))
        {
            const std::string escaped = PerlEscapeName(n);
            result.push_back({ escaped, std::format("$args{{{}}} // 0", escaped), "", "" });
        }
        return result;
    }

    // Array types
    if (param.macro_name == "TArrayString" || param.macro_name == "TArrayInt" ||
        param.macro_name == "TByteString" || param.macro_name == "TByteStringLazy")
    {
        std::vector<std::string> names = PerlSplitMacroArg(param.macro_arg);
        if (names.size() >= 2)
        {
            result.push_back({ PerlEscapeName(names[0]),
                               std::format("$args{{{}}} // 0", PerlEscapeName(names[0])), "", "" });
            result.push_back({ PerlEscapeName(names[1]),
                               std::format("$args{{{}}}", PerlEscapeName(names[1])), "", "" });
        }
        return result;
    }

    // TBool
    if (param.macro_name == "TBool" || param.raw_type == "TBool")
    {
        const std::string name =
            PerlEscapeName(param.param_name.empty() ? "flag" : param.param_name);
        result.push_back({ name, std::format("$args{{{}}} // 0", name), "", "" });
        return result;
    }

    // Plain type fallback
    const std::string name = PerlEscapeName(param.param_name.empty() ? "arg" : param.param_name);
    const std::string default_val = PerlArgDefault(param, name);
    result.push_back({ name, std::format("$args{{{}}} // {}", name, default_val), "", "" });
    return result;
}

// Map constructor method_name to Perl sub name.
static std::string PerlCtorName(const FunctionDecl& func)
{
    if (func.method_name == "Create")
    {
        return "new";
    }
    // "CreateEmpty" -> "new_empty"
    const std::string suffix = func.method_name.substr(6);  // strip "Create"
    if (suffix.empty())
    {
        return "new";
    }
    return "new_" + ToSnakeCase(suffix);
}

// Count valid functions in a class (for diagnostics and skipping empty classes).
static size_t CountValidFunctions(const ClassInfo& class_info)
{
    size_t count = 0;
    for (const auto& func: class_info.methods)
    {
        if (IsValidFunction(func))
        {
            ++count;
        }
    }
    return count;
}

// Check if any method in a class uses wxString (input or output).
[[nodiscard]] static bool ClassNeedsString(const ClassInfo& class_info)
{
    for (const auto& func: class_info.methods)
    {
        if (!IsValidFunction(func))
        {
            continue;
        }
        if (IsStringReturn(func))
        {
            return true;
        }
        for (const auto& param: func.params)
        {
            if (param.macro_name == "TClass" && param.macro_arg == "wxString")
            {
                return true;
            }
        }
    }
    return false;
}

// =========================================================================
// OO class file helpers
// =========================================================================

static void EmitOOConstructor(std::ostream& out, const FunctionDecl& func,
                              const std::string& raw_pkg)
{
    const std::string method_name = PerlCtorName(func);

    // Build constructor arg groups
    std::vector<std::vector<PerlConstructorArg>> arg_groups;
    for (const auto& param: func.params)
    {
        if (param.macro_name == "TSelf")
        {
            continue;
        }
        arg_groups.push_back(ConvertToPerlCtorParam(param));
    }

    out << "sub " << method_name << " {\n";
    out << "    my ($class, %args) = @_;\n";

    // Pre-call statements
    for (const auto& group: arg_groups)
    {
        for (const auto& ctor_arg: group)
        {
            if (!ctor_arg.pre_call.empty())
            {
                out << "    " << ctor_arg.pre_call << "\n";
            }
        }
    }

    // C call
    out << "    my $ptr = $" << raw_pkg << "::" << CFuncName(func) << "->(\n";
    bool first_arg = true;
    for (const auto& group: arg_groups)
    {
        for (const auto& ctor_arg: group)
        {
            if (!first_arg)
            {
                out << ",\n";
            }
            out << "        " << ctor_arg.call_expr;
            first_arg = false;
        }
    }
    out << "\n    );\n";

    // Post-call cleanup
    for (const auto& group: arg_groups)
    {
        for (const auto& ctor_arg: group)
        {
            if (!ctor_arg.post_call.empty())
            {
                out << "    " << ctor_arg.post_call << "\n";
            }
        }
    }

    out << "    return undef unless defined $ptr;\n";
    out << "    return bless { ptr => $ptr }, $class;\n";
    out << "}\n\n";
}

static void EmitOOMethod(std::ostream& out, const FunctionDecl& func, const std::string& raw_pkg)
{
    const bool is_void = IsVoidReturn(func.return_type);
    const bool is_string_ret = IsStringReturn(func);
    const bool is_bool_ret = IsBoolReturn(func);
    const bool is_class_ret = IsClassReturn(func);
    const bool pseudo_self = HasPseudoSelf(func);

    const std::string method_name = ToSnakeCase(func.method_name);

    // Build param list (skip self/pseudo-self)
    std::vector<std::vector<PerlIdiomParam>> param_groups;
    bool has_post_calls = false;
    for (size_t pi = 0; pi < func.params.size(); ++pi)
    {
        const Param& param = func.params[pi];
        if (param.macro_name == "TSelf")
        {
            continue;
        }
        if (pseudo_self && pi == 0)
        {
            continue;
        }
        std::vector<PerlIdiomParam> group = ConvertToPerlMethodParam(param);
        for (const auto& local_param: group)
        {
            if (!local_param.post_call.empty())
            {
                has_post_calls = true;
            }
        }
        param_groups.push_back(std::move(group));
    }

    // Method signature
    out << "sub " << method_name << " {\n";
    out << "    my ($self";
    for (const auto& group: param_groups)
    {
        for (const auto& local_param: group)
        {
            out << ", $" << local_param.name;
        }
    }
    out << ") = @_;\n";

    // Pre-call statements
    for (const auto& group: param_groups)
    {
        for (const auto& local_param: group)
        {
            if (!local_param.pre_call.empty())
            {
                out << "    " << local_param.pre_call << "\n";
            }
        }
    }

    // Build call expression
    std::string call_args = "$self->{ptr}";
    for (const auto& group: param_groups)
    {
        for (const auto& local_param: group)
        {
            call_args += ", " + local_param.call_expr;
        }
    }

    std::string call_expr;
    call_expr.reserve(raw_pkg.size() + call_args.size() + 32);
    call_expr += "$";
    call_expr += raw_pkg;
    call_expr += "::";
    call_expr += CFuncName(func);
    call_expr += "->(";
    call_expr += call_args;
    call_expr += ")";

    // Emit call + return
    if (is_void)
    {
        out << "    " << call_expr << ";\n";
        for (const auto& group: param_groups)
        {
            for (const auto& local_param: group)
            {
                if (!local_param.post_call.empty())
                {
                    out << "    " << local_param.post_call << "\n";
                }
            }
        }
    }
    else if (has_post_calls)
    {
        // Capture result, cleanup, then return.
        if (is_string_ret)
        {
            out << "    my $_result = kwxPerl::String::from_wx_and_delete(" << call_expr << ");\n";
        }
        else if (is_bool_ret)
        {
            out << "    my $_result = " << call_expr << " ? 1 : 0;\n";
        }
        else
        {
            out << "    my $_result = " << call_expr << ";\n";
        }

        for (const auto& group: param_groups)
        {
            for (const auto& local_param: group)
            {
                if (!local_param.post_call.empty())
                {
                    out << "    " << local_param.post_call << "\n";
                }
            }
        }
        out << "    return $_result;\n";
    }
    else
    {
        if (is_string_ret)
        {
            out << "    return kwxPerl::String::from_wx_and_delete(" << call_expr << ");\n";
        }
        else if (is_bool_ret)
        {
            out << "    return " << call_expr << " ? 1 : 0;\n";
        }
        else if (is_class_ret)
        {
            const std::string ret_pkg = PerlOOPackageName(func.return_arg);
            out << "    my $_ptr = " << call_expr << ";\n";
            out << "    return defined $_ptr ? bless({ ptr => $_ptr }, '" << ret_pkg
                << "') : undef;\n";
        }
        else
        {
            out << "    return " << call_expr << ";\n";
        }
    }

    out << "}\n\n";
}

static void EmitOODestructor(std::ostream& out, const FunctionDecl& func,
                             const std::string& raw_pkg)
{
    bool has_non_self_params = false;
    for (const auto& param: func.params)
    {
        if (param.macro_name != "TSelf")
        {
            has_non_self_params = true;
        }
    }

    if (has_non_self_params)
    {
        // Not a true destructor: emit as a regular method.
        std::vector<std::vector<PerlIdiomParam>> param_groups;
        for (const auto& param: func.params)
        {
            if (param.macro_name == "TSelf")
            {
                continue;
            }
            param_groups.push_back(ConvertToPerlMethodParam(param));
        }

        out << "sub " << ToSnakeCase(func.method_name) << " {\n";
        out << "    my ($self";
        for (const auto& group: param_groups)
        {
            for (const auto& local_param: group)
            {
                out << ", $" << local_param.name;
            }
        }
        out << ") = @_;\n";

        std::string call_args = "$self->{ptr}";
        for (const auto& group: param_groups)
        {
            for (const auto& local_param: group)
            {
                call_args += ", " + local_param.call_expr;
            }
        }

        if (IsVoidReturn(func.return_type))
        {
            out << "    $" << raw_pkg << "::" << CFuncName(func) << "->" << "(" << call_args
                << ");\n";
        }
        else
        {
            out << "    return $" << raw_pkg << "::" << CFuncName(func) << "->" << "(" << call_args
                << ");\n";
        }
        out << "}\n\n";
    }
    else
    {
        // True destructor: guard against double-delete.
        out << "sub delete {\n";
        out << "    my ($self) = @_;\n";
        out << "    if (defined $self->{ptr}) {\n";
        out << "        $" << raw_pkg << "::" << CFuncName(func) << "->" << "("
            << "$self->{ptr});\n";
        out << "        $self->{ptr} = undef;\n";
        out << "    }\n";
        out << "}\n\n";
    }
}

// =========================================================================
// PerlEmitter public interface
// =========================================================================

void PerlEmitter::Generate(const ParsedFFI& ffi, const fs::path& outDir)
{
    const fs::path rawDir = outDir / "lib" / "wx" / "ffi" / "raw";
    const fs::path ooDir = outDir / "lib" / "wx";
    (void) fs::create_directories(rawDir);
    (void) fs::create_directories(ooDir);

    GenerateRawClassFiles(ffi, rawDir);
    GenerateRawConstants(ffi, rawDir);
    GenerateRawFreeFunctions(ffi, rawDir);
    GenerateRawInit(ffi, rawDir);
    GenerateOOClasses(ffi, ooDir);

    std::println(stderr, "Perl: generated files in {}", outDir.string());
}

VerifyResult PerlEmitter::Verify(const ParsedFFI& /* ffi */, const fs::path& /* dir */)
{
    VerifyResult result;
    result.success = false;
    result.messages.emplace_back("Perl verify: use 'kwxgen verify' command instead");
    return result;
}

// =========================================================================
// Layer 1: Raw FFI modules (lib/wx/ffi/raw/<Class>.pm)
// =========================================================================

void PerlEmitter::GenerateRawClassFiles(const ParsedFFI& ffi, const fs::path& rawDir)
{
    static const std::unordered_set<std::string> kSkipClass = { "wxString" };

    size_t classCount = 0;
    size_t totalMethods = 0;

    for (const auto& class_info: ffi.classes)
    {
        if (class_info.methods.empty() || kSkipClass.contains(class_info.name))
        {
            continue;
        }
        const size_t validCount = CountValidFunctions(class_info);
        if (validCount == 0)
        {
            continue;
        }

        const fs::path path = rawDir / PerlPMFileName(class_info.name);
        ConditionalFileWriter out(path);

        const std::string pkg = PerlRawPackageName(class_info.name);

        WriteGeneratedHeader(out, "#", false);
        out << "package " << pkg << ";\n\n";
        out << "use strict;\n";
        out << "use warnings;\n\n";

        // Declare package variables
        out << "our (\n";
        bool firstVar = true;
        for (const auto& func: class_info.methods)
        {
            if (!IsValidFunction(func))
            {
                continue;
            }
            if (!firstVar)
            {
                out << ",\n";
            }
            out << "    $" << CFuncName(func);
            firstVar = false;
        }
        out << "\n);\n\n";

        // _init sub
        out << "sub _init {\n";
        out << "    my ($ffi) = @_;\n\n";

        for (const auto& func: class_info.methods)
        {
            if (!IsValidFunction(func))
            {
                continue;
            }

            const std::string funcName = CFuncName(func);
            const std::string retType = PerlReturnType(func.return_type, func.return_macro);

            std::vector<PerlParam> pParams;
            for (const auto& param: func.params)
            {
                std::vector<PerlParam> expanded = ExpandParamToPerl(param);
                for (auto& perl_param: expanded)
                {
                    pParams.push_back(std::move(perl_param));
                }
            }

            out << "    $" << funcName << " = $ffi->function(\n";
            out << "        " << funcName << " => [";
            for (size_t i = 0; i < pParams.size(); ++i)
            {
                if (i > 0)
                {
                    out << ", ";
                }
                out << "'" << pParams[i].platypus_type << "'";
            }
            out << "]\n";
            out << "        => '" << retType << "'\n";
            out << "    );\n\n";

            ++totalMethods;
        }

        out << "}\n\n";
        out << "1;\n";
        ++classCount;
    }

    std::println(stderr, "  raw/ class modules:         {} classes, {} functions", classCount,
                 totalMethods);
}

// =========================================================================
// Layer 1: Raw Constants (lib/wx/ffi/raw/Constants.pm)
// =========================================================================

void PerlEmitter::GenerateRawConstants(const ParsedFFI& ffi, const fs::path& rawDir)
{
    const fs::path path = rawDir / "Constants.pm";
    ConditionalFileWriter out(path);

    WriteGeneratedHeader(out, "#", false);
    out << "package wx::FFI::Raw::Constants;\n\n";
    out << "use strict;\n";
    out << "use warnings;\n";
    out << "use Exporter 'import';\n\n";
    out << "our @EXPORT_OK;\n";
    out << "our %EXPORT_TAGS;\n";
    out << "my %constants;\n\n";

    out << "sub _init {\n";
    out << "    my ($ffi) = @_;\n\n";

    // Events
    std::vector<EventDecl> sortedEvents = ffi.events;
    std::ranges::sort(sortedEvents,
                      [](const EventDecl& left, const EventDecl& right)
                      {
                          return left.event_name < right.event_name;
                      });

    out << "    # Events (" << sortedEvents.size() << ")\n";
    for (const auto& event: sortedEvents)
    {
        out << "    $constants{" << event.event_name << "} = $ffi->function(" << event.export_name
            << " => [] => 'int')->();\n";
    }
    out << "\n";

    // Keys
    std::vector<KeyDecl> sortedKeys = ffi.keys;
    std::ranges::sort(sortedKeys,
                      [](const KeyDecl& left, const KeyDecl& right)
                      {
                          return left.key_name < right.key_name;
                      });

    out << "    # Keys (" << sortedKeys.size() << ")\n";
    for (const auto& key_entry: sortedKeys)
    {
        out << "    $constants{" << key_entry.key_name << "} = $ffi->function("
            << key_entry.export_name << " => [] => 'int')->();\n";
    }
    out << "\n";

    // Constants (defs)
    std::vector<ConstantDecl> sortedConstants = ffi.constants;
    std::ranges::sort(sortedConstants,
                      [](const ConstantDecl& left, const ConstantDecl& right)
                      {
                          return left.export_name < right.export_name;
                      });

    out << "    # Constants (" << sortedConstants.size() << ")\n";
    for (const auto& constant: sortedConstants)
    {
        const std::string retType =
            (constant.return_type.find('*') != std::string::npos) ? "opaque" : "int";
        const std::string constName = constant.export_name.substr(3);
        out << "    $constants{" << constName << "} = $ffi->function(" << constant.export_name
            << " => [] => '" << retType << "')->();\n";
    }
    out << "\n";

    // Create constant subs dynamically
    out << "    # Create constant subroutines\n";
    out << "    for my $name (sort keys %constants) {\n";
    out << "        no strict 'refs';\n";
    out << "        *{\"wx::FFI::Raw::Constants::$name\"} = sub () { "
           "$constants{$name} };\n";
    out << "        push @EXPORT_OK, $name;\n";
    out << "    }\n\n";

    // Build export tags
    out << "    # Build export tags\n";
    out << "    my @events = grep { /^EVT_/ } @EXPORT_OK;\n";
    out << "    my @keys_list = grep { /^K_/ } @EXPORT_OK;\n";
    out << "    my @consts = grep { !/^EVT_/ && !/^K_/ } @EXPORT_OK;\n";
    out << "    $EXPORT_TAGS{events} = \\@events;\n";
    out << "    $EXPORT_TAGS{keys} = \\@keys_list;\n";
    out << "    $EXPORT_TAGS{constants} = \\@consts;\n";
    out << "    $EXPORT_TAGS{all} = \\@EXPORT_OK;\n";
    out << "}\n\n";

    // Lookup helper
    out << "sub get {\n";
    out << "    my ($name) = @_;\n";
    out << "    return $constants{$name} if exists $constants{$name};\n";
    out << "    $name =~ s/^wx//;\n";
    out << "    return $constants{\"wx$name\"} // $constants{$name};\n";
    out << "}\n\n";

    out << "1;\n";

    std::println(stderr, "  raw/Constants.pm:           {} events, {} keys, {} constants",
                 sortedEvents.size(), sortedKeys.size(), sortedConstants.size());
}

// =========================================================================
// Layer 1: Raw Free Functions (lib/wx/ffi/raw/Functions.pm)
// =========================================================================

void PerlEmitter::GenerateRawFreeFunctions(const ParsedFFI& ffi, const fs::path& rawDir)
{
    if (ffi.free_functions.empty())
    {
        return;
    }

    const fs::path path = rawDir / "Functions.pm";
    ConditionalFileWriter out(path);

    WriteGeneratedHeader(out, "#", false);
    out << "package wx::FFI::Raw::Functions;\n\n";
    out << "use strict;\n";
    out << "use warnings;\n\n";

    // Declare variables
    out << "our (\n";
    bool firstVar = true;
    for (const auto& func: ffi.free_functions)
    {
        if (!IsValidFunction(func))
        {
            continue;
        }
        if (!firstVar)
        {
            out << ",\n";
        }
        out << "    $" << CFuncName(func);
        firstVar = false;
    }
    out << "\n);\n\n";

    // _init sub
    out << "sub _init {\n";
    out << "    my ($ffi) = @_;\n\n";

    size_t count = 0;
    for (const auto& func: ffi.free_functions)
    {
        if (!IsValidFunction(func))
        {
            continue;
        }

        const std::string funcName = CFuncName(func);
        const std::string retType = PerlReturnType(func.return_type, func.return_macro);

        std::vector<PerlParam> pParams;
        for (const auto& param: func.params)
        {
            std::vector<PerlParam> expanded = ExpandParamToPerl(param);
            for (auto& perl_param: expanded)
            {
                pParams.push_back(std::move(perl_param));
            }
        }

        out << "    $" << funcName << " = $ffi->function(\n";
        out << "        " << funcName << " => [";
        for (size_t i = 0; i < pParams.size(); ++i)
        {
            if (i > 0)
            {
                out << ", ";
            }
            out << "'" << pParams[i].platypus_type << "'";
        }
        out << "]\n";
        out << "        => '" << retType << "'\n";
        out << "    );\n\n";

        ++count;
    }

    out << "}\n\n";
    out << "1;\n";

    std::println(stderr, "  raw/Functions.pm:           {} free functions", count);
}

// =========================================================================
// Layer 1: Master Init (lib/wx/ffi/raw/Init.pm)
// =========================================================================

void PerlEmitter::GenerateRawInit(const ParsedFFI& ffi, const fs::path& rawDir)
{
    static const std::unordered_set<std::string> kSkipClass = { "wxString" };

    const fs::path path = rawDir / "Init.pm";
    ConditionalFileWriter out(path);

    WriteGeneratedHeader(out, "#", false);
    out << "package wx::FFI::Raw::Init;\n\n";
    out << "use strict;\n";
    out << "use warnings;\n\n";

    // Collect and sort class names
    std::vector<std::string> classNames;
    for (const auto& class_info: ffi.classes)
    {
        if (class_info.methods.empty() || kSkipClass.contains(class_info.name))
        {
            continue;
        }
        if (CountValidFunctions(class_info) == 0)
        {
            continue;
        }
        classNames.push_back(class_info.name);
    }
    std::ranges::sort(classNames);

    // use statements
    out << "use wx::FFI::Raw::Constants;\n";
    for (const auto& name: classNames)
    {
        out << "use " << PerlRawPackageName(name) << ";\n";
    }
    if (!ffi.free_functions.empty())
    {
        out << "use wx::FFI::Raw::Functions;\n";
    }
    out << "\n";

    // init_all sub
    out << "sub init_all {\n";
    out << "    my ($ffi) = @_;\n\n";
    out << "    wx::FFI::Raw::Constants::_init($ffi);\n";
    for (const auto& name: classNames)
    {
        out << "    " << PerlRawPackageName(name) << "::_init($ffi);\n";
    }
    if (!ffi.free_functions.empty())
    {
        out << "    wx::FFI::Raw::Functions::_init($ffi);\n";
    }

    out << "}\n\n";
    out << "1;\n";

    std::println(stderr, "  raw/Init.pm:                master init ({} modules)",
                 classNames.size());
}

// =========================================================================
// Layer 2: OO wrapper modules (lib/wx/<Class>.pm)
// =========================================================================

void PerlEmitter::GenerateOOClasses(const ParsedFFI& ffi, const fs::path& ooDir)
{
    static const std::unordered_set<std::string> kSkipClass = { "wxString" };

    size_t classCount = 0;
    size_t methodCount = 0;

    for (const auto& class_info: ffi.classes)
    {
        if (class_info.methods.empty() || kSkipClass.contains(class_info.name))
        {
            continue;
        }
        if (CountValidFunctions(class_info) == 0)
        {
            continue;
        }

        const fs::path path = ooDir / PerlPMFileName(class_info.name);
        ConditionalFileWriter out(path);

        EmitOOClassFile(out, class_info, ffi);
        ++classCount;

        for (const auto& func: class_info.methods)
        {
            if (IsValidFunction(func))
            {
                ++methodCount;
            }
        }
    }

    std::println(stderr, "  wx/ OO modules:             {} classes, {} methods", classCount,
                 methodCount);
}

void PerlEmitter::EmitOOClassFile(std::ostream& out, const ClassInfo& class_info,
                                  const ParsedFFI& ffi)
{
    const std::string ooPkg = PerlOOPackageName(class_info.name);
    const std::string rawPkg = PerlRawPackageName(class_info.name);

    // Build the set of wrapped classes for parent resolution.
    std::unordered_set<std::string> wrappedClasses;
    for (const auto& other: ffi.classes)
    {
        if (!other.methods.empty() && other.name != "wxString")
        {
            wrappedClasses.insert(other.name);
        }
    }

    const std::string parentClass = FindWrappedParent(class_info.name, ffi, wrappedClasses);

    // Check for mixins
    std::vector<std::string> mixinParents;
    const std::unordered_map<std::string, std::vector<std::string>>::const_iterator mixin_it =
        ffi.mixin_map.find(class_info.name);
    if (mixin_it != ffi.mixin_map.end())
    {
        for (const auto& mixin: mixin_it->second)
        {
            if (wrappedClasses.contains(mixin))
            {
                mixinParents.push_back(mixin);
            }
        }
    }

    const bool needsString = ClassNeedsString(class_info);

    // --- Header ---
    WriteGeneratedHeader(out, "#", false);
    out << "package " << ooPkg << ";\n\n";
    out << "use strict;\n";
    out << "use warnings;\n";

    // Inheritance: primary parent + mixins
    if (!parentClass.empty() || !mixinParents.empty())
    {
        out << "use parent qw(";
        bool first = true;
        if (!parentClass.empty())
        {
            out << PerlOOPackageName(parentClass);
            first = false;
        }
        for (const auto& mixin: mixinParents)
        {
            if (!first)
            {
                out << " ";
            }
            out << PerlOOPackageName(mixin);
            first = false;
        }
        out << ");\n";
    }

    out << "use " << rawPkg << ";\n";
    if (needsString)
    {
        out << "use kwxPerl::String;\n";
    }
    out << "\n";

    // --- Pointer accessor ---
    out << "sub ptr { return $_[0]->{ptr} }\n\n";

    // --- Constructors ---
    for (const auto& func: class_info.methods)
    {
        if (!IsValidFunction(func) || !func.is_constructor || func.has_self)
        {
            continue;
        }
        EmitOOConstructor(out, func, rawPkg);
    }

    // --- Methods (non-constructor, non-destructor) ---
    for (const auto& func: class_info.methods)
    {
        if (!IsValidFunction(func) || (func.is_constructor && !func.has_self) || func.is_destructor)
        {
            continue;
        }
        EmitOOMethod(out, func, rawPkg);
    }

    // --- Destructor ---
    for (const auto& func: class_info.methods)
    {
        if (!IsValidFunction(func) || !func.is_destructor)
        {
            continue;
        }
        EmitOODestructor(out, func, rawPkg);
    }

    // --- Event connect convenience (only for wxWindow) ---
    if (class_info.name == "wxWindow")
    {
        out << "sub connect {\n";
        out << "    my ($self, $event_type, $callback, %opts) = @_;\n";
        out << "    kwxPerl::Event::connect(\n";
        out << "        $self->{ptr},\n";
        out << "        $opts{id}      // -1,\n";
        out << "        $opts{last_id} // -1,\n";
        out << "        $event_type,\n";
        out << "        $callback,\n";
        out << "    );\n";
        out << "}\n\n";
    }

    out << "1;\n";
}

// NOLINTEND(readability-magic-string)
