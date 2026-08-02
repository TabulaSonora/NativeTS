// Imports the engine the way a host would and asks it something that needs no ROM: the embedded
// manifest names the one DLL build every offset is pinned to. Reaching that string exercises the
// public headers, the archive and the assets compiled into it.
//
// It also stands as the check that the private build dependency stayed private -- this file parses
// no JSON and links no JSON library, and the engine still answers.

#include <tabulasonora/table_manifest.hpp>

#include <cstdio>
#include <string>

int main()
{
    const ts::DllIdentity& dll = ts::TableManifest::defaults().dll();

    const std::string expected = "117e6aa147a96fbde5e10d2caf16c89965acc1e44235fd245992216cc620bdb1";
    if (dll.sha256 != expected) {
        std::fprintf(stderr, "imported engine reports SHA-256 %s, expected %s\n",
                     dll.sha256.c_str(), expected.c_str());
        return 1;
    }

    std::printf("imported ts::tabulasonora, pinned to %s %s\n",
                dll.product.c_str(), dll.version.c_str());
    return 0;
}
