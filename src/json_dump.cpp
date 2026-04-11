/////////////////////////////////////////////////////////////////////////////
// Purpose:   Dump the parsed FFI model as JSON output
// Author:    Ralph Walden
// Copyright: Copyright (c) 2026 KeyWorks Software (Ralph Walden)
// License:   Apache License -- see LICENSE
/////////////////////////////////////////////////////////////////////////////

#include "json_dump.h"

#include <fstream>
#include <iostream>

// Escape a string for JSON output.
static std::string JsonEscape(const std::string& text)
{
    std::string result;
    constexpr size_t ESCAPE_RESERVE = 4;
    result.reserve(text.size() + ESCAPE_RESERVE);
    for (const char curr: text)
    {
        switch (curr)
        {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += curr;
        }
    }
    return result;
}

static void WriteParam(std::ostream& output, const Param& param, const std::string& indent)
{
    output << indent << "{\n";
    output << indent << "  \"raw_type\": \"" << JsonEscape(param.raw_type) << "\",\n";
    output << indent << "  \"macro_name\": \"" << JsonEscape(param.macro_name) << "\",\n";
    output << indent << "  \"macro_arg\": \"" << JsonEscape(param.macro_arg) << "\",\n";
    output << indent << "  \"param_name\": \"" << JsonEscape(param.param_name) << "\"\n";
    output << indent << "}";
}

static void WriteFunctionDecl(std::ostream& output, const FunctionDecl& func_decl,
                              const std::string& indent)
{
    output << indent << "{\n";
    output << indent << "  \"class_name\": \"" << JsonEscape(func_decl.class_name) << "\",\n";
    output << indent << "  \"method_name\": \"" << JsonEscape(func_decl.method_name) << "\",\n";
    output << indent << "  \"c_func_name\": \"" << JsonEscape(func_decl.c_func_name) << "\",\n";
    output << indent << "  \"return_type\": \"" << JsonEscape(func_decl.return_type) << "\",\n";
    output << indent << "  \"return_macro\": \"" << JsonEscape(func_decl.return_macro) << "\",\n";
    output << indent << "  \"return_arg\": \"" << JsonEscape(func_decl.return_arg) << "\",\n";
    output << indent << "  \"is_constructor\": " << (func_decl.is_constructor ? "true" : "false")
           << ",\n";
    output << indent << "  \"is_destructor\": " << (func_decl.is_destructor ? "true" : "false")
           << ",\n";
    output << indent << "  \"has_self\": " << (func_decl.has_self ? "true" : "false") << ",\n";
    output << indent << "  \"params\": [";
    if (func_decl.params.empty())
    {
        output << "]";
    }
    else
    {
        output << "\n";
        for (size_t i = 0; i < func_decl.params.size(); ++i)
        {
            WriteParam(output, func_decl.params[i], indent + "    ");
            if (i + 1 < func_decl.params.size())
            {
                output << ",";
            }
            output << "\n";
        }
        output << indent << "  ]";
    }
    output << "\n" << indent << "}";
}

void DumpJson(const ParsedFFI& parsed_ffi, std::ostream& output)
{
    output << "{\n";

    // Summary counts
    size_t total_methods = 0;
    for (const auto& class_info: parsed_ffi.classes)
    {
        total_methods += class_info.methods.size();
    }
    output << "  \"summary\": {\n";
    output << "    \"events\": " << parsed_ffi.events.size() << ",\n";
    output << "    \"keys\": " << parsed_ffi.keys.size() << ",\n";
    output << "    \"constants\": " << parsed_ffi.constants.size() << ",\n";
    output << "    \"classes\": " << parsed_ffi.classes.size() << ",\n";
    output << "    \"total_methods\": " << total_methods << ",\n";
    output << "    \"free_functions\": " << parsed_ffi.free_functions.size() << "\n";
    output << "  },\n";

    // Events
    output << "  \"events\": [\n";
    for (size_t i = 0; i < parsed_ffi.events.size(); ++i)
    {
        output << "    { \"export_name\": \"" << JsonEscape(parsed_ffi.events[i].export_name)
               << "\", \"event_name\": \"" << JsonEscape(parsed_ffi.events[i].event_name) << "\" }";
        if (i + 1 < parsed_ffi.events.size())
        {
            output << ",";
        }
        output << "\n";
    }
    output << "  ],\n";

    // Keys
    output << "  \"keys\": [\n";
    for (size_t i = 0; i < parsed_ffi.keys.size(); ++i)
    {
        output << "    { \"export_name\": \"" << JsonEscape(parsed_ffi.keys[i].export_name)
               << "\", \"key_name\": \"" << JsonEscape(parsed_ffi.keys[i].key_name) << "\" }";
        if (i + 1 < parsed_ffi.keys.size())
        {
            output << ",";
        }
        output << "\n";
    }
    output << "  ],\n";

    // Constants
    output << "  \"constants\": [\n";
    for (size_t i = 0; i < parsed_ffi.constants.size(); ++i)
    {
        const ConstantDecl& constant = parsed_ffi.constants[i];
        output << "    { \"export_name\": \"" << JsonEscape(constant.export_name)
               << "\", \"constant_name\": \"" << JsonEscape(constant.constant_name)
               << "\", \"return_type\": \"" << JsonEscape(constant.return_type) << "\" }";
        if (i + 1 < parsed_ffi.constants.size())
        {
            output << ",";
        }
        output << "\n";
    }
    output << "  ],\n";

    // Free functions
    output << "  \"free_functions\": [\n";
    for (size_t i = 0; i < parsed_ffi.free_functions.size(); ++i)
    {
        WriteFunctionDecl(output, parsed_ffi.free_functions[i], "    ");
        if (i + 1 < parsed_ffi.free_functions.size())
        {
            output << ",";
        }
        output << "\n";
    }
    output << "  ],\n";

    // Classes (empty for Phase 3)
    output << "  \"classes\": [\n";
    for (size_t i = 0; i < parsed_ffi.classes.size(); ++i)
    {
        const ClassInfo& class_info = parsed_ffi.classes[i];
        output << "    {\n";
        output << "      \"name\": \"" << JsonEscape(class_info.name) << "\",\n";
        output << "      \"parent\": \"" << JsonEscape(class_info.parent) << "\",\n";
        output << "      \"is_window_derived\": "
               << (class_info.is_window_derived ? "true" : "false") << ",\n";
        output << "      \"is_object_derived\": "
               << (class_info.is_object_derived ? "true" : "false") << ",\n";
        output << "      \"is_mixin\": " << (class_info.is_mixin ? "true" : "false") << ",\n";
        output << "      \"methods\": [\n";
        for (size_t j = 0; j < class_info.methods.size(); ++j)
        {
            WriteFunctionDecl(output, class_info.methods[j], "        ");
            if (j + 1 < class_info.methods.size())
            {
                output << ",";
            }
            output << "\n";
        }
        output << "      ]\n";
        output << "    }";
        if (i + 1 < parsed_ffi.classes.size())
        {
            output << ",";
        }
        output << "\n";
    }
    output << "  ],\n";

    // Parent map
    output << "  \"parent_map\": {\n";
    {
        size_t index = 0;
        for (const auto& [child, parent]: parsed_ffi.parent_map)
        {
            output << "    \"" << JsonEscape(child) << "\": \"" << JsonEscape(parent) << "\"";
            if (++index < parsed_ffi.parent_map.size())
            {
                output << ",";
            }
            output << "\n";
        }
    }
    output << "  },\n";

    // Mixin map
    output << "  \"mixin_map\": {\n";
    {
        size_t index = 0;
        for (const auto& [consumer, mixins]: parsed_ffi.mixin_map)
        {
            output << "    \"" << JsonEscape(consumer) << "\": [";
            for (size_t i = 0; i < mixins.size(); ++i)
            {
                if (i > 0)
                {
                    output << ", ";
                }
                output << "\"" << JsonEscape(mixins[i]) << "\"";
            }
            output << "]";
            if (++index < parsed_ffi.mixin_map.size())
            {
                output << ",";
            }
            output << "\n";
        }
    }
    output << "  }\n";

    output << "}\n";
}

bool DumpJsonToFile(const ParsedFFI& parsed_ffi, const std::string& file_path)
{
    std::ofstream file(file_path);
    if (!file.is_open())
    {
        std::cerr << "Error: cannot open output file " << file_path << "\n";
        return false;
    }
    DumpJson(parsed_ffi, file);
    return true;
}
