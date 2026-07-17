#include <iostream>
#include <lsp/requests/initialize.h>
int main() {
    lsp::requests::Initialize::Params p;
    auto u = p.rootUri;
    auto pth = p.rootPath;
    return 0;
}
