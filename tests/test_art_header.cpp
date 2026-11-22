#include "mtg_protocol.h"

#include <cassert>
#include <cstring>
#include <iostream>

using mtg_protocol::ArtworkHeader;
using mtg_protocol::ArtworkHeaderResult;

int main() {
    ArtworkHeader header;
    assert(mtg_protocol::parseArtworkHeader(
        "MTG1 ART A1B2C3D4 259200 0123ABCD", header) == ArtworkHeaderResult::Ok);
    assert(std::strcmp(header.id, "A1B2C3D4") == 0);
    assert(header.bytes == 259200);
    assert(header.crc == 0x0123ABCDu);

    assert(mtg_protocol::parseArtworkHeader(
        "MTG1 ART a1b2c3d4 259200 89abcdef", header) == ArtworkHeaderResult::Ok);
    assert(std::strcmp(header.id, "A1B2C3D4") == 0);
    assert(header.crc == 0x89ABCDEFu);

    assert(mtg_protocol::parseArtworkHeader(
        "MTG1 ART A1B2C3D4 64800 0123ABCD", header) == ArtworkHeaderResult::WrongSize);
    assert(header.bytes == 64800);

    const char *bad[] = {
        "MTG1 ART A1B2C3D4 259200 0123ABCG",
        "MTG1 ART A1B2C3D 259200 0123ABCD",
        "MTG1 ART A1B2C3D4 -1 0123ABCD",
        "MTG1 ART A1B2C3D4 259200 0123ABCD trailing",
        "MTG1 ART A1B2C3D4 259200",
        "MTG1 ART A1B2C3D4 99999999999999999999999999 0123ABCD",
        "MTG1 ART A1B2C3D4 259200 0123ABCD ",
        "MTG1 BEGIN 259200 0123ABCD"
    };
    for (const char *line : bad)
        assert(mtg_protocol::parseArtworkHeader(line, header) == ArtworkHeaderResult::Invalid);

    std::cout << "Artwork header parser: all tests passed\n";
}
