/////////////////////////////////////////////////////////////////////////////
// Purpose:   Perl FFI::Platypus code generator
// Author:    Ralph Walden
// Copyright: Copyright (c) 2026 KeyWorks Software (Ralph Walden)
// License:   Apache License -- see ../LICENSE
/////////////////////////////////////////////////////////////////////////////

#pragma once

#include "../emitter.h"

class PerlEmitter : public LanguageEmitter
{
public:
    void Generate(const ParsedFFI& ffi, const std::filesystem::path& outDir) override;
    VerifyResult Verify(const ParsedFFI& ffi, const std::filesystem::path& dir) override;
    [[nodiscard]] std::string_view Name() const override { return "perl"; }

private:
    // Layer 1: Raw FFI modules (lib/wx/ffi/raw/)
    void GenerateRawClassFiles(const ParsedFFI& ffi, const std::filesystem::path& rawDir);
    void GenerateRawConstants(const ParsedFFI& ffi, const std::filesystem::path& rawDir);
    void GenerateRawFreeFunctions(const ParsedFFI& ffi, const std::filesystem::path& rawDir);
    void GenerateRawInit(const ParsedFFI& ffi, const std::filesystem::path& rawDir);

    // Layer 2: OO wrapper modules (lib/wx/)
    void GenerateOOClasses(const ParsedFFI& ffi, const std::filesystem::path& ooDir);
    void EmitOOClassFile(std::ostream& out, const ClassInfo& class_info, const ParsedFFI& ffi);
};
