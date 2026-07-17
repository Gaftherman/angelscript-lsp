#include "analysis/PredefinedLoader.h"
#include <angelscript.h>
#include <iostream>

using namespace analysis;

int main() {
    asIScriptEngine* engine = asCreateScriptEngine();
    SymbolTable table;
    const char* src = "class array<T> { uint length() const; }";
    PredefinedLoader::LoadFromSource(src, engine, table, "string", "array");
    
    asIScriptModule* mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
    mod->AddScriptSection("test", "void Main() { array<int> arra; }");
    int r = mod->Build();
    std::cout << "Build result: " << r << std::endl;
    return r;
}
