#include "tabulasonora/table_manifest.hpp"

#include <CLI/CLI.hpp>

#include <exception>
#include <iostream>
#include <string>

namespace {

/// Reports what the embedded offset map says the engine expects.
///
/// This is `info` without a DLL: it answers "which file do I need?" before you have one, which is
/// the question a new user actually has. `info <SCCore.dll>` arrives with the ROM reader.
int manifest_command()
{
    const ts::TableManifest& manifest = ts::TableManifest::defaults();
    const ts::DllIdentity& dll = manifest.dll();

    std::cout << "expects   " << dll.product << ' ' << dll.version << " (" << dll.file_name << ")\n"
              << "size      " << dll.size << " bytes\n"
              << "sha256    " << dll.sha256 << '\n'
              << "timestamp " << dll.pe_timestamp << " (PE TimeDateStamp)\n"
              << "tables    " << manifest.cached_tables().size() << " static, "
              << manifest.live_regions().size() << " live regions\n";

    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"A native implementation of the Roland Sound Canvas VA synth voice.",
                 "tabula-sonora"};
    app.require_subcommand(1);

    CLI::App* manifest = app.add_subcommand("manifest", "Show the pinned DLL build and table map.");

    CLI11_PARSE(app, argc, argv);

    try {
        if (manifest->parsed()) {
            return manifest_command();
        }
    } catch (const std::exception& error) {
        std::cerr << "tabula-sonora: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
