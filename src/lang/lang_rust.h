/////////////////////////////////////////////////////////////////////////////
// Purpose:   Rust FFI code generator with safe wrappers
// Author:    Ralph Walden
// Copyright: Copyright (c) 2026 KeyWorks Software (Ralph Walden)
// License:   Apache License -- see ../LICENSE
/////////////////////////////////////////////////////////////////////////////

#pragma once

#include "../emitter.h"

class RustEmitter : public LanguageEmitter
{
public:
    void Generate(const ParsedFFI& ffi, const std::filesystem::path& outDir) override;
    [[nodiscard]] VerifyResult Verify(const ParsedFFI& ffi,
                                      const std::filesystem::path& dir) override;
    [[nodiscard]] std::string_view Name() const override { return "rust"; }

private:
    static void GenerateCargoToml(const std::filesystem::path& outDir);
    static void GenerateSys(const ParsedFFI& ffi, const std::filesystem::path& srcDir);
    static void GenerateTraits(const ParsedFFI& ffi, const std::filesystem::path& srcDir);
    static void GenerateEvents(const ParsedFFI& ffi, const std::filesystem::path& srcDir);
    static void GenerateKeys(const ParsedFFI& ffi, const std::filesystem::path& srcDir);
    static void GenerateConstants(const ParsedFFI& ffi, const std::filesystem::path& srcDir);
    static void GenerateFreeFunctions(const ParsedFFI& ffi, const std::filesystem::path& srcDir);
    static void GenerateClassFiles(const ParsedFFI& ffi, const std::filesystem::path& srcDir);
    static void GenerateLib(const ParsedFFI& ffi, const std::filesystem::path& srcDir);

    // Emit a single class safe wrapper file.
    static void EmitClassFile(std::ostream& out, const ClassInfo& cls, const ParsedFFI& ffi);
};
