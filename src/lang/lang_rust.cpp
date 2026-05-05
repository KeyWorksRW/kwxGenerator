/////////////////////////////////////////////////////////////////////////////
// Purpose:   Rust FFI code generator with safe wrappers
// Author:    Ralph Walden
// Copyright: Copyright (c) 2026 KeyWorks Software (Ralph Walden)
// License:   Apache License -- see ../LICENSE
/////////////////////////////////////////////////////////////////////////////

#include "lang_rust.h"

#include "../file_writer.h"
#include "lang_common.h"
#include "rust_type_map.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <print>
#include <set>
#include <tuple>
#include <vector>

// NOLINTBEGIN(readability-magic-string)

namespace fs = std::filesystem;

// Build a C function name from a FunctionDecl.

// Strip wx/kwx prefix: "wxButton" → "Button"
[[nodiscard]] static std::string StripPrefix(const std::string& name)
{
    if (name.size() > WX_PREFIX.length() && name.starts_with(WX_PREFIX) &&
        std::isupper(name[WX_PREFIX.length()]))
    {
        return name.substr(WX_PREFIX.length());
    }
    if (name.size() > KWX_PREFIX.length() && name.starts_with(KWX_PREFIX) &&
        std::isupper(name[KWX_PREFIX.length()]))
    {
        return name.substr(KWX_PREFIX.length());
    }
    return name;
}

// Convert CamelCase to snake_case: "SetDefault" → "set_default"

// Convert a method name to a safe Rust method name (snake_case, keyword-escaped).
[[nodiscard]] static std::string RustSafeMethodName(const std::string& methodName)
{
    const std::string snake = ToSnakeCase(methodName);
    // Check for Rust keywords
    if (snake == "move" || snake == "break" || snake == "continue" || snake == "return" ||
        snake == "type" || snake == "self" || snake == "super" || snake == "crate" ||
        snake == "match" || snake == "ref" || snake == "mod" || snake == "use" || snake == "fn" ||
        snake == "let" || snake == "mut" || snake == "pub" || snake == "if" || snake == "else" ||
        snake == "for" || snake == "while" || snake == "loop" || snake == "in" || snake == "as" ||
        snake == "impl" || snake == "trait" || snake == "struct" || snake == "enum" ||
        snake == "union" || snake == "const" || snake == "static" || snake == "extern" ||
        snake == "unsafe" || snake == "async" || snake == "await" || snake == "dyn" ||
        snake == "where" || snake == "yield" || snake == "box" || snake == "macro" ||
        snake == "true" || snake == "false" || snake == "abstract" || snake == "become" ||
        snake == "do" || snake == "final" || snake == "override" || snake == "priv" ||
        snake == "try" || snake == "typeof" || snake == "unsized" || snake == "virtual")
    {
        return snake + "_";
    }
    // Identifiers starting with a digit are invalid in Rust
    if (!snake.empty() && std::isdigit(static_cast<unsigned char>(snake[0])))
    {
        return "_" + snake;
    }
    return snake;
}

// Lowercase a string entirely.

// Rust module name from class name: "wxButton" → "button"
[[nodiscard]] static std::string RustModuleName(const std::string& className)
{
    return ToSnakeCase(StripPrefix(className));
}

// Rust file name from class name: "wxButton" → "button.rs"
[[nodiscard]] static std::string RustFileName(const std::string& className)
{
    return RustModuleName(className) + ".rs";
}

// Emit a single extern "C" declaration for sys.rs.
static void EmitExternDecl(std::ostream& out, const std::string& funcName,
                           const std::string& returnType, const std::vector<RustFFIParam>& params)
{
    out << "    pub fn " << funcName << "(";
    for (size_t i = 0; i < params.size(); ++i)
    {
        if (i > 0)
        {
            out << ", ";
        }
        out << params[i].name << ": " << params[i].rust_type;
    }
    out << ")";
    if (!returnType.empty())
    {
        out << " -> " << returnType;
    }
    out << ";\n";
}

// Emit an extern "C" declaration from a FunctionDecl.
static void EmitFunctionExternDecl(std::ostream& out, const FunctionDecl& func)
{
    const std::string retType = RustFFIReturnType(func.return_type, func.return_macro);
    const std::string funcName = CFuncName(func);

    std::vector<RustFFIParam> params;
    for (const auto& param: func.params)
    {
        std::vector<RustFFIParam> expanded = ExpandParamToRustFFI(param);
        for (auto& rust_param: expanded)
        {
            params.push_back(std::move(rust_param));
        }
    }

    EmitExternDecl(out, funcName, retType, params);
}

// Check if a function declaration looks valid (skip malformed ones).

// A true destructor takes only self as its parameter.
[[nodiscard]] static bool IsDestructor(const FunctionDecl& func)
{
    return func.is_destructor && func.params.size() == 1;
}

// Check if a class has a Delete method (for Drop implementation).
[[nodiscard]] static bool HasDeleteMethod(const ClassInfo& cls)
{
    return std::ranges::any_of(cls.methods, IsDestructor);
}

// Determine the safe Rust return type for a method.
[[nodiscard]] static std::string
    RustSafeReturnType(const FunctionDecl& func, [[maybe_unused]] const std::string& rustClassName)
{
    if (func.return_type == "void" || func.return_type.empty())
    {
        return "";
    }
    if (func.return_macro == "TClass" || func.return_macro == "TSelf" ||
        func.return_macro == "TClassRef")
    {
        return "*mut c_void";
    }
    if (func.return_type == "TBool" || func.return_type == "TBoolInt")
    {
        return "bool";
    }
    if (func.return_type == "TString" || func.return_type == "TStringOut" ||
        func.return_type == "TStringVoid")
    {
        return "*mut c_void";
    }
    if (func.return_type == "int" || func.return_type == "TArrayLen" ||
        func.return_type == "TByteStringLen")
    {
        return "i32";
    }
    if (func.return_type == "long" || func.return_type == "time_t")
    {
        return "i64";
    }
    if (func.return_type == "unsigned" || func.return_type == "unsigned int")
    {
        return "u32";
    }
    if (func.return_type == "unsigned long" || func.return_type == "wxUIntPtr")
    {
        return "u64";
    }
    if (func.return_type == "uintptr_t")
    {
        return "usize";
    }
    if (func.return_type == "double")
    {
        return "f64";
    }
    if (func.return_type == "float")
    {
        return "f32";
    }
    if (func.return_type == "TChar")
    {
        return "u8";
    }
    if (func.return_type == "TUInt8")
    {
        return "u8";
    }
    if (func.return_type == "size_t")
    {
        return "usize";
    }
    if (func.return_type == "void*")
    {
        return "*mut c_void";
    }
    if (func.return_type.contains('*'))
    {
        return "*mut c_void";
    }

    return "i32";
}

// Generate the conversion expression wrapping a raw FFI return value to safe Rust type.
// `call_expr` is the unsafe FFI call expression.
[[nodiscard]] static std::string WrapReturnExpr(const FunctionDecl& func,
                                                const std::string& callExpr)
{
    const std::string safeType = RustSafeReturnType(func, "");

    if (safeType.empty())
    {
        return callExpr;  // void
    }
    if (safeType == "bool")
    {
        return callExpr + " != 0";
    }
    if (safeType == "i32")
    {
        return callExpr + " as i32";
    }
    if (safeType == "i64")
    {
        return callExpr + " as i64";
    }
    if (safeType == "u32")
    {
        return callExpr + " as u32";
    }
    if (safeType == "u64")
    {
        return callExpr + " as u64";
    }
    if (safeType == "f64")
    {
        return callExpr + " as f64";
    }
    if (safeType == "f32")
    {
        return callExpr + " as f32";
    }
    if (safeType == "u8")
    {
        return callExpr + " as u8";
    }
    if (safeType == "usize")
    {
        return callExpr + " as usize";
    }

    // Pointers pass through
    return callExpr;
}

// Info about a safe Rust parameter in a method signature.
struct RustSafeParam
{
    std::string name;
    std::string safe_type;  // Safe Rust type for the method signature
    std::string ffi_expr;   // Expression to convert and pass to the FFI call
};

// Convert one Param to safe Rust parameters (for method wrappers).
[[nodiscard]] static std::vector<RustSafeParam> ConvertParamToSafe(const Param& param)
{
    std::vector<RustSafeParam> result;

    // TSelf as a non-receiver parameter — treat as opaque pointer
    if (param.macro_name == "TSelf")
    {
        RustSafeParam safe_param;
        safe_param.name = RustEscapeName(param.param_name.empty() ? "arg" : param.param_name);
        safe_param.safe_type = "*mut c_void";
        safe_param.ffi_expr = safe_param.name;
        result.push_back(std::move(safe_param));
        return result;
    }

    // Expanded geometry types: individual i32 params
    if (param.macro_name == "TPoint" || param.macro_name == "TSize" ||
        param.macro_name == "TRect" || param.macro_name == "TVector")
    {
        for (auto& n: RustSplitMacroArg(param.macro_arg))
        {
            const std::string name = RustEscapeName(n);
            result.push_back({ name, "i32", name + " as c_int" });
        }
        return result;
    }

    if (param.macro_name == "TPointLong" || param.macro_name == "TSizeLong" ||
        param.macro_name == "TRectLong" || param.macro_name == "TVectorLong")
    {
        for (auto& n: RustSplitMacroArg(param.macro_arg))
        {
            const std::string name = RustEscapeName(n);
            result.push_back({ name, "i64", name + " as c_long" });
        }
        return result;
    }

    // Output geometry — pass through as raw pointers
    if (param.macro_name == "TPointOut" || param.macro_name == "TSizeOut" ||
        param.macro_name == "TRectOut" || param.macro_name == "TVectorOut")
    {
        for (auto& n: RustSplitMacroArg(param.macro_arg))
        {
            const std::string name = RustEscapeName(n);
            result.push_back({ name, "*mut i32", name + " as *mut c_int" });
        }
        return result;
    }

    if (param.macro_name == "TPointOutVoid" || param.macro_name == "TSizeOutVoid" ||
        param.macro_name == "TRectOutVoid" || param.macro_name == "TVectorOutVoid")
    {
        for (auto& n: RustSplitMacroArg(param.macro_arg))
        {
            const std::string name = RustEscapeName(n);
            result.push_back({ name, "*mut c_void", name });
        }
        return result;
    }

    if (param.macro_name == "TSizeOutDouble")
    {
        for (auto& n: RustSplitMacroArg(param.macro_arg))
        {
            const std::string name = RustEscapeName(n);
            result.push_back({ name, "*mut f64", name + " as *mut c_double" });
        }
        return result;
    }

    if (param.macro_name == "TColorRGB")
    {
        for (auto& n: RustSplitMacroArg(param.macro_arg))
        {
            const std::string name = RustEscapeName(n);
            result.push_back({ name, "u8", name });
        }
        return result;
    }

    // Array types
    if (param.macro_name == "TArrayString" || param.macro_name == "TArrayInt" ||
        param.macro_name == "TByteString" || param.macro_name == "TByteStringLazy")
    {
        std::vector<std::string> names = RustSplitMacroArg(param.macro_arg);
        if (names.size() >= 2)
        {
            const std::string count_name = RustEscapeName(names[0]);
            const std::string array_name = RustEscapeName(names[1]);
            result.push_back({ count_name, "i32", count_name + " as c_int" });
            result.push_back({ array_name, "*mut c_void", array_name });
        }
        return result;
    }

    if (param.macro_name == "TArrayObjectOutVoid")
    {
        const std::string name =
            RustEscapeName(param.param_name.empty() ? "arr" : param.param_name);
        result.push_back({ name, "*mut c_void", name });
        return result;
    }

    // Single parameter
    RustSafeParam safe_param;
    safe_param.name = RustEscapeName(param.param_name.empty() ? "arg" : param.param_name);

    if (param.macro_name == "TClass" || param.macro_name == "TClassRef")
    {  // NOLINT(bugprone-branch-clone)
        safe_param.safe_type = "*mut c_void";
        safe_param.ffi_expr = safe_param.name;
    }
    else if (param.macro_name == "TBool" || param.raw_type == "TBool")
    {
        safe_param.safe_type = "bool";
        safe_param.ffi_expr = safe_param.name + " as c_int";
    }
    else if (param.raw_type == "TBoolInt" || param.raw_type == "TBool*")
    {
        safe_param.safe_type = "i32";
        safe_param.ffi_expr = safe_param.name + " as c_int";
    }
    else if (param.macro_name == "TClosureFun" || param.raw_type == "TClosureFun")
    {
        safe_param.safe_type = "*mut c_void";
        safe_param.ffi_expr = safe_param.name;
    }
    else if (param.raw_type == "TStringVoid" || param.macro_name == "TStringVoid")
    {
        safe_param.safe_type = "*mut c_void";
        safe_param.ffi_expr = safe_param.name;
    }
    else if (param.raw_type == "TArrayIntOutVoid" || param.raw_type == "TArrayIntPtrOutVoid" ||
             param.raw_type == "TArrayStringOutVoid" || param.raw_type == "TByteStringOut" ||
             param.raw_type == "TByteStringLazyOut" || param.raw_type == "TArrayObjectOutVoid")
    {
        safe_param.safe_type = "*mut c_void";
        safe_param.ffi_expr = safe_param.name;
    }
    else if (param.raw_type == "TChar")
    {
        safe_param.safe_type = "u8";
        safe_param.ffi_expr = safe_param.name + " as c_char";
    }
    else if (param.raw_type == "TUInt8")
    {
        safe_param.safe_type = "u8";
        safe_param.ffi_expr = safe_param.name;
    }
    else if (param.raw_type == "TString")
    {
        // char* input: &str — safe Rust callers pass a CStr/pointer
        safe_param.safe_type = "*const c_char";
        safe_param.ffi_expr = safe_param.name;
    }
    else if (param.raw_type == "TStringOut")
    {
        // char* output buffer
        safe_param.safe_type = "*mut c_char";
        safe_param.ffi_expr = safe_param.name;
    }
    else
    {
        // Plain C types
        const std::string& raw_type = param.raw_type;
        if (raw_type.empty() || raw_type == "int")
        {  // NOLINT(bugprone-branch-clone)
            safe_param.safe_type = "i32";
            safe_param.ffi_expr = safe_param.name + " as c_int";
        }
        else if (raw_type == "long")
        {
            safe_param.safe_type = "i64";
            safe_param.ffi_expr = safe_param.name + " as c_long";
        }
        else if (raw_type == "unsigned" || raw_type == "unsigned int")
        {
            safe_param.safe_type = "u32";
            safe_param.ffi_expr = safe_param.name + " as c_uint";
        }
        else if (raw_type == "unsigned long" || raw_type == "wxUIntPtr")
        {
            safe_param.safe_type = "u64";
            safe_param.ffi_expr = safe_param.name + " as c_ulong";
        }
        else if (raw_type == "uintptr_t")
        {
            safe_param.safe_type = "usize";
            safe_param.ffi_expr = safe_param.name + " as usize";
        }
        else if (raw_type == "double")
        {
            safe_param.safe_type = "f64";
            safe_param.ffi_expr = safe_param.name + " as c_double";
        }
        else if (raw_type == "float")
        {
            safe_param.safe_type = "f32";
            safe_param.ffi_expr = safe_param.name + " as c_float";
        }
        else if (raw_type == "size_t")
        {
            safe_param.safe_type = "usize";
            safe_param.ffi_expr = safe_param.name;
        }
        else if (raw_type.contains('*'))
        {
            safe_param.safe_type = "*mut c_void";
            safe_param.ffi_expr = safe_param.name;
        }
        else
        {
            safe_param.safe_type = "i32";
            safe_param.ffi_expr = safe_param.name + " as c_int";
        }
    }

    result.push_back(std::move(safe_param));
    return result;
}

// Build the list of trait names a class should implement based on its hierarchy.
// Returns the chain from the most derived parent to the root.
[[nodiscard]] static std::vector<std::string> BuildTraitChain(const ClassInfo& cls,
                                                              const ParsedFFI& ffi)
{
    std::vector<std::string> chain;
    std::string current = cls.parent;
    std::set<std::string> visited;

    while (!current.empty() && !visited.contains(current))
    {
        visited.insert(current);
        chain.push_back(current);
        const std::unordered_map<std::string, std::string>::const_iterator parent_iter =
            ffi.parent_map.find(current);
        if (parent_iter != ffi.parent_map.end())
        {
            current = parent_iter->second;
        }
        else
        {
            break;
        }
    }

    return chain;
}

// -------------------------------------------------------------------------
// RustEmitter public interface
// -------------------------------------------------------------------------

void RustEmitter::Generate(const ParsedFFI& ffi, const fs::path& outDir)
{
    std::ignore = fs::create_directories(outDir);
    const fs::path srcDir = outDir / "src";
    std::ignore = fs::create_directories(srcDir);

    GenerateCargoToml(outDir);
    GenerateSys(ffi, srcDir);
    GenerateTraits(ffi, srcDir);
    GenerateEvents(ffi, srcDir);
    GenerateKeys(ffi, srcDir);
    GenerateConstants(ffi, srcDir);
    GenerateFreeFunctions(ffi, srcDir);
    GenerateClassFiles(ffi, srcDir);
    GenerateLib(ffi, srcDir);

    std::println(stderr, "Rust: generated crate in {}", outDir.string());
}

VerifyResult RustEmitter::Verify(const ParsedFFI& /* ffi */, const fs::path& /* dir */)
{
    VerifyResult result;
    result.success = false;
    result.messages.emplace_back("Rust verify: use 'kwxgen verify' command instead");
    return result;
}

// -------------------------------------------------------------------------
// Cargo.toml
// -------------------------------------------------------------------------

void RustEmitter::GenerateCargoToml(const fs::path& outDir)
{
    const fs::path path = outDir / "Cargo.toml";
    ConditionalFileWriter out(path);
    if (!out.is_open())
    {
        std::println(stderr, "Error: cannot create {}", path.string());
        return;
    }

    out << "# Code generated by kwxgen. DO NOT EDIT.\n\n";
    out << "[package]\n";
    out << "name = \"kwxffi-sys\"\n";
    out << "version = \"0.1.0\"\n";
    out << "edition = \"2021\"\n";
    out << "description = \"Raw FFI bindings to kwxFFI (wxWidgets C wrapper)\"\n";
    out << "publish = false\n";
    out << "\n";
    out << "[lib]\n";
    out << "name = \"kwxffi_sys\"\n";
    out << "path = \"src/lib.rs\"\n";

    std::println(stderr, "  Cargo.toml");
}

// -------------------------------------------------------------------------
// src/sys.rs — All extern "C" declarations
// -------------------------------------------------------------------------

void RustEmitter::GenerateSys(const ParsedFFI& ffi, const fs::path& srcDir)
{
    const fs::path path = srcDir / "sys.rs";
    ConditionalFileWriter out(path);
    if (!out.is_open())
    {
        std::println(stderr, "Error: cannot create {}", path.string());
        return;
    }

    WriteGeneratedHeader(out);
    out << "#![allow(non_snake_case)]\n\n";
    out << "use std::os::raw::*;\n\n";

    out << "extern \"C\" {\n";

    // Events
    {
        std::vector<EventDecl> sorted = ffi.events;
        std::ranges::sort(sorted, {}, &EventDecl::event_name);

        out << "    // Events\n";
        std::string lastEvtExport;
        for (const auto& event: sorted)
        {
            if (event.export_name == lastEvtExport)
            {
                continue;
            }
            lastEvtExport = event.export_name;
            out << "    pub fn " << event.export_name << "() -> c_int;\n";
        }
        out << "\n";
    }

    // Keys
    {
        std::vector<KeyDecl> sorted = ffi.keys;
        std::ranges::sort(sorted, {}, &KeyDecl::key_name);

        out << "    // Keys\n";
        for (const auto& k: sorted)
        {
            out << "    pub fn " << k.export_name << "() -> c_int;\n";
        }
        out << "\n";
    }

    // Constants
    {
        std::vector<ConstantDecl> sorted = ffi.constants;
        std::ranges::sort(sorted, {}, &ConstantDecl::export_name);

        out << "    // Constants\n";
        for (const auto& cnst: sorted)
        {
            const std::string retType = cnst.return_type.contains('*') ? "*mut c_void" : "c_int";
            out << "    pub fn " << cnst.export_name << "() -> " << retType << ";\n";
        }
        out << "\n";
    }

    // Free functions
    if (!ffi.free_functions.empty())
    {
        out << "    // Free functions\n";
        for (const auto& func: ffi.free_functions)
        {
            if (!IsValidFunction(func))
            {
                continue;
            }
            EmitFunctionExternDecl(out, func);
        }
        out << "\n";
    }

    // Class methods
    size_t methodCount = 0;
    size_t skippedCount = 0;
    for (const auto& cls: ffi.classes)
    {
        if (cls.methods.empty())
        {
            continue;
        }

        out << "    // " << cls.name << "\n";
        for (const auto& method: cls.methods)
        {
            if (!IsValidFunction(method))
            {
                ++skippedCount;
                continue;
            }
            EmitFunctionExternDecl(out, method);
            ++methodCount;
        }
        out << "\n";
    }

    out << "}\n";

    std::print(stderr, "  sys.rs:              {} events, {} keys, {} constants, {} methods",
               ffi.events.size(), ffi.keys.size(), ffi.constants.size(), methodCount);
    if (skippedCount > 0)
    {
        std::print(stderr, " ({} skipped)", skippedCount);
    }
    std::println(stderr, "");
}

// -------------------------------------------------------------------------
// src/traits.rs — Trait hierarchy for safe wrappers
// -------------------------------------------------------------------------

void RustEmitter::GenerateTraits(const ParsedFFI& ffi, const fs::path& srcDir)
{
    const fs::path path = srcDir / "traits.rs";
    ConditionalFileWriter out(path);
    if (!out.is_open())
    {
        std::println(stderr, "Error: cannot create {}", path.string());
        return;
    }

    WriteGeneratedHeader(out);
    out << "use std::os::raw::c_void;\n\n";

    // Collect all classes that serve as parents.
    std::set<std::string> parentClasses;
    for (const auto& [child, parent]: ffi.parent_map)
    {
        parentClasses.insert(parent);
    }

    // Also add classes that are explicitly parents of other classes.
    for (const auto& cls: ffi.classes)
    {
        if (!cls.parent.empty())
        {
            parentClasses.insert(cls.parent);
        }
    }

    // Root trait: WxObject — all wxWidgets objects impl this.
    out << "/// Base trait for all wxWidgets objects providing raw pointer access.\n";
    out << "pub trait WxObject {\n";
    out << "    /// Returns the underlying raw pointer.\n";
    out << "    fn as_ptr(&self) -> *mut c_void;\n";
    out << "\n";
    out << "    /// Returns true if the underlying pointer is null.\n";
    out << "    fn is_null(&self) -> bool {\n";
    out << "        self.as_ptr().is_null()\n";
    out << "    }\n";
    out << "}\n\n";

    // Generate a trait for each class that serves as a parent, establishing the hierarchy.
    // Sort for deterministic output.
    std::vector<std::string> sortedParents(parentClasses.begin(), parentClasses.end());
    std::ranges::sort(sortedParents);

    size_t traitCount = 0;
    for (const auto& parentName: sortedParents)
    {
        const std::string traitName = StripPrefix(parentName);

        // Skip wxObject — it's already defined as the root trait WxObject above.
        if (traitName == "Object")
        {
            continue;
        }

        // Find parent of this parent to establish supertrait.
        const std::unordered_map<std::string, std::string>::const_iterator parent_iter =
            ffi.parent_map.find(parentName);
        std::string supertrait;
        if (parent_iter != ffi.parent_map.end() && !parent_iter->second.empty())
        {
            supertrait = "Wx" + StripPrefix(parent_iter->second);
            // Check if the supertrait's parent is also a parent class; if not, use WxObject
            if (!parentClasses.contains(parent_iter->second))
            {
                supertrait = "WxObject";
            }
        }
        else
        {
            supertrait = "WxObject";
        }

        out << "pub trait Wx" << traitName << ": " << supertrait << " {}\n";
        ++traitCount;
    }

    std::println(stderr, "  traits.rs:           {} traits", traitCount);
}

// -------------------------------------------------------------------------
// src/events.rs
// -------------------------------------------------------------------------

void RustEmitter::GenerateEvents(const ParsedFFI& ffi, const fs::path& srcDir)
{
    const fs::path path = srcDir / "events.rs";
    ConditionalFileWriter out(path);
    if (!out.is_open())
    {
        std::println(stderr, "Error: cannot create {}", path.string());
        return;
    }

    WriteGeneratedHeader(out);
    out << "use crate::sys;\n\n";

    std::vector<EventDecl> sorted = ffi.events;
    std::ranges::sort(sorted, {}, &EventDecl::event_name);

    std::string lastEvtName;
    for (const auto& event: sorted)
    {
        if (event.event_name == lastEvtName)
        {
            continue;
        }
        lastEvtName = event.event_name;
        out << "#[inline]\n";
        out << "pub fn " << event.event_name << "() -> i32 {\n";
        out << "    unsafe { sys::" << event.export_name << "() as i32 }\n";
        out << "}\n\n";
    }

    std::println(stderr, "  events.rs:           {} events", ffi.events.size());
}

// -------------------------------------------------------------------------
// src/keys.rs
// -------------------------------------------------------------------------

void RustEmitter::GenerateKeys(const ParsedFFI& ffi, const fs::path& srcDir)
{
    const fs::path path = srcDir / "keys.rs";
    ConditionalFileWriter out(path);
    if (!out.is_open())
    {
        std::println(stderr, "Error: cannot create {}", path.string());
        return;
    }

    WriteGeneratedHeader(out);
    out << "use crate::sys;\n\n";

    std::vector<KeyDecl> sorted = ffi.keys;
    std::ranges::sort(sorted, {}, &KeyDecl::key_name);

    for (const auto& k: sorted)
    {
        out << "#[inline]\n";
        out << "pub fn " << k.key_name << "() -> i32 {\n";
        out << "    unsafe { sys::" << k.export_name << "() as i32 }\n";
        out << "}\n\n";
    }

    std::println(stderr, "  keys.rs:             {} keys", ffi.keys.size());
}

// -------------------------------------------------------------------------
// src/constants.rs
// -------------------------------------------------------------------------

void RustEmitter::GenerateConstants(const ParsedFFI& ffi, const fs::path& srcDir)
{
    const fs::path path = srcDir / "constants.rs";
    ConditionalFileWriter out(path);
    if (!out.is_open())
    {
        std::println(stderr, "Error: cannot create {}", path.string());
        return;
    }

    WriteGeneratedHeader(out);
    out << "use std::os::raw::c_void;\n\n";
    out << "use crate::sys;\n\n";

    std::vector<ConstantDecl> sorted = ffi.constants;
    std::ranges::sort(sorted, {}, &ConstantDecl::export_name);

    for (const auto& cnst: sorted)
    {
        const bool isPointer = cnst.return_type.contains('*');
        const std::string retType = isPointer ? "*mut c_void" : "i32";
        const std::string cast = isPointer ? "" : " as i32";

        out << "#[inline]\n";
        out << "pub fn " << cnst.constant_name << "() -> " << retType << " {\n";
        out << "    unsafe { sys::" << cnst.export_name << "()" << cast << " }\n";
        out << "}\n\n";
    }

    std::println(stderr, "  constants.rs:        {} constants", ffi.constants.size());
}

// -------------------------------------------------------------------------
// src/freefuncs.rs
// -------------------------------------------------------------------------

void RustEmitter::GenerateFreeFunctions(const ParsedFFI& ffi, const fs::path& srcDir)
{
    if (ffi.free_functions.empty())
    {
        return;
    }

    const fs::path path = srcDir / "freefuncs.rs";
    ConditionalFileWriter out(path);
    if (!out.is_open())
    {
        std::println(stderr, "Error: cannot create {}", path.string());
        return;
    }

    WriteGeneratedHeader(out);
    out << "#![allow(non_snake_case)]\n\n";
    out << "use std::os::raw::*;\n\n";
    out << "use crate::sys;\n\n";

    size_t count = 0;
    for (const auto& func: ffi.free_functions)
    {
        if (!IsValidFunction(func))
        {
            continue;
        }

        const std::string funcName = CFuncName(func);
        const std::string retType = RustSafeReturnType(func, "");

        // Collect safe params (no TSelf for free functions)
        std::vector<RustSafeParam> params;
        for (const auto& param: func.params)
        {
            std::vector<RustSafeParam> expanded = ConvertParamToSafe(param);
            for (auto& safe_param: expanded)
            {
                params.push_back(std::move(safe_param));
            }
        }

        // Function signature
        out << "#[inline]\n";
        out << "pub fn " << funcName << "(";
        for (size_t i = 0; i < params.size(); ++i)
        {
            if (i > 0)
            {
                out << ", ";
            }
            out << params[i].name << ": " << params[i].safe_type;
        }
        out << ")";
        if (!retType.empty())
        {
            out << " -> " << retType;
        }
        out << " {\n";

        // Build FFI call
        std::string call = "sys::" + funcName + "(";
        for (size_t i = 0; i < params.size(); ++i)
        {
            if (i > 0)
            {
                call += ", ";
            }
            call += params[i].ffi_expr;
        }
        call += ")";

        if (retType.empty())
        {
            out << "    unsafe { " << call << " }\n";
        }
        else
        {
            const std::string wrapped = WrapReturnExpr(func, call);
            out << "    unsafe { " << wrapped << " }\n";
        }

        out << "}\n\n";
        ++count;
    }

    std::println(stderr, "  freefuncs.rs:        {} free functions", count);
}

// -------------------------------------------------------------------------
// Per-class safe wrapper files
// -------------------------------------------------------------------------

void RustEmitter::GenerateClassFiles(const ParsedFFI& ffi, const fs::path& srcDir)
{
    size_t fileCount = 0;

    for (const auto& cls: ffi.classes)
    {
        if (cls.methods.empty())
        {
            continue;
        }

        const std::string fileName = RustFileName(cls.name);
        const fs::path path = srcDir / fileName;
        ConditionalFileWriter out(path);
        if (!out.is_open())
        {
            std::println(stderr, "Error: cannot create {}", path.string());
            continue;
        }

        EmitClassFile(out, cls, ffi);
        ++fileCount;
    }

    std::println(stderr, "  class files:         {} files", fileCount);
}

void RustEmitter::EmitClassFile(std::ostream& out, const ClassInfo& cls, const ParsedFFI& ffi)
{
    WriteGeneratedHeader(out);
    out << "#![allow(non_snake_case)]\n\n";
    out << "use std::os::raw::*;\n\n";
    out << "use crate::sys;\n";
    out << "use crate::traits::*;\n\n";

    const std::string rustName = StripPrefix(cls.name);
    const bool hasDelete = HasDeleteMethod(cls);

    // Struct definition
    out << "pub struct " << rustName << " {\n";
    out << "    ptr: *mut c_void,\n";
    if (hasDelete)
    {
        out << "    owned: bool,\n";
    }
    out << "}\n\n";

    // WxObject trait implementation
    out << "impl WxObject for " << rustName << " {\n";
    out << "    fn as_ptr(&self) -> *mut c_void {\n";
    out << "        self.ptr\n";
    out << "    }\n";
    out << "}\n\n";

    // Trait implementations for parent chain
    {
        // Build the full trait chain: immediate parent up to the root.
        const std::vector<std::string> chain = BuildTraitChain(cls, ffi);

        // Collect the set of classes that are used as parents (and thus have traits).
        std::set<std::string> parentClasses;
        for (const auto& [child, parent]: ffi.parent_map)
        {
            parentClasses.insert(parent);
        }
        for (const auto& cls_info: ffi.classes)
        {
            if (!cls_info.parent.empty())
            {
                parentClasses.insert(cls_info.parent);
            }
        }

        for (const auto& parentName: chain)
        {
            const std::string traitName = "Wx" + StripPrefix(parentName);
            if (traitName == "WxObject")
            {
                continue;  // Already explicitly implemented above
            }
            if (parentClasses.contains(parentName))
            {
                out << "impl " << traitName << " for " << rustName << " {}\n";
            }
        }

        // If the class itself is used as a parent, it has a trait too.
        if (parentClasses.contains(cls.name))
        {
            out << "impl Wx" << rustName << " for " << rustName << " {}\n";
        }

        if (!chain.empty())
        {
            out << "\n";
        }
    }

    // Drop implementation for classes with Delete
    if (hasDelete)
    {
        // Find the actual Delete method for the correct FFI function name
        std::string deleteFuncName;
        for (const auto& method: cls.methods)
        {
            if (IsDestructor(method))
            {
                deleteFuncName = CFuncName(method);
                break;
            }
        }

        out << "impl Drop for " << rustName << " {\n";
        out << "    fn drop(&mut self) {\n";
        out << "        if self.owned && !self.ptr.is_null() {\n";
        out << "            unsafe { sys::" << deleteFuncName << "(self.ptr); }\n";
        out << "        }\n";
        out << "    }\n";
        out << "}\n\n";
    }

    // from_ptr constructor (non-owning)
    out << "impl " << rustName << " {\n";
    out << "    /// Wraps a raw pointer as a non-owning reference. Does not call Delete on "
           "drop.\n";
    out << "    ///\n";
    out << "    /// # Safety\n";
    out << "    /// The pointer must be a valid " << cls.name << " pointer.\n";
    out << "    pub unsafe fn from_ptr(ptr: *mut c_void) -> Self {\n";
    if (hasDelete)
    {
        out << "        Self { ptr, owned: false }\n";
    }
    else
    {
        out << "        Self { ptr }\n";
    }
    out << "    }\n\n";

    // Emit methods
    size_t methodCount = 0;
    std::set<std::string> emittedMethods;
    for (const auto& method: cls.methods)
    {
        if (!IsValidFunction(method))
        {
            continue;
        }

        // Skip destructor — handled by Drop
        if (IsDestructor(method))
        {
            continue;
        }

        // Collect safe params (excluding the first TSelf which becomes &self)
        std::vector<RustSafeParam> params;
        bool selfSkipped = false;
        for (const auto& param: method.params)
        {
            if (!selfSkipped && param.macro_name == "TSelf")
            {
                selfSkipped = true;
                continue;
            }
            std::vector<RustSafeParam> expanded = ConvertParamToSafe(param);
            for (auto& safe_param: expanded)
            {
                params.push_back(std::move(safe_param));
            }
        }

        const std::string cFuncName = CFuncName(method);
        const std::string retType = RustSafeReturnType(method, rustName);

        if (method.is_constructor && !method.has_self)
        {
            // Constructor: pub fn new(...) -> Option<Self>
            // Only treat as constructor if return type is a pointer.
            const bool isPointerReturn = method.return_type.contains('*') ||
                                         method.return_macro == "TClass" ||
                                         method.return_macro == "TSelf";
            if (!isPointerReturn)
            {
                continue;  // Skip non-pointer constructors (e.g., returns int)
            }

            const std::string methodName =
                (method.method_name == "Create") ? "new" : RustSafeMethodName(method.method_name);

            if (!emittedMethods.insert(methodName).second)
            {
                continue;  // Skip duplicate method name
            }

            out << "    pub fn " << methodName << "(";
            for (size_t i = 0; i < params.size(); ++i)
            {
                if (i > 0)
                {
                    out << ", ";
                }
                out << params[i].name << ": " << params[i].safe_type;
            }
            out << ") -> Option<Self> {\n";

            // Build FFI call
            out << "        let ptr = unsafe { sys::" << cFuncName << "(";
            for (size_t i = 0; i < params.size(); ++i)
            {
                if (i > 0)
                {
                    out << ", ";
                }
                out << params[i].ffi_expr;
            }
            out << ") };\n";

            out << "        if ptr.is_null() {\n";
            out << "            None\n";
            out << "        } else {\n";
            if (hasDelete)
            {
                out << "            Some(Self { ptr, owned: true })\n";
            }
            else
            {
                out << "            Some(Self { ptr })\n";
            }
            out << "        }\n";
            out << "    }\n\n";
        }
        else
        {
            // Regular method: pub fn method_name(&self, ...) -> RetType
            const std::string methodName = RustSafeMethodName(method.method_name);

            if (!emittedMethods.insert(methodName).second)
            {
                continue;  // Skip duplicate method name
            }

            out << "    pub fn " << methodName << "(&self";
            for (const auto& safe_param: params)
            {
                out << ", " << safe_param.name << ": " << safe_param.safe_type;
            }
            out << ")";
            if (!retType.empty())
            {
                out << " -> " << retType;
            }
            out << " {\n";

            // Build FFI call
            std::string call = "sys::" + cFuncName + "(";
            bool first = true;
            if (method.has_self)
            {
                call += "self.ptr";
                first = false;
            }
            for (const auto& safe_param: params)
            {
                if (!first)
                {
                    call += ", ";
                }
                call += safe_param.ffi_expr;
                first = false;
            }
            call += ")";

            if (retType.empty())
            {
                out << "        unsafe { " << call << " }\n";
            }
            else
            {
                const std::string wrapped = WrapReturnExpr(method, call);
                out << "        unsafe { " << wrapped << " }\n";
            }

            out << "    }\n\n";
        }
        ++methodCount;
    }

    out << "}\n";
}

// -------------------------------------------------------------------------
// src/lib.rs — Module root
// -------------------------------------------------------------------------

void RustEmitter::GenerateLib(const ParsedFFI& ffi, const fs::path& srcDir)
{
    const fs::path path = srcDir / "lib.rs";
    ConditionalFileWriter out(path);
    if (!out.is_open())
    {
        std::println(stderr, "Error: cannot create {}", path.string());
        return;
    }

    WriteGeneratedHeader(out);
    out << "#![allow(non_snake_case, non_camel_case_types, unused_imports)]\n\n";

    // Core modules
    out << "pub mod sys;\n";
    out << "pub mod traits;\n";
    out << "pub mod events;\n";
    out << "pub mod keys;\n";
    out << "pub mod constants;\n";

    if (!ffi.free_functions.empty())
    {
        out << "pub mod freefuncs;\n";
    }

    out << "\n";

    // Class modules — one per class with methods
    std::vector<std::string> modNames;
    for (const auto& cls: ffi.classes)
    {
        if (cls.methods.empty())
        {
            continue;
        }
        modNames.push_back(RustModuleName(cls.name));
    }

    // Sort for deterministic output
    std::ranges::sort(modNames);

    // Remove duplicates (unlikely but defensive)
    modNames.erase(std::ranges::unique(modNames).begin(), modNames.end());

    for (const auto& modName: modNames)
    {
        out << "pub mod " << modName << ";\n";
    }

    std::println(stderr, "  lib.rs:              {} class modules", modNames.size());
}

// NOLINTEND(readability-magic-string)
