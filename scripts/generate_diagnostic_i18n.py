import os
import re
import json
import sys

AS_TEXTS_PATH = os.path.join("build", "_deps", "angelscript_repo-src", "sdk", "angelscript", "source", "as_texts.h")
OUTPUT_DIR = os.path.join("server", "src", "i18n")
OUTPUT_INC = os.path.join("server", "include", "i18n")
JSON_PATH = os.path.join("scripts", "diagnostics_es.json")

def main():
    if not os.path.exists(AS_TEXTS_PATH):
        print(f"Error: {AS_TEXTS_PATH} not found")
        sys.exit(1)
        
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    os.makedirs(OUTPUT_INC, exist_ok=True)
    
    macros = []
    
    # Parse as_texts.h to extract all defined diagnostic messages
    with open(AS_TEXTS_PATH, 'r', encoding='utf-8') as f:
        for line in f:
            match = re.match(r'^#define\s+(TXT_[A-Za-z0-9_]+)\s+"([^"]+)"', line.strip())
            if match:
                macro_name = match.group(1)
                english_text = match.group(2)
                macros.append((macro_name, english_text))
                
    # Load existing translations if they exist
    translations = {}
    if os.path.exists(JSON_PATH):
        with open(JSON_PATH, 'r', encoding='utf-8') as f:
            translations = json.load(f)
            
    # Auto-translate common diagnostics (seed the dictionary)
    common_translations = {
        "Expected expression value": "Se esperaba un valor de expresión",
        "Initialization lists cannot be used with '%s'": "Las listas de inicialización no se pueden usar con el tipo '%s'",
        "No matching signatures to '%s'": "No hay firmas coincidentes para la función '%s'",
        "Candidates are:": "Las firmas candidatas son:",
        "Can't implicitly convert from '%s' to '%s'.": "No se puede convertir implícitamente de '%s' a '%s'.",
        "Expected '%s'": "Se esperaba '%s'",
        "Expected data type": "Se esperaba un tipo de dato",
        "Expected identifier": "Se esperaba un identificador",
        "Identifier '%s' is not a data type in global namespace": "El identificador '%s' no es un tipo de dato en el espacio de nombres global",
        "Identifier '%s' is not a data type": "El identificador '%s' no es un tipo de dato",
        "Name '%s' is not a variable.": "El nombre '%s' no es una variable.",
        "Unrecognized characters found": "Se encontraron caracteres no reconocidos",
        "No appropriate opAssign method found in '%s' for value assignment": "No se encontró un método opAssign apropiado en '%s' para la asignación",
        "Data type can't be '%s'": "El tipo de dato no puede ser '%s'",
        "Compiling %s": "Compilando %s",
        "Variable '%s' is already defined": "La variable '%s' ya está definida",
        "Function '%s' already defined": "La función '%s' ya está definida",
        "'%s' is not declared": "'%s' no está declarado",
        "Multiple matching signatures to '%s'": "Existen múltiples firmas coincidentes para '%s'",
        "Method '%s' not found": "No se encontró el método '%s'",
        "Global variable '%s' not found": "No se encontró la variable global '%s'",
        "No default constructor for object of type '%s'.": "No hay constructor por defecto para el objeto de tipo '%s'."
    }
    
    # Fill missing translations with the english original as fallback
    for _, eng in macros:
        if eng not in translations:
            translations[eng] = common_translations.get(eng, eng)
            
    # Save the dictionary sorted alphabetically so it looks clean and professional
    with open(JSON_PATH, 'w', encoding='utf-8') as f:
        json.dump(translations, f, indent=4, ensure_ascii=False, sort_keys=True)
        
    print(f"Found {len(macros)} errors. Updated dictionary at {JSON_PATH}")
    
    # Generate C++ code
    static_mappings = []
    regex_mappings = []
    
    for macro, eng in macros:
        translated = translations.get(eng, eng)
        # Check if it has %s, %d, %u, etc.
        if '%' in eng and translated != eng:
            # Convert printf format to regex pattern
            pattern = re.escape(eng)
            pattern = pattern.replace('%s', '(.*)')
            pattern = pattern.replace('%d', '(-?\\\\d+)')
            pattern = pattern.replace('%u', '(\\\\d+)')
            pattern = pattern.replace('%i', '(-?\\\\d+)')
            
            # Count the number of capture groups
            num_groups = pattern.count('(.*)') + pattern.count('(-?\\d+)') + pattern.count('(\\d+)')
            
            # Build the replacement string using regex matches
            replacement_str = ""
            if num_groups == 1:
                if '%s' in translated or '%d' in translated or '%i' in translated or '%u' in translated:
                    replacement_str = translated.replace('%s', '" + match[1].str() + "').replace('%d', '" + match[1].str() + "').replace('%i', '" + match[1].str() + "').replace('%u', '" + match[1].str() + "')
                else:
                    replacement_str = translated
            elif num_groups == 2:
                # Naive replacement for 2 variables, assumes same order
                parts = re.split(r'\%[sdiu]', translated)
                if len(parts) >= 3:
                    replacement_str = parts[0] + '" + match[1].str() + "' + parts[1] + '" + match[2].str() + "' + parts[2]
                else:
                    replacement_str = translated
            else:
                 # Fallback for complex ones
                 replacement_str = translated
                 
            # Note: For regex, C++ needs raw strings R"()"
            regex_mappings.append(f"""
    {{
        static const std::regex rgx(R"({pattern})");
        std::smatch match;
        if (std::regex_match(originalMsg, match, rgx)) {{
            return std::string("{replacement_str}");
        }}
    }}""")
        elif '%' not in eng:
            if eng != translated: # Only add if it's actually translated to save map size
                escaped_eng = eng.replace('"', '\\"')
                escaped_translated = translated.replace('"', '\\"')
                static_mappings.append(f'        {{"{escaped_eng}", "{escaped_translated}"}}')
            
    header_content = """#pragma once
#include <string>
#include "i18n/LspStrings.h"

namespace i18n {
    class DiagnosticI18n {
    public:
        // Translates an AngelScript diagnostic message based on the given locale
        static std::string Translate(const std::string& originalMsg, Locale locale);
    };
}
"""

    cpp_content = f"""#include "i18n/DiagnosticI18n.h"
#include <ankerl/unordered_dense.h>
#include <regex>

namespace i18n {{

std::string DiagnosticI18n::Translate(const std::string& originalMsg, Locale locale)
{{
    if (locale == Locale::EN || locale == Locale::UNKNOWN) {{
        return originalMsg;
    }}
    
    if (locale == Locale::ES) {{
        // 1. Static exact matches (O(1) lookup using unordered_dense)
        static const ankerl::unordered_dense::map<std::string, std::string> staticMap = {{
{",\n".join(static_mappings)}
        }};
        
        auto it = staticMap.find(originalMsg);
        if (it != staticMap.end()) {{
            return it->second;
        }}
        
        // 2. Dynamic regex matches (Sequential)
{''.join(regex_mappings)}
    }}
    
    // Fallback
    return originalMsg;
}}

}} // namespace i18n
"""
    
    with open(os.path.join(OUTPUT_INC, "DiagnosticI18n.h"), 'w', encoding='utf-8') as f:
        f.write(header_content)
        
    with open(os.path.join(OUTPUT_DIR, "DiagnosticI18n.cpp"), 'w', encoding='utf-8') as f:
        f.write(cpp_content)
        
    print("Generated DiagnosticI18n.h and DiagnosticI18n.cpp successfully.")

if __name__ == "__main__":
    main()
